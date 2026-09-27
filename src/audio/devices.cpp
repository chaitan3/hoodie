#include "audio/devices.h"

#include <portaudio.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>

namespace hoodie::audio {
namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool is_number(const std::string& s) {
  return !s.empty() &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

}  // namespace

void ensure_portaudio_initialized() {
  static std::once_flag once;
  std::call_once(once, []() {
    const PaError err = Pa_Initialize();
    if (err != paNoError) {
      throw std::runtime_error(std::string("Pa_Initialize failed: ") + Pa_GetErrorText(err));
    }
    std::atexit([]() { Pa_Terminate(); });
  });
}

std::vector<DeviceInfo> list_devices() {
  ensure_portaudio_initialized();

  std::vector<DeviceInfo> devices;
  const PaDeviceIndex count = Pa_GetDeviceCount();
  if (count < 0) return devices;

  const PaDeviceIndex default_in = Pa_GetDefaultInputDevice();
  const PaDeviceIndex default_out = Pa_GetDefaultOutputDevice();

  devices.reserve(static_cast<std::size_t>(count));
  for (PaDeviceIndex i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (info == nullptr) continue;
    DeviceInfo device;
    device.index = i;
    device.name = info->name != nullptr ? info->name : "<unnamed>";
    const PaHostApiInfo* host = Pa_GetHostApiInfo(info->hostApi);
    device.host_api = (host != nullptr && host->name != nullptr) ? host->name : "?";
    device.max_input_channels = info->maxInputChannels;
    device.max_output_channels = info->maxOutputChannels;
    device.default_sample_rate = info->defaultSampleRate;
    device.is_default_input = (i == default_in);
    device.is_default_output = (i == default_out);
    devices.push_back(std::move(device));
  }
  return devices;
}

void print_devices(std::ostream& os) {
  ensure_portaudio_initialized();

  // Host APIs that this PortAudio build actually compiled in (before any device
  // filtering). If a sound server is in use, expect one named "pulse"/"pipewire"
  // or an ALSA PCM whose name is "pulse"/"piped" below.
  os << "host APIs:";
  const PaHostApiIndex host_count = Pa_GetHostApiCount();
  for (PaHostApiIndex h = 0; h < host_count; ++h) {
    const PaHostApiInfo* host = Pa_GetHostApiInfo(h);
    if (host != nullptr && host->name != nullptr) os << " " << host->name;
  }
  os << "\n";

  const std::vector<DeviceInfo> devices = list_devices();
  if (devices.empty()) {
    os << "no audio devices found\n";
    return;
  }
  for (const DeviceInfo& d : devices) {
    // Flag devices that reach a sound server (PulseAudio/PipeWire), either as a
    // native host API or via the ALSA pulse/pipewire plugin.
    const std::string lower_name = to_lower(d.name);
    const std::string lower_host = to_lower(d.host_api);
    const bool server_routed =
        lower_host.find("pulse") != std::string::npos ||
        lower_name.find("pulse") != std::string::npos ||
        lower_name.find("pipewire") != std::string::npos;

    std::string role;
    if (d.is_default_input) role += " default-in";
    if (d.is_default_output) role += " default-out";
    if (server_routed) role += " [pulse/pipewire]";
    os << "[" << d.index << "] " << d.name << "\n"
       << "      in=" << d.max_input_channels << " out=" << d.max_output_channels
       << " default_rate=" << static_cast<int>(d.default_sample_rate)
       << " host=" << d.host_api << role << "\n";
  }
}

std::optional<int> resolve_device(const std::string& spec, bool want_input, std::string* error) {
  ensure_portaudio_initialized();

  const auto fail = [&](const std::string& message) -> std::optional<int> {
    if (error != nullptr) *error = message;
    return std::nullopt;
  };

  const std::vector<DeviceInfo> devices = list_devices();
  if (devices.empty()) return fail("no audio devices available");

  if (spec.empty()) {
    const PaDeviceIndex index = want_input ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
    if (index == paNoDevice) {
      return fail(want_input ? "no default input device" : "no default output device");
    }
    return static_cast<int>(index);
  }

  if (is_number(spec)) {
    const int index = std::atoi(spec.c_str());
    if (index < 0 || index >= static_cast<int>(devices.size())) {
      return fail("device index out of range: " + spec);
    }
    return index;
  }

  const std::string needle = to_lower(spec);
  std::optional<int> match;
  bool match_is_default = false;
  for (const DeviceInfo& d : devices) {
    const bool usable = want_input ? d.max_input_channels > 0 : d.max_output_channels > 0;
    if (!usable) continue;
    if (to_lower(d.name).find(needle) == std::string::npos) continue;

    const bool is_default = want_input ? d.is_default_input : d.is_default_output;
    if (!match.has_value() || (is_default && !match_is_default)) {
      match = d.index;
      match_is_default = is_default;
    }
  }
  if (!match.has_value()) {
    return fail("no " + std::string(want_input ? "input" : "output") +
                " device matching '" + spec + "'");
  }
  return match;
}

}  // namespace hoodie::audio
