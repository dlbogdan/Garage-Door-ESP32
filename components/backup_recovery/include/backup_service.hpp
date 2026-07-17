#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "backup_envelope.hpp"
#include "esp_err.h"

namespace gate::backup {

inline constexpr char kProjectId[] = "garage-door-esp32";
inline constexpr char kNvsPartitionLabel[] = "nvs";
inline constexpr char kRecoveryPartitionLabel[] = "backup_stage";

struct PartitionLayout final {
  std::uint32_t nvs_offset{0};
  std::uint32_t nvs_size{0};
  std::uint32_t recovery_offset{0};
  std::uint32_t recovery_size{0};
};

// Discovers the two partitions used by disaster recovery and derives the
// compatibility fingerprint authenticated by the backup envelope.
esp_err_t discover_layout(PartitionLayout* layout,
                          EnvelopeMetadata* metadata,
                          std::uint32_t source_config_schema);

// Briefly deinitializes the default NVS partition, captures its complete raw
// image, and reinitializes NVS before performing the expensive encryption.
// Callers must first place the application in maintenance mode so no task can
// attempt an NVS write during the snapshot window.
esp_err_t create_full_backup(const std::string& administrator_password,
                             std::uint32_t source_config_schema,
                             std::vector<std::uint8_t>* envelope);

// Authenticates and decrypts an uploaded backup against this firmware's actual
// partition layout. The plaintext is returned only after AES-GCM succeeds.
esp_err_t decrypt_full_backup(const std::vector<std::uint8_t>& envelope,
                             const std::string& old_administrator_password,
                             std::vector<std::uint8_t>* nvs_image,
                             EnvelopeInfo* authenticated_info = nullptr);

}  // namespace gate::backup
