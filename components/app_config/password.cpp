#include "password.hpp"

#include <algorithm>
#include <array>

#include "esp_random.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

namespace gate::config {

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
  const int result = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA256,
      reinterpret_cast<const unsigned char*>(password.data()), password.size(),
      salt.data(), salt.size(), kPasswordIterations, verifier.size(),
      verifier.data());
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
  const int result = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA256,
      reinterpret_cast<const unsigned char*>(password.data()), password.size(),
      admin.salt.data(), admin.salt.size(), admin.pbkdf2_iterations,
      candidate.size(), candidate.data());
  if (result != 0) return false;

  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < candidate.size(); ++index) {
    difference |= candidate[index] ^ admin.password_verifier[index];
  }
  std::fill(candidate.begin(), candidate.end(), 0);
  return difference == 0;
}

}  // namespace gate::config
