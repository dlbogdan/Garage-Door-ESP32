#include "homespan_compatibility.hpp"

#include <atomic>

#include "Arduino.h"
#include "HomeSpan.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "gate_runtime.hpp"
#include "homekit_projection.hpp"

namespace gate::homekit {
namespace {

constexpr char kTag[] = "homekit";
constexpr std::uint16_t kHapPort = 1201;
std::atomic_bool homekit_active{false};
std::atomic_bool homekit_paired{false};

struct GarageDoorService : Service::GarageDoorOpener {
  Characteristic::CurrentDoorState* current;
  Characteristic::TargetDoorState* target;
  Characteristic::ObstructionDetected* obstruction;

  GarageDoorService()
      : current(new Characteristic::CurrentDoorState(
            Characteristic::CurrentDoorState::STOPPED)),
        target(new Characteristic::TargetDoorState(
            Characteristic::TargetDoorState::OPEN)),
        obstruction(new Characteristic::ObstructionDetected(false)) {}

  void publish_runtime_state() {
    const gate::runtime::Snapshot state = gate::runtime::snapshot();
    gate::controller::Snapshot controller_state;
    controller_state.state = state.state;
    controller_state.target = state.target;
    controller_state.movement = state.movement;
    controller_state.stable_observation = state.observation;
    controller_state.observation_valid = state.observation_valid;
    controller_state.pulse_active = state.pulse_active;
    controller_state.fault = state.fault;
    const Projection next = project(controller_state);
    if (current->getVal() != next.current) current->setVal(next.current);
    if (target->getVal() != next.target) target->setVal(next.target);
    if (obstruction->getVal() != next.obstruction) {
      obstruction->setVal(next.obstruction);
    }
  }

  boolean update() override {
    const gate::controller::Target requested =
        target->getNewVal() == Characteristic::TargetDoorState::OPEN
            ? gate::controller::Target::kOpen
            : gate::controller::Target::kClosed;
    const gate::runtime::RequestResult result =
        gate::runtime::request_target(requested);
    if (result != gate::runtime::RequestResult::kAccepted) {
      ESP_LOGW(kTag, "HomeKit target request rejected (%d)",
               static_cast<int>(result));
      return false;
    }
    // Publish the reducer result while handling the accepted write. In
    // particular, an opposite request while travelling must enqueue an
    // explicit STOPPED notification rather than relying only on a later poll.
    // Do not set TargetDoorState here: HomeSpan still owns the in-flight target
    // write and commits it after update() returns.
    const gate::runtime::Snapshot state = gate::runtime::snapshot();
    gate::controller::Snapshot controller_state;
    controller_state.state = state.state;
    controller_state.target = state.target;
    controller_state.movement = state.movement;
    controller_state.stable_observation = state.observation;
    controller_state.observation_valid = state.observation_valid;
    controller_state.pulse_active = state.pulse_active;
    controller_state.fault = state.fault;
    const Projection next = project(controller_state);
    if (current->getVal() != next.current) current->setVal(next.current);
    if (obstruction->getVal() != next.obstruction) {
      obstruction->setVal(next.obstruction);
    }
    return true;
  }

  void loop() override { publish_runtime_state(); }
};

void pairing_changed(boolean paired) {
  homekit_paired.store(paired);
  ESP_LOGI(kTag, "Apple Home pairing state: %s", paired ? "paired" : "unpaired");
}

}  // namespace

void initialize_networking() {
  initArduino();
  ESP_LOGI(kTag, "Arduino/HomeSpan networking initialized");
}

esp_err_t connect_bootstrap_station(const char* ssid, const char* password) {
  if (ssid == nullptr || ssid[0] == '\0' || password == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  if (homekit_active.load()) return ESP_ERR_INVALID_STATE;

  WiFi.setAutoReconnect(true);
  const wl_status_t status = WiFi.begin(ssid, password);
  if (status == WL_CONNECT_FAILED) {
    ESP_LOGE(kTag, "Could not start staged-onboarding station connection");
    return ESP_FAIL;
  }
  ESP_LOGI(kTag, "Arduino networking is connecting to staged-onboarding Wi-Fi");
  return ESP_OK;
}

esp_err_t start(const gate::config::AppConfig& config) {
  if (homekit_active.load()) return ESP_ERR_INVALID_STATE;
  if (!gate::runtime::active()) return ESP_ERR_INVALID_STATE;

  homeSpan.setLogLevel(1)
      .setSerialInputDisable(true)
      .setPortNum(kHapPort)
      .setWifiCredentials(config.wifi.ssid.c_str(), config.wifi.password.c_str())
      .setQRID(config.homekit.setup_id.c_str())
      .setPairingCode(config.homekit.setup_code.c_str())
      .setPairCallback(pairing_changed);
  homeSpan.begin(Category::GarageDoorOpeners, config.homekit.display_name.c_str(),
                 "garage-door", "ESP32-WROOM-32");

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name(config.homekit.display_name.c_str());
  new Characteristic::Manufacturer("Garage-Door-ESP32");
  new Characteristic::Model("ESP32-WROOM-32");
  new Characteristic::FirmwareRevision(esp_app_get_description()->version);
  new GarageDoorService();

  homeSpan.autoPoll(8192, 2, 1);
  homekit_active.store(true);
  ESP_LOGI(kTag, "HomeSpan Garage Door Opener started on HAP port %u",
           static_cast<unsigned>(kHapPort));
  return ESP_OK;
}

bool active() { return homekit_active.load(); }

bool paired() {
  if (homekit_paired.load()) return true;
  const HS_STATUS status = homeSpan.getStatus().first;
  return status == HS_PAIRED || status == HS_CONNECTED;
}

}  // namespace gate::homekit
