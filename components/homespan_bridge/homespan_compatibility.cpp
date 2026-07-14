#include "homespan_compatibility.hpp"

#include "HomeSpan.h"

namespace gate::homekit {

void build_garage_service_compatibility_graph() {
  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("Gate");
  new Characteristic::Manufacturer("Open Gate Controller");
  new Characteristic::Model("ESP32-WROOM-32");
  new Characteristic::FirmwareRevision("0.1.0");

  new Service::GarageDoorOpener();
  new Characteristic::CurrentDoorState(
      Characteristic::CurrentDoorState::CLOSED);
  new Characteristic::TargetDoorState(
      Characteristic::TargetDoorState::CLOSED);
  new Characteristic::ObstructionDetected(false);
}

}  // namespace gate::homekit
