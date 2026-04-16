#include <flutter_linux/flutter_linux.h>

#include "include/omni_ble/omni_ble_plugin.h"

// This file exposes some plugin internals for unit testing. See
// https://github.com/flutter/flutter/issues/88724 for current limitations
// in the unit-testable API.

// Handles the getCapabilities method call.
FlMethodResponse *get_capabilities();
