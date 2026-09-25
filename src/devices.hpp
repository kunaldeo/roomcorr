// Small helpers over `pactl` (PipeWire's pulse compatibility layer) for the
// one-off device chores: finding the mic, switching the X4 to 5.1, volumes.
#pragma once

#include <optional>
#include <string>

#include "config.hpp"
#include "json.hpp"

namespace rc {

std::string shell_quote(const std::string& s);
std::string run_capture(const std::string& cmd);  // stdout of a shell command
int run_cmd(const std::string& cmd);              // exit status

Json pactl_list(const std::string& what);  // "sinks" | "sources" | "cards"

std::optional<std::string> find_mic(const Config& cfg);   // source node.name
std::optional<std::string> find_x4_card();                // card name
bool sink_exists(const std::string& name);
double sink_volume(const std::string& name);  // 0..1 (first channel)
void set_sink_volume(const std::string& name, double fraction);

// Searches ~/Downloads and the config dir for a UMIK-1 90° calibration file.
std::optional<std::string> find_mic_cal_file();

}  // namespace rc
