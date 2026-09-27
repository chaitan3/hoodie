#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace hoodie::audio {

struct DeviceInfo {
  int index = -1;
  std::string name;
  std::string host_api;  // PortAudio host API name, e.g. "ALSA", "JACK", "pulse"
  int max_input_channels = 0;
  int max_output_channels = 0;
  double default_sample_rate = 0.0;
  bool is_default_input = false;
  bool is_default_output = false;
};

// Initialise PortAudio once (idempotent, thread-safe).
void ensure_portaudio_initialized();

std::vector<DeviceInfo> list_devices();

void print_devices(std::ostream& os);

// Resolve a device specification to a PortAudio device index:
//   ""      -> system default for the requested direction
//   "3"     -> device index 3
//   "name"  -> case-insensitive substring match on the device name
// Returns std::nullopt and sets `error` if nothing matches.
std::optional<int> resolve_device(const std::string& spec, bool want_input, std::string* error);

}  // namespace hoodie::audio
