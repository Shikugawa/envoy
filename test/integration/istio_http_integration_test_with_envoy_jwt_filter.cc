/* Copyright 2019 Istio Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This test suite verifies the end-to-end behaviour of the HTTP filter chain
// with JWT + AuthN + Mixer. That chain is used in Istio, when authentication is
// active. Filters exchanges data between each other using request info (dynamic
// metadata) and that information can only be observed at the end (i.e from
// request to mixer backends).

#include "extensions/filters/http/well_known_names.h"
#include "fmt/printf.h"
#include "gmock/gmock.h"
#include "include/istio/utils/attribute_names.h"
#include "mixer/v1/mixer.pb.h"
#include "src/envoy/utils/filter_names.h"
#include "src/envoy/utils/trace_headers.h"
#include "test/integration/http_protocol_integration.h"

using ::google::protobuf::util::error::Code;
using ::testing::Contains;
using ::testing::Not;

namespace Envoy {
namespace {

// From
// https://github.com/istio/istio/blob/master/security/tools/jwt/samples/demo.jwt
constexpr char kGoodToken[] =
    "eyJhbGciOiJSUzI1NiIsImtpZCI6IkRIRmJwb0lVcXJZOHQyenBBMnFYZkNtcjVWTzVaRXI0Un"
    "pIVV8tZW52dlEiLC"
    "J0eXAiOiJKV1QifQ."
    "eyJleHAiOjQ2ODU5ODk3MDAsImZvbyI6ImJhciIsImlhdCI6MTUzMjM4OTcwMCwiaXNzIjoidG"
    "VzdGluZ0BzZWN1cmUuaXN0aW8uaW8iLCJzdWIiOiJ0ZXN0aW5nQHNlY3VyZS5pc3Rpby5pbyJ9"
    ".CfNnxWP2tcnR9q0v"
    "xyxweaF3ovQYHYZl82hAUsn21bwQd9zP7c-LS9qd_vpdLG4Tn1A15NxfCjp5f7QNBUo-"
    "KC9PJqYpgGbaXhaGx7bEdFW"
    "jcwv3nZzvc7M__"
    "ZpaCERdwU7igUmJqYGBYQ51vr2njU9ZimyKkfDe3axcyiBZde7G6dabliUosJvvKOPcKIWPccC"
    "gef"
    "Sj_GNfwIip3-SsFdlR7BtbVUcqR-yv-"
    "XOxJ3Uc1MI0tz3uMiiZcyPV7sNCU4KRnemRIMHVOfuvHsU60_GhGbiSFzgPT"
    "Aa9WTltbnarTbxudb_YEOx12JiwYToeX0DCPb43W1tzIBxgm8NxUg";

// Key-value pairs from the above JWT's payload.
constexpr char kExpectedIss[] = "\"iss\":\"testing@secure.istio.io\"";
constexpr char kExpectedIat[] = "\"iat\":1532389700";
constexpr char kExpectedExp[] = "\"exp\":4685989700";
constexpr char kExpectedSub[] = "\"sub\":\"testing@secure.istio.io\"";
constexpr char kExpectedFoo[] = "\"foo\":\"bar\"";

// Generated by gen-jwt.py as described in
// https://github.com/istio/istio/blob/master/security/tools/jwt/samples/README.md.
// `security/tools/jwt/samples/gen-jwt.py security/tools/jwt/samples/key.pem
//  --expire=3153600000 --claims=rbac:rbac --iss "testing-rbac@secure.istio.io"`
constexpr char kRbacGoodToken[] =
    "eyJhbGciOiJSUzI1NiIsImtpZCI6IkRIRmJwb0lVcXJZOHQyenBBMnFYZkNtcjVWTzVaRXI0Un"
    "pIVV8tZW52dlEiLCJ0eXAiOiJKV1QifQ.eyJleHAiOjQ2ODc3ODQwODEsImlhdCI6MTUzNDE4N"
    "DA4MSwiaXNzIjoidGVzdGluZy1yYmFjQHNlY3VyZS5pc3Rpby5pbyIsInJiYWMiOiJyYmFjIiw"
    "ic3ViIjoidGVzdGluZy1yYmFjQHNlY3VyZS5pc3Rpby5pbyJ9.Cn4PADSzZ249_DMCFWF_JokR"
    "bVgY-yoGkVqpW-aYHTYDShuLxfAdF1AAq5TLAi72A0UWBxwcZMIGcAudRdyM8-6ppXlj3P3Xg1"
    "87d25-4EWR0SgVnW8DT2LCpeX9amPsKkKdo0L_ICfHzATsiqIN2GGvrIZWYHHrD1gNGwLBMSVU"
    "tQxxkaw3k_yzAdzaitxJyMRGjTmTdl4ovdIBsxB9898wExet2etLz3ngfiM7EG5cpsd01Fxf_9"
    "6LiXF8D4aM3k_cSQPrj3vGwRW4jSM27x0iGNaZIKNdoIZ861sfguiq6mMb1sVDbGhIW857M7z3"
    "2R75bzlngKzeSEbBHXTF8g";

// Generate by gen-jwt.py as described in
// https://github.com/istio/istio/blob/master/security/tools/jwt/samples/README.md
// to generate token with invalid issuer.
// `security/tools/jwt/samples/gen-jwt.py security/tools/jwt/samples/key.pem
//  --expire=3153600000 --iss "wrong-issuer@secure.istio.io"`
constexpr char kBadToken[] =
    "eyJhbGciOiJSUzI1NiIsImtpZCI6IkRIRmJwb0lVcXJZOHQyenBBMnFYZkNtcjVWTzVaRXI0Un"
    "pIVV8tZW52dlEiLCJ"
    "0eXAiOiJKV1QifQ."
    "eyJleHAiOjQ2ODcxODkyNTEsImlhdCI6MTUzMzU4OTI1MSwiaXNzIjoid3JvbmctaXNzdWVyQH"
    "N"
    "lY3VyZS5pc3Rpby5pbyIsInN1YiI6Indyb25nLWlzc3VlckBzZWN1cmUuaXN0aW8uaW8ifQ."
    "Ye7RKrEgr3mUxRE1OF5"
    "sCaaH6kg_OT-"
    "mAM1HI3tTUp0ljVuxZLCcTXPvvEAjyeiNUm8fjeeER0fsXv7y8wTaA4FFw9x8NT9xS8pyLi6Rs"
    "Twdjkq"
    "0-Plu93VQk1R98BdbEVT-T5vVz7uACES4LQBqsvvTcLBbBNUvKs_"
    "eJyZG71WJuymkkbL5Ki7CB73sQUMl2T3eORC7DJt"
    "yn_C9Dxy2cwCzHrLZnnGz839_bX_yi29dI4veYCNBgU-"
    "9ZwehqfgSCJWYUoBTrdM06N3jEemlWB83ZY4OXoW0pNx-ecu"
    "3asJVbwyxV2_HT6_aUsdHwTYwHv2hXBjdKEfwZxSsBxbKpA";

constexpr char kExpectedPrincipal[] =
    "testing@secure.istio.io/testing@secure.istio.io";
constexpr char kRbacPrincipal[] =
    "testing-rbac@secure.istio.io/testing-rbac@secure.istio.io";

constexpr char kDestinationNamespace[] = "pod";
constexpr char kDestinationUID[] = "kubernetes://dest.pod";
constexpr char kSourceUID[] = "kubernetes://src.pod";
constexpr char kTelemetryBackend[] = "telemetry-backend";
constexpr char kPolicyBackend[] = "policy-backend";
constexpr char kZipkinBackend[] = "zipkin-backend";

// Generates basic test request header.
Http::TestRequestHeaderMapImpl BaseRequestHeaders() {
  return Http::TestRequestHeaderMapImpl{{":method", "GET"},
                                        {":path", "/"},
                                        {":scheme", "http"},
                                        {":authority", "host"},
                                        {"x-forwarded-for", "10.0.0.1"}};
}

// Generates test request header with given token.
Http::TestRequestHeaderMapImpl HeadersWithToken(const std::string& token) {
  auto headers = BaseRequestHeaders();
  headers.addCopy("Authorization", "Bearer " + token);
  return headers;
}

std::string MakeEnvoyJwtFilterConfig() {
  constexpr char kJwtFilterTemplate[] = R"(
  name: %s
  config:
    providers:
      testing: 
        issuer: testing@secure.istio.io
        local_jwks:
          inline_string: "%s"
        payload_in_metadata: testing@secure.istio.io
      testing-rbac:
        issuer: testing-rbac@secure.istio.io
        local_jwks:
          inline_string: "%s"
        payload_in_metadata: testing-rbac@secure.istio.io
    rules: 
    - match:
        prefix: /
      requires:
        requires_any:
          requirements: 
          - provider_name: testing
          - provider_name: testing-rbac
          - allow_missing_or_failed:
  )";
  // From
  // https://github.com/istio/istio/blob/master/security/tools/jwt/samples/jwks.json
  constexpr char kJwksInline[] =
      "{ \"keys\":[ "
      "{\"e\":\"AQAB\",\"kid\":\"DHFbpoIUqrY8t2zpA2qXfCmr5VO5ZEr4RzHU_-envvQ\","
      "\"kty\":\"RSA\",\"n\":\"xAE7eB6qugXyCAG3yhh7pkDkT65pHymX-"
      "P7KfIupjf59vsdo91bSP9C8H07pSAGQO1MV"
      "_xFj9VswgsCg4R6otmg5PV2He95lZdHtOcU5DXIg_"
      "pbhLdKXbi66GlVeK6ABZOUW3WYtnNHD-91gVuoeJT_"
      "DwtGGcp4ignkgXfkiEm4sw-4sfb4qdt5oLbyVpmW6x9cfa7vs2WTfURiCrBoUqgBo_-"
      "4WTiULmmHSGZHOjzwa8WtrtOQGsAFjIbno85jp6MnGGGZPYZbDAa_b3y5u-"
      "YpW7ypZrvD8BgtKVjgtQgZhLAGezMt0ua3DRrWnKqTZ0BJ_EyxOGuHJrLsn00fnMQ\"}]}";

  return fmt::sprintf(kJwtFilterTemplate,
                      Extensions::HttpFilters::HttpFilterNames::get().JwtAuthn,
                      StringUtil::escape(kJwksInline),
                      StringUtil::escape(kJwksInline));
}

std::string MakeAuthFilterConfig() {
  constexpr char kAuthnFilterWithJwtTemplate[] = R"(
    name: %s
    config:
      policy:
        origins:
        - jwt:
            issuer: testing@secure.istio.io
            jwks_uri: http://localhost:8081/
        - jwt:
            issuer: testing-rbac@secure.istio.io
            jwks_uri: http://localhost:8081/
        principalBinding: USE_ORIGIN
)";
  return fmt::sprintf(kAuthnFilterWithJwtTemplate,
                      Utils::IstioFilterName::kAuthentication);
}

std::string MakeRbacFilterConfig() {
  constexpr char kRbacFilterTemplate[] = R"(
  name: envoy.filters.http.rbac
  config:
    rules:
      policies:
        "foo":
          permissions:
            - any: true
          principals:
            - metadata:
                filter: %s
                path:
                  - key: %s
                value:
                  string_match:
                    exact: %s
)";
  return fmt::sprintf(
      kRbacFilterTemplate, Utils::IstioFilterName::kAuthentication,
      istio::utils::AttributeName::kRequestAuthPrincipal, kExpectedPrincipal);
}

std::string MakeMixerFilterConfig() {
  constexpr char kMixerFilterTemplate[] = R"(
  name: mixer
  config:
    defaultDestinationService: "default"
    mixerAttributes:
      attributes: {
      }
    serviceConfigs: {
      "default": {}
    }
    transport:
      attributes_for_mixer_proxy:
        attributes: {
          "source.uid": {
            string_value: %s
          }
        }
      report_cluster: %s
      check_cluster: %s
  )";
  return fmt::sprintf(kMixerFilterTemplate, kSourceUID, kTelemetryBackend,
                      kPolicyBackend);
}

// checkJwtRawClaim finds the serialized jwt payload and check to see if all
// key-value pairs from the jwt claim is there. Returns false if it cannot find
// the serialized jwt payload or if any key-value pair does not matched the
// expectation.
bool checkJwtRawClaim(
    const ::google::protobuf::RepeatedPtrField<::std::string>& words) {
  for (auto& word : words) {
    google::protobuf::Struct payload;
    Protobuf::util::Status status =
        Protobuf::util::JsonStringToMessage(word.data(), &payload);
    if (status.ok()) {
      if ((word.find(kExpectedIss) == std::string::npos) ||
          (word.find(kExpectedIat) == std::string::npos) ||
          (word.find(kExpectedExp) == std::string::npos) ||
          (word.find(kExpectedSub) == std::string::npos) ||
          (word.find(kExpectedFoo) == std::string::npos)) {
        return false;
      }
      return true;
    }
  }
  return false;
}

// This integration is exact the same as one in istio_http_integration_test.cc,
// except this test uses Envoy jwt filter, rather than Istio jwt filter.
class IstioHttpIntegrationTestWithEnvoyJwtFilter
    : public HttpProtocolIntegrationTest {
 public:
  void createUpstreams() override {
    HttpProtocolIntegrationTest::createUpstreams();
    fake_upstreams_.emplace_back(new FakeUpstream(
        0, FakeHttpConnection::Type::HTTP2, version_, timeSystem()));
    telemetry_upstream_ = fake_upstreams_.back().get();

    fake_upstreams_.emplace_back(new FakeUpstream(
        0, FakeHttpConnection::Type::HTTP2, version_, timeSystem()));
    policy_upstream_ = fake_upstreams_.back().get();

    fake_upstreams_.emplace_back(new FakeUpstream(
        0, FakeHttpConnection::Type::HTTP2, version_, timeSystem()));
    zipkin_upstream_ = fake_upstreams_.back().get();
  }

  void SetUp() override {
    config_helper_.addConfigModifier(addNodeMetadata());

    config_helper_.addFilter(MakeMixerFilterConfig());
    config_helper_.addFilter(MakeRbacFilterConfig());
    config_helper_.addFilter(MakeAuthFilterConfig());
    config_helper_.addFilter(MakeEnvoyJwtFilterConfig());

    config_helper_.addConfigModifier(addCluster(kTelemetryBackend));
    config_helper_.addConfigModifier(addCluster(kPolicyBackend));
    config_helper_.addConfigModifier(addCluster(kZipkinBackend));

    config_helper_.addConfigModifier(addTracer());
    config_helper_.addConfigModifier(addTracingRate());

    HttpProtocolIntegrationTest::initialize();
  }

  void TearDown() override {
    cleanupConnection(fake_upstream_connection_);
    cleanupConnection(telemetry_connection_);
    cleanupConnection(policy_connection_);
    cleanupConnection(zipkin_connection_);
  }

  ConfigHelper::ConfigModifierFunction addNodeMetadata() {
    return [](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
      ::google::protobuf::Struct meta;
      MessageUtil::loadFromJson(
          fmt::sprintf(R"({
        "ISTIO_VERSION": "1.0.1",
        "NODE_UID": "%s",
        "NODE_NAMESPACE": "%s"
      })",
                       kDestinationUID, kDestinationNamespace),
          meta);
      bootstrap.mutable_node()->mutable_metadata()->MergeFrom(meta);
    };
  }

  ConfigHelper::ConfigModifierFunction addTracer() {
    return [](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
      auto* http_tracing = bootstrap.mutable_tracing()->mutable_http();
      http_tracing->set_name("envoy.zipkin");
      envoy::config::trace::v3::ZipkinConfig zipkin_config;
      zipkin_config.set_collector_cluster(kZipkinBackend);
      zipkin_config.set_collector_endpoint("/api/v1/spans");
      zipkin_config.set_collector_endpoint_version(
          envoy::config::trace::v3::ZipkinConfig::HTTP_JSON);
      http_tracing->mutable_typed_config()->PackFrom(zipkin_config);
    };
  }

  ConfigHelper::HttpModifierFunction addTracingRate() {
    return [](envoy::extensions::filters::network::http_connection_manager::v3::
                  HttpConnectionManager& hcm) {
      auto* tracing = hcm.mutable_tracing();
      tracing->mutable_client_sampling()->set_value(100.0);
      tracing->mutable_random_sampling()->set_value(100.0);
      tracing->mutable_overall_sampling()->set_value(100.0);
    };
  }

  ConfigHelper::ConfigModifierFunction addCluster(const std::string& name) {
    return [name](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
      auto* cluster = bootstrap.mutable_static_resources()->add_clusters();
      cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
      cluster->mutable_http2_protocol_options();
      cluster->set_name(name);
    };
  }

  void waitForTelemetryRequest(::istio::mixer::v1::ReportRequest* request) {
    AssertionResult result = telemetry_upstream_->waitForHttpConnection(
        *dispatcher_, telemetry_connection_);
    RELEASE_ASSERT(result, result.message());
    result = telemetry_connection_->waitForNewStream(*dispatcher_,
                                                     telemetry_request_);
    RELEASE_ASSERT(result, result.message());

    result = telemetry_request_->waitForGrpcMessage(*dispatcher_, *request);
    RELEASE_ASSERT(result, result.message());
  }

  // Must be called after waitForTelemetryRequest
  void sendTelemetryResponse() {
    telemetry_request_->startGrpcStream();
    telemetry_request_->sendGrpcMessage(::istio::mixer::v1::ReportResponse{});
    telemetry_request_->finishGrpcStream(Grpc::Status::Ok);
  }

  void waitForPolicyRequest(::istio::mixer::v1::CheckRequest* request) {
    AssertionResult result = policy_upstream_->waitForHttpConnection(
        *dispatcher_, policy_connection_);
    RELEASE_ASSERT(result, result.message());
    result =
        policy_connection_->waitForNewStream(*dispatcher_, policy_request_);
    RELEASE_ASSERT(result, result.message());

    result = policy_request_->waitForGrpcMessage(*dispatcher_, *request);
    RELEASE_ASSERT(result, result.message());
  }

  // Must be called after waitForPolicyRequest
  void sendPolicyResponse() {
    policy_request_->startGrpcStream();
    ::istio::mixer::v1::CheckResponse response;
    response.mutable_precondition()->mutable_status()->set_code(Code::OK);
    policy_request_->sendGrpcMessage(response);
    policy_request_->finishGrpcStream(Grpc::Status::Ok);
  }

  void cleanupConnection(FakeHttpConnectionPtr& connection) {
    if (connection != nullptr) {
      AssertionResult result = connection->close();
      RELEASE_ASSERT(result, result.message());
      result = connection->waitForDisconnect();
      RELEASE_ASSERT(result, result.message());
    }
  }

  FakeUpstream* telemetry_upstream_{};
  FakeHttpConnectionPtr telemetry_connection_{};
  FakeStreamPtr telemetry_request_{};

  FakeUpstream* policy_upstream_{};
  FakeHttpConnectionPtr policy_connection_{};
  FakeStreamPtr policy_request_{};

  FakeUpstream* zipkin_upstream_{};
  FakeHttpConnectionPtr zipkin_connection_{};
  FakeStreamPtr zipkin_request_{};
};

INSTANTIATE_TEST_SUITE_P(
    Protocols, IstioHttpIntegrationTestWithEnvoyJwtFilter,
    testing::ValuesIn(HttpProtocolIntegrationTest::getProtocolTestParams()),
    HttpProtocolIntegrationTest::protocolTestParamsToString);

TEST_P(IstioHttpIntegrationTestWithEnvoyJwtFilter, NoJwt) {
  // initialize();
  codec_client_ =
      makeHttpConnection(makeClientConnection((lookupPort("http"))));
  auto response = codec_client_->makeHeaderOnlyRequest(BaseRequestHeaders());

  ::istio::mixer::v1::ReportRequest report_request;
  waitForTelemetryRequest(&report_request);
  // As authentication fail, report should not have 'word' that might come
  // authN.
  EXPECT_THAT(report_request.default_words(),
              ::testing::AllOf(Contains(kDestinationUID), Contains("10.0.0.1"),
                               Not(Contains(kExpectedPrincipal))));
  sendTelemetryResponse();

  response->waitForEndStream();
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("401", response->headers().Status()->value().getStringView());
}

TEST_P(IstioHttpIntegrationTestWithEnvoyJwtFilter, BadJwt) {
  codec_client_ =
      makeHttpConnection(makeClientConnection((lookupPort("http"))));
  auto response =
      codec_client_->makeHeaderOnlyRequest(HeadersWithToken(kBadToken));

  ::istio::mixer::v1::ReportRequest report_request;
  waitForTelemetryRequest(&report_request);
  EXPECT_THAT(report_request.default_words(),
              ::testing::AllOf(Contains(kDestinationUID), Contains("10.0.0.1"),
                               Not(Contains(kExpectedPrincipal))));
  sendTelemetryResponse();

  response->waitForEndStream();
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("401", response->headers().Status()->value().getStringView());
}

TEST_P(IstioHttpIntegrationTestWithEnvoyJwtFilter, RbacDeny) {
  codec_client_ =
      makeHttpConnection(makeClientConnection((lookupPort("http"))));
  auto response =
      codec_client_->makeHeaderOnlyRequest(HeadersWithToken(kRbacGoodToken));

  ::istio::mixer::v1::ReportRequest report_request;
  waitForTelemetryRequest(&report_request);
  // As authentication succeeded, report should have 'word' that comes from
  // authN.
  EXPECT_THAT(report_request.default_words(),
              ::testing::AllOf(Contains(kDestinationUID), Contains("10.0.0.1"),
                               Contains(kRbacPrincipal)));
  sendTelemetryResponse();

  response->waitForEndStream();
  EXPECT_TRUE(response->complete());

  // Expecting error code 403 for RBAC deny.
  EXPECT_EQ("403", response->headers().Status()->value().getStringView());
}

TEST_P(IstioHttpIntegrationTestWithEnvoyJwtFilter, GoodJwt) {
  codec_client_ =
      makeHttpConnection(makeClientConnection((lookupPort("http"))));
  auto response =
      codec_client_->makeHeaderOnlyRequest(HeadersWithToken(kGoodToken));

  ::istio::mixer::v1::CheckRequest check_request;
  waitForPolicyRequest(&check_request);
  // Check request should see authn attributes.
  EXPECT_THAT(
      check_request.attributes().words(),
      ::testing::AllOf(Contains(kDestinationUID), Contains("10.0.0.1"),
                       Contains(kExpectedPrincipal),
                       Contains("testing@secure.istio.io"), Contains("sub"),
                       Contains("iss"), Contains("foo"), Contains("bar")));
  EXPECT_TRUE(checkJwtRawClaim(check_request.attributes().words()));
  sendPolicyResponse();

  waitForNextUpstreamRequest(0);
  // Send backend response.
  upstream_request_->encodeHeaders(
      Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  response->waitForEndStream();

  // Report (log) is sent after backen response.
  ::istio::mixer::v1::ReportRequest report_request;
  waitForTelemetryRequest(&report_request);
  // Report request should also see the same authn attributes.
  EXPECT_THAT(
      report_request.default_words(),
      ::testing::AllOf(Contains(kDestinationUID), Contains("10.0.0.1"),
                       Contains("testing@secure.istio.io"), Contains("sub"),
                       Contains("iss"), Contains("foo"), Contains("bar")));
  EXPECT_TRUE(checkJwtRawClaim(check_request.attributes().words()));
  sendTelemetryResponse();

  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().Status()->value().getStringView());
}

TEST_P(IstioHttpIntegrationTestWithEnvoyJwtFilter, TracingHeader) {
  codec_client_ =
      makeHttpConnection(makeClientConnection((lookupPort("http"))));
  auto response =
      codec_client_->makeHeaderOnlyRequest(HeadersWithToken(kGoodToken));

  ::istio::mixer::v1::CheckRequest check_request;
  waitForPolicyRequest(&check_request);
  sendPolicyResponse();

  waitForNextUpstreamRequest(0);
  // Send backend response.
  upstream_request_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "200"}},
                                   true);
  response->waitForEndStream();

  ::istio::mixer::v1::ReportRequest report_request;
  waitForTelemetryRequest(&report_request);
  sendTelemetryResponse();

  response->waitForEndStream();

  EXPECT_TRUE(response->complete());
  Http::TestResponseHeaderMapImpl upstream_headers(
      upstream_request_->headers());
  // Trace headers should be added into upstream request
  EXPECT_TRUE(upstream_headers.has(Envoy::Utils::kTraceID));
  EXPECT_TRUE(upstream_headers.has(Envoy::Utils::kSpanID));
  EXPECT_TRUE(upstream_headers.has(Envoy::Utils::kSampled));

  // span id should be included in default words of report request
  EXPECT_THAT(
      report_request.default_words(),
      ::testing::AllOf(Contains(upstream_headers.get_(Envoy::Utils::kSpanID))));
}

}  // namespace
}  // namespace Envoy
