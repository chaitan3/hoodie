#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace hoodie::agent {

// A song requested by the play_music tool; the caller resolves/plays it after
// the agent turn completes.
struct MusicRequest {
  std::string id;
  std::string title;
};

// Mutable state shared between tools and the caller across one agent run.
struct ToolContext {
  std::optional<MusicRequest> music;
};

// A single callable tool exposed to the model via a JSON schema.
struct Tool {
  std::string name;
  std::string description;
  std::string parameters_json;  // JSON Schema object for the arguments
  std::function<std::string(const std::string& arguments_json, ToolContext&)> invoke;
};

class ToolRegistry {
 public:
  void add(Tool tool) { tools_.push_back(std::move(tool)); }
  bool empty() const { return tools_.empty(); }

  // OpenAI "tools" array JSON: [{"type":"function","function":{...}}, ...]
  std::string tools_json() const;

  // Invoke a tool by name. Throws std::runtime_error if the tool is unknown.
  std::string invoke(const std::string& name, const std::string& arguments_json,
                     ToolContext& context) const;

 private:
  std::vector<Tool> tools_;
};

}  // namespace hoodie::agent
