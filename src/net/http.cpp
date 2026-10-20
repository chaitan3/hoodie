#include "net/http.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace hoodie::net {
namespace {

std::once_flag g_curl_once;

void ensure_curl_initialized() {
  std::call_once(g_curl_once, []() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::atexit([]() { curl_global_cleanup(); });
  });
}

struct WriteContext {
  std::vector<uint8_t>* body = nullptr;
  // When on_chunk is set, capture at most this many leading response bytes for
  // diagnostics. 0 disables capture, so streaming audio is not accumulated.
  std::size_t body_limit = 0;
  const HttpClient::ChunkCallback* on_chunk = nullptr;
  const util::CancelToken* cancel = nullptr;
};

struct ReadContext {
  const HttpClient::ReadCallback* read = nullptr;
  const util::CancelToken* cancel = nullptr;
};

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<WriteContext*>(userdata);
  const std::size_t count = size * nmemb;
  if (ctx->cancel != nullptr && ctx->cancel->cancelled()) return 0;  // abort transfer
  if (ctx->on_chunk != nullptr) {
    if (ctx->body != nullptr && ctx->body_limit > 0 && ctx->body->size() < ctx->body_limit) {
      const std::size_t room = ctx->body_limit - ctx->body->size();
      const std::size_t take = std::min(room, count);
      const auto* bytes = reinterpret_cast<const uint8_t*>(ptr);
      ctx->body->insert(ctx->body->end(), bytes, bytes + take);
    }
    (*ctx->on_chunk)(reinterpret_cast<const uint8_t*>(ptr), count);
  } else if (ctx->body != nullptr) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(ptr);
    ctx->body->insert(ctx->body->end(), bytes, bytes + count);
  }
  return count;
}

size_t read_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<ReadContext*>(userdata);
  if (ctx->cancel != nullptr && ctx->cancel->cancelled()) return CURL_READFUNC_ABORT;
  return (*ctx->read)(reinterpret_cast<uint8_t*>(ptr), size * nmemb);
}

std::string trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
  auto* result = static_cast<HttpResult*>(userdata);
  const std::size_t count = size * nitems;
  std::string line(buffer, count);
  if (line.rfind("HTTP/", 0) == 0) return count;  // status line
  const auto colon = line.find(':');
  if (colon == std::string::npos) return count;
  std::string key = to_lower(trim(line.substr(0, colon)));
  std::string value = trim(line.substr(colon + 1));
  if (!key.empty()) result->headers[key] = value;
  return count;
}

int progress_callback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  auto* cancel = static_cast<const util::CancelToken*>(userdata);
  return (cancel->cancelled()) ? 1 : 0;  // non-zero aborts
}

}  // namespace

HttpClient::HttpClient() { ensure_curl_initialized(); }

HttpClient::~HttpClient() = default;

HttpResult HttpClient::get(const std::string& url, const Headers& headers,
                           const util::CancelToken& cancel) {
  return perform(url, headers, /*is_post=*/false, "", nullptr, nullptr, cancel);
}

HttpResult HttpClient::post_json(const std::string& url, const Headers& headers,
                                 const std::string& json, const util::CancelToken& cancel) {
  return perform(url, headers, /*is_post=*/true, json, nullptr, nullptr, cancel);
}

HttpResult HttpClient::post_multipart(const std::string& url, const Headers& headers,
                                      const std::vector<MultipartField>& fields,
                                      const util::CancelToken& cancel) {
  return perform(url, headers, /*is_post=*/true, "", &fields, nullptr, cancel);
}

HttpResult HttpClient::post_json_stream(const std::string& url, const Headers& headers,
                                        const std::string& json, const ChunkCallback& on_chunk,
                                        const util::CancelToken& cancel) {
  return perform(url, headers, /*is_post=*/true, json, nullptr, &on_chunk, cancel);
}

