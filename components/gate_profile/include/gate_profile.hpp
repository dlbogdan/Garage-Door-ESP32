#pragma once

#include <string>

#include "app_config.hpp"
#include "esp_err.h"

namespace gate::profile {

inline constexpr char kFormat[] = "garage-door-gate-profile";
inline constexpr char kProject[] = "garage-door-esp32";
inline constexpr int kVersion = 1;
inline constexpr std::size_t kMaximumJsonSize = 24 * 1024;

struct Metadata {
  std::string vendor;
  std::string model;
  std::string name;
  std::string notes;
};

struct Candidate {
  gate::config::AppConfig config;
  Metadata metadata;
  std::string normalized_json;
  std::string digest;
};

std::string serialize(const gate::config::AppConfig& config,
                      const Metadata& metadata = {});

esp_err_t parse(const std::string& json,
                const gate::config::AppConfig& current,
                Candidate* candidate,
                std::string* error);

std::string sha256_hex(const std::string& value);

}  // namespace gate::profile
