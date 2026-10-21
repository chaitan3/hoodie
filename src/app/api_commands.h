#pragma once

#include <string>

#include "app/config.h"

namespace hoodie::app {

struct ApiCommandOptions {
  std::string out_file;        // write TTS audio here instead of playing
  std::string output_device;   // playback device override
  std::string system_prompt;   // LLM system prompt
};

// Each returns a process exit code (0 = success).

// Transcribe a local audio file and print the transcript to stdout.
int run_transcribe(const Config& config, const ApiCommandOptions& options,
                   const std::string& audio_file);

// Send a text prompt to the LLM (no tools) and print the reply to stdout.
int run_ask(const Config& config, const ApiCommandOptions& options, const std::string& text);

// Run the tool-calling agent and print the final reply. Intentionally does not
// synthesize speech or play music; use --roundtrip for the audio pipeline.
int run_agent(const Config& config, const ApiCommandOptions& options, const std::string& text);

// Synthesise speech, then play it or write it to options.out_file.
int run_say(const Config& config, const ApiCommandOptions& options, const std::string& text);

// ASR -> agent -> TTS round trip on a local audio file.
int run_roundtrip(const Config& config, const ApiCommandOptions& options,
                  const std::string& audio_file);

// Invoke a single built-in tool directly (diagnostic) and print its output.
int run_tool(const Config& config, const std::string& name, const std::string& args_json);

}  // namespace hoodie::app
