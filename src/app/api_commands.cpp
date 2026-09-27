#include "app/api_commands.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "agent/agent.h"
#include "agent/text_normalize.h"
#include "agent/tools.h"
#include "api/clients.h"
#include "api/openai_types.h"
#include "audio/player.h"
#include "audio/wav.h"
#include "net/http.h"
#include "util/process.h"

namespace hoodie::app {
namespace {

namespace fs = std::filesystem;

constexpr const char* kDefaultSystemPrompt =
    "You are a helpful assistant. Reply in one short sentence.";

void play_frame(const audio::AudioFrame& frame, const std::string& device) {
  audio::AudioPlayer::Options player_options;
  player_options.device = device;
  player_options.sample_rate = 0;  // device default
  player_options.channels = 1;

  audio::AudioPlayer player(player_options);
  player.start();
  player.enqueue(frame);
  const bool drained = player.wait_drained();
  player.stop();
  if (!drained) spdlog::warn("playback did not drain within timeout");
}

void play_or_write(const audio::AudioFrame& frame, const ApiCommandOptions& options) {
  if (!options.out_file.empty()) {
    audio::write_audio_file(options.out_file, frame);
    spdlog::info("wrote {} ({:.2f} s, {} Hz, {} ch)", options.out_file, frame.duration_seconds(),
                 frame.sample_rate, frame.channels);
    return;
  }
  play_frame(frame, options.output_device);
}

struct ApiContext {
  net::HttpClient http;
  api::ClientSet clients;

  explicit ApiContext(const Config& config) : clients(api::make_clients(config, http)) {}
};

std::string chat_once(ApiContext& ctx, const ApiCommandOptions& options, const std::string& prompt) {
  api::ChatRequest request;
  request.model = "";  // use client default
  request.messages.push_back(api::Message::system(
      options.system_prompt.empty() ? kDefaultSystemPrompt : options.system_prompt));
  request.messages.push_back(api::Message::user(prompt));

  const api::ChatResult result = ctx.clients.llm->chat(request);
  return result.message.content;
}

void play_music_request(const agent::MusicRequest& music, const ApiCommandOptions& options) {
  if (music.id.empty()) return;

  std::error_code error;
  const fs::path directory = fs::temp_directory_path(error) / "hoodie_music";
  fs::create_directories(directory, error);
  const fs::path base = directory / "out";

  const util::ProcessResult result = util::run_process(
      {"yt-dlp", "--force-overwrite", "-f", "ba", "-x", "--audio-format", "vorbis", "-o",
       base.string(), music.id});
  if (result.exit_code != 0) {
    spdlog::warn("music: download failed (exit {}): {}", result.exit_code, result.output);
    return;
  }

  fs::path ogg = base;
  ogg += ".ogg";
  if (!fs::exists(ogg, error)) {
    spdlog::warn("music: expected {} but it was not produced", ogg.string());
    return;
  }
  spdlog::info("music: playing '{}' from {}", music.title, ogg.string());
  play_frame(audio::read_audio_file(ogg.string()), options.output_device);
}

}  // namespace

int run_transcribe(const Config& config, const ApiCommandOptions& /*options*/,
                   const std::string& audio_file) {
  ApiContext ctx(config);
  const audio::AudioFrame frame = audio::read_audio_file(audio_file);
  const std::string text = ctx.clients.asr->transcribe(frame);
  std::cout << text << "\n";
  return 0;
}

int run_ask(const Config& config, const ApiCommandOptions& options, const std::string& text) {
  ApiContext ctx(config);
  std::cout << chat_once(ctx, options, text) << "\n";
  return 0;
}

int run_agent(const Config& config, const ApiCommandOptions& options, const std::string& text) {
  ApiContext ctx(config);
  agent::ToolRegistry tools = agent::builtin_tools(ctx.http, config);
  agent::Agent agent(*ctx.clients.llm, std::move(tools),
                     agent::AgentOptions{
                         options.system_prompt.empty() ? kDefaultSystemPrompt : options.system_prompt,
                         /*model=*/"", /*max_steps=*/6});

  agent::ToolContext tool_context;
  const agent::Agent::Result result = agent.run(text, tool_context);
  std::cout << result.text << "\n";
  spdlog::info("agent: completed in {} step(s)", result.steps);

  const audio::AudioFrame speech = ctx.clients.tts->synthesize(agent::normalize_for_tts(result.text));
  play_or_write(speech, options);

  if (tool_context.music.has_value()) {
    play_music_request(*tool_context.music, options);
  }
  return 0;
}

int run_say(const Config& config, const ApiCommandOptions& options, const std::string& text) {
  ApiContext ctx(config);
  const audio::AudioFrame audio = ctx.clients.tts->synthesize(agent::normalize_for_tts(text));
  play_or_write(audio, options);
  return 0;
}

int run_roundtrip(const Config& config, const ApiCommandOptions& options,
                  const std::string& audio_file) {
  ApiContext ctx(config);

  const audio::AudioFrame frame = audio::read_audio_file(audio_file);
  const std::string transcript = ctx.clients.asr->transcribe(frame);
  spdlog::info("transcript: {}", transcript);
  std::cout << transcript << "\n";

  agent::ToolRegistry tools = agent::builtin_tools(ctx.http, config);
  agent::Agent agent(*ctx.clients.llm, std::move(tools),
                     agent::AgentOptions{
                         options.system_prompt.empty() ? kDefaultSystemPrompt : options.system_prompt,
                         /*model=*/"", /*max_steps=*/6});
  agent::ToolContext tool_context;
  const agent::Agent::Result result = agent.run(transcript, tool_context);
  spdlog::info("reply: {}", result.text);
  std::cout << result.text << "\n";

  const audio::AudioFrame speech = ctx.clients.tts->synthesize(agent::normalize_for_tts(result.text));
  play_or_write(speech, options);

  if (tool_context.music.has_value()) {
    play_music_request(*tool_context.music, options);
  }
  return 0;
}

int run_tool(const Config& config, const std::string& name, const std::string& args_json) {
  net::HttpClient http;
  agent::ToolRegistry tools = agent::builtin_tools(http, config);
  agent::ToolContext context;
  try {
    std::cout << tools.invoke(name, args_json.empty() ? "{}" : args_json, context) << "\n";
  } catch (const std::exception& e) {
    spdlog::error("tool '{}': {}", name, e.what());
    return 1;
  }
  if (context.music.has_value()) {
    spdlog::info("music requested: '{}' (id {})", context.music->title, context.music->id);
  }
  return 0;
}

}  // namespace hoodie::app
