#pragma once

#include <string>

#include "app_config.hpp"
#include "esp_err.h"

namespace gate::config {

inline constexpr std::uint32_t kPasswordIterations = 60000;

// Generates a random per-device salt and stores only a PBKDF2-HMAC-SHA-256
// verifier. The plaintext password is never persisted.
esp_err_t derive_admin_password(const std::string& password,
                                AdminConfig* admin);

// Re-derives the verifier and compares every byte without early exit.
bool verify_admin_password(const std::string& password,
                           const AdminConfig& admin);

}  // namespace gate::config
