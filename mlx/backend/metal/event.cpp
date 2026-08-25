// Copyright © 2024 Apple Inc.

#include "mlx/backend/metal/event.h"
#include "mlx/scheduler.h"

namespace mlx::core::metal {

///////////////////////////////////////////////////////////////////////////////
// EventImpl implementations
///////////////////////////////////////////////////////////////////////////////

namespace {
// TODO: Use std::atomic<std::shared_ptr> when it gets supported in Xcode.
std::shared_ptr<std::string> pending_cpu_error;
} // namespace

void set_pending_cpu_error(std::shared_ptr<std::string> error) {
  // Keep the earliest: a later fault must not overwrite one nobody has reported yet.
  std::shared_ptr<std::string> expected;
  std::atomic_compare_exchange_strong(
      &pending_cpu_error, &expected, std::move(error));
}

std::shared_ptr<std::string> take_pending_cpu_error() {
  return std::atomic_exchange(&pending_cpu_error, {});
}

void drop_pending_cpu_error_if(const std::shared_ptr<std::string>& error) {
  if (!error) {
    return;
  }
  // Identity only: Metal reproduces the same text for a different fault, and the
  // CPU lambda parks the very pointer it poisons with.
  auto expected = error;
  std::atomic_compare_exchange_strong(&pending_cpu_error, &expected, {});
}

EventImpl::EventImpl(Device& d) {
  auto p = new_scoped_memory_pool();
  mtl_event_ = NS::TransferPtr(d.mtl_device()->newSharedEvent());
  if (!mtl_event_) {
    throw std::runtime_error(
        "[Event::Event] Failed to create Metal shared event.");
  }
}

EventImpl::~EventImpl() {
  auto p = new_scoped_memory_pool();
  mtl_event_.reset();
}

void EventImpl::wait(uint64_t value) {
  check_error();
  mtl_event_->waitUntilSignaledValue(value, -1); // never times out
  check_error();
}

void EventImpl::signal(uint64_t value) {
  mtl_event_->setSignaledValue(value);
}

void EventImpl::set_error(std::shared_ptr<std::string> error) {
  std::atomic_store(&error_, std::move(error));
}

void EventImpl::check_error() {
  // Events are per-eval, so a poisoned one must keep failing every later wait.
  auto error = std::atomic_load(&error_);
  if (error) {
    throw std::runtime_error(*error);
  }
}

} // namespace mlx::core::metal

///////////////////////////////////////////////////////////////////////////////
// Event implementations
///////////////////////////////////////////////////////////////////////////////

namespace mlx::core {

Event::Event(Stream stream) : stream_(stream) {
  event_ = std::make_shared<metal::EventImpl>(metal::device(stream.device));
}

void Event::wait() {
  auto* impl = static_cast<metal::EventImpl*>(event_.get());
  try {
    impl->wait(value());
  } catch (...) {
    // This caller thread is reporting it, which is what marks the error delivered
    // — but only if the stream still holds the very error being rethrown.
    impl->mark_reported();
    if (auto reported = impl->error()) {
      if (stream_.device == Device::gpu) {
        metal::device(stream_.device)
            .mark_error_reported(stream_.index, reported);
      }
      // The CPU-stream lambda parks the same pointer it poisons with.
      metal::drop_pending_cpu_error_if(reported);
    }
    throw;
  }
  if (stream_.device == Device::gpu) {
    if (auto err =
            metal::device(stream_.device).take_undelivered_error(stream_.index)) {
      metal::drop_pending_cpu_error_if(err);
      throw std::runtime_error(*err);
    }
  }
  // Last: taking the slot before a throw above would destroy it unreported.
  if (auto pending = metal::take_pending_cpu_error()) {
    throw std::runtime_error(*pending);
  }
}

void Event::wait(Stream stream) {
  auto impl = std::static_pointer_cast<metal::EventImpl>(event_);
  if (stream.device == Device::cpu) {
    scheduler::enqueue(stream, [impl = std::move(impl), value = value()]() {
      try {
        impl->wait(value);
      } catch (const std::exception& e) {
        // The scheduler thread cannot throw, and the caller waits on a different
        // event: re-poison this one and park the error for the caller thread.
        // Park the very pointer the event carries, or the identity drain on the
        // caller's throw path never matches.
        auto error = impl->error();
        if (!error) {
          error = std::make_shared<std::string>(e.what());
        }
        impl->set_error(error);
        if (!impl->reported()) {
          metal::set_pending_cpu_error(error);
          // The report can land between the check and the park; re-check to
          // avoid throwing a reported fault at the next healthy caller.
          if (impl->reported()) {
            metal::drop_pending_cpu_error_if(error);
          }
        }
      }
    });
  } else {
    auto& d = metal::device(stream.device);
    d.wait_event(stream.index, std::move(impl), value());
  }
}

void Event::signal(Stream stream) {
  auto impl = std::static_pointer_cast<metal::EventImpl>(event_);
  if (stream.device == Device::cpu) {
    scheduler::enqueue(stream, [impl = std::move(impl), value = value()]() {
      impl->signal(value);
    });
  } else {
    auto& d = metal::device(stream.device);
    d.signal_event(stream.index, std::move(impl), value());
  }
}

bool Event::is_signaled() const {
  auto* mtl_event = static_cast<metal::EventImpl*>(event_.get())->mtl_event();
  return mtl_event->signaledValue() >= value();
}

bool Event::poisoned() const {
  if (!valid()) {
    return false;
  }
  return static_cast<metal::EventImpl*>(event_.get())->poisoned();
}

} // namespace mlx::core
