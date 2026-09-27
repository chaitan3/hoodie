#include "agent/tool_registry.h"

#include <nlohmann/json.hpp>

namespace hoodie::agent {

std::string ToolRegistry::tools_json() const {
  nlohmann::json array = nlohmann::json::array();
  for (const Tool& tool : tools_) {
    array.push_back({{"type", "function"},
                     {"function",
                      {{"name", tool.name},
                       {"description", tool.description},
                       {"parameters", nlohmann::json::parse(tool.parameters_json)}}}});
  }
  return array.dump();
}

std::string ToolRegistry::invoke(const std::string& name, const std::string& arguments_json,
                                 ToolContext& context) const {
  for (const Tool& tool : tools_) {
    if (tool.name == name) return tool.invoke(arguments_json, context);
  }
  throw std::runtime_error("unknown tool: " + name);
}

}  // namespace hoodie::agent
