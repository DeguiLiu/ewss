#ifndef EWSS_TLS_HPP_
#define EWSS_TLS_HPP_

// ============================================================================
// TLS Configuration and Abstraction Layer
// ============================================================================
//
// Optional TLS support via mbedTLS. Enable with CMake option EWSS_WITH_TLS=ON.
// When disabled, all TLS functions are no-ops and the server runs plain WS.
//
// Usage:
//   ewss::TlsConfig tls;
//   tls.cert_path = "/path/to/cert.pem";
//   tls.key_path = "/path/to/key.pem";
//   server.set_tls(tls);
//

#include <cstdint>
#include <string>

#ifdef EWSS_WITH_TLS

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#endif  // EWSS_WITH_TLS

namespace ewss {

// ============================================================================
// TLS Configuration
// ============================================================================

struct TlsConfig {
  std::string cert_path;          // Server certificate (PEM)
  std::string key_path;           // Server private key (PEM)
  std::string ca_path;            // CA certificate for client auth (optional)
  bool require_client_cert = false;

  // Minimal cipher suite for embedded (TLS 1.2+)
  // Default: TLS_AES_128_GCM_SHA256
  int min_tls_version = 0;       // 0 = TLS 1.2 minimum
};

#ifdef EWSS_WITH_TLS

// ============================================================================
// TLS Context (one per server, manages certificates and config)
// ============================================================================

class TlsContext {
 public:
  TlsContext() {
    mbedtls_ssl_config_init(&conf_);
    mbedtls_x509_crt_init(&srvcert_);
    mbedtls_pk_init(&pkey_);
    mbedtls_entropy_init(&entropy_);
    mbedtls_ctr_drbg_init(&ctr_drbg_);
  }

  ~TlsContext() {
    mbedtls_ssl_config_free(&conf_);
    mbedtls_x509_crt_free(&srvcert_);
    mbedtls_pk_free(&pkey_);
    mbedtls_entropy_free(&entropy_);
    mbedtls_ctr_drbg_free(&ctr_drbg_);
  }

  // Non-copyable
  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;

  // Initialize TLS context with config
  // Returns 0 on success, mbedtls error code on failure
  int init(const TlsConfig& config) {
    const char* pers = "ewss_tls";

    // Seed RNG
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg_, mbedtls_entropy_func,
                                     &entropy_,
                                     reinterpret_cast<const unsigned char*>(pers),
                                     strlen(pers));
    if (ret != 0) return ret;

    // Load certificate
    ret = mbedtls_x509_crt_parse_file(&srvcert_, config.cert_path.c_str());
    if (ret != 0) return ret;

    // Load CA if provided
    if (!config.ca_path.empty()) {
      ret = mbedtls_x509_crt_parse_file(&srvcert_, config.ca_path.c_str());
      if (ret != 0) return ret;
    }

    // Load private key
    ret = mbedtls_pk_parse_keyfile(&pkey_, config.key_path.c_str(),
                                    nullptr, mbedtls_ctr_drbg_random,
                                    &ctr_drbg_);
    if (ret != 0) return ret;

    // Setup SSL config
    ret = mbedtls_ssl_config_defaults(&conf_,
                                       MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) return ret;

    mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &ctr_drbg_);
    mbedtls_ssl_conf_ca_chain(&conf_, srvcert_.next, nullptr);
    ret = mbedtls_ssl_conf_own_cert(&conf_, &srvcert_, &pkey_);
    if (ret != 0) return ret;

    // Client authentication
    if (config.require_client_cert) {
      mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
      mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_NONE);
    }

    // Minimum TLS version (1.2)
    mbedtls_ssl_conf_min_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_2);

    initialized_ = true;
    return 0;
  }

  bool is_initialized() const { return initialized_; }

  const mbedtls_ssl_config* config() const { return &conf_; }

 private:
  mbedtls_ssl_config conf_;
  mbedtls_x509_crt srvcert_;
  mbedtls_pk_context pkey_;
  mbedtls_entropy_context entropy_;
  mbedtls_ctr_drbg_context ctr_drbg_;
  bool initialized_ = false;
};

// ============================================================================
// TLS Session (one per connection)
// ============================================================================

class TlsSession {
 public:
  TlsSession() {
    mbedtls_ssl_init(&ssl_);
    mbedtls_net_init(&net_);
  }

  ~TlsSession() {
    mbedtls_ssl_free(&ssl_);
    mbedtls_net_free(&net_);
  }

  // Non-copyable
  TlsSession(const TlsSession&) = delete;
  TlsSession& operator=(const TlsSession&) = delete;

  // Setup session with context and socket fd
  int setup(const TlsContext& ctx, int fd) {
    int ret = mbedtls_ssl_setup(&ssl_, ctx.config());
    if (ret != 0) return ret;

    net_.fd = fd;
    mbedtls_ssl_set_bio(&ssl_, &net_,
                         mbedtls_net_send, mbedtls_net_recv, nullptr);
    return 0;
  }

  // Perform TLS handshake
  int handshake() {
    return mbedtls_ssl_handshake(&ssl_);
  }

  // Read decrypted data
  int read(uint8_t* buf, size_t len) {
    return mbedtls_ssl_read(&ssl_, buf, len);
  }

  // Write data (encrypted)
  int write(const uint8_t* buf, size_t len) {
    return mbedtls_ssl_write(&ssl_, buf, len);
  }

  // Close TLS session
  int close_notify() {
    return mbedtls_ssl_close_notify(&ssl_);
  }

 private:
  mbedtls_ssl_context ssl_;
  mbedtls_net_context net_;
};

#else  // !EWSS_WITH_TLS

// ============================================================================
// Stub implementations when TLS is disabled
// ============================================================================

class TlsContext {
 public:
  int init(const TlsConfig& /* config */) { return -1; }
  bool is_initialized() const { return false; }
};

class TlsSession {
 public:
  int setup(const TlsContext& /* ctx */, int /* fd */) { return -1; }
  int handshake() { return -1; }
  int read(uint8_t* /* buf */, size_t /* len */) { return -1; }
  int write(const uint8_t* /* buf */, size_t /* len */) { return -1; }
  int close_notify() { return -1; }
};

#endif  // EWSS_WITH_TLS

}  // namespace ewss

#endif  // EWSS_TLS_HPP_
