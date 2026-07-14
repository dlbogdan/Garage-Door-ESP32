#include "bootstrap_credentials.hpp"

#include <algorithm>
#include <cstring>

#include "esp_random.h"
#include "nvs.h"

namespace gate::bootstrap {
namespace {

constexpr char kNamespace[] = "gate_boot";
constexpr char kCredentialsKey[] = "credentials";
constexpr std::uint32_t kMagic = 0x42535450;

template <std::size_t Size>
void copy_string(char (&destination)[Size], const std::string& source) {
  std::memset(destination, 0, Size);
  std::memcpy(destination, source.data(), std::min(source.size(), Size - 1));
}

void generate_ap_password(Credentials* credentials) {
  constexpr char alphabet[] =
      "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  for (std::size_t index = 0; index < sizeof(credentials->ap_password) - 1;
       ++index) {
    credentials->ap_password[index] =
        alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }
  credentials->ap_password[sizeof(credentials->ap_password) - 1] = '\0';
}

}  // namespace

static_assert(sizeof(Credentials) == 117,
              "gate_boot credential blob layout must remain compatible");

esp_err_t save(const Credentials& credentials) {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, kCredentialsKey, &credentials,
                          sizeof(credentials));
  }
  if (result == ESP_OK) result = nvs_commit(handle);
  if (handle != 0) nvs_close(handle);
  return result;
}

esp_err_t load_or_create(Credentials* credentials) {
  if (credentials == nullptr) return ESP_ERR_INVALID_ARG;
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (result == ESP_OK) {
    std::size_t size = sizeof(*credentials);
    result = nvs_get_blob(handle, kCredentialsKey, credentials, &size);
    nvs_close(handle);
    if (result == ESP_OK && size == sizeof(*credentials) &&
        credentials->magic == kMagic &&
        strnlen(credentials->ap_password,
                sizeof(credentials->ap_password)) >= 8) {
      return ESP_OK;
    }
  }

  *credentials = {};
  credentials->magic = kMagic;
  generate_ap_password(credentials);
  return save(*credentials);
}

void set_station(Credentials* credentials, const std::string& ssid,
                 const std::string& password) {
  if (credentials == nullptr) return;
  copy_string(credentials->station_ssid, ssid);
  copy_string(credentials->station_password, password);
}

}  // namespace gate::bootstrap
