#pragma once

#include "agent/tool_registry.h"
#include "app/config.h"
#include "net/http.h"

namespace hoodie::agent {

// Build the built-in tool set: get_time, get_weather, play_music, search.
// `http` is used by the network-backed tools and must outlive the registry.
ToolRegistry builtin_tools(net::HttpClient& http, const Config& config);

}  // namespace hoodie::agent
