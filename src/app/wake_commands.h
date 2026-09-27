#pragma once

#include <string>

#include "app/config.h"

namespace hoodie::app {

// Run the wake-word detector over a local audio file and print a summary to
// stdout, e.g. "alexa: max=0.93 at=1.28s detected=yes". `threshold_override`
// <= 0 uses the configured threshold. Returns 0 regardless of detection.
int run_wakeword_check(const Config& config, const std::string& audio_file,
                       float threshold_override);

// Run Silero VAD over a local audio file and print frame statistics and
// detected speech segments. `threshold_override` <= 0 uses 0.5.
int run_vad_check(const Config& config, const std::string& audio_file, float threshold_override);

// Run VAD + endpointer over a local audio file and print the resulting speech
// segments (start/end in seconds).
int run_endpoint_check(const Config& config, const std::string& audio_file);

// Diagnostic: run the melspectrogram model on constant input three times to
// check ONNX Runtime determinism.
int run_onnx_probe(const Config& config);

}  // namespace hoodie::app
