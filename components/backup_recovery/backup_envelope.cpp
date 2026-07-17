#include "backup_envelope.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"

namespace gate::backup {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {'G', 'D', 'N', 'V', 'S', 'B',
                                                'K', 'P'};
constexpr std::uint16_t kKdfPbkdf2Sha256 = 1;
constexpr std::uint16_t kCipherAes256Gcm = 1;
constexpr std::size_t kKeySize = 32;
constexpr std::uint32_t kKdfIterationsPerYield = 256;
constexpr std::size_t kOffsetMagic = 0;
constexpr std::size_t kOffsetVersion = 8;
constexpr std::size_t kOffsetHeaderSize = 10;
constexpr std::size_t kOffsetKdf = 12;
constexpr std::size_t kOffsetCipher = 14;
constexpr std::size_t kOffsetIterations = 16;
constexpr std::size_t kOffsetSourceSchema = 20;
constexpr std::size_t kOffsetNvsOffset = 24;
constexpr std::size_t kOffsetNvsSize = 28;
constexpr std::size_t kOffsetCiphertextSize = 32;
constexpr std::size_t kOffsetProjectId = 36;
constexpr std::size_t kOffsetFingerprint = 60;
constexpr std::size_t kOffsetSalt = 92;
constexpr std::size_t kOffsetNonce = 108;

static_assert(kOffsetNonce + kNonceSize == kEnvelopeHeaderSize);

void put_u16(std::uint8_t* output, std::uint16_t value) {
  output[0] = static_cast<std::uint8_t>(value);
  output[1] = static_cast<std::uint8_t>(value >> 8);
}

void put_u32(std::uint8_t* output, std::uint32_t value) {
  output[0] = static_cast<std::uint8_t>(value);
  output[1] = static_cast<std::uint8_t>(value >> 8);
  output[2] = static_cast<std::uint8_t>(value >> 16);
  output[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint16_t get_u16(const std::uint8_t* input) {
  return static_cast<std::uint16_t>(input[0]) |
         (static_cast<std::uint16_t>(input[1]) << 8);
}

std::uint32_t get_u32(const std::uint8_t* input) {
  return static_cast<std::uint32_t>(input[0]) |
         (static_cast<std::uint32_t>(input[1]) << 8) |
         (static_cast<std::uint32_t>(input[2]) << 16) |
         (static_cast<std::uint32_t>(input[3]) << 24);
}

void secure_clear(std::uint8_t* data, std::size_t size) {
  volatile std::uint8_t* cursor = data;
  while (size-- > 0) *cursor++ = 0;
}

esp_err_t default_random_fill(std::uint8_t* output, std::size_t size,
                              void*) {
  if (output == nullptr) return ESP_ERR_INVALID_ARG;
  esp_fill_random(output, size);
  return ESP_OK;
}

esp_err_t derive_key(const std::string& password, const std::uint8_t* salt,
                     std::uint32_t iterations,
                     std::array<std::uint8_t, kKeySize>* key) {
  if (password.size() < 10 || password.size() > 128 || salt == nullptr ||
      iterations < 50000 || iterations > 1000000 || key == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  // PBKDF2-HMAC-SHA-256 produces exactly one block for a 32-byte key. Implement
  // that RFC 8018 block incrementally so the HTTP task can periodically block
  // and let its core's idle task service the task watchdog. This remains byte-
  // compatible with mbedtls_pkcs5_pbkdf2_hmac_ext().
  const mbedtls_md_info_t* info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) return ESP_FAIL;

  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  int crypto_result = mbedtls_md_setup(&context, info, 1);
  if (crypto_result == 0) {
    crypto_result = mbedtls_md_hmac_starts(
        &context, reinterpret_cast<const unsigned char*>(password.data()),
        password.size());
  }

  std::array<std::uint8_t, kKeySize> u{};
  constexpr std::array<std::uint8_t, 4> kBlockIndex{0, 0, 0, 1};
  if (crypto_result == 0) {
    crypto_result = mbedtls_md_hmac_update(&context, salt, kSaltSize);
  }
  if (crypto_result == 0) {
    crypto_result = mbedtls_md_hmac_update(
        &context, kBlockIndex.data(), kBlockIndex.size());
  }
  if (crypto_result == 0) {
    crypto_result = mbedtls_md_hmac_finish(&context, u.data());
  }
  if (crypto_result == 0) *key = u;

  for (std::uint32_t iteration = 1;
       crypto_result == 0 && iteration < iterations; ++iteration) {
    crypto_result = mbedtls_md_hmac_reset(&context);
    if (crypto_result == 0) {
      crypto_result = mbedtls_md_hmac_update(&context, u.data(), u.size());
    }
    if (crypto_result == 0) {
      crypto_result = mbedtls_md_hmac_finish(&context, u.data());
    }
    if (crypto_result == 0) {
      for (std::size_t index = 0; index < key->size(); ++index) {
        (*key)[index] ^= u[index];
      }
    }
    if ((iteration % kKdfIterationsPerYield) == 0) {
      vTaskDelay(1);
    }
  }

  mbedtls_md_free(&context);
  secure_clear(u.data(), u.size());
  if (crypto_result != 0) secure_clear(key->data(), key->size());
  return crypto_result == 0 ? ESP_OK : ESP_FAIL;
}

bool valid_metadata(const EnvelopeMetadata& metadata) {
  return !metadata.project_id.empty() &&
         metadata.project_id.size() < kProjectIdSize && metadata.nvs_size > 0 &&
         metadata.nvs_size <= kMaximumNvsImageSize;
}

bool same_metadata(const EnvelopeMetadata& actual,
                   const EnvelopeMetadata& expected) {
  return actual.project_id == expected.project_id &&
         actual.nvs_offset == expected.nvs_offset &&
         actual.nvs_size == expected.nvs_size &&
         actual.layout_fingerprint == expected.layout_fingerprint;
}

esp_err_t parse_header(const std::vector<std::uint8_t>& envelope,
                       EnvelopeInfo* info) {
  if (info == nullptr || envelope.size() < kEnvelopeHeaderSize + kTagSize) {
    return ESP_ERR_INVALID_ARG;
  }
  const std::uint8_t* header = envelope.data();
  if (!std::equal(kMagic.begin(), kMagic.end(), header + kOffsetMagic) ||
      get_u16(header + kOffsetVersion) != kEnvelopeVersion ||
      get_u16(header + kOffsetHeaderSize) != kEnvelopeHeaderSize ||
      get_u16(header + kOffsetKdf) != kKdfPbkdf2Sha256 ||
      get_u16(header + kOffsetCipher) != kCipherAes256Gcm) {
    return ESP_ERR_INVALID_VERSION;
  }

  info->kdf_iterations = get_u32(header + kOffsetIterations);
  info->metadata.source_config_schema =
      get_u32(header + kOffsetSourceSchema);
  info->metadata.nvs_offset = get_u32(header + kOffsetNvsOffset);
  info->metadata.nvs_size = get_u32(header + kOffsetNvsSize);
  info->ciphertext_size = get_u32(header + kOffsetCiphertextSize);
  const auto* project = reinterpret_cast<const char*>(header + kOffsetProjectId);
  const auto terminator = static_cast<const char*>(
      std::memchr(project, '\0', kProjectIdSize));
  if (terminator == nullptr) return ESP_ERR_INVALID_ARG;
  info->metadata.project_id.assign(project, terminator);
  std::copy_n(header + kOffsetFingerprint, kLayoutFingerprintSize,
              info->metadata.layout_fingerprint.begin());

  if (info->kdf_iterations < 50000 || info->kdf_iterations > 1000000 ||
      !valid_metadata(info->metadata) ||
      info->ciphertext_size != info->metadata.nvs_size ||
      static_cast<std::size_t>(info->ciphertext_size) !=
          envelope.size() - kEnvelopeHeaderSize - kTagSize) {
    return ESP_ERR_INVALID_SIZE;
  }
  return ESP_OK;
}

}  // namespace

esp_err_t encrypt_envelope(const std::vector<std::uint8_t>& plaintext,
                           const std::string& password,
                           const EnvelopeMetadata& metadata,
                           std::vector<std::uint8_t>* envelope,
                           RandomFill random_fill, void* random_context) {
  if (envelope == nullptr || !valid_metadata(metadata) || plaintext.empty() ||
      plaintext.size() != metadata.nvs_size ||
      plaintext.size() > std::numeric_limits<std::uint32_t>::max()) {
    return ESP_ERR_INVALID_ARG;
  }
  if (random_fill == nullptr) random_fill = default_random_fill;

  envelope->assign(kEnvelopeHeaderSize + plaintext.size() + kTagSize, 0);
  std::uint8_t* header = envelope->data();
  std::copy(kMagic.begin(), kMagic.end(), header + kOffsetMagic);
  put_u16(header + kOffsetVersion, kEnvelopeVersion);
  put_u16(header + kOffsetHeaderSize, kEnvelopeHeaderSize);
  put_u16(header + kOffsetKdf, kKdfPbkdf2Sha256);
  put_u16(header + kOffsetCipher, kCipherAes256Gcm);
  put_u32(header + kOffsetIterations, kDefaultKdfIterations);
  put_u32(header + kOffsetSourceSchema, metadata.source_config_schema);
  put_u32(header + kOffsetNvsOffset, metadata.nvs_offset);
  put_u32(header + kOffsetNvsSize, metadata.nvs_size);
  put_u32(header + kOffsetCiphertextSize,
          static_cast<std::uint32_t>(plaintext.size()));
  std::copy(metadata.project_id.begin(), metadata.project_id.end(),
            header + kOffsetProjectId);
  std::copy(metadata.layout_fingerprint.begin(),
            metadata.layout_fingerprint.end(), header + kOffsetFingerprint);
  if (random_fill(header + kOffsetSalt, kSaltSize, random_context) != ESP_OK ||
      random_fill(header + kOffsetNonce, kNonceSize, random_context) != ESP_OK) {
    envelope->clear();
    return ESP_FAIL;
  }

  std::array<std::uint8_t, kKeySize> key{};
  esp_err_t result = derive_key(password, header + kOffsetSalt,
                                kDefaultKdfIterations, &key);
  if (result != ESP_OK) {
    envelope->clear();
    return result;
  }

  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  int crypto_result = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES,
                                         key.data(), key.size() * 8);
  if (crypto_result == 0) {
    crypto_result = mbedtls_gcm_crypt_and_tag(
        &context, MBEDTLS_GCM_ENCRYPT, plaintext.size(), header + kOffsetNonce,
        kNonceSize, header, kEnvelopeHeaderSize, plaintext.data(),
        header + kEnvelopeHeaderSize, kTagSize,
        header + kEnvelopeHeaderSize + plaintext.size());
  }
  mbedtls_gcm_free(&context);
  secure_clear(key.data(), key.size());
  if (crypto_result != 0) {
    envelope->clear();
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t inspect_envelope(const std::vector<std::uint8_t>& envelope,
                           EnvelopeInfo* info) {
  return parse_header(envelope, info);
}

esp_err_t decrypt_envelope(const std::vector<std::uint8_t>& envelope,
                           const std::string& password,
                           const EnvelopeMetadata& expected,
                           std::vector<std::uint8_t>* plaintext) {
  if (plaintext == nullptr || !valid_metadata(expected)) {
    return ESP_ERR_INVALID_ARG;
  }
  EnvelopeInfo info;
  esp_err_t result = parse_header(envelope, &info);
  if (result != ESP_OK) return result;
  if (!same_metadata(info.metadata, expected)) return ESP_ERR_INVALID_VERSION;

  const std::uint8_t* header = envelope.data();
  std::array<std::uint8_t, kKeySize> key{};
  result = derive_key(password, header + kOffsetSalt, info.kdf_iterations, &key);
  if (result != ESP_OK) return result;

  plaintext->assign(info.ciphertext_size, 0);
  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  int crypto_result = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES,
                                         key.data(), key.size() * 8);
  if (crypto_result == 0) {
    crypto_result = mbedtls_gcm_auth_decrypt(
        &context, info.ciphertext_size, header + kOffsetNonce, kNonceSize,
        header, kEnvelopeHeaderSize,
        header + kEnvelopeHeaderSize + info.ciphertext_size, kTagSize,
        header + kEnvelopeHeaderSize, plaintext->data());
  }
  mbedtls_gcm_free(&context);
  secure_clear(key.data(), key.size());
  if (crypto_result != 0) {
    secure_clear(plaintext->data(), plaintext->size());
    plaintext->clear();
    return ESP_ERR_INVALID_CRC;
  }
  return ESP_OK;
}

}  // namespace gate::backup
