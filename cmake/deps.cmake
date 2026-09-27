# Third-party dependencies.
#
# Policy: heavy/system libraries (PortAudio, libsndfile, soxr, libcurl,
# onnxruntime) come from the platform package manager via find_package /
# pkg-config, because that is the sane path on Raspberry Pi OS and Arch. Only
# small header-only / easily-embedded libraries are fetched.

include(FetchContent)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

function(hoodie_fetch_deps)
  # --- spdlog (logging) -----------------------------------------------------
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.3
    GIT_SHALLOW    TRUE)

  # --- nlohmann/json (config + API payloads) --------------------------------
  # Populate only; skip nlohmann's own CMakeLists (it declares an old
  # cmake_minimum_required that newer CMake rejects). We expose the headers as
  # an INTERFACE target ourselves.
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  .hoodie-skip-cmake)

  FetchContent_MakeAvailable(spdlog nlohmann_json)

  add_library(hoodie_nlohmann_json INTERFACE)
  target_include_directories(hoodie_nlohmann_json INTERFACE
    ${nlohmann_json_SOURCE_DIR}/include
    ${nlohmann_json_SOURCE_DIR}/single_include)
endfunction()

function(hoodie_find_system_deps)
  # --- audio I/O + DSP (provided by the platform) ---------------------------
  pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
  pkg_check_modules(SNDFILE  REQUIRED IMPORTED_TARGET sndfile)
  pkg_check_modules(SOXR     REQUIRED IMPORTED_TARGET soxr)
  # --- HTTP (provided by the platform) --------------------------------------
  pkg_check_modules(LIBCURL  REQUIRED IMPORTED_TARGET libcurl)

  # --- ONNX Runtime (wake word + VAD; no pkg-config on Arch) ----------------
  # Hints let a non-system install work: -DONNXRUNTIME_ROOT=/opt/onnxruntime
  # (or export ONNXRUNTIME_ROOT). Headers must live under <root>/include/onnxruntime/.
  find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime/onnxruntime_cxx_api.h
    HINTS ${ONNXRUNTIME_ROOT} $ENV{ONNXRUNTIME_ROOT}
    PATH_SUFFIXES include)
  find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime
    HINTS ${ONNXRUNTIME_ROOT} $ENV{ONNXRUNTIME_ROOT}
    PATH_SUFFIXES lib lib64)
  if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
    message(FATAL_ERROR
      "onnxruntime not found. Install onnxruntime-cpu (Arch), or run "
      "scripts/install_onnxruntime.sh (Raspberry Pi / Debian), or set ONNXRUNTIME_ROOT.")
  endif()
  message(STATUS "onnxruntime: ${ONNXRUNTIME_LIBRARY} (include ${ONNXRUNTIME_INCLUDE_DIR})")
  add_library(hoodie_onnxruntime INTERFACE)
  target_include_directories(hoodie_onnxruntime INTERFACE ${ONNXRUNTIME_INCLUDE_DIR})
  target_link_libraries(hoodie_onnxruntime INTERFACE ${ONNXRUNTIME_LIBRARY})
endfunction()

# Entry point: call once from the top-level CMakeLists.
function(hoodie_setup_deps)
  hoodie_fetch_deps()
  hoodie_find_system_deps()

  add_library(hoodie_deps INTERFACE)
  target_link_libraries(hoodie_deps INTERFACE
    spdlog::spdlog
    hoodie_nlohmann_json
    PkgConfig::PORTAUDIO
    PkgConfig::SNDFILE
    PkgConfig::SOXR
    PkgConfig::LIBCURL
    hoodie_onnxruntime
    Threads::Threads)
  add_library(hoodie::deps ALIAS hoodie_deps)
endfunction()

