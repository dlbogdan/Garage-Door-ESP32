#include "backup_service.hpp"

#include <array>
#include <cstring>

#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"

namespace gate::backup {
namespace {

constexpr std::uint8_t kRecoveryPartitionSubtype = 0x40;
constexpr std::size_t kFingerprintMaterialSize = 40;

void put_u32(std::uint8_t* output, std::uint32_t value) {
  output[0] = static_cast<std::uint8_t>(value);
  output[1] = static_cast<std::uint8_t>(value >> 8);
  output[2] = static_cast<std::uint8_t>(value >> 16);
  output[3] = static_cast<std::uint8_t>(value >> 24);
}

esp_err_t fingerprint_layout(const PartitionLayout& layout,
                             std::array<std::uint8_t, kLayoutFingerprintSize>*
                                 fingerprint) {
  if (fingerprint == nullptr) return ESP_ERR_INVALID_ARG;
  std::array<std::uint8_t, kFingerprintMaterialSize> material{};
  constexpr char domain[] = "gate-backup-layout-v1";
  std::memcpy(material.data(), domain, sizeof(domain) - 1);
  put_u32(material.data() + 24, layout.nvs_offset);
  put_u32(material.data() + 28, layout.nvs_size);
  put_u32(material.data() + 32, layout.recovery_offset);
  put_u32(material.data() + 36, layout.recovery_size);
  return mbedtls_sha256(material.data(), material.size(), fingerprint->data(), 0) ==
                 0
             ? ESP_OK
             : ESP_FAIL;
}

const esp_partition_t* find_nvs() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_DATA_NVS,
                                  kNvsPartitionLabel);
}

const esp_partition_t* find_recovery() {
  return esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      static_cast<esp_partition_subtype_t>(kRecoveryPartitionSubtype),
      kRecoveryPartitionLabel);
}

void secure_clear(std::vector<std::uint8_t>* bytes) {
  if (bytes == nullptr) return;
  volatile std::uint8_t* cursor = bytes->data();
  std::size_t remaining = bytes->size();
  while (remaining-- > 0) *cursor++ = 0;
  bytes->clear();
}

}  // namespace

esp_err_t discover_layout(PartitionLayout* layout, EnvelopeMetadata* metadata,
                          std::uint32_t source_config_schema) {
  if (layout == nullptr || metadata == nullptr) return ESP_ERR_INVALID_ARG;
  const esp_partition_t* nvs = find_nvs();
  const esp_partition_t* recovery = find_recovery();
  if (nvs == nullptr || recovery == nullptr || nvs->size == 0 ||
      nvs->size > kMaximumNvsImageSize || recovery->size < nvs->size + 0x3000) {
    return ESP_ERR_NOT_FOUND;
  }

  layout->nvs_offset = nvs->address;
  layout->nvs_size = nvs->size;
  layout->recovery_offset = recovery->address;
  layout->recovery_size = recovery->size;

  metadata->project_id = kProjectId;
  metadata->source_config_schema = source_config_schema;
  metadata->nvs_offset = layout->nvs_offset;
  metadata->nvs_size = layout->nvs_size;
  return fingerprint_layout(*layout, &metadata->layout_fingerprint);
}

esp_err_t create_full_backup(const std::string& administrator_password,
                             std::uint32_t source_config_schema,
                             std::vector<std::uint8_t>* envelope) {
  if (envelope == nullptr) return ESP_ERR_INVALID_ARG;
  PartitionLayout layout;
  EnvelopeMetadata metadata;
  esp_err_t result = discover_layout(&layout, &metadata, source_config_schema);
  if (result != ESP_OK) return result;

  const esp_partition_t* nvs = find_nvs();
  if (nvs == nullptr) return ESP_ERR_NOT_FOUND;
  std::vector<std::uint8_t> image(nvs->size);

  result = nvs_flash_deinit();
  if (result != ESP_OK) {
    secure_clear(&image);
    return result;
  }
  const esp_err_t read_result = esp_partition_read(nvs, 0, image.data(), image.size());
  const esp_err_t init_result = nvs_flash_init();
  if (read_result != ESP_OK || init_result != ESP_OK) {
    secure_clear(&image);
    return read_result != ESP_OK ? read_result : init_result;
  }

  result = encrypt_envelope(image, administrator_password, metadata, envelope);
  secure_clear(&image);
  return result;
}

esp_err_t decrypt_full_backup(const std::vector<std::uint8_t>& envelope,
                             const std::string& old_administrator_password,
                             std::vector<std::uint8_t>* nvs_image,
                             EnvelopeInfo* authenticated_info) {
  if (nvs_image == nullptr) return ESP_ERR_INVALID_ARG;
  EnvelopeInfo untrusted_info;
  esp_err_t result = inspect_envelope(envelope, &untrusted_info);
  if (result != ESP_OK) return result;

  PartitionLayout layout;
  EnvelopeMetadata expected;
  result = discover_layout(&layout, &expected,
                           untrusted_info.metadata.source_config_schema);
  if (result != ESP_OK) return result;
  result = decrypt_envelope(envelope, old_administrator_password, expected,
                            nvs_image);
  if (result != ESP_OK) return result;
  if (authenticated_info != nullptr) *authenticated_info = untrusted_info;
  return ESP_OK;
}

}  // namespace gate::backup
