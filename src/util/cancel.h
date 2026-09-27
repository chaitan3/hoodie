#pragma once

#include <atomic>
#include <memory>

namespace hoodie::util {

// Cooperative cancellation flag. Copies share the same underlying state, so a
// token can be handed to in-flight requests and cancelled from elsewhere
// (barge-in, shutdown). Cheap to copy.
class CancelToken {
 public:
  CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

  void cancel() const { flag_->store(true, std::memory_order_relaxed); }
  bool cancelled() const { return flag_->load(std::memory_order_relaxed); }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace hoodie::util
