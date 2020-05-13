#include "envoy/extensions/filters/http/dynamic_forward_proxy/v3/dynamic_forward_proxy.pb.h"

#include "extensions/filters/http/dynamic_forward_proxy/proxy_filter.h"
#include "extensions/filters/http/well_known_names.h"

#include "test/extensions/common/dynamic_forward_proxy/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/mocks/upstream/transport_socket_match.h"

#include "absl/memory/memory.h"

using testing::AtLeast;
using testing::Eq;
using testing::InSequence;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace DynamicForwardProxy {
namespace {

using LoadDnsCacheEntryStatus = Common::DynamicForwardProxy::DnsCache::LoadDnsCacheEntryStatus;
using MockLoadDnsCacheEntryResult =
    Common::DynamicForwardProxy::MockDnsCache::MockLoadDnsCacheEntryResult;

class ProxyFilterTest : public testing::Test,
                        public Extensions::Common::DynamicForwardProxy::DnsCacheManagerFactory {
public:
  ProxyFilterTest() {
    transport_socket_match_ = new NiceMock<Upstream::MockTransportSocketMatcher>(
        Network::TransportSocketFactoryPtr(transport_socket_factory_));
    cm_.thread_local_cluster_.cluster_.info_->transport_socket_matcher_.reset(
        transport_socket_match_);

    envoy::extensions::filters::http::dynamic_forward_proxy::v3::FilterConfig proto_config;
    EXPECT_CALL(*dns_cache_manager_, getCache(_));
    filter_config_ = std::make_shared<ProxyFilterConfig>(proto_config, *this, cm_);
    filter_ = std::make_unique<ProxyFilter>(filter_config_);
    filter_->setDecoderFilterCallbacks(callbacks_);

    // Allow for an otherwise strict mock.
    EXPECT_CALL(callbacks_, connection()).Times(AtLeast(0));
    EXPECT_CALL(callbacks_, streamId()).Times(AtLeast(0));

    // Configure max pending to 1 so we can test circuit breaking.
    cm_.thread_local_cluster_.cluster_.info_->resetResourceManager(0, 1, 0, 0, 0);

    // This temporal pointer variable is used for aggregate initialization since we can't apply
    // aggregated initializer to std::make_unique directly.
    auto* cb_stats__ptr = new Extensions::Common::DynamicForwardProxy::DnsCacheCircuitBreakersStats{
        ALL_DNS_CACHE_CIRCUIT_BREAKERS_STATS(POOL_GAUGE_PREFIX(store_, "circuit_breakers"),
                                             POOL_GAUGE_PREFIX(store_, "circuit_breakers"))};
    cb_stats_ =
        absl::WrapUnique<Extensions::Common::DynamicForwardProxy::DnsCacheCircuitBreakersStats>(
            cb_stats__ptr);

    envoy::extensions::common::dynamic_forward_proxy::v3::DnsCacheCircuitBreakers cb_config;
    std::string config_yaml = R"EOF(
      max_pending_requests: 1
    )EOF";
    TestUtility::loadFromYaml(config_yaml, cb_config);

    dns_cache_resource_manager_ =
        std::make_unique<Extensions::Common::DynamicForwardProxy::DnsCacheResourceManager>(
            *cb_stats_, loader_, "default", cb_config);
  }

  ~ProxyFilterTest() override {
    EXPECT_TRUE(
        cm_.thread_local_cluster_.cluster_.info_->resource_manager_->pendingRequests().canCreate());
  }

  Extensions::Common::DynamicForwardProxy::DnsCacheManagerSharedPtr get() override {
    return dns_cache_manager_;
  }

  std::shared_ptr<Extensions::Common::DynamicForwardProxy::MockDnsCacheManager> dns_cache_manager_{
      new Extensions::Common::DynamicForwardProxy::MockDnsCacheManager()};
  Network::MockTransportSocketFactory* transport_socket_factory_{
      new Network::MockTransportSocketFactory()};
  NiceMock<Upstream::MockTransportSocketMatcher>* transport_socket_match_;
  Upstream::MockClusterManager cm_;
  ProxyFilterConfigSharedPtr filter_config_;
  std::unique_ptr<ProxyFilter> filter_;
  Http::MockStreamDecoderFilterCallbacks callbacks_;
  NiceMock<Stats::MockIsolatedStatsStore> store_;
  NiceMock<Runtime::MockLoader> loader_;
  Http::TestRequestHeaderMapImpl request_headers_{{":authority", "foo"}};
  std::unique_ptr<Extensions::Common::DynamicForwardProxy::DnsCacheCircuitBreakersStats> cb_stats_;
  std::unique_ptr<Extensions::Common::DynamicForwardProxy::DnsCacheResourceManager>
      dns_cache_resource_manager_;
};

// Default port 80 if upstream TLS not configured.
TEST_F(ProxyFilterTest, HttpDefaultPort) {
  InSequence s;

  EXPECT_CALL(callbacks_, route());
  EXPECT_CALL(cm_, get(_));
  EXPECT_CALL(*(dns_cache_manager_->dns_cache_), dnsCacheResourceManager());
  EXPECT_CALL(*transport_socket_factory_, implementsSecureTransport()).WillOnce(Return(false));
  Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle* handle =
      new Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle();
  EXPECT_CALL(*dns_cache_manager_->dns_cache_, loadDnsCacheEntry_(Eq("foo"), 80, _))
      .WillOnce(Return(MockLoadDnsCacheEntryResult{LoadDnsCacheEntryStatus::Loading, handle}));
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(*handle, onDestroy());
  filter_->onDestroy();
}

// Default port 443 if upstream TLS is configured.
TEST_F(ProxyFilterTest, HttpsDefaultPort) {
  InSequence s;

  EXPECT_CALL(callbacks_, route());
  EXPECT_CALL(cm_, get(_));
  EXPECT_CALL(*(dns_cache_manager_->dns_cache_), dnsCacheResourceManager());
  EXPECT_CALL(*transport_socket_factory_, implementsSecureTransport()).WillOnce(Return(true));
  Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle* handle =
      new Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle();
  EXPECT_CALL(*dns_cache_manager_->dns_cache_, loadDnsCacheEntry_(Eq("foo"), 443, _))
      .WillOnce(Return(MockLoadDnsCacheEntryResult{LoadDnsCacheEntryStatus::Loading, handle}));
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(*handle, onDestroy());
  filter_->onDestroy();
}

// Cache overflow.
TEST_F(ProxyFilterTest, CacheOverflow) {
  InSequence s;

  EXPECT_CALL(callbacks_, route());
  EXPECT_CALL(cm_, get(_));
  EXPECT_CALL(*(dns_cache_manager_->dns_cache_), dnsCacheResourceManager());
  EXPECT_CALL(*transport_socket_factory_, implementsSecureTransport()).WillOnce(Return(true));
  EXPECT_CALL(*dns_cache_manager_->dns_cache_, loadDnsCacheEntry_(Eq("foo"), 443, _))
      .WillOnce(Return(MockLoadDnsCacheEntryResult{LoadDnsCacheEntryStatus::Overflow, nullptr}));
  EXPECT_CALL(callbacks_, sendLocalReply(Http::Code::ServiceUnavailable, Eq("DNS cache overflow"),
                                         _, _, Eq("DNS cache overflow")));
  EXPECT_CALL(callbacks_, encodeHeaders_(_, false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  filter_->onDestroy();
}

// Circuit breaker overflow
TEST_F(ProxyFilterTest, CircuitBreakerOverflow) {
  InSequence s;

  EXPECT_CALL(callbacks_, route());
  EXPECT_CALL(cm_, get(_));
  EXPECT_CALL(*(dns_cache_manager_->dns_cache_), dnsCacheResourceManager());
  EXPECT_CALL(*transport_socket_factory_, implementsSecureTransport()).WillOnce(Return(true));
  Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle* handle =
      new Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle();
  EXPECT_CALL(*dns_cache_manager_->dns_cache_, loadDnsCacheEntry_(Eq("foo"), 443, _))
      .WillOnce(Return(MockLoadDnsCacheEntryResult{LoadDnsCacheEntryStatus::Loading, handle}));
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));

  // Create a second filter for a 2nd request.
  auto filter2 = std::make_unique<ProxyFilter>(filter_config_);
  filter2->setDecoderFilterCallbacks(callbacks_);
  EXPECT_CALL(callbacks_, route());
  EXPECT_CALL(cm_, get(_));
  EXPECT_CALL(*(dns_cache_manager_->dns_cache_), dnsCacheResourceManager());
  EXPECT_CALL(callbacks_, sendLocalReply(Http::Code::ServiceUnavailable,
                                         Eq("Dynamic forward proxy pending request overflow"), _, _,
                                         Eq("Dynamic forward proxy pending request overflow")));
  EXPECT_CALL(callbacks_, encodeHeaders_(_, false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter2->decodeHeaders(request_headers_, false));

  EXPECT_EQ(1,
            cm_.thread_local_cluster_.cluster_.info_->stats_.upstream_rq_pending_overflow_.value());
  filter2->onDestroy();
  EXPECT_CALL(*handle, onDestroy());
  filter_->onDestroy();
}

// Circuit breaker overflow with DNS Cache resource manager
TEST_F(ProxyFilterTest, CircuitBreakerOverflowWithDnsCacheResourceManager) {
  InSequence s;

  EXPECT_CALL(callbacks_, route());
  EXPECT_CALL(cm_, get(_));
  EXPECT_CALL(*(dns_cache_manager_->dns_cache_), dnsCacheResourceManager())
      .WillOnce(ReturnRef(dns_cache_resource_manager_));

  EXPECT_CALL(*transport_socket_factory_, implementsSecureTransport()).WillOnce(Return(true));
  Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle* handle =
      new Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle();
  EXPECT_CALL(*dns_cache_manager_->dns_cache_, loadDnsCacheEntry_(Eq("foo"), 443, _))
      .WillOnce(Return(MockLoadDnsCacheEntryResult{LoadDnsCacheEntryStatus::Loading, handle}));
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));

  // Check if the Circuit Breaking is activated.
  EXPECT_EQ(0U, cb_stats_->rq_pending_remaining_.value());
  EXPECT_EQ(1U, cb_stats_->rq_pending_opening_.value());

  // Create a second filter for a 2nd request.
  auto filter2 = std::make_unique<ProxyFilter>(filter_config_);
  filter2->setDecoderFilterCallbacks(callbacks_);
  EXPECT_CALL(callbacks_, route());
  EXPECT_CALL(cm_, get(_));
  EXPECT_CALL(*(dns_cache_manager_->dns_cache_), dnsCacheResourceManager())
      .WillOnce(ReturnRef(dns_cache_resource_manager_));
  EXPECT_CALL(callbacks_, sendLocalReply(Http::Code::ServiceUnavailable,
                                         Eq("Dynamic forward proxy pending request overflow"), _, _,
                                         Eq("Dynamic forward proxy pending request overflow")));
  EXPECT_CALL(callbacks_, encodeHeaders_(_, false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter2->decodeHeaders(request_headers_, false));

  filter2->onDestroy();
  EXPECT_CALL(*handle, onDestroy());
  filter_->onDestroy();
}

// No route handling.
TEST_F(ProxyFilterTest, NoRoute) {
  InSequence s;

  EXPECT_CALL(callbacks_, route()).WillOnce(Return(nullptr));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
}

// No cluster handling.
TEST_F(ProxyFilterTest, NoCluster) {
  InSequence s;

  EXPECT_CALL(callbacks_, route());
  EXPECT_CALL(cm_, get(_)).WillOnce(Return(nullptr));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
}

TEST_F(ProxyFilterTest, HostRewrite) {
  InSequence s;

  envoy::extensions::filters::http::dynamic_forward_proxy::v3::PerRouteConfig proto_config;
  proto_config.set_host_rewrite_literal("bar");
  ProxyPerRouteConfig config(proto_config);

  EXPECT_CALL(callbacks_, route());
  EXPECT_CALL(cm_, get(_));
  EXPECT_CALL(*(dns_cache_manager_->dns_cache_), dnsCacheResourceManager());
  EXPECT_CALL(*transport_socket_factory_, implementsSecureTransport()).WillOnce(Return(false));
  Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle* handle =
      new Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle();
  EXPECT_CALL(callbacks_.route_->route_entry_,
              perFilterConfig(HttpFilterNames::get().DynamicForwardProxy))
      .WillOnce(Return(&config));
  EXPECT_CALL(*dns_cache_manager_->dns_cache_, loadDnsCacheEntry_(Eq("bar"), 80, _))
      .WillOnce(Return(MockLoadDnsCacheEntryResult{LoadDnsCacheEntryStatus::Loading, handle}));
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(*handle, onDestroy());
  filter_->onDestroy();
}

TEST_F(ProxyFilterTest, HostRewriteViaHeader) {
  InSequence s;

  envoy::extensions::filters::http::dynamic_forward_proxy::v3::PerRouteConfig proto_config;
  proto_config.set_host_rewrite_header("x-set-header");
  ProxyPerRouteConfig config(proto_config);

  EXPECT_CALL(callbacks_, route());
  EXPECT_CALL(cm_, get(_));
  EXPECT_CALL(*(dns_cache_manager_->dns_cache_), dnsCacheResourceManager());
  EXPECT_CALL(*transport_socket_factory_, implementsSecureTransport()).WillOnce(Return(false));
  Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle* handle =
      new Extensions::Common::DynamicForwardProxy::MockLoadDnsCacheEntryHandle();
  EXPECT_CALL(callbacks_.route_->route_entry_,
              perFilterConfig(HttpFilterNames::get().DynamicForwardProxy))
      .WillOnce(Return(&config));
  EXPECT_CALL(*dns_cache_manager_->dns_cache_, loadDnsCacheEntry_(Eq("bar:82"), 80, _))
      .WillOnce(Return(MockLoadDnsCacheEntryResult{LoadDnsCacheEntryStatus::Loading, handle}));

  Http::TestRequestHeaderMapImpl headers{{":authority", "foo"}, {"x-set-header", "bar:82"}};
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(headers, false));

  EXPECT_CALL(*handle, onDestroy());
  filter_->onDestroy();
}

} // namespace
} // namespace DynamicForwardProxy
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
