// XTE-DCDU: configuration loader
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"
#include "log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace xtedcdu {

namespace {

std::string trim(std::string s) {
    auto issp = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back()))  s.pop_back();
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool to_int(const std::string& v, int& out) {
    try {
        size_t p = 0;
        int x = std::stoi(v, &p);
        if (p == 0) return false;
        out = x;
        return true;
    } catch (...) { return false; }
}

bool to_double(const std::string& v, double& out) {
    try {
        size_t p = 0;
        double x = std::stod(v, &p);
        if (p == 0) return false;
        out = x;
        return true;
    } catch (...) { return false; }
}

bool to_float(const std::string& v, float& out) {
    double d;
    if (!to_double(v, d)) return false;
    out = static_cast<float>(d);
    return true;
}

void apply_kv(Config& c, const std::string& section,
              const std::string& key, const std::string& val) {
    auto sec = lower(section);
    auto k   = lower(key);
    auto vlow = lower(val);

    auto set_int = [&](int& dst) {
        int x;
        if (to_int(val, x)) dst = x;
        else XTED_ERR("config: invalid int for [%s] %s = '%s'", sec.c_str(), k.c_str(), val.c_str());
    };
    auto set_dbl = [&](double& dst) {
        double x;
        if (to_double(val, x)) dst = x;
        else XTED_ERR("config: invalid number for [%s] %s = '%s'", sec.c_str(), k.c_str(), val.c_str());
    };
    auto set_bool = [&](bool& dst) {
        if (vlow == "true" || vlow == "yes" || vlow == "on" || val == "1") dst = true;
        else if (vlow == "false" || vlow == "no" || vlow == "off" || val == "0") dst = false;
        else XTED_ERR("config: invalid bool for [%s] %s = '%s'", sec.c_str(), k.c_str(), val.c_str());
    };

    if (sec == "aircraft") {
        if      (k == "acf_prefix")   c.acf_prefix = val;
        else if (k == "panel_width")  set_int(c.panel_width);
        else if (k == "panel_height") set_int(c.panel_height);
        else XTED_VLOG("config: unknown key [aircraft] %s", k.c_str());
    } else if (sec == "window") {
        if      (k == "x1") set_int(c.x1);
        else if (k == "y1") set_int(c.y1);
        else if (k == "x2") set_int(c.x2);
        else if (k == "y2") set_int(c.y2);
        else XTED_VLOG("config: unknown key [window] %s", k.c_str());
    } else if (sec == "output") {
        if (k == "mode") {
            if (vlow == "client") c.mode = TcpMode::Client;
            else if (vlow == "server") c.mode = TcpMode::Server;
            else XTED_ERR("config: unknown mode '%s'", val.c_str());
        }
        else if (k == "esp32_host")          c.esp32_host = val;
        else if (k == "esp32_port")          set_int(c.esp32_port);
        else if (k == "listen_host")         c.listen_host = val;
        else if (k == "listen_port")         set_int(c.listen_port);
        else if (k == "reconnect_start_ms")  set_int(c.reconnect_start_ms);
        else if (k == "reconnect_max_ms")    set_int(c.reconnect_max_ms);
        else if (k == "reconnect_factor")    set_dbl(c.reconnect_factor);
        else XTED_VLOG("config: unknown key [output] %s", k.c_str());
    } else if (sec == "encoding") {
        if      (k == "jpeg_quality")          set_int(c.jpeg_quality);
        else if (k == "raw_rgb565")            set_bool(c.raw_rgb565);
        else if (k == "heartbeat_ms")          set_int(c.heartbeat_ms);
        else if (k == "min_send_interval_ms")  set_int(c.min_send_interval_ms);
        else XTED_VLOG("config: unknown key [encoding] %s", k.c_str());
    } else if (sec == "readback") {
        if      (k == "pbo_count")            set_int(c.pbo_count);
        else if (k == "flight_loop_interval") set_dbl(c.flight_loop_interval);
        else if (k == "start_texture_id")     set_int(c.start_texture_id);
        else if (k == "texture_scan_max_id")  set_int(c.texture_scan_max_id);
        else XTED_VLOG("config: unknown key [readback] %s", k.c_str());
    } else if (sec == "logging") {
        if (k == "level") c.log_level = vlow;
        else XTED_VLOG("config: unknown key [logging] %s", k.c_str());
    } else if (sec.size() > 7 && sec.compare(0, 7, "button.") == 0) {
        // [button.0] .. [button.10]
        int idx = -1;
        try {
            size_t end = 0;
            idx = std::stoi(sec.substr(7), &end);
            if (end == 0) idx = -1;
        } catch (...) { idx = -1; }
        if (idx < 0 || idx >= kButtonCount) {
            XTED_VLOG("config: unknown section [%s]", sec.c_str());
        } else {
            ButtonConfig& bc = c.buttons[(size_t)idx];
            auto set_flt = [&](float& dst) {
                float x;
                if (to_float(val, x)) dst = x;
                else XTED_ERR("config: invalid float for [%s] %s = '%s'",
                              sec.c_str(), k.c_str(), val.c_str());
            };
            if (k == "type") {
                if      (vlow == "command") bc.action = ButtonAction::Command;
                else if (vlow == "dataref") bc.action = ButtonAction::Dataref;
                else if (vlow == "none")    bc.action = ButtonAction::None;
                else XTED_ERR("config: unknown button type '%s' in [%s]",
                              val.c_str(), sec.c_str());
            } else if (k == "command") {
                bc.command_name = val;
            } else if (k == "dataref") {
                bc.dataref_name = val;
            } else if (k == "value_press") {
                set_flt(bc.value_press);
            } else if (k == "value_release") {
                set_flt(bc.value_release);
            } else if (k == "send_release") {
                bc.send_release = (vlow == "true" || vlow == "1" || vlow == "yes");
            } else {
                XTED_VLOG("config: unknown key [%s] %s", sec.c_str(), k.c_str());
            }
        }
    } else {
        XTED_VLOG("config: unknown section [%s]", sec.c_str());
    }
}

} // namespace

