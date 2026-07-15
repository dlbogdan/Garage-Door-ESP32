#include "web_auth.hpp"

#include <cstdint>

#include "esp_random.h"
#include "esp_timer.h"
#include "password.hpp"

namespace gate::web_auth {
namespace {

struct Session {
  bool active{false};
  std::string token;
  std::string csrf;
  std::int64_t created_us{0};
  std::int64_t last_seen_us{0};
};

Session session;
constexpr std::int64_t kSessionIdleUs = 30LL * 60 * 1000000;
constexpr std::int64_t kSessionAbsoluteUs = 8LL * 60 * 60 * 1000000;

std::string random_hex(std::size_t bytes) {
  constexpr char hex[] = "0123456789abcdef";
  std::string output(bytes * 2, '0');
  for (std::size_t index = 0; index < bytes; ++index) {
    const std::uint8_t value = static_cast<std::uint8_t>(esp_random());
    output[index * 2] = hex[value >> 4];
    output[index * 2 + 1] = hex[value & 0x0f];
  }
  return output;
}

bool constant_time_equal(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
  }
  return difference == 0;
}

bool request_cookie(httpd_req_t* request, const char* name, std::string* value) {
  const std::size_t length = httpd_req_get_hdr_value_len(request, "Cookie");
  if (length == 0 || length > 512) return false;
  std::string cookies(length + 1, '\0');
  if (httpd_req_get_hdr_value_str(request, "Cookie", cookies.data(),
                                  cookies.size()) != ESP_OK) return false;
  cookies.resize(length);
  const std::string prefix = std::string(name) + "=";
  std::size_t start = 0;
  while (start < cookies.size()) {
    while (start < cookies.size() && cookies[start] == ' ') ++start;
    const std::size_t end = cookies.find(';', start);
    const std::string item = cookies.substr(start, end - start);
    if (item.rfind(prefix, 0) == 0) {
      *value = item.substr(prefix.size());
      return true;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

}  // namespace

bool authorize(httpd_req_t* request, bool require_csrf,
               bool refresh_activity) {
  const std::int64_t now = esp_timer_get_time();
  if (!session.active || now - session.last_seen_us > kSessionIdleUs ||
      now - session.created_us > kSessionAbsoluteUs) {
    session = {};
    return false;
  }
  std::string presented_token;
  if (!request_cookie(request, "gate_session", &presented_token) ||
      !constant_time_equal(presented_token, session.token)) return false;
  if (require_csrf) {
    const std::size_t length =
        httpd_req_get_hdr_value_len(request, "X-CSRF-Token");
    if (length == 0 || length > 128) return false;
    std::string presented_csrf(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(request, "X-CSRF-Token",
                                    presented_csrf.data(),
                                    presented_csrf.size()) != ESP_OK) return false;
    presented_csrf.resize(length);
    if (!constant_time_equal(presented_csrf, session.csrf)) return false;
  }
  if (refresh_activity) session.last_seen_us = now;
  return true;
}

bool verify_reauthorization(httpd_req_t* request,
                            const gate::config::AdminConfig& admin) {
  const std::size_t length =
      httpd_req_get_hdr_value_len(request, "X-Admin-Password");
  if (length == 0 || length > 128) return false;
  std::string password(length + 1, '\0');
  if (httpd_req_get_hdr_value_str(request, "X-Admin-Password", password.data(),
                                  password.size()) != ESP_OK) return false;
  password.resize(length);
  const bool valid = gate::config::verify_admin_password(password, admin);
  std::fill(password.begin(), password.end(), '\0');
  return valid;
}

void create_session() {
  const std::int64_t now = esp_timer_get_time();
  session = {true, random_hex(32), random_hex(24), now, now};
}

void clear_session() { session = {}; }
const std::string& token() { return session.token; }
const std::string& csrf() { return session.csrf; }

}  // namespace gate::web_auth
