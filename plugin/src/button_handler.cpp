// XTE-DCDU: dispatch button events from the ESP32 to X-Plane datarefs/commands
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later

#include "button_handler.hpp"
#include "log.hpp"

#include "XPLMDataAccess.h"
#include "XPLMUtilities.h"

#include <array>
#include <string>

namespace xtedcdu {

namespace {

// Lazily resolved handles. Invalidated by reset_button_cache().
std::array<XPLMDataRef,    kButtonCount> s_datarefs{};
std::array<XPLMCommandRef, kButtonCount> s_commands{};

// Track the names used to resolve each handle so we re-resolve on name change.
std::array<std::string, kButtonCount> s_dr_name{};
std::array<std::string, kButtonCount> s_cmd_name{};

XPLMDataRef resolve_dataref(int idx, const std::string& name) {
    if (s_dr_name[idx] == name) return s_datarefs[idx];
    s_dr_name[idx]  = name;
    s_datarefs[idx] = name.empty() ? nullptr : XPLMFindDataRef(name.c_str());
    if (!name.empty() && !s_datarefs[idx]) {
        XTED_ERR("button %d: dataref '%s' not found in sim", idx, name.c_str());
    }
    return s_datarefs[idx];
}

XPLMCommandRef resolve_command(int idx, const std::string& name) {
    if (s_cmd_name[idx] == name) return s_commands[idx];
    s_cmd_name[idx]  = name;
    s_commands[idx]  = name.empty() ? nullptr : XPLMFindCommand(name.c_str());
    if (!name.empty() && !s_commands[idx]) {
        XTED_ERR("button %d: command '%s' not found in sim", idx, name.c_str());
    }
    return s_commands[idx];
}

} // namespace

void dispatch_button_event(const Config& cfg, uint8_t button_id, uint8_t state) {
    if (button_id >= kButtonCount) {
        XTED_ERR("button: id %u out of range (max %d)", button_id, kButtonCount - 1);
        return;
    }

    const ButtonConfig& bc = cfg.buttons[button_id];

    if (bc.action == ButtonAction::None) {
        XTED_VLOG("button %u: not mapped (state=%u)", button_id, state);
        return;
    }

    if (bc.action == ButtonAction::Command) {
        if (state != 1) return;  // fire only on press
        XPLMCommandRef cmd = resolve_command((int)button_id, bc.command_name);
        if (!cmd) return;
        XPLMCommandOnce(cmd);
        XTED_VLOG("button %u: command '%s' fired", button_id, bc.command_name.c_str());
        return;
    }

    if (bc.action == ButtonAction::Dataref) {
        if (state == 0 && !bc.send_release) return;
        XPLMDataRef dr = resolve_dataref((int)button_id, bc.dataref_name);
        if (!dr) return;

        float val = (state == 1) ? bc.value_press : bc.value_release;

        XPLMDataTypeID types = XPLMGetDataRefTypes(dr);
        if (types & xplmType_Int) {
            XPLMSetDatai(dr, static_cast<int>(val));
        } else if (types & xplmType_Float) {
            XPLMSetDataf(dr, val);
        } else {
            XTED_ERR("button %u: dataref '%s' is not a writable numeric type",
                     button_id, bc.dataref_name.c_str());
            return;
        }
        XTED_VLOG("button %u: '%s' = %.2f (state=%u)",
                  button_id, bc.dataref_name.c_str(), val, state);
    }
}

void reset_button_cache() {
    for (int i = 0; i < kButtonCount; ++i) {
        s_datarefs[i] = nullptr;
        s_commands[i] = nullptr;
        s_dr_name[i].clear();
        s_cmd_name[i].clear();
    }
    XTED_VLOG("button: handle cache cleared");
}

} // namespace xtedcdu
