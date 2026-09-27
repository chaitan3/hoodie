cmake_minimum_required(VERSION 3.24)
project(hoodie VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HOODIE_BUILD_TESTS "Build hoodie tests" OFF)

include(cmake/deps.cmake)
hoodie_setup_deps()

add_executable(hoodie
  src/main.cpp
  src/app/config.cpp
  src/app/api_commands.cpp
  src/app/assistant.cpp
  src/app/playback.cpp
  src/util/logging.cpp
  src/audio/capture.cpp
  src/audio/devices.cpp
  src/audio/player.cpp
  src/audio/resampler.cpp
  src/audio/wav.cpp
  src/net/http.cpp
  src/api/asr_client.cpp
  src/api/tts_client.cpp
  src/api/llm_client.cpp
  src/api/openai_types.cpp
  src/api/clients.cpp
  src/ml/onnx.cpp
  src/wake/wakeword.cpp
  src/wake/vad.cpp
  src/app/wake_commands.cpp
  src/util/process.cpp
  src/agent/text_normalize.cpp
  src/agent/tool_registry.cpp
  src/agent/tools.cpp
  src/agent/agent.cpp
)

target_include_directories(hoodie PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hoodie PRIVATE hoodie::deps)

if(MSVC)
  target_compile_options(hoodie PRIVATE /W4)
else()
  target_compile_options(hoodie PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS hoodie RUNTIME DESTINATION bin)
install(FILES config/hoodie.example.json DESTINATION share/hoodie)

if(HOODIE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
