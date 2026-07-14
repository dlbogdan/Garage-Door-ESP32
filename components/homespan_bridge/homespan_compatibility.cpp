#include "homespan_compatibility.hpp"

#include <atomic>

#include "Arduino.h"
#include "HomeSpan.h"
#include "esp_log.h"
#include "gate_runtime.hpp"

namespace gate::homekit {
namespace {

constexpr char kTag[] = "homekit";
constexpr std::uint16_t kHapPort = 1201;
std::atomic_bool homekit_active{false};
std::atomic_bool homekit_paired{false};

int homekit_current_state(gate::controller::State state) {
  using gate::controller::State;
  switch (state) {
    case State::kClosed: return Characteristic::CurrentDoorState::CLOSED;
    case State::kOpening: return Characteristic::CurrentDoorState::OPENING;
    case State::kOpen: return Characteristic::CurrentDoorState::OPEN;
    case State::kClosing: return Characteristic::CurrentDoorState::CLOSING;
    case State::kStoppedOpening:
    case State::kStoppedClosing:
    case State::kUnknownStopped:
      return Characteristic::CurrentDoorState::STOPPED;
  }
  return Characteristic::CurrentDoorState::STOPPED;
}

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
    return true;
  }

  void loop() override {
    const gate::runtime::Snapshot state = gate::runtime::snapshot();
    const int next_current = homekit_current_state(state.state);
    const int next_target =
        state.target == gate::controller::Target::kOpen
            ? Characteristic::TargetDoorState::OPEN
            : Characteristic::TargetDoorState::CLOSED;
    if (current->getVal() != next_current) current->setVal(next_current);
    if (target->getVal() != next_target) target->setVal(next_target);
    if (obstruction->getVal() != state.obstruction) {
      obstruction->setVal(state.obstruction);
    }
  }
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
  new Characteristic::FirmwareRevision("0.2.0");
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
