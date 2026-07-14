#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace gate::bootstrap {

#pragma pack(push, 1)
struct Credentials {
  std::uint32_t magic;
  char ap_password[16];
  char station_ssid[33];
  char station_password[64];
};
#pragma pack(pop)

esp_err_t load_or_create(Credentials* credentials);
esp_err_t save(const Credentials& credentials);
void set_station(Credentials* credentials, const std::string& ssid,
                 const std::string& password);

}  // namespace gate::bootstrap
