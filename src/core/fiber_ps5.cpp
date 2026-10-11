/**
 * @file        rex/core/fiber_ps5.cpp
 * @brief       PS5 backend for rex::thread::Fiber (a stack switch of its own)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * fiber_posix.cpp's makecontext/swapcontext does not work on the console:
 * EdgeOfTimeRecomp's ps5/probe made a context with Sony's makecontext and
 * switched to it 100000 times with libkernel's swapcontext, and every switch
 * returned at once without the fiber running. So a fiber here is a saved
 * stack pointer, switched by fiber_switch_x64.h, which needs nothing from the
 * console's libc. The semantics are fiber_posix.cpp's.
 */

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <rex/thread/fiber.h>

#include <cassert>
#include <exception>

#include "fiber_switch_x64.h"

REX_FIBER_SWITCH_X64_DEFINE;

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  // The thread's own stack: sp_ is filled in when it first switches away.
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);
  f->sp_ = rex_fiber_initial_sp_x64(f->stack_.data(), f->stack_.size(), &Fiber::Trampoline);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before the switch entered here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
  // fiber_posix.cpp's contexts have no uc_link, so a returning fiber ends its
  // thread there; ReXGlue's entries never return (FiberEntryPoint terminates).
  std::terminate();
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_fiber_switch_x64(&from->sp_, target->sp_);
}

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // stack_ is freed by the vector destructor.
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_PS5
