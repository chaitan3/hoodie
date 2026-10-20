#include "net/sse.h"

#include <utility>

namespace hoodie::net {

void SseParser::feed(const uint8_t* data, std::size_t size, const Callback& on_event) {
  for (std::size_t i = 0; i < size; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c == '\n') {
      if (!line_.empty() && line_.back() == '\r') line_.pop_back();
      process_line(on_event);
      line_.clear();
    } else {
      line_.push_back(c);
    }
  }
}

void SseParser::process_line(const Callback& on_event) {
  if (line_.empty()) {
    if (has_data_ && on_event) on_event(current_);
    current_ = SseEvent{};
    has_data_ = false;
    return;
  }
  if (line_.front() == ':') return;  // comment

  const auto colon = line_.find(':');
  if (colon == std::string::npos) return;  // field without a value; ignored

  std::string field = line_.substr(0, colon);
  std::string value = line_.substr(colon + 1);
  if (!value.empty() && value.front() == ' ') value.erase(value.begin());

  if (field == "data") {
    if (has_data_) current_.data.push_back('\n');
    current_.data += value;
    has_data_ = true;
  } else if (field == "event") {
    current_.event = std::move(value);
  } else if (field == "id") {
    current_.id = std::move(value);
  }
  // "retry" and unknown fields are ignored.
}

void SseParser::finish(const Callback& on_event) {
  if (!line_.empty()) {
    if (line_.back() == '\r') line_.pop_back();
    process_line(on_event);
    line_.clear();
  }
  if (has_data_ && on_event) on_event(current_);
  current_ = SseEvent{};
  has_data_ = false;
}

void SseParser::reset() {
  line_.clear();
  current_ = SseEvent{};
  has_data_ = false;
}

}  // namespace hoodie::net
