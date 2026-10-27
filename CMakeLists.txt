cmake_minimum_required(VERSION 3.25)
project(jmcomic VERSION 1.0.0 LANGUAGES C CXX)

include(cmake/Dependencies.cmake)

add_executable(jmcomic
  src/main.cpp
  src/models.cpp
  src/storage.cpp
  src/api/client.cpp
  src/api/http.cpp
  src/auth/crypto.cpp
  src/downloader/manager.cpp
  src/image/image.cpp
  src/tui/app.cpp
)
target_compile_features(jmcomic PRIVATE cxx_std_23)
set_target_properties(jmcomic PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(jmcomic PRIVATE include external/stb external/libwebp/src)
target_link_libraries(jmcomic PRIVATE
  nlohmann_json::nlohmann_json
  Threads::Threads
  OpenSSL::Crypto
  CURL::libcurl_static
  webpdemux
  ftxui::component
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(jmcomic PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  set_source_files_properties(src/image/image.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-conversion;-Wno-missing-field-initializers")
endif()

install(TARGETS jmcomic RUNTIME DESTINATION bin)
