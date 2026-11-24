cmake_minimum_required(VERSION 3.13)
project(moonlight-tizen4-bridge LANGUAGES C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(WIN32)
  # Ship a single self-contained .exe on Windows.
  set(OPENSSL_USE_STATIC_LIBS TRUE)
  add_compile_definitions(_WIN32_WINNT=0x0601)
endif()

# Moonlight core protocol library (git submodule)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/moonlight-common-c moonlight-common-c)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED opus)

add_executable(moonlight-bridge
  src/main.c
  src/platform.c
  src/http.c
  src/json.c
  src/gamestream.c
  src/fmp4.c
  src/ws.c
  src/stream.c
)

target_include_directories(moonlight-bridge PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/moonlight-common-c/src
  ${OPUS_INCLUDE_DIRS}
)
target_link_directories(moonlight-bridge PRIVATE ${OPUS_LIBRARY_DIRS})
target_link_libraries(moonlight-bridge PRIVATE
  moonlight-common-c OpenSSL::SSL OpenSSL::Crypto ${OPUS_LIBRARIES} Threads::Threads m)

if(WIN32)
  target_link_libraries(moonlight-bridge PRIVATE ws2_32 winmm crypt32 iphlpapi bcrypt)
  target_link_options(moonlight-bridge PRIVATE -static)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(moonlight-bridge PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
