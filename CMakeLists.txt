cmake_minimum_required(VERSION 3.24)
project(VC3Recomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/utf-8 /W3 /permissive- /Zc:preprocessor /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(VC2_BUILD_RUNTIME "Build the game runtime and launcher (downloads SDL3 / Dear ImGui)" ON)
set(VC2_GENERATED_DIR "" CACHE PATH "Developer use: directory with generated game C sources to build vc2game.exe directly")

# ---------------------------------------------------------------- core library (no third-party deps)
add_library(vc2core STATIC
  src/common/iso9660.cpp
  src/crypto/prx_decrypt.cpp
  src/loader/elf.cpp
  src/recomp/analysis.cpp
  src/recomp/disasm.cpp
  src/recomp/codegen.cpp
  src/recomp/pipeline.cpp
)
target_include_directories(vc2core PUBLIC src)

add_executable(vc2tool tools/vc2tool.cpp)
target_link_libraries(vc2tool PRIVATE vc2core)

if(NOT VC2_BUILD_RUNTIME)
  return()
endif()

# ---------------------------------------------------------------- third party
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.10/SDL3-3.2.10.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(SDL3 imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static opengl32)

# ---------------------------------------------------------------- build information
# Version, git commit, dates and compiler, regenerated on every build (cmake/BuildInfo.cmake) and
# compiled into the launcher and the runtime; also their Windows version resources.
set(VC3_PRODUCT "Valkyria Chronicles III Recompiled")
set(VC3_BUILDINFO_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(buildinfo ALL
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUT_DIR=${VC3_BUILDINFO_DIR}
          -DBASE_VERSION=${PROJECT_VERSION} "-DPRODUCT=${VC3_PRODUCT}"
          "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}" -DBUILD_TYPE=$<CONFIG>
          -DLAUNCHER_FILE=VC3Launcher.exe -DRUNTIME_FILE=vc3runtime.dll
          -P ${CMAKE_SOURCE_DIR}/cmake/BuildInfo.cmake
  BYPRODUCTS ${VC3_BUILDINFO_DIR}/build_info.cpp ${VC3_BUILDINFO_DIR}/launcher_version.rc
             ${VC3_BUILDINFO_DIR}/runtime_version.rc
  COMMENT "Build information"
  VERBATIM)

# ---------------------------------------------------------------- runtime DLL
file(GLOB_RECURSE VC2_RUNTIME_SOURCES CONFIGURE_DEPENDS src/runtime/*.cpp)
add_library(vc3runtime SHARED ${VC2_RUNTIME_SOURCES} ${VC3_BUILDINFO_DIR}/build_info.cpp
            ${VC3_BUILDINFO_DIR}/runtime_version.rc)
add_dependencies(vc3runtime buildinfo)
target_compile_definitions(vc3runtime PRIVATE VC2_RUNTIME_BUILD)
target_include_directories(vc3runtime PRIVATE src src/runtime)
target_link_libraries(vc3runtime PRIVATE vc2core imgui SDL3::SDL3-static opengl32 bcrypt winmm ole32 windowscodecs)
if(MSVC)
  target_compile_options(vc3runtime PRIVATE /O2 /Oi)
endif()

# SDK folder consumed by the launcher when it builds the game executable
set(VC2_SDK_DIR ${CMAKE_BINARY_DIR}/bin/sdk)
add_custom_command(TARGET vc3runtime POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${VC2_SDK_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_LINKER_FILE:vc3runtime> ${VC2_SDK_DIR}/vc3runtime.lib
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/runtime/psp_recomp.h ${VC2_SDK_DIR}/psp_recomp.h
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/runtime/vc2_runtime.h ${VC2_SDK_DIR}/vc2_runtime.h)

# ---------------------------------------------------------------- launcher
file(GLOB_RECURSE VC2_LAUNCHER_SOURCES CONFIGURE_DEPENDS src/launcher/*.cpp)
if(VC2_LAUNCHER_SOURCES)
  # host/config.cpp is shared with the runtime so both read/write vc3recomp.ini identically.
  add_executable(VC3Launcher WIN32 ${VC2_LAUNCHER_SOURCES} src/runtime/host/config.cpp
                 ${VC3_BUILDINFO_DIR}/build_info.cpp ${VC3_BUILDINFO_DIR}/launcher_version.rc)
  add_dependencies(VC3Launcher buildinfo)
  target_include_directories(VC3Launcher PRIVATE src src/launcher)
  target_link_libraries(VC3Launcher PRIVATE vc2core imgui SDL3::SDL3-static opengl32 bcrypt ole32 oleaut32 uuid shell32
                        windowscodecs comdlg32)
  if(MSVC)
    target_compile_options(VC3Launcher PRIVATE /O2)
    # per-monitor DPI awareness comes from SDL3; embed the default manifest
    target_link_options(VC3Launcher PRIVATE /MANIFEST:EMBED)
  endif()
  # the launcher needs the runtime DLL + SDK (sdk/) next to it at run time
  add_dependencies(VC3Launcher vc3runtime)
  # RecompLauncher setup file, shipped at the top of every release next to the launcher
  add_custom_command(TARGET VC3Launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/recomplauncher.json
            $<TARGET_FILE_DIR:VC3Launcher>/recomplauncher.json)
endif()

# ---------------------------------------------------------------- developer: build game from generated sources
if(VC2_GENERATED_DIR)
  file(GLOB VC2_GEN_SOURCES ${VC2_GENERATED_DIR}/*.c)
  add_executable(vc2game ${VC2_GEN_SOURCES})
  target_include_directories(vc2game PRIVATE src/runtime ${VC2_GENERATED_DIR})
  target_link_libraries(vc2game PRIVATE vc3runtime)
  set_source_files_properties(${VC2_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/O2;/W0")
  target_link_options(vc2game PRIVATE /STACK:8388608)
endif()

# ---------------------------------------------------------------- VFPU interpreter self-test
# Compiles the VFPU interpreter standalone with a stub guest memory / logger (see tools/vfpu_test.cpp).
add_executable(vfpu_test tools/vfpu_test.cpp src/runtime/cpu/vfpu.cpp)
target_compile_definitions(vfpu_test PRIVATE VC2_RUNTIME_BUILD)
target_include_directories(vfpu_test PRIVATE src src/runtime)
if(MSVC)
  target_compile_options(vfpu_test PRIVATE /O2)
endif()

# ---------------------------------------------------------------- movie (PSMF / H.264) self-test
# Demuxes PMF movies straight from the ISO and decodes frames to PNG (see tools/pmf_test.cpp).
# "pmf_test <iso> <outdir> atrac" also runs the sceAtrac3plus HLE module against a stub guest memory.
add_executable(pmf_test tools/pmf_test.cpp src/runtime/media/psmf.cpp src/runtime/media/h264_decoder.cpp
               src/runtime/media/atrac_decoder.cpp src/runtime/hle/atrac.cpp)
target_compile_definitions(pmf_test PRIVATE VC2_RUNTIME_BUILD)
target_include_directories(pmf_test PRIVATE src src/runtime)
target_link_libraries(pmf_test PRIVATE vc2core)
if(MSVC)
  target_compile_options(pmf_test PRIVATE /O2)
endif()

# ---------------------------------------------------------------- ATRAC3plus decoder self-test
# Decodes SND0.AT3 and PMF movie audio straight from the game ISOs with the runtime's own decoder
# (src/runtime/media/atrac3plus/) and writes WAVs for analysis (see tools/atrac_test.cpp).
file(GLOB VC2_ATRAC_SOURCES CONFIGURE_DEPENDS src/runtime/media/atrac3plus/*.cpp)
add_executable(atrac_test tools/atrac_test.cpp ${VC2_ATRAC_SOURCES} src/runtime/media/atrac_decoder.cpp
               src/runtime/media/psmf.cpp)
target_include_directories(atrac_test PRIVATE src src/runtime)
target_link_libraries(atrac_test PRIVATE vc2core)
if(MSVC)
  target_compile_options(atrac_test PRIVATE /O2)
endif()
