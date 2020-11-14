#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "envoy/common/pure.h"
#include "envoy/secret/secret_manager.h"
#include "envoy/ssl/certificate_validation_context_config.h"
#include "envoy/ssl/handshaker.h"
#include "envoy/ssl/tls_certificate_config.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace Ssl {

class TlsCertificateConfigProvidersFactory {
public:
  virtual ~TlsCertificateConfigProvidersFactory() = default;

  /**
   * Create the certificate config providers factories.
   */
  virtual std::vector<Secret::TlsCertificateConfigProviderSharedPtr> create() PURE;
};

using TlsCertificateConfigProvidersFactoryPtr =
    std::unique_ptr<TlsCertificateConfigProvidersFactory>;

using CertificateValidationContextPtr =
    std::shared_ptr<envoy::extensions::transport_sockets::tls::v3::CertificateValidationContext>;

class CertificateValidationContextConfigProviderFactory {
public:
  virtual ~CertificateValidationContextConfigProviderFactory() = default;

  /**
   * Create the validation context config provider.
   */
  virtual Secret::CertificateValidationContextConfigProviderSharedPtr create() PURE;

  /**
   * Get the default certificate validation context. This value is nullptr by default, except for
   * when the validation context type is a combined validation context.
   */
  virtual CertificateValidationContextPtr defaultCertificateValidationContext() const PURE;
};

using CertificateValidationContextConfigProviderFactoryPtr =
    std::unique_ptr<CertificateValidationContextConfigProviderFactory>;

class TlsSessionTicketKeysConfigProviderFactory {
public:
  virtual ~TlsSessionTicketKeysConfigProviderFactory() = default;

  /**
   * Create the tls session ticket keys config provider.
   */
  virtual Secret::TlsSessionTicketKeysConfigProviderSharedPtr create() PURE;
};

using TlsSessionTicketKeysConfigProviderFactoryPtr =
    std::unique_ptr<TlsSessionTicketKeysConfigProviderFactory>;

/**
 * Supplies the configuration for an SSL context.
 */
class ContextConfig {
public:
  virtual ~ContextConfig() = default;

  /**
   * The list of supported protocols exposed via ALPN. Client connections will send these
   * protocols to the server. Server connections will use these protocols to select the next
   * protocol if the client supports ALPN.
   */
  virtual const std::string& alpnProtocols() const PURE;

  /**
   * The ':' delimited list of supported cipher suites
   */
  virtual const std::string& cipherSuites() const PURE;

  /**
   * The ':' delimited list of supported ECDH curves.
   */
  virtual const std::string& ecdhCurves() const PURE;

  /**
   * @return std::vector<std::reference_wrapper<const TlsCertificateConfig>> TLS
   * certificate configs.
   */
  virtual std::vector<std::reference_wrapper<const TlsCertificateConfig>>
  tlsCertificates() const PURE;

  /**
   * @return CertificateValidationContextConfig the certificate validation context config.
   */
  virtual const CertificateValidationContextConfig* certificateValidationContext() const PURE;

  /**
   * @return The minimum TLS protocol version to negotiate.
   */
  virtual unsigned minProtocolVersion() const PURE;

  /**
   * @return The maximum TLS protocol version to negotiate.
   */
  virtual unsigned maxProtocolVersion() const PURE;

  /**
   * @return true if the ContextConfig is able to provide secrets to create SSL context,
   * and false if dynamic secrets are expected but are not downloaded from SDS server yet.
   */
  virtual bool isReady() const PURE;

  /**
   * Add secret callback into context config. When dynamic secrets are in use and new secrets
   * are downloaded from SDS server, this callback is invoked to update SSL context.
   * @param callback callback that is executed by context config.
   */
  virtual void setSecretUpdateCallback(std::function<void()> callback) PURE;

  /**
   * @return a callback which can be used to create Handshaker instances.
   */
  virtual HandshakerFactoryCb createHandshaker() const PURE;

  /**
   * @return the set of capabilities for handshaker instances created by this context.
   */
  virtual HandshakerCapabilities capabilities() const PURE;
};

class ClientContextConfig : public virtual ContextConfig {
public:
  /**
   * @return The server name indication if it's set and ssl enabled
   * Otherwise, ""
   */
  virtual const std::string& serverNameIndication() const PURE;

  /**
   * @return true if server-initiated TLS renegotiation will be allowed.
   */
  virtual bool allowRenegotiation() const PURE;

  /**
   * @return The maximum number of session keys to store.
   */
  virtual size_t maxSessionKeys() const PURE;

  /**
   * @return const std::string& with the signature algorithms for the context.
   *         This is a :-delimited list of algorithms, see
   *         https://tools.ietf.org/id/draft-ietf-tls-tls13-21.html#rfc.section.4.2.3
   *         for names.
   */
  virtual const std::string& signingAlgorithmsForTest() const PURE;
};

using ClientContextConfigPtr = std::unique_ptr<ClientContextConfig>;

class ServerContextConfig : public virtual ContextConfig {
public:
  struct SessionTicketKey {
    std::array<uint8_t, 16> name_;         // 16 == SSL_TICKET_KEY_NAME_LEN
    std::array<uint8_t, 32> hmac_key_;     // 32 == SHA256_DIGEST_LENGTH
    std::array<uint8_t, 256 / 8> aes_key_; // AES256 key size, in bytes
  };

  enum class OcspStaplePolicy {
    LenientStapling,
    StrictStapling,
    MustStaple,
  };

  /**
   * @return True if client certificate is required, false otherwise.
   */
  virtual bool requireClientCertificate() const PURE;

  /**
   * @return OcspStaplePolicy The rule for determining whether to staple OCSP
   * responses on new connections.
   */
  virtual OcspStaplePolicy ocspStaplePolicy() const PURE;

  /**
   * @return The keys to use for encrypting and decrypting session tickets.
   * The first element is used for encrypting new tickets, and all elements
   * are candidates for decrypting received tickets.
   */
  virtual const std::vector<SessionTicketKey>& sessionTicketKeys() const PURE;

  /**
   * @return timeout in seconds for the session.
   * Session timeout is used to specify lifetime hint of tls tickets.
   */
  virtual absl::optional<std::chrono::seconds> sessionTimeout() const PURE;

  /**
   * @return True if stateless TLS session resumption is disabled, false otherwise.
   */
  virtual bool disableStatelessSessionResumption() const PURE;
};

using ServerContextConfigPtr = std::unique_ptr<ServerContextConfig>;

} // namespace Ssl
} // namespace Envoy
