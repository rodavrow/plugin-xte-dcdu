// XTE-DCDU: dispatch button events from the ESP32 to X-Plane datarefs/commands
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include <cstdint>

namespace xtedcdu {

// Dispatch a single button event received from the ESP32.
//
// MUST be called from the X-Plane main thread (e.g. inside the flight-loop
// callback). Writing to datarefs or firing commands from any other thread is
// undefined behaviour in the XPLM API.
//
// For ButtonAction::Command  — fires the mapped X-Plane command on press
//                              (state == 1). Release events are ignored.
// For ButtonAction::Dataref  — writes value_press on press, value_release on
//                              release (if send_release == true). Supports
//                              integer and float datarefs; the value is cast
//                              to the dataref's native type.
// For ButtonAction::None     — logged at verbose level and silently ignored.
//
// DataRef and command handles are resolved lazily on first use and cached per
// button index. Call reset_button_cache() whenever the config is reloaded so
// that stale handles are discarded.
void dispatch_button_event(const Config& cfg, uint8_t button_id, uint8_t state);

// Invalidate all cached XPLMDataRef / XPLMCommandRef handles. Call whenever
// the plugin config is reloaded or the aircraft changes.
void reset_button_cache();

} // namespace xtedcdu
