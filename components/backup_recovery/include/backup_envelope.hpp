#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace gate::backup {

inline constexpr std::uint16_t kEnvelopeVersion = 1;
inline constexpr std::uint32_t kDefaultKdfIterations = 120000;
inline constexpr std::size_t kSaltSize = 16;
inline constexpr std::size_t kNonceSize = 12;
inline constexpr std::size_t kTagSize = 16;
inline constexpr std::size_t kLayoutFingerprintSize = 32;
inline constexpr std::size_t kProjectIdSize = 24;
inline constexpr std::size_t kEnvelopeHeaderSize = 120;
inline constexpr std::size_t kMaximumNvsImageSize = 128 * 1024;

struct EnvelopeMetadata final {
  std::string project_id;
  std::uint32_t source_config_schema{0};
  std::uint32_t nvs_offset{0};
  std::uint32_t nvs_size{0};
  std::array<std::uint8_t, kLayoutFingerprintSize> layout_fingerprint{};
};

struct EnvelopeInfo final {
  EnvelopeMetadata metadata;
  std::uint32_t kdf_iterations{0};
  std::uint32_t ciphertext_size{0};
};

using RandomFill = esp_err_t (*)(std::uint8_t* output, std::size_t size,
                                 void* context);

// Encrypts a complete raw NVS image. All header fields are authenticated as
// AES-GCM associated data; only the salt and nonce are intentionally public.
esp_err_t encrypt_envelope(const std::vector<std::uint8_t>& plaintext,
                           const std::string& password,
                           const EnvelopeMetadata& metadata,
                           std::vector<std::uint8_t>* envelope,
                           RandomFill random_fill = nullptr,
                           void* random_context = nullptr);

// Parses non-secret metadata and enforces structural limits. This does not
// authenticate the file and must never be treated as restore authorization.
esp_err_t inspect_envelope(const std::vector<std::uint8_t>& envelope,
                           EnvelopeInfo* info);

// Authenticates the complete envelope before returning any plaintext.
esp_err_t decrypt_envelope(const std::vector<std::uint8_t>& envelope,
                           const std::string& password,
                           const EnvelopeMetadata& expected,
                           std::vector<std::uint8_t>* plaintext);

}  // namespace gate::backup
