cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(halo2_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp-toolkit" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
# Diagnostic helpers such as the DEL screenshot hotkey. Default OFF so a
# release configure does not compile them in. This test package is built with
# -DRECOMP_DEBUG_TOOLS=ON; keep the option off for any release build.
option(RECOMP_DEBUG_TOOLS "Compile in diagnostic helpers such as the DEL screenshot hotkey" OFF)

set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/gpu/d3d11_backend.c
    src/gpu/rc_hlsl.c
    src/gpu/render_scale.c
    src/gpu/shader_cache.c
    src/gpu/texture_decode.c
    src/gpu/texture_sampler.c
    src/gpu/fxaa.c
    src/gpu/vsh_hlsl.c
    src/settings.c
    src/overlay.c
    src/perf_counters.c
    src/perf_sampler.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)
if(RECOMP_DEBUG_TOOLS)
    list(APPEND GAME_SOURCES src/gpu/screenshot.c)
endif()

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME}
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# SDL3 controllers (Windows, 1.5 only): Xbox pads stay on XInput, every other
# pad (PlayStation, Switch, generic) comes from SDL3. The toolkit's
# xinput_device.c is left alone so a 1.0 build is unchanged.
set(HALO2_SDL3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL3-3.4.20" CACHE PATH
    "SDL3 development package: include/SDL3, lib/x64/SDL3.lib, lib/x64/SDL3.dll")
option(HALO2_SDL3 "Windows: SDL3 for non-Xbox controllers beside XInput" ON)
if(HALO2_SDL3)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "HALO2_SDL3 is for the 64-bit Windows player build")
    endif()
    if(NOT EXISTS "${HALO2_SDL3_ROOT}/include/SDL3/SDL.h" OR NOT EXISTS "${HALO2_SDL3_ROOT}/lib/x64/SDL3.lib")
        message(FATAL_ERROR "HALO2_SDL3_ROOT must name an SDL3 development package (include/SDL3/SDL.h, lib/x64/SDL3.lib)")
    endif()
    get_target_property(_h2_input_sources xbox_input SOURCES)
    set(_h2_input_kept "")
    foreach(_s IN LISTS _h2_input_sources)
        if(NOT _s MATCHES "xinput_device\\.c$")
            list(APPEND _h2_input_kept "${_s}")
        endif()
    endforeach()
    set_property(TARGET xbox_input PROPERTY SOURCES "${_h2_input_kept}")
    target_sources(xbox_input PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/overrides/xinput_device.c")
    target_include_directories(xbox_input PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/overrides"
        "${HALO2_SDL3_ROOT}/include")
    target_link_libraries(xbox_input PUBLIC "${HALO2_SDL3_ROOT}/lib/x64/SDL3.lib")
    target_compile_definitions(xbox_input PRIVATE HALO2_SDL3=1)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HALO2_SDL3=1)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HALO2_SDL3_ROOT}/lib/x64/SDL3.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
endif()


# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    d3dcompiler
    bcrypt
    dxgi
    dxguid
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
    winmm
    shell32
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)
if(RECOMP_DEBUG_TOOLS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RECOMP_DEBUG_TOOLS=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE windowscodecs ole32)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /MP8)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )

# -- PERF (analysis/perf): RelWithDebInfo defaults to /Ob1 (only explicit inline).
# Full inlining for the GPU back end and the toolkit's pushbuffer/kernel code.
# Configure with -DHALO2_PERF_OB2=OFF to go back.
option(HALO2_PERF_OB2 "Use /Ob2 for the GPU back end and toolkit kernel" ON)
if(MSVC AND HALO2_PERF_OB2)
    set_source_files_properties(src/gpu/d3d11_backend.c src/gpu/texture_decode.c
        src/gpu/texture_sampler.c src/gpu/render_scale.c PROPERTIES COMPILE_OPTIONS "/Ob2")
    if(TARGET xbox_kernel)
        target_compile_options(xbox_kernel PRIVATE /Ob2)
    endif()
endif()

# -- PERF round 3 (analysis/perf): whole-program optimisation for the GPU back
# end and the toolkit runtime libraries. The generated recomp code stays out of
# it (link time). On by default (measured -1.8 ms GPU-thread busy, l1..l5 runs);
# configure -DHALO2_PERF_LTO=OFF to go back.
option(HALO2_PERF_LTO "/GL + /LTCG for the GPU back end and toolkit libraries" ON)
if(MSVC AND HALO2_PERF_LTO)
    set_property(SOURCE src/gpu/d3d11_backend.c src/gpu/texture_decode.c
        src/gpu/texture_sampler.c src/gpu/render_scale.c APPEND PROPERTY COMPILE_OPTIONS "/GL")
    foreach(t xbox_kernel xbox_nv2a xbox_apu xbox_dsound xbox_d3d8 xbox_video xbox_usb xbox_input)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE /GL)
            set_property(TARGET ${t} APPEND PROPERTY STATIC_LIBRARY_OPTIONS /LTCG)
        endif()
    endforeach()
    target_link_options(${PROJECT_NAME} PRIVATE /LTCG /INCREMENTAL:NO)
endif()

# Halo 2 Native 1.5 settings editor (Video tab). Same ini as the F1 overlay.
add_executable(halo2_config WIN32 native/config/config.c)
set_target_properties(halo2_config PROPERTIES OUTPUT_NAME "Config")
target_link_libraries(halo2_config PRIVATE user32 gdi32 comctl32 shell32)
add_custom_command(TARGET halo2_config POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:halo2_config>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
