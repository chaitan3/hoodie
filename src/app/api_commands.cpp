#include "app/api_commands.h"

#include <spdlog/spdlog.h>

#include <iostream>
#include <string>
#include <vector>

#include "agent/agent.h"
#include "agent/text_normalize.h"
#include "agent/tools.h"
#include "api/clients.h"
#include "api/openai_types.h"
#include "app/playback.h"
#include "audio/wav.h"
#include "net/http.h"

namespace hoodie::app {
namespace {

constexpr const char* kDefaultSystemPrompt =
    "You are a helpful assistant. Reply in one short sentence.";

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

  const api::ChatResult result = api::chat_blocking(*ctx.clients.llm, request);
  return result.message.content;
}

}  // namespace

int run_transcribe(const Config& config, const ApiCommandOptions& /*options*/,
                   const std::string& audio_file) {
  ApiContext ctx(config);
  const audio::AudioFrame frame = audio::read_audio_file(audio_file);
  const std::string text = api::transcribe_blocking(*ctx.clients.asr, frame);
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

  const audio::AudioFrame speech =
      api::synthesize_blocking(*ctx.clients.tts, agent::normalize_for_tts(result.text));
  play_or_write(speech, options.out_file, options.output_device);

  if (tool_context.music.has_value()) {
    play_music_request(*tool_context.music, options.out_file, options.output_device);
  }
  return 0;
}

int run_say(const Config& config, const ApiCommandOptions& options, const std::string& text) {
  ApiContext ctx(config);
  const audio::AudioFrame audio =
      api::synthesize_blocking(*ctx.clients.tts, agent::normalize_for_tts(text));
  play_or_write(audio, options.out_file, options.output_device);
  return 0;
}

int run_roundtrip(const Config& config, const ApiCommandOptions& options,
                  const std::string& audio_file) {
  ApiContext ctx(config);

  const audio::AudioFrame frame = audio::read_audio_file(audio_file);
  const std::string transcript = api::transcribe_blocking(*ctx.clients.asr, frame);
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

  const audio::AudioFrame speech =
      api::synthesize_blocking(*ctx.clients.tts, agent::normalize_for_tts(result.text));
  play_or_write(speech, options.out_file, options.output_device);

  if (tool_context.music.has_value()) {
    play_music_request(*tool_context.music, options.out_file, options.output_device);
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
