#include "restore_staging.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "backup_service.hpp"
#include "esp_log.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"

namespace gate::backup {
namespace {

constexpr std::uint8_t kRecoveryPartitionSubtype = 0x40;
constexpr std::size_t kSectorSize = 0x1000;
constexpr std::size_t kManifestSlotCount = 2;
constexpr std::size_t kPayloadOffset = kSectorSize * kManifestSlotCount;
constexpr std::uint32_t kManifestVersion = 1;
constexpr std::uint32_t kMaximumAttempts = 3;
constexpr char kTag[] = "restore_staging";
constexpr std::array<std::uint8_t, 8> kManifestMagic = {
    'G', 'D', 'R', 'S', 'T', 'O', 'R', 'E'};

struct Manifest final {
  std::array<std::uint8_t, 8> magic{};
  std::uint32_t version{0};
  RestoreState state{RestoreState::kFailed};
  std::uint32_t sequence{0};
  std::uint32_t attempts{0};
  std::uint32_t image_size{0};
  std::array<std::uint8_t, 32> image_sha256{};
  std::array<std::uint8_t, 32> manifest_sha256{};
};

static_assert(sizeof(Manifest) < kSectorSize);

const esp_partition_t* staging_partition() {
  return esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      static_cast<esp_partition_subtype_t>(kRecoveryPartitionSubtype),
      kRecoveryPartitionLabel);
}

const esp_partition_t* nvs_partition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_DATA_NVS,
                                  kNvsPartitionLabel);
}

esp_err_t sha256(const void* data, std::size_t size,
                 std::array<std::uint8_t, 32>* digest) {
  if (data == nullptr || digest == nullptr) return ESP_ERR_INVALID_ARG;
  return mbedtls_sha256(static_cast<const unsigned char*>(data), size,
                        digest->data(), 0) == 0
             ? ESP_OK
             : ESP_FAIL;
}

void seal_manifest(Manifest* manifest) {
  manifest->manifest_sha256.fill(0);
  sha256(manifest, offsetof(Manifest, manifest_sha256),
         &manifest->manifest_sha256);
}

bool valid_state(RestoreState state) {
  return state == RestoreState::kReady || state == RestoreState::kApplying ||
         state == RestoreState::kFailed || state == RestoreState::kApplied ||
         state == RestoreState::kValidated;
}

const char* state_name(RestoreState state) {
  switch (state) {
    case RestoreState::kReady:
      return "ready";
    case RestoreState::kApplying:
      return "applying";
    case RestoreState::kFailed:
      return "failed";
    case RestoreState::kApplied:
      return "applied";
    case RestoreState::kValidated:
      return "validated";
  }
  return "unknown";
}

bool valid_manifest(const Manifest& manifest, std::size_t partition_size) {
  if (manifest.magic != kManifestMagic ||
      manifest.version != kManifestVersion || !valid_state(manifest.state) ||
      manifest.sequence == 0 || manifest.image_size == 0 ||
      manifest.image_size > kMaximumNvsImageSize ||
      kPayloadOffset + manifest.image_size > partition_size) {
    return false;
  }
  std::array<std::uint8_t, 32> expected{};
  if (sha256(&manifest, offsetof(Manifest, manifest_sha256), &expected) !=
      ESP_OK) {
    return false;
  }
  return expected == manifest.manifest_sha256;
}

esp_err_t read_current_manifest(const esp_partition_t* partition,
                                Manifest* manifest, std::size_t* slot) {
  if (partition == nullptr || manifest == nullptr || slot == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  bool found = false;
  Manifest selected;
  std::size_t selected_slot = 0;
  for (std::size_t index = 0; index < kManifestSlotCount; ++index) {
    Manifest candidate;
    if (esp_partition_read(partition, index * kSectorSize, &candidate,
                           sizeof(candidate)) == ESP_OK &&
        valid_manifest(candidate, partition->size) &&
        (!found || candidate.sequence > selected.sequence)) {
      selected = candidate;
      selected_slot = index;
      found = true;
    }
  }
  if (!found) return ESP_ERR_NOT_FOUND;
  *manifest = selected;
  *slot = selected_slot;
  return ESP_OK;
}

esp_err_t write_manifest(const esp_partition_t* partition,
                         const Manifest& source, std::size_t slot) {
  if (partition == nullptr || slot >= kManifestSlotCount) {
    return ESP_ERR_INVALID_ARG;
  }
  Manifest manifest = source;
  seal_manifest(&manifest);
  const std::size_t offset = slot * kSectorSize;
  esp_err_t result = esp_partition_erase_range(partition, offset, kSectorSize);
  if (result != ESP_OK) return result;
  result = esp_partition_write(partition, offset, &manifest, sizeof(manifest));
  if (result != ESP_OK) return result;
  Manifest readback;
  result = esp_partition_read(partition, offset, &readback, sizeof(readback));
  return result == ESP_OK && std::memcmp(&manifest, &readback, sizeof(manifest)) == 0
             ? ESP_OK
             : ESP_ERR_INVALID_CRC;
}

esp_err_t verify_partition_payload(const esp_partition_t* partition,
                                   std::size_t offset, std::size_t size,
                                   const std::array<std::uint8_t, 32>& expected) {
  std::vector<std::uint8_t> bytes(size);
  esp_err_t result = esp_partition_read(partition, offset, bytes.data(), size);
  if (result != ESP_OK) return result;
  std::array<std::uint8_t, 32> actual{};
  result = sha256(bytes.data(), bytes.size(), &actual);
  std::fill(bytes.begin(), bytes.end(), 0);
  return result == ESP_OK && actual == expected ? ESP_OK : ESP_ERR_INVALID_CRC;
}

}  // namespace

