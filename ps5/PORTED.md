# What this fork takes from TheSimpsonsGame-PS5-Builder

[TheSimpsonsGame-PS5-Builder](https://github.com/LoZazaMastro/TheSimpsonsGame-PS5-Builder)
(LoZazaMastro and contributors) runs The Simpsons Game's ReXGlue recompilation natively
on the PS5. Its copy of ReXGlue (`tools/rexglue-sdk`, ReXGlue v0.8.0) has a PS5 app
layer under ReXGlue's BSD 3-Clause License. This fork ports parts of it to v0.10.0,
read at `59e7086` (forked as
[KongaTime/TheSimpsonsGame-PS5-Builder](https://github.com/KongaTime/TheSimpsonsGame-PS5-Builder)).
Each ported file says so at its top. The ported files keep the port's `*_prospero` names,
and `REX_PLATFORM_PROSPERO` is the same platform as this fork's `REX_PLATFORM_PS5`, so
later changes there carry over file by file.

| This fork | From the port | Changes |
| --- | --- | --- |
| `include/rex/ui/windowed_app_context_prospero.h`, `src/ui/windowed_app_context_prospero.cpp` | the same paths | without its UI metrics tick |
| `include/rex/ui/window_prospero.h`, `src/ui/window_prospero.cpp` | the same paths | without its `MenuItem::Create` (v0.10.0 has one for every platform) |
| `src/ui/windowed_app_main_prospero.cpp` | the same path | `rex_ps5_app_main`, called by the title's `main`, in place of `main`; v0.10.0's start-up order |
| `include/rex/ui/surface_prospero.h`, `src/ui/surface_prospero.cpp`, the PS5 entry in `include/rex/ui/surface.h` | the same paths | — |
| `include/rex/ui/startup_prospero.h` | the same path | the defaults are the fork's (`src/core/startup_ps5.cpp`, weak, for a title to replace); there, the port's GPL app CRT defines them |
| `include/rex/input/prospero/pad_abi.h` | the same path | — (it adapts ps5-payload-dev/SDL's PS5 joystick ABI, under SDL's zlib licence, kept in the file) |
| `include/rex/input/prospero/pad_mapping.h` | the same path | — |
| `include/rex/input/prospero/prospero_input_driver.h`, `src/input/prospero/prospero_input_driver.cpp` | the same paths | rewritten for v0.10.0's device-based `InputDriver`; its pad handling kept, its trace checkpoints and frame-generation shortcut left out; keystrokes as v0.10.0's SDL driver makes them |
| `include/rex/audio/prospero/audio_out.h` | the same path | — (ps5-payload-dev/SDL's PS5 AudioOut ABI, under SDL's zlib licence, kept in the file) |
| `include/rex/audio/prospero/pcm_conversion.h` | the same path | v0.10.0's stereo fold and output gain |
| `include/rex/platform/fault_prospero.h`, `src/core/fault_prospero.cpp` | `include/rex/platform/fault_prospero.h`, `src/core/fault_prospero.cpp` and the record half of `src/core/exception_handler_prospero.cpp` | without the port's trace ring, stack dump and heap-failure hook; the code segment's bounds from the fork's `MakeCodeReadable` instead of `dladdr`; a frame-pointer backtrace and ELF-relative addresses of the fork's own |
| `src/core/abort_prospero.cpp` | the same path | without its heap-failure hook (the port's own malloc) |
| `ForwardUnhandledException` in `src/core/exception_handler_posix.cpp` | `src/core/exception_handler_prospero.cpp` | records the fault first, then passes it on (the port records only on the default disposition) |
| `include/rex/ui/vulkan/display_prospero.h`, `src/ui/vulkan/vulkan_display_prospero.cpp`, `include/rex/ui/vulkan/functions/instance_khr_display.inc` | the same paths | for PS5_Vulkan's RADV: 119.88 Hz comes from param.json (no `SIMPSONS_VIDEOOUT_120HZ`), the output clock from the advertised modes (no `simpsons_radv_videoout_refresh_millihertz`), and a 60 Hz request on a 119.88 Hz output paced with `wsi_videoout_set_flip_rate(1)` |
| the PS5 blocks of `src/ui/vulkan/vulkan_instance.cpp`, `vulkan_presenter.cpp`, `spirv_tools_context.cpp`, `src/ui/renderdoc_api.cpp`, `src/graphics/vulkan/command_processor.cpp` | the same paths | RADV through `vk_icdGetInstanceProcAddr`, no loader or layers, VK_KHR_display, the display surface, deferred paints on an unproven fence, a refresher that reports a failed submission; without frame generation, live settings, tracing and metrics; SPIRV-Tools only when a build links `SPIRV-Tools-static` |
| `include/rex/audio/prospero/prospero_audio_{driver,system}.h`, `src/audio/prospero/prospero_audio_{driver,system}.cpp` | the same paths | without its diagnostics, dump files, live mute and thread priority; `audio_mute` as v0.10.0's SDL driver has it |

What the fork does on its own, next to those:

- `CreateDefaultInputSystem` uses the PS5 driver on the console (`src/input/input_system.cpp`).
- `ReXApp` (`src/ui/rex_app.cpp`) uses the PS5 sound system. It finds the game in
  `/app0/gamedata` and keeps its logs in the state folder. It marks each start-up stage
  in the startup log.
- `GetExecutablePath` and `GetUserFolder` answer `/app0/eboot.bin` and
  `<state folder>/userdata` (`src/core/filesystem_posix.cpp`).
- SDL3 is neither built nor linked for the PS5.

Not taken: the port's guest memory (`memory_prospero.cpp`; this fork's `memory_ps5.cpp`
is console-checked), fibers, the emulated GPU's PS5 changes beyond those above (most attach to the port's native
render-target and lazy-allocation paths, which this tree doesn't have), frame generation, the
settings overlay, its trace ring and checkpoints (`trace_prospero.cpp`) and its
per-thread signal registry (`signal_thread_prospero.cpp`). EdgeOfTimeRecomp's
`docs/PS5-PORTING.md` says what comes next.
