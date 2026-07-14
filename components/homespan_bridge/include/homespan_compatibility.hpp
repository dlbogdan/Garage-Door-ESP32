#pragma once

namespace gate::homekit {

// Constructs the exact HAP service graph without starting networking. Used as
// a compile-time compatibility gate until provisioned credentials are present.
void build_garage_service_compatibility_graph();

}  // namespace gate::homekit
