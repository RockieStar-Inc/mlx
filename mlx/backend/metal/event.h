// Copyright © 2026 Apple Inc.

#include <atomic>
#include <memory>
#include <string>

#include "mlx/backend/metal/device.h"
#include "mlx/event.h"

namespace mlx::core::metal {

/// A fault caught on a CPU scheduler thread has no caller to throw to; park it here
/// so the next caller-thread wait or synchronize reports it.
void set_pending_cpu_error(std::shared_ptr<std::string> error);
std::shared_ptr<std::string> take_pending_cpu_error();

class EventImpl {
 public:
  EventImpl(Device& d);
  ~EventImpl();

  void wait(uint64_t value);
  void signal(uint64_t value);
  void set_error(std::shared_ptr<std::string> error);
  void check_error();

  std::shared_ptr<std::string> error() const {
    return std::atomic_load(&error_);
  }

  bool poisoned() const {
    return std::atomic_load(&error_) != nullptr;
  }

  auto* mtl_event() {
    return mtl_event_.get();
  }

 private:
  // TODO: Use std::atomic<std::shared_ptr> when it gets supported in Xcode.
  std::shared_ptr<std::string> error_;

  NS::SharedPtr<MTL::SharedEvent> mtl_event_;
};

} // namespace mlx::core::metal
