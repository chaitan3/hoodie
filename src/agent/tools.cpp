#include "agent/tools.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cctype>
#include <ctime>
#include <string>

#include "util/process.h"

namespace hoodie::agent {
namespace {

using nlohmann::json;

constexpr const char* kWeatherUrl = "https://api.weather.gov/gridpoints/BOX/73,90/forecast";

std::string url_encode(const std::string& value) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0f]);
    }
  }
  return out;
}

std::string get_time_impl() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  const int hour12 = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
  const char* am_pm = local.tm_hour < 12 ? "AM" : "PM";
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "The time is %d %02d %s", hour12, local.tm_min, am_pm);
  return buffer;
}

std::string get_weather_impl(net::HttpClient& http) {
  const net::HttpResult result = http.get(kWeatherUrl);
  if (!result.ok()) {
    return "I could not retrieve the weather right now.";
  }
  try {
    const json root = json::parse(result.body_string());
    const json& period = root.at("properties").at("periods").at(0);
    const int temperature = period.at("temperature").get<int>();
    const std::string unit = period.value("temperatureUnit", "F");
    const int temp_c = unit == "C"
                           ? temperature
                           : static_cast<int>((temperature - 32) * 5 / 9);
    std::string forecast = period.at("detailedForecast").get<std::string>();

    // NWS text carries the raw temperature without a unit. Replace that exact
    // token with an explicitly labelled Celsius value so the model cannot
    // mistake the converted number for Fahrenheit.
    const std::string value = std::to_string(temperature);
    const std::string celsius = std::to_string(temp_c) + " degrees Celsius";
    bool replaced = false;
    for (std::size_t pos = forecast.find(value); pos != std::string::npos;
         pos = forecast.find(value, pos + 1)) {
      const bool left_ok =
          pos == 0 || !std::isdigit(static_cast<unsigned char>(forecast[pos - 1]));
      const std::size_t end = pos + value.size();
      const bool right_ok =
          end == forecast.size() ||
          !std::isdigit(static_cast<unsigned char>(forecast[end]));
      if (!left_ok || !right_ok) continue;
      forecast.replace(pos, value.size(), celsius);
      replaced = true;
      break;
    }
    if (!replaced) {
      forecast = "The temperature is " + celsius + ". " + forecast;
    }
    return forecast;
  } catch (const std::exception& e) {
    return std::string("Weather data was unavailable (") + e.what() + ").";
  }
}

std::string play_music_impl(const std::string& arguments_json, ToolContext& context) {
  std::string phrase;
  try {
    const json args = json::parse(arguments_json.empty() ? "{}" : arguments_json);
    phrase = args.value("phrase", "");
  } catch (const std::exception&) {
    return "I need a song name to play something.";
  }
  if (phrase.empty()) return "I need a song name to play something.";

  const util::ProcessResult result =
      util::run_process({"yt-dlp", "--dump-json", "--no-playlist", "ytsearch1:" + phrase});
  if (result.exit_code != 0 || result.output.empty()) {
    spdlog::warn("play_music: yt-dlp failed (exit {})", result.exit_code);
    return "I could not find the song \"" + phrase + "\".";
  }

  try {
    const json info = json::parse(result.output);
    MusicRequest request;
    request.id = info.value("id", "");
    request.title = info.value("title", phrase);
    context.music = request;
    return "The requested song is " + request.title;
  } catch (const std::exception&) {
    return "I could not find the song \"" + phrase + "\".";
  }
}

std::string search_impl(net::HttpClient& http, const std::string& arguments_json) {
  std::string query;
  try {
    const json args = json::parse(arguments_json.empty() ? "{}" : arguments_json);
    query = args.value("query", "");
  } catch (const std::exception&) {
    return "I need a search query.";
  }
  if (query.empty()) return "I need a search query.";

  const std::string url = "https://api.duckduckgo.com/?q=" + url_encode(query) +
                          "&format=json&no_html=1&no_redirect=1";
  const net::HttpResult result = http.get(url);
  if (!result.ok()) return "Search is unavailable right now.";
  try {
    const json root = json::parse(result.body_string());
    std::string answer = root.value("AbstractText", "");
    if (answer.empty()) answer = root.value("Answer", "");
    if (answer.empty() && root.contains("RelatedTopics") && root.at("RelatedTopics").is_array()) {
      for (const json& topic : root.at("RelatedTopics")) {
        if (topic.contains("Text") && topic.at("Text").is_string()) {
          answer = topic.at("Text").get<std::string>();
          break;
        }
      }
    }
    return answer.empty() ? ("No results for \"" + query + "\".") : answer;
  } catch (const std::exception&) {
    return "Search returned an unexpected response.";
  }
}

}  // namespace

ToolRegistry builtin_tools(net::HttpClient& http, const Config& /*config*/) {
  ToolRegistry registry;

  registry.add(Tool{"get_time",
                    "Get the current local time.",
                    R"({"type":"object","properties":{},"required":[]})",
                    [](const std::string&, ToolContext&) { return get_time_impl(); }});

  registry.add(Tool{"get_weather",
                    "Get the current weather forecast.",
                    R"({"type":"object","properties":{},"required":[]})",
                    [&http](const std::string&, ToolContext&) { return get_weather_impl(http); }});

  registry.add(
      Tool{"play_music",
           "Find and play a song by name.",
           R"({"type":"object","properties":{"phrase":{"type":"string","description":"Song title or search phrase"}},"required":["phrase"]})",
           [](const std::string& args, ToolContext& ctx) { return play_music_impl(args, ctx); }});

  registry.add(
      Tool{"search",
           "Search the web for a short factual answer.",
           R"({"type":"object","properties":{"query":{"type":"string","description":"The search query"}},"required":["query"]})",
           [&http](const std::string& args, ToolContext&) { return search_impl(http, args); }});

  return registry;
}

}  // namespace hoodie::agent