esp_err_t stage_restore_image(const std::vector<std::uint8_t>& nvs_image) {
  const esp_partition_t* staging = staging_partition();
  const esp_partition_t* nvs = nvs_partition();
  if (staging == nullptr || nvs == nullptr || nvs_image.size() != nvs->size ||
      kPayloadOffset + nvs_image.size() > staging->size) {
    return ESP_ERR_INVALID_SIZE;
  }

  esp_err_t result = esp_partition_erase_range(staging, 0, staging->size);
  if (result != ESP_OK) return result;
  result = esp_partition_write(staging, kPayloadOffset, nvs_image.data(),
                               nvs_image.size());
  if (result != ESP_OK) return result;

  Manifest manifest;
  manifest.magic = kManifestMagic;
  manifest.version = kManifestVersion;
  manifest.state = RestoreState::kReady;
  manifest.sequence = 1;
  manifest.attempts = 0;
  manifest.image_size = nvs_image.size();
  result = sha256(nvs_image.data(), nvs_image.size(), &manifest.image_sha256);
  if (result != ESP_OK) return result;
  result = verify_partition_payload(staging, kPayloadOffset, nvs_image.size(),
                                    manifest.image_sha256);
  if (result != ESP_OK) return result;
  result = write_manifest(staging, manifest, 0);
  if (result == ESP_OK) {
    ESP_LOGI(kTag, "Restore staged: slot=0 sequence=1 state=ready size=%lu",
             static_cast<unsigned long>(manifest.image_size));
  }
  return result;
}

esp_err_t restore_status(RestoreStatus* status) {
  if (status == nullptr) return ESP_ERR_INVALID_ARG;
  *status = {};
  const esp_partition_t* staging = staging_partition();
  if (staging == nullptr) return ESP_ERR_NOT_FOUND;
  Manifest manifest;
  std::size_t slot = 0;
  const esp_err_t result = read_current_manifest(staging, &manifest, &slot);
  if (result == ESP_ERR_NOT_FOUND) return ESP_OK;
  if (result != ESP_OK) return result;
  status->present = true;
  status->state = manifest.state;
  status->sequence = manifest.sequence;
  status->attempts = manifest.attempts;
  status->image_size = manifest.image_size;
  return ESP_OK;
}

