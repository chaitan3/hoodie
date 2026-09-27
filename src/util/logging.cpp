#include "util/logging.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace hoodie::util {
namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

spdlog::level::level_enum parse_level(const std::string& raw) {
  const std::string level = to_lower(raw);
  if (level == "trace") return spdlog::level::trace;
  if (level == "debug") return spdlog::level::debug;
  if (level == "info") return spdlog::level::info;
  if (level == "warn" || level == "warning") return spdlog::level::warn;
  if (level == "error") return spdlog::level::err;
  if (level == "critical") return spdlog::level::critical;
  if (level == "off") return spdlog::level::off;
  return spdlog::level::info;
}

}  // namespace

void init_logging(const std::string& level) {
  auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto logger = std::make_shared<spdlog::logger>("hoodie", std::move(sink));
  logger->set_pattern("%Y-%m-%d %H:%M:%S.%e %^%l%$ %v");
  logger->set_level(parse_level(level));
  logger->flush_on(spdlog::level::warn);
  spdlog::set_default_logger(std::move(logger));
}

}  // namespace hoodie::util
