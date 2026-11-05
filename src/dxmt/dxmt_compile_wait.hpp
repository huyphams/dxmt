#pragma once

#include <atomic>
#include <cstdint>

namespace dxmt {

/* The threads that wait for a shader or pipeline to compile, or compile one
 * themselves because nothing else has started on it.  A frame's commands are
 * held until each new pipeline in them has compiled, which takes seconds with
 * Metal's cache cold, so an embedder that watches for a hung renderer reads
 * this (dxmt_compile_waits) to tell one that waits for Metal's compiler.  The
 * hook (dxmt_set_compile_wait_hook) is called as the count leaves 0 and goes
 * back to it, on the waiting thread; it reads the count again rather than
 * trusting the order of the calls, which threads race. */
class CompileWait {
public:
  CompileWait() {
    if (count_.fetch_add(1, std::memory_order_acq_rel) == 0)
      Notify();
  }
  ~CompileWait() {
    if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1)
      Notify();
  }
  CompileWait(const CompileWait &) = delete;
  CompileWait &operator=(const CompileWait &) = delete;

  static uint32_t
  Count() {
    return count_.load(std::memory_order_acquire);
  }

  static void
  SetHook(void (*hook)(void *), void *context) {
    hook_.store(nullptr, std::memory_order_release);
    hook_context_.store(context, std::memory_order_release);
    hook_.store(hook, std::memory_order_release);
  }

private:
  static void
  Notify() {
    if (auto hook = hook_.load(std::memory_order_acquire))
      hook(hook_context_.load(std::memory_order_acquire));
  }

  static inline std::atomic<uint32_t> count_ = 0;
  static inline std::atomic<void (*)(void *)> hook_ = nullptr;
  static inline std::atomic<void *> hook_context_ = nullptr;
};

} // namespace dxmt
