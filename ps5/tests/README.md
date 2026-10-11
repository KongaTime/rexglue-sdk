# PS5 tests

Tests for the fork's PS5 code, outside the runtime.

| Test | What it checks |
| --- | --- |
| `memory_ps5_test.cpp` | `src/core/memory_ps5.cpp`, used the way `Memory::Initialize` uses it: the 4.5 GiB guest arena and its nine views at `0x10_0000_0000`, the physical heap's 512 MiB commit, aliasing between views, per-view protection, no-access windows (MMIO), decommit, anonymous memory and a clean teardown |

Each one builds three ways:

- **On the build host**, against PS5_PayloadSDK's platform layer and its host model
  of the console's kernel. The model can also make kernel calls fail, which checks
  that every failure unwinds:

  ```bash
  ps5/tests/run-host.sh        # PS5_PAYLOAD_SDK_FORK=<checkout>, default ../PS5_PayloadSDK
  ```

- **As a payload for the console**, against the real kernel. This answers what the
  model cannot: whether the console accepts no-access protection on these shared
  views, and how long backing the guest's physical memory takes. Its lines also go
  to `/data/rexglue-memory-test.txt`:

  ```bash
  make -C ps5/tests test PS5_PAYLOAD_SDK=<sdk> PS5_HOST=<console address>
  ```

  `<sdk>` is the payload SDK as EdgeOfTimeRecomp's `ps5/tools/build-rexglue.sh`
  installs it (`.deps/native/ps5-payload-sdk`).

  A payload's process has next to no direct memory (`sceKernelGetDirectMemorySize`
  answers 0 there, 2026-10-11), so on the console the guest-memory checks past the
  arena's reservation need the next way.

- **Inside a title**, whose process has the console's 12 GiB of direct memory.
  Compiled with `MEMORY_PS5_TEST_EMBEDDED`, the test is `memory_ps5_test_main()` in
  place of `main()` and writes `/app0/rexglue-memory-test.txt`.
  [PS5_EdgeOfTimeTitle](https://github.com/KongaTime/PS5_EdgeOfTimeTitle)'s program
  runs it.
