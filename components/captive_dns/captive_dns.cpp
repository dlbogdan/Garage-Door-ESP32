#include "captive_dns.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace gate::captive_dns {
namespace {

constexpr char kTag[] = "captive_dns";
std::atomic_bool running{false};
TaskHandle_t task_handle = nullptr;
int socket_fd = -1;

void dns_task(void*) {
  socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0) {
    ESP_LOGE(kTag, "Could not create socket");
    running.store(false);
    task_handle = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(53);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    ESP_LOGE(kTag, "Could not bind socket");
    close(socket_fd);
    socket_fd = -1;
    running.store(false);
    task_handle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  std::array<std::uint8_t, 512> packet{};
  while (running.load()) {
    sockaddr_in client{};
    socklen_t client_length = sizeof(client);
    const int length = recvfrom(socket_fd, packet.data(), packet.size(), 0,
                                reinterpret_cast<sockaddr*>(&client),
                                &client_length);
    if (!running.load()) break;
    if (length < 12) continue;

    std::size_t question_end = 12;
    while (question_end < static_cast<std::size_t>(length) &&
           packet[question_end] != 0) {
      const std::size_t label_length = packet[question_end];
      if (label_length > 63 || question_end + label_length + 1 >=
                                   static_cast<std::size_t>(length)) {
        question_end = packet.size();
        break;
      }
      question_end += label_length + 1;
    }
    if (question_end + 5 > static_cast<std::size_t>(length)) continue;
    question_end += 5;
    if (question_end + 16 > packet.size()) continue;

    packet[2] = 0x81;
    packet[3] = 0x80;
    packet[6] = 0;
    packet[7] = 1;
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    std::size_t response_length = question_end;
    const std::uint8_t answer[] = {
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x3C, 0x00, 0x04, 192,  168,  4,    1};
    std::memcpy(packet.data() + response_length, answer, sizeof(answer));
    response_length += sizeof(answer);
    sendto(socket_fd, packet.data(), response_length, 0,
           reinterpret_cast<sockaddr*>(&client), client_length);
  }

  if (socket_fd >= 0) close(socket_fd);
  socket_fd = -1;
  task_handle = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

esp_err_t start() {
  if (running.exchange(true)) return ESP_ERR_INVALID_STATE;
  if (xTaskCreate(dns_task, "captive_dns", 3072, nullptr, 4, &task_handle) !=
      pdPASS) {
    running.store(false);
    task_handle = nullptr;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void stop() {
  if (!running.exchange(false)) return;
  if (socket_fd >= 0) shutdown(socket_fd, SHUT_RDWR);
}

bool active() { return running.load(); }

}  // namespace gate::captive_dns
