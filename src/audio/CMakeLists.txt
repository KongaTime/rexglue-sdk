# rexaudio - Audio subsystem library
# Audio processing and XMA decoding for Xbox 360 emulation

add_library(rexaudio OBJECT
    
    audio_driver.cpp
    audio_system.cpp
    downmix.cpp
    xma_context.cpp
    xma_decoder.cpp
    xma_register_file.cpp
    # NOP backend (always available)
    nop/nop_audio_system.cpp
)
add_library(rex::audio ALIAS rexaudio)

if(PROSPERO)
    # PS5: sceAudioOut, ported from TheSimpsonsGame-PS5-Builder (ps5/PORTED.md)
    target_sources(rexaudio PRIVATE
        prospero/prospero_audio_system.cpp
        prospero/prospero_audio_driver.cpp)
else()
    target_sources(rexaudio PRIVATE
        sdl/sdl_audio_system.cpp
        sdl/sdl_audio_driver.cpp)
endif()

rex_target_pch(rexaudio
    <mutex>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/thread.h>
    <rex/thread/mutex.h>
    <rex/ppc/context.h>
    <rex/system/kernel_state.h>
    <rex/system/xthread.h>
)

target_include_directories(rexaudio PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(rexaudio
    PUBLIC rexcore $<$<NOT:$<BOOL:${PROSPERO}>>:SDL3::SDL3>
    PRIVATE libavcodec libavutil
)
