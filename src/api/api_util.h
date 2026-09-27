#pragma once

#include <string>

#include "api/api_error.h"
#include "api/openai_types.h"
#include "net/http.h"

namespace hoodie::api {

inline net::Headers json_headers(const std::string& api_key) {
  net::Headers headers{{"Content-Type", "application/json"}};
  if (!api_key.empty()) headers.emplace_back("Authorization", "Bearer " + api_key);
  return headers;
}

inline net::Headers bearer_headers(const std::string& api_key) {
  net::Headers headers;
  if (!api_key.empty()) headers.emplace_back("Authorization", "Bearer " + api_key);
  return headers;
}

// Convert a failed transport/HTTP result into an ApiError. No-op if successful.
inline void throw_if_http_error(const net::HttpResult& result, const std::string& context) {
  if (result.cancelled) throw ApiError(0, context + ": request cancelled");
  if (result.transport_error) throw ApiError(0, context + ": " + result.error);
  if (!result.ok()) {
    const std::string detail = extract_error_message(result.body_string());
    throw ApiError(result.status,
                   context + ": HTTP " + std::to_string(result.status) + ": " + detail,
                   result.body_string());
  }
}

}  // namespace hoodie::api
