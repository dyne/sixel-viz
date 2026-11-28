cmake_minimum_required(VERSION 3.20)
project(sixel-viz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SI_BUILD_VERSION "" CACHE STRING "Version to report, in vN.N.N format")
if(SI_BUILD_VERSION STREQUAL "")
  find_package(Git QUIET)
  if(Git_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0 --match "v[0-9]*"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE source_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  endif()
  set(SI_BUILD_VERSION v0.0.0)
  if(source_version MATCHES "^v[0-9]+[.][0-9]+[.][0-9]+$")
    set(SI_BUILD_VERSION "${source_version}")
  endif()
elseif(NOT SI_BUILD_VERSION MATCHES "^v[0-9]+[.][0-9]+[.][0-9]+$")
  message(FATAL_ERROR "SI_BUILD_VERSION must be in vN.N.N format.")
endif()

set(SI_MUSL_SDK_VERSION "" CACHE STRING "Dyne musl SDK version used for this build")
if(SI_MUSL_SDK_VERSION STREQUAL "")
  set(SI_MUSL_SDK_VERSION "n/a")
elseif(NOT SI_MUSL_SDK_VERSION MATCHES "^[a-zA-Z0-9.+_-]+$")
  message(FATAL_ERROR "SI_MUSL_SDK_VERSION must be a version label.")
endif()
configure_file(src/build_version.hpp.in generated/build_version.hpp @ONLY)

include(GNUInstallDirs)
find_package(PkgConfig QUIET)
include(cmake/MediaDependencies.cmake)

add_library(si_media STATIC src/media.cpp src/exif.cpp)
target_include_directories(si_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(si_media PUBLIC FFmpeg::FFmpeg)

add_library(si_sixel STATIC src/sixel.cpp)
target_include_directories(si_sixel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(si_sixel PRIVATE LibSixel::LibSixel)

add_library(si_terminal STATIC src/terminal.cpp src/terminal_playback.cpp)
target_include_directories(si_terminal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(WIN32)
  target_link_libraries(si_terminal PRIVATE user32)
endif()

add_library(si_fit STATIC src/fit.cpp)
target_include_directories(si_fit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(si_fit PUBLIC FFmpeg::FFmpeg PRIVATE si_sixel)
target_link_libraries(si_terminal PRIVATE si_fit si_sixel)

add_library(si_playback STATIC src/playback.cpp)
target_link_libraries(si_playback PUBLIC si_media)

add_executable(si src/main.cpp src/gallery.cpp src/gallery_input.cpp)
target_include_directories(si PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(si PRIVATE si_playback si_sixel si_terminal si_fit)
if(MINGW)
  target_link_options(si PRIVATE -municode)
endif()
install(TARGETS si RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
