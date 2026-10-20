#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "util/cancel.h"

namespace hoodie::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

// One part of a multipart/form-data request. If `filename` is non-empty the
// part is sent as a file (with `data` as its bytes); otherwise `value` is sent.
struct MultipartField {
  std::string name;
  std::string value;
  std::string filename;
  std::string content_type;
  const std::vector<uint8_t>* data = nullptr;
};

struct HttpResult {
  long status = 0;
  std::string content_type;
  std::map<std::string, std::string> headers;  // lower-cased keys
  std::vector<uint8_t> body;
  bool transport_error = false;
  bool cancelled = false;
  std::string error;  // transport/cancel description

  bool ok() const {
    return !transport_error && !cancelled && status >= 200 && status < 300;
  }
  std::string body_string() const { return std::string(body.begin(), body.end()); }
};

// Minimal synchronous libcurl wrapper. Each request creates its own CURL easy
// handle, so one instance may be used concurrently. Supports in-memory bodies,
// multipart uploads, streaming response callbacks, and full-duplex requests.
class HttpClient {
 public:
  using ChunkCallback = std::function<void(const uint8_t* data, std::size_t size)>;
  // Fills up to `capacity` bytes; returns the number written, 0 for end of body.
  // May block; the request is sent with chunked Transfer-Encoding.
  using ReadCallback = std::function<std::size_t(uint8_t* buffer, std::size_t capacity)>;

  HttpClient();
  ~HttpClient();

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  HttpResult get(const std::string& url, const Headers& headers = {},
                 const util::CancelToken& cancel = {});

  HttpResult post_json(const std::string& url, const Headers& headers, const std::string& json,
                       const util::CancelToken& cancel = {});

  HttpResult post_multipart(const std::string& url, const Headers& headers,
                            const std::vector<MultipartField>& fields,
                            const util::CancelToken& cancel = {});

  // Streaming variant: `on_chunk` is invoked for each body fragment as it
  // arrives; the body is not accumulated.
  HttpResult post_json_stream(const std::string& url, const Headers& headers,
                              const std::string& json, const ChunkCallback& on_chunk,
                              const util::CancelToken& cancel = {});

  // Full-duplex request: `read` produces the chunked request body while
  // `on_chunk` receives the response body on the same connection. Used for live
  // ASR ingest. Blocking inside `read` is allowed.
  HttpResult post_duplex(const std::string& url, const Headers& headers,
                         const ReadCallback& read, const ChunkCallback& on_chunk,
                         const util::CancelToken& cancel = {});

 private:
  HttpResult perform(const std::string& url, const Headers& headers, bool is_post,
                     const std::string& json, const std::vector<MultipartField>* fields,
                     const ChunkCallback* on_chunk, const util::CancelToken& cancel);
};

}  // namespace hoodie::net