HttpResult HttpClient::post_duplex(const std::string& url, const Headers& headers,
                                   const ReadCallback& read, const ChunkCallback& on_chunk,
                                   const util::CancelToken& cancel) {
  HttpResult result;

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    result.transport_error = true;
    result.error = "curl_easy_init failed";
    return result;
  }

  WriteContext write_ctx;
  write_ctx.body = &result.body;
  write_ctx.body_limit = 4096;  // keep the error body, not the SSE stream
  write_ctx.on_chunk = &on_chunk;
  write_ctx.cancel = &cancel;

  ReadContext read_ctx;
  read_ctx.read = &read;
  read_ctx.cancel = &cancel;

  char error_buffer[CURL_ERROR_SIZE] = {0};

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  // Live ingest is defined in terms of HTTP/1.1 chunked upload. Forcing 1.1
  // also avoids an HTTPS proxy terminating an h2 request body and buffering it
  // before forwarding.
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "hoodie/0.1");
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &progress_callback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  // Chunked upload + POST, mirroring `curl -T - -X POST` (unknown body size).
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(-1));
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, &read_callback);
  curl_easy_setopt(curl, CURLOPT_READDATA, &read_ctx);

  struct curl_slist* header_list = nullptr;
  for (const auto& header : headers) {
    header_list = curl_slist_append(header_list, (header.first + ": " + header.second).c_str());
  }
  if (header_list != nullptr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

  const CURLcode code = curl_easy_perform(curl);

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
  const auto content_type = result.headers.find("content-type");
  if (content_type != result.headers.end()) {
    result.content_type = content_type->second;
  }

  if (code != CURLE_OK) {
    if (cancel.cancelled()) {
      result.cancelled = true;
      result.error = "request cancelled";
    } else if (code == CURLE_OPERATION_TIMEDOUT) {
      result.transport_error = true;
      result.error = "request timed out";
    } else {
      result.transport_error = true;
      result.error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(code);
    }
  }

  if (header_list != nullptr) curl_slist_free_all(header_list);
  curl_easy_cleanup(curl);
  return result;
}

HttpResult HttpClient::perform(const std::string& url, const Headers& headers, bool is_post,
                               const std::string& json, const std::vector<MultipartField>* fields,
                               const ChunkCallback* on_chunk, const util::CancelToken& cancel) {
  HttpResult result;

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    result.transport_error = true;
    result.error = "curl_easy_init failed";
    return result;
  }

  WriteContext write_ctx;
  write_ctx.body = &result.body;
  write_ctx.on_chunk = on_chunk;
  write_ctx.cancel = &cancel;

  char error_buffer[CURL_ERROR_SIZE] = {0};

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "hoodie/0.1");
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &progress_callback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  struct curl_slist* header_list = nullptr;
  for (const auto& header : headers) {
    header_list = curl_slist_append(header_list, (header.first + ": " + header.second).c_str());
  }
  if (header_list != nullptr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

  curl_mime* mime = nullptr;
  if (fields != nullptr) {
    mime = curl_mime_init(curl);
    for (const MultipartField& field : *fields) {
      curl_mimepart* part = curl_mime_addpart(mime);
      curl_mime_name(part, field.name.c_str());
      if (!field.filename.empty()) {
        if (!field.content_type.empty()) curl_mime_type(part, field.content_type.c_str());
        curl_mime_filename(part, field.filename.c_str());
        const std::vector<uint8_t>* data = field.data;
        static const std::vector<uint8_t> empty;
        if (data == nullptr) data = &empty;
        curl_mime_data(part, reinterpret_cast<const char*>(data->data()),
                       static_cast<curl_off_t>(data->size()));
      } else {
        curl_mime_data(part, field.value.c_str(), CURL_ZERO_TERMINATED);
      }
    }
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
  } else if (is_post) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json.size()));
  }

  const CURLcode code = curl_easy_perform(curl);

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
  const auto content_type = result.headers.find("content-type");
  if (content_type != result.headers.end()) {
    result.content_type = content_type->second;
  }

  if (code != CURLE_OK) {
    if (cancel.cancelled()) {
      result.cancelled = true;
      result.error = "request cancelled";
    } else if (code == CURLE_OPERATION_TIMEDOUT) {
      result.transport_error = true;
      result.error = "request timed out";
    } else {
      result.transport_error = true;
      result.error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(code);
    }
  }

  if (mime != nullptr) curl_mime_free(mime);
  if (header_list != nullptr) curl_slist_free_all(header_list);
  curl_easy_cleanup(curl);
  return result;
}

}  // namespace hoodie::net
