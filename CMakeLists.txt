cmake_minimum_required(VERSION 3.24)
project(CalorieTrackerGUI CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# SDL3
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.2.10
  FIND_PACKAGE_ARGS 3.2 CONFIG)  # use system SDL3 if found, else download

# Dear ImGui (no CMakeLists upstream; we build it below)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9)

# nlohmann/json
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  FIND_PACKAGE_ARGS NAMES nlohmann_json)  # use system copy if found, else download

FetchContent_MakeAvailable(SDL3 imgui json)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3)

# libcurl: system package (apt install libcurl4-openssl-dev / brew install curl / vcpkg install curl)
find_package(CURL REQUIRED)

add_executable(calorie_tracker_gui main.cpp)
target_link_libraries(calorie_tracker_gui PRIVATE imgui_lib SDL3::SDL3 nlohmann_json::nlohmann_json CURL::libcurl)
target_compile_definitions(calorie_tracker_gui PRIVATE
    FDC_FONT_PATH="${imgui_SOURCE_DIR}/misc/fonts/DroidSans.ttf")