bool load_config(const std::string& path, Config& out) {
    std::ifstream f(path);
    if (!f.is_open()) {
        XTED_LOG("config: file not found at '%s'", path.c_str());
        return false;
    }

    std::string line, section;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        // strip comments
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '#' || line[i] == ';') { line.resize(i); break; }
        }
        std::string t = trim(line);
        if (t.empty()) continue;

        if (t.front() == '[' && t.back() == ']') {
            section = trim(t.substr(1, t.size() - 2));
            continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos) {
            XTED_ERR("config: line %d: missing '=' in '%s'", lineno, t.c_str());
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        apply_kv(out, section, key, val);
    }

    apply_log_level(out);
    XTED_LOG("config: loaded from '%s'", path.c_str());
    return true;
}

bool write_default_config(const std::string& path) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f <<
"# xte-dcdu.cfg - configuration for XTE-DCDU headless plugin\n"
"\n"
"[aircraft]\n"
"acf_prefix   = a320\n"
"panel_width  = 4096\n"
"panel_height = 4096\n"
"\n"
"[window]\n"
"x1 = 1534\n"
"y1 = 2867\n"
"x2 = 1803\n"
"y2 = 3068\n"
"\n"
"[output]\n"
"mode       = client\n"
"esp32_host = 192.168.1.50\n"
"esp32_port = 4711\n"
"listen_host = 0.0.0.0\n"
"listen_port = 52501\n"
"reconnect_start_ms = 1000\n"
"reconnect_max_ms   = 30000\n"
"reconnect_factor   = 2.0\n"
"\n"
"[encoding]\n"
"jpeg_quality         = 100\n"
"raw_rgb565           = true\n"
"heartbeat_ms         = 2000\n"
"min_send_interval_ms = 100\n"
"\n"
"[readback]\n"
"pbo_count            = 2\n"
"flight_loop_interval = 0.05\n"
"start_texture_id     = 0\n"
"texture_scan_max_id  = 10000\n"
"\n"
"[logging]\n"
"level = normal\n";
    return true;
}

