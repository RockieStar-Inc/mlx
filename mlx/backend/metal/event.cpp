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
  std::atomic_store(&pending_cpu_error, std::move(error));
}

std::shared_ptr<std::string> take_pending_cpu_error() {
  return std::atomic_exchange(&pending_cpu_error, {});
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
  try {
    static_cast<metal::EventImpl*>(event_.get())->wait(value());
  } catch (...) {
    // This caller thread is reporting it, which is what marks the error delivered.
    if (stream_.device == Device::gpu) {
      metal::device(stream_.device).mark_error_reported(stream_.index);
    }
    throw;
  }
  auto pending = metal::take_pending_cpu_error();
  if (stream_.device == Device::gpu) {
    if (auto err =
            metal::device(stream_.device).take_undelivered_error(stream_.index)) {
      throw std::runtime_error(*err);
    }
  }
  if (pending) {
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
        auto error = std::make_shared<std::string>(e.what());
        impl->set_error(error);
        metal::set_pending_cpu_error(std::move(error));
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
