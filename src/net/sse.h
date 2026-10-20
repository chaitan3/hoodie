#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace hoodie::net {

struct SseEvent {
  std::string event;  // "event:" field (may be empty)
  std::string data;   // concatenated "data:" lines
  std::string id;     // "id:" field (may be empty)
};

// Incremental Server-Sent Events parser. Feed arbitrary byte fragments; one
// callback is invoked per complete event (terminated by a blank line).
class SseParser {
 public:
  using Callback = std::function<void(const SseEvent&)>;

  void feed(const uint8_t* data, std::size_t size, const Callback& on_event);
  void feed(const char* data, std::size_t size, const Callback& on_event) {
    feed(reinterpret_cast<const uint8_t*>(data), size, on_event);
  }

  // Dispatch a pending event if the stream ended without a trailing blank line.
  void finish(const Callback& on_event);

  void reset();

 private:
  void process_line(const Callback& on_event);

  std::string line_;
  SseEvent current_;
  bool has_data_ = false;
};

}  // namespace hoodie::net