void apply_log_level(const Config& cfg) {
    if      (cfg.log_level == "quiet")   current_log_level() = LogLevel::Quiet;
    else if (cfg.log_level == "verbose") current_log_level() = LogLevel::Verbose;
    else                                 current_log_level() = LogLevel::Normal;
}

bool save_endpoint_to_config(const std::string& path,
                             const std::string& host,
                             int port) {
    // Read the existing file (if any) into memory so we can preserve comments,
    // formatting, and any keys we don't touch.
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        if (in.is_open()) {
            std::string line;
            while (std::getline(in, line)) lines.push_back(line);
        }
    }

    // Walk the file, tracking the current section. Replace the value portion of
    // esp32_host / esp32_port when they appear inside [output]; preserve
    // surrounding whitespace and inline comments.
    bool in_output = false;
    bool host_done = false, port_done = false;
    int  output_end_idx = -1;     // last line index belonging to [output]
    int  output_header_idx = -1;  // index of the [output] header itself

    auto detect_section = [](const std::string& s) -> std::string {
        std::string t = trim(s);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
            return lower(trim(t.substr(1, t.size() - 2)));
        }
        return {};
    };

    auto patch_value = [](const std::string& line, const std::string& new_val) -> std::string {
        // Replace the text between '=' and the start of any '#' / ';' comment.
        auto eq = line.find('=');
        if (eq == std::string::npos) return line;
        size_t comment = std::string::npos;
        for (size_t i = eq + 1; i < line.size(); ++i) {
            if (line[i] == '#' || line[i] == ';') { comment = i; break; }
        }
        std::string before = line.substr(0, eq + 1);
        std::string after  = (comment == std::string::npos) ? std::string{}
                                                            : line.substr(comment);
        // Keep a single space after '=' for readability and a space before any
        // preserved trailing comment.
        std::string out = before + ' ' + new_val;
        if (!after.empty()) out += ' ' + after;
        return out;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& raw = lines[i];
        std::string body = raw;
        // Strip comments for key detection without modifying the stored line.
        for (size_t j = 0; j < body.size(); ++j) {
            if (body[j] == '#' || body[j] == ';') { body.resize(j); break; }
        }
        std::string t = trim(body);
        if (t.empty()) continue;

        std::string sec = detect_section(raw);
        if (!sec.empty()) {
            in_output = (sec == "output");
            if (in_output) output_header_idx = (int)i;
            continue;
        }

        if (in_output) {
            output_end_idx = (int)i;
            auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string key = lower(trim(t.substr(0, eq)));
            if (key == "esp32_host" && !host_done) {
                lines[i] = patch_value(raw, host);
                host_done = true;
            } else if (key == "esp32_port" && !port_done) {
                lines[i] = patch_value(raw, std::to_string(port));
                port_done = true;
            }
        }
    }

    // Append missing pieces.
    if (output_header_idx < 0) {
        // No [output] section at all — append a complete one.
        if (!lines.empty() && !lines.back().empty()) lines.push_back("");
        lines.push_back("[output]");
        if (!host_done) { lines.push_back("esp32_host = " + host); host_done = true; }
        if (!port_done) { lines.push_back("esp32_port = " + std::to_string(port)); port_done = true; }
    } else if (!host_done || !port_done) {
        // Section exists but one or both keys are missing. Insert immediately
        // after the last line of [output] so the keys live with their section.
        int insert_at = (output_end_idx >= 0) ? output_end_idx + 1
                                              : output_header_idx + 1;
        std::vector<std::string> to_insert;
        if (!host_done) to_insert.push_back("esp32_host = " + host);
        if (!port_done) to_insert.push_back("esp32_port = " + std::to_string(port));
        lines.insert(lines.begin() + insert_at, to_insert.begin(), to_insert.end());
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        XTED_ERR("config: cannot write '%s'", path.c_str());
        return false;
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        // Preserve trailing newlines; always end the file with one.
        if (i + 1 < lines.size() || !lines[i].empty()) out << '\n';
    }
    return true;
}

} // namespace xtedcdu
