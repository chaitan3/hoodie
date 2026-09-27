#pragma once

#include <stdexcept>
#include <string>

namespace hoodie::api {

// Thrown when the remote server returns a non-2xx response or an
// unparseable body. `status` is 0 for transport/parse failures.
struct ApiError : std::runtime_error {
  ApiError(long http_status, std::string message, std::string response_body = {})
      : std::runtime_error(std::move(message)),
        status(http_status),
        body(std::move(response_body)) {}

  long status = 0;
  std::string body;
};

}  // namespace hoodie::api
