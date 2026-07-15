#include "ota_api.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "esp_heap_caps.h"
#include "gate_runtime.hpp"
#include "ota_manager.hpp"
#include "web_auth.hpp"

namespace gate::ota_api {
namespace {

Context api_context{};

std::string json_escape(const char* input) {
  std::string output;
  for (; input != nullptr && *input != '\0'; ++input) {
    if (*input == '"' || *input == '\\') output.push_back('\\');
    if (static_cast<unsigned char>(*input) >= 0x20) output.push_back(*input);
  }
  return output;
}

esp_err_t send_error(httpd_req_t* request, const char* status,
                     const char* message) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_sendstr(request, message);
}

esp_err_t status_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request)) {
    return send_error(request, "401 Unauthorized", "Authentication required.");
  }
  const gate::ota::Status ota = gate::ota::status();
  const std::string response =
      "{\"version\":\"" + json_escape(ota.version) +
      "\",\"project\":\"" + json_escape(ota.project_name) +
      "\",\"runningPartition\":\"" + json_escape(ota.running_partition) +
      "\",\"updatePartition\":\"" + json_escape(ota.update_partition) +
      "\",\"phase\":\"" + gate::ota::phase_name(ota.phase) +
      "\",\"maximumImageSize\":" + std::to_string(ota.maximum_image_size) +
      ",\"totalBytes\":" + std::to_string(ota.total_bytes) +
      ",\"writtenBytes\":" + std::to_string(ota.written_bytes) +
      ",\"pendingVerification\":" +
      std::string(ota.pending_verification ? "true" : "false") +
      ",\"rollbackEnabled\":" +
      std::string(ota.rollback_enabled ? "true" : "false") +
      ",\"maintenance\":" +
      std::string(gate::runtime::maintenance_active() ? "true" : "false") +
      ",\"error\":\"" + json_escape(ota.error) + "\"}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t upload_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request, true)) {
    return send_error(request, "401 Unauthorized",
                      "Authentication and CSRF token required.");
  }
  if (api_context.config == nullptr ||
      !gate::web_auth::verify_reauthorization(request,
                                               api_context.config->admin)) {
    return send_error(request, "403 Forbidden",
                      "Administrator password is incorrect or missing.");
  }
  if (request->content_len <= 0) {
    return send_error(request, "400 Bad Request", "Firmware body is empty.");
  }
  char content_type[64]{};
  if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                  sizeof(content_type)) != ESP_OK ||
      std::strncmp(content_type, "application/octet-stream", 24) != 0) {
    return send_error(request, "415 Unsupported Media Type",
                      "Expected application/octet-stream.");
  }

  esp_err_t result = gate::ota::begin(request->content_len);
  if (result != ESP_OK) {
    return send_error(request,
                      result == ESP_ERR_INVALID_SIZE ? "413 Payload Too Large"
                                                    : "409 Conflict",
                      result == ESP_ERR_INVALID_STATE
                          ? "Gate must be stopped with no active relay pulse."
                          : esp_err_to_name(result));
  }

  constexpr std::size_t kBufferSize = 4096;
  auto* buffer = static_cast<char*>(heap_caps_malloc(kBufferSize, MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    gate::ota::abort("Could not allocate OTA receive buffer");
    return send_error(request, "503 Service Unavailable",
                      "Insufficient memory for firmware upload.");
  }
  std::size_t remaining = request->content_len;
  while (remaining > 0) {
    const int received =
        httpd_req_recv(request, buffer, std::min(remaining, kBufferSize));
    if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (received <= 0) {
      heap_caps_free(buffer);
      gate::ota::abort("Firmware upload connection was interrupted");
      return send_error(request, "400 Bad Request", "Upload was interrupted.");
    }
    result = gate::ota::write(buffer, static_cast<std::size_t>(received));
    if (result != ESP_OK) {
      heap_caps_free(buffer);
      gate::ota::abort("Firmware flash write failed");
      return send_error(request, "500 Internal Server Error",
                        "Could not write firmware.");
    }
    remaining -= static_cast<std::size_t>(received);
  }
  heap_caps_free(buffer);

  result = gate::ota::finish();
  if (result != ESP_OK) {
    return send_error(request, "422 Unprocessable Content",
                      gate::ota::status().error);
  }
  httpd_resp_set_type(request, "application/json");
  const esp_err_t response =
      httpd_resp_sendstr(request, "{\"accepted\":true,\"restarting\":true}");
  if (response == ESP_OK && api_context.schedule_restart != nullptr) {
    api_context.schedule_restart();
  }
  return response;
}

}  // namespace

esp_err_t register_routes(httpd_handle_t server, Context context) {
  if (server == nullptr || context.config == nullptr) return ESP_ERR_INVALID_ARG;
  api_context = context;
  const httpd_uri_t status{.uri = "/api/v1/system/firmware",
                           .method = HTTP_GET,
                           .handler = status_handler,
                           .user_ctx = nullptr};
  const httpd_uri_t upload{.uri = "/api/v1/system/firmware",
                           .method = HTTP_POST,
                           .handler = upload_handler,
                           .user_ctx = nullptr};
  esp_err_t result = httpd_register_uri_handler(server, &status);
  return result == ESP_OK ? httpd_register_uri_handler(server, &upload) : result;
}

}  // namespace gate::ota_api
