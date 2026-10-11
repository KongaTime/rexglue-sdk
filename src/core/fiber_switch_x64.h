/**
 * @file        rex/core/fiber_switch_x64.h
 * @brief       A fiber context switch for x86-64 System V hosts whose ucontext
 *              calls cannot be used (the PS5)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * On the PS5, makecontext (Sony's libc) does not build a context that
 * swapcontext (libkernel) can enter: ps5/probe in EdgeOfTimeRecomp measured
 * every switch to a made context returning at once, the fiber never running.
 * This switch needs no libc: it saves what the System V ABI says a callee
 * keeps (rbx, rbp, r12-r15, the stack pointer) and the floating-point control
 * state (MXCSR, the x87 control word) on the current stack, and resumes the
 * other stack the same way. Usable from C and C++; the assembly is emitted
 * once, by the file that expands REX_FIBER_SWITCH_X64_DEFINE.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Saves the caller's context on its stack, stores that stack pointer in
 * *from_sp, and resumes the context saved at to_sp. Returns when another
 * switch resumes *from_sp. */
void rex_fiber_switch_x64(void** from_sp, void* to_sp);

/* The stack pointer for a new fiber, so that the first switch to it enters
 * `entry` as if it had been called: 16-byte aligned per the ABI, with the
 * caller's floating-point control state, and a zero return address above it
 * (entry must not return). */
static inline void* rex_fiber_initial_sp_x64(void* stack_base, size_t stack_size,
                                             void (*entry)(void)) {
  uintptr_t top = ((uintptr_t)stack_base + stack_size) & ~(uintptr_t)15;
  uint64_t* sp = (uint64_t*)top;
  uint32_t mxcsr;
  uint16_t fpu_control;
  __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
  __asm__ volatile("fnstcw %0" : "=m"(fpu_control));
  *--sp = 0;                       /* entry's return address: it must not return */
  *--sp = (uint64_t)(uintptr_t)entry; /* where the switch's ret goes */
  *--sp = 0;                       /* rbp */
  *--sp = 0;                       /* rbx */
  *--sp = 0;                       /* r12 */
  *--sp = 0;                       /* r13 */
  *--sp = 0;                       /* r14 */
  *--sp = 0;                       /* r15 */
  *--sp = mxcsr;                   /* 8(%rsp) */
  *--sp = fpu_control;             /* (%rsp) */
  return sp;
}

#ifdef __cplusplus
}
#endif

/* The switch itself. At entry rsp is 8 mod 16 (the call pushed the return
 * address); the frame it leaves is the one rex_fiber_initial_sp_x64 builds. */
#define REX_FIBER_SWITCH_X64_DEFINE                         \
  __asm__(                                                  \
      ".text\n"                                             \
      ".p2align 4\n"                                        \
      ".globl rex_fiber_switch_x64\n"                       \
      ".hidden rex_fiber_switch_x64\n"                      \
      ".type rex_fiber_switch_x64, @function\n"             \
      "rex_fiber_switch_x64:\n"                             \
      "  pushq %rbp\n"                                      \
      "  pushq %rbx\n"                                      \
      "  pushq %r12\n"                                      \
      "  pushq %r13\n"                                      \
      "  pushq %r14\n"                                      \
      "  pushq %r15\n"                                      \
      "  subq $16, %rsp\n"                                  \
      "  stmxcsr 8(%rsp)\n"                                 \
      "  fnstcw (%rsp)\n"                                   \
      "  movq %rsp, (%rdi)\n"                               \
      "  movq %rsi, %rsp\n"                                 \
      "  fldcw (%rsp)\n"                                    \
      "  ldmxcsr 8(%rsp)\n"                                 \
      "  addq $16, %rsp\n"                                  \
      "  popq %r15\n"                                       \
      "  popq %r14\n"                                       \
      "  popq %r13\n"                                       \
      "  popq %r12\n"                                       \
      "  popq %rbx\n"                                       \
      "  popq %rbp\n"                                       \
      "  ret\n"                                             \
      ".size rex_fiber_switch_x64, .-rex_fiber_switch_x64\n")
