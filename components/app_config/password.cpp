#include "password.hpp"

#include <algorithm>
#include <array>
#include <vector>

#include "esp_random.h"
#include "mbedtls/md.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace gate::config {
namespace {

constexpr std::size_t kSha256Size = 32;

int derive_pbkdf2(const std::string& password,
                  const std::uint8_t* salt, std::size_t salt_size,
                  std::uint32_t iterations, std::uint8_t* output,
                  std::size_t output_size) {
  const mbedtls_md_info_t* info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr || iterations == 0 || output == nullptr) return -1;

  std::vector<std::uint8_t> salt_block(salt, salt + salt_size);
  salt_block.resize(salt_size + 4);
  std::array<std::uint8_t, kSha256Size> previous{};
  std::array<std::uint8_t, kSha256Size> current{};
  std::array<std::uint8_t, kSha256Size> accumulated{};
  std::size_t written = 0;
  for (std::uint32_t block = 1; written < output_size; ++block) {
    salt_block[salt_size] = static_cast<std::uint8_t>(block >> 24);
    salt_block[salt_size + 1] = static_cast<std::uint8_t>(block >> 16);
    salt_block[salt_size + 2] = static_cast<std::uint8_t>(block >> 8);
    salt_block[salt_size + 3] = static_cast<std::uint8_t>(block);
    int result = mbedtls_md_hmac(
        info, reinterpret_cast<const unsigned char*>(password.data()),
        password.size(), salt_block.data(), salt_block.size(), previous.data());
    if (result != 0) return result;
    accumulated = previous;

    for (std::uint32_t round = 1; round < iterations; ++round) {
      result = mbedtls_md_hmac(
          info, reinterpret_cast<const unsigned char*>(password.data()),
          password.size(), previous.data(), previous.size(), current.data());
      if (result != 0) return result;
      for (std::size_t index = 0; index < accumulated.size(); ++index) {
        accumulated[index] ^= current[index];
      }
      previous.swap(current);
#ifdef ESP_PLATFORM
      if ((round & 0x7fU) == 0) vTaskDelay(1);
#endif
    }

    const std::size_t count =
        std::min(accumulated.size(), output_size - written);
    std::copy_n(accumulated.begin(), count, output + written);
    written += count;
  }
  std::fill(previous.begin(), previous.end(), 0);
  std::fill(current.begin(), current.end(), 0);
  std::fill(accumulated.begin(), accumulated.end(), 0);
  return 0;
}

}  // namespace

esp_err_t derive_admin_password(const std::string& password,
                                AdminConfig* admin) {
  if (admin == nullptr || password.size() < 10 || password.size() > 128) {
    return ESP_ERR_INVALID_ARG;
  }

  std::array<std::uint8_t, 16> salt{};
  for (std::size_t offset = 0; offset < salt.size(); offset += sizeof(uint32_t)) {
    const uint32_t random = esp_random();
    const std::size_t count = std::min(sizeof(random), salt.size() - offset);
    std::copy_n(reinterpret_cast<const std::uint8_t*>(&random), count,
                salt.begin() + offset);
  }
  std::array<std::uint8_t, 32> verifier{};
  const int result = derive_pbkdf2(password, salt.data(), salt.size(),
                                   kPasswordIterations, verifier.data(),
                                   verifier.size());
  if (result != 0) return ESP_FAIL;

  admin->salt.assign(salt.begin(), salt.end());
  admin->password_verifier.assign(verifier.begin(), verifier.end());
  admin->pbkdf2_iterations = kPasswordIterations;
  return ESP_OK;
}

bool verify_admin_password(const std::string& password,
                           const AdminConfig& admin) {
  if (password.empty() || admin.salt.empty() ||
      admin.password_verifier.empty() || admin.pbkdf2_iterations < 50000) {
    return false;
  }
  std::vector<std::uint8_t> candidate(admin.password_verifier.size());
  const int result = derive_pbkdf2(
      password, admin.salt.data(), admin.salt.size(), admin.pbkdf2_iterations,
      candidate.data(), candidate.size());
  if (result != 0) return false;

  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < candidate.size(); ++index) {
    difference |= candidate[index] ^ admin.password_verifier[index];
  }
  std::fill(candidate.begin(), candidate.end(), 0);
  return difference == 0;
}

}  // namespace gate::config