esp_err_t apply_staged_restore_early(bool* applied) {
  if (applied == nullptr) return ESP_ERR_INVALID_ARG;
  *applied = false;
  const esp_partition_t* staging = staging_partition();
  const esp_partition_t* nvs = nvs_partition();
  if (staging == nullptr || nvs == nullptr) return ESP_ERR_NOT_FOUND;
  Manifest manifest;
  std::size_t current_slot = 0;
  esp_err_t result = read_current_manifest(staging, &manifest, &current_slot);
  if (result == ESP_ERR_NOT_FOUND) {
    ESP_LOGD(kTag, "No staged restore manifest");
    return ESP_OK;
  }
  if (result != ESP_OK) return result;
  ESP_LOGI(kTag,
           "Restore manifest: slot=%lu sequence=%lu state=%s attempts=%lu size=%lu",
           static_cast<unsigned long>(current_slot),
           static_cast<unsigned long>(manifest.sequence), state_name(manifest.state),
           static_cast<unsigned long>(manifest.attempts),
           static_cast<unsigned long>(manifest.image_size));
  if (manifest.state == RestoreState::kFailed ||
      manifest.state == RestoreState::kApplied ||
      manifest.state == RestoreState::kValidated) {
    return ESP_OK;
  }
  if (manifest.image_size != nvs->size) {
    ESP_LOGE(kTag, "Restore image size %lu does not match NVS size %lu",
             static_cast<unsigned long>(manifest.image_size),
             static_cast<unsigned long>(nvs->size));
    return ESP_ERR_INVALID_SIZE;
  }
  if (manifest.attempts >= kMaximumAttempts) {
    manifest.state = RestoreState::kFailed;
    ++manifest.sequence;
    return write_manifest(staging, manifest, 1 - current_slot);
  }
  result = verify_partition_payload(staging, kPayloadOffset, manifest.image_size,
                                    manifest.image_sha256);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Staged restore payload verification failed: %s",
             esp_err_to_name(result));
    manifest.state = RestoreState::kFailed;
    ++manifest.sequence;
    write_manifest(staging, manifest, 1 - current_slot);
    return result;
  }

  manifest.state = RestoreState::kApplying;
  ++manifest.attempts;
  ++manifest.sequence;
  current_slot = 1 - current_slot;
  result = write_manifest(staging, manifest, current_slot);
  if (result != ESP_OK) return result;
  ESP_LOGI(kTag, "Applying restore attempt %lu from manifest slot %lu",
           static_cast<unsigned long>(manifest.attempts),
           static_cast<unsigned long>(current_slot));

  std::vector<std::uint8_t> image(manifest.image_size);
  result = esp_partition_read(staging, kPayloadOffset, image.data(), image.size());
  ESP_LOGI(kTag, "Staged payload read: %s", esp_err_to_name(result));
  if (result == ESP_OK) {
    result = esp_partition_erase_range(nvs, 0, nvs->size);
    ESP_LOGI(kTag, "NVS erase: %s", esp_err_to_name(result));
  }
  if (result == ESP_OK) {
    result = esp_partition_write(nvs, 0, image.data(), image.size());
    ESP_LOGI(kTag, "NVS write (%lu bytes): %s",
             static_cast<unsigned long>(image.size()), esp_err_to_name(result));
  }
  std::fill(image.begin(), image.end(), 0);
  if (result == ESP_OK) {
    result = verify_partition_payload(nvs, 0, manifest.image_size,
                                      manifest.image_sha256);
    ESP_LOGI(kTag, "NVS readback verification: %s", esp_err_to_name(result));
  }
  if (result == ESP_OK) {
    manifest.state = RestoreState::kApplied;
    ++manifest.sequence;
    result = write_manifest(staging, manifest, 1 - current_slot);
    if (result == ESP_OK) {
      *applied = true;
      ESP_LOGI(kTag,
               "Restore image applied; retained staging payload pending clean-boot confirmation");
    }
    return result;
  }
  ESP_LOGE(kTag, "Restore attempt %lu failed after publication: %s",
           static_cast<unsigned long>(manifest.attempts), esp_err_to_name(result));
  if (manifest.attempts >= kMaximumAttempts) {
    manifest.state = RestoreState::kFailed;
    ++manifest.sequence;
    write_manifest(staging, manifest, 1 - current_slot);
  }
  return result;
}

esp_err_t confirm_staged_restore(bool* restart_required) {
  if (restart_required == nullptr) return ESP_ERR_INVALID_ARG;
  *restart_required = false;
  const esp_partition_t* staging = staging_partition();
  if (staging == nullptr) return ESP_ERR_NOT_FOUND;
  Manifest manifest;
  std::size_t slot = 0;
  const esp_err_t result = read_current_manifest(staging, &manifest, &slot);
  if (result == ESP_ERR_NOT_FOUND) return ESP_OK;
  if (result != ESP_OK) return result;
  if (manifest.state == RestoreState::kApplied) {
    manifest.state = RestoreState::kValidated;
    ++manifest.sequence;
    const esp_err_t write_result = write_manifest(staging, manifest, 1 - slot);
    if (write_result == ESP_OK) {
      *restart_required = true;
      ESP_LOGI(kTag,
               "First clean validation boot passed; retaining payload for one more boot");
    }
    return write_result;
  }
  if (manifest.state == RestoreState::kValidated) {
    ESP_LOGI(kTag,
             "Second clean validation boot passed; erasing retained staging payload");
    return erase_staged_restore();
  }
  return ESP_OK;
}

esp_err_t retry_applied_restore() {
  const esp_partition_t* staging = staging_partition();
  if (staging == nullptr) return ESP_ERR_NOT_FOUND;
  Manifest manifest;
  std::size_t slot = 0;
  esp_err_t result = read_current_manifest(staging, &manifest, &slot);
  if (result != ESP_OK) return result;
  if (manifest.state != RestoreState::kApplied &&
      manifest.state != RestoreState::kValidated) {
    return ESP_ERR_INVALID_STATE;
  }
  manifest.state = RestoreState::kReady;
  ++manifest.sequence;
  result = write_manifest(staging, manifest, 1 - slot);
  if (result == ESP_OK) {
    ESP_LOGW(kTag,
             "Rearmed retained restore after NVS validation failure; attempts=%lu",
             static_cast<unsigned long>(manifest.attempts));
  }
  return result;
}

esp_err_t erase_staged_restore() {
  const esp_partition_t* staging = staging_partition();
  return staging == nullptr
             ? ESP_ERR_NOT_FOUND
             : esp_partition_erase_range(staging, 0, staging->size);
}

}  // namespace gate::backup
