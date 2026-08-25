// Copyright © 2023 Apple Inc.

#include "mlx/scheduler.h"
#include "mlx/backend/gpu/device_info.h"
#include "mlx/backend/gpu/eval.h"

namespace mlx::core {

Stream default_stream(Device d) {
  if (!gpu::is_available() && d == Device::gpu) {
    throw std::invalid_argument(
        "[default_stream] Cannot get gpu stream without gpu backend.");
  }
  return scheduler::scheduler().get_default_stream(d);
}

void set_default_stream(Stream s) {
  if (!gpu::is_available() && s.device == Device::gpu) {
    throw std::invalid_argument(
        "[set_default_stream] Cannot set gpu stream without gpu backend.");
  }
  return scheduler::scheduler().set_default_stream(s);
}

Stream get_stream(int index) {
  return scheduler::scheduler().get_stream(index);
}

std::vector<Stream> get_streams() {
  return scheduler::scheduler().get_streams();
}

Stream new_stream(Device d) {
  if (!gpu::is_available() && d == Device::gpu) {
    throw std::invalid_argument(
        "[new_stream] Cannot make gpu stream without gpu backend.");
  }
  return scheduler::scheduler().new_stream(d);
}

Stream new_stream() {
  return scheduler::scheduler().new_stream(default_device());
}

void synchronize(Stream s) {
  if (s.device == mlx::core::Device::cpu) {
    auto p = std::make_shared<std::promise<void>>();
    std::future<void> f = p->get_future();
    scheduler::enqueue(s, [p = std::move(p)]() { p->set_value(); });
    f.wait();
  } else {
    gpu::synchronize(s);
  }
}

void synchronize() {
  synchronize(default_stream(default_device()));
}

namespace scheduler {

/** A singleton scheduler to manage devices, streams, and task execution. */
Scheduler& scheduler() {
  // Deliberately leaked everywhere, not only on Windows (VS bug
  // https://developercommunity.visualstudio.com/t/1654756): command-buffer
  // completion handlers call notify_task_completion past static destruction at
  // exit, and a destroyed mutex there aborts the process.
  static Scheduler* scheduler = new Scheduler;
  return *scheduler;
}

} // namespace scheduler
} // namespace mlx::core
