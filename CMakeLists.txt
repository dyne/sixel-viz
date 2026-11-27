cmake_minimum_required(VERSION 3.20)
project(sixel-viz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(xtd CONFIG REQUIRED)
if(NOT TARGET xtd.core)
  message(FATAL_ERROR "The resolved xtd SDK does not export xtd.core.")
endif()
# The xtd package's CMake helpers set a directory-wide C++ standard.
set(CMAKE_CXX_STANDARD 20)

include(GNUInstallDirs)
find_package(PkgConfig QUIET)
include(cmake/MediaDependencies.cmake)

add_library(si_media STATIC src/media.cpp)
target_include_directories(si_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(si_media PUBLIC FFmpeg::FFmpeg)

add_library(si_sixel STATIC src/sixel.cpp)
target_include_directories(si_sixel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(si_sixel PRIVATE LibSixel::LibSixel)

add_executable(si src/main.cpp)
target_link_libraries(si PRIVATE xtd.core si_media si_sixel)
if(MINGW)
  target_link_options(si PRIVATE -municode)
endif()
install(TARGETS si RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
