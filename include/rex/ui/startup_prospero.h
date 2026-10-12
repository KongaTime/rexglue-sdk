// Ported from TheSimpsonsGame-PS5-Builder (LoZazaMastro and contributors),
// tools/rexglue-sdk/include/rex/ui/startup_prospero.h at 59e7086, BSD 3-Clause
// License (see LICENSE). The defaults here are the fork's (startup_ps5.cpp);
// there, the port's native app CRT implements both.
#pragma once

// Safe before cvars and ReXGlue logging are initialized. Each call appends a
// marker line to <prospero_state_root()>/logs/startup.log and writes it to
// stderr. The fork's definition is weak: a title replaces it with its own (its
// trace file and klog, say).
extern "C" void prospero_startup_log(const char* message);

// Where runtime state (user data, cache, logs) lives: /app0 by default, the
// title's folder. Weak too: a title whose /app0 is read-only (a mounted image)
// names a writable folder instead. Game data stays under /app0.
extern "C" const char* prospero_state_root();
