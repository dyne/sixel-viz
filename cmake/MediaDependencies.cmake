set(FFmpeg_ROOT "" CACHE PATH "FFmpeg development SDK prefix")
set(LibSixel_ROOT "" CACHE PATH "libsixel development SDK prefix")

function(import_sdk_library target sdk component header library)
  set(search_args)
  if(${sdk}_ROOT)
    list(APPEND search_args HINTS "${${sdk}_ROOT}" NO_DEFAULT_PATH)
    # An explicit prefix takes precedence over a previously found system SDK.
    unset(${component}_INCLUDE_DIR CACHE)
    unset(${component}_LIBRARY CACHE)
  endif()
  find_path(${component}_INCLUDE_DIR NAMES "${header}"
    ${search_args} PATH_SUFFIXES include include/sixel "include/${CMAKE_LIBRARY_ARCHITECTURE}")
  find_library(${component}_LIBRARY NAMES "${library}" "lib${library}"
    ${search_args} PATH_SUFFIXES lib lib64 "lib/${CMAKE_LIBRARY_ARCHITECTURE}")
  if(NOT ${component}_INCLUDE_DIR OR NOT ${component}_LIBRARY)
    message(FATAL_ERROR
      "Required ${sdk} development SDK component ${component} was not found "
      "(need ${header} and ${library}). Set ${sdk}_ROOT to its installed prefix.")
  endif()
  add_library(${target} UNKNOWN IMPORTED)
  set_target_properties(${target} PROPERTIES
    IMPORTED_LOCATION "${${component}_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${${component}_INCLUDE_DIR}")
  mark_as_advanced(${component}_INCLUDE_DIR ${component}_LIBRARY)
endfunction()

if(PkgConfig_FOUND AND NOT FFmpeg_ROOT)
  pkg_check_modules(FFmpeg QUIET IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
endif()
add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
if(TARGET PkgConfig::FFmpeg)
  target_link_libraries(FFmpeg::FFmpeg INTERFACE PkgConfig::FFmpeg)
else()
  import_sdk_library(FFmpeg::avformat FFmpeg avformat libavformat/avformat.h avformat)
  import_sdk_library(FFmpeg::avcodec FFmpeg avcodec libavcodec/avcodec.h avcodec)
  import_sdk_library(FFmpeg::swscale FFmpeg swscale libswscale/swscale.h swscale)
  import_sdk_library(FFmpeg::avutil FFmpeg avutil libavutil/avutil.h avutil)
  target_link_libraries(FFmpeg::FFmpeg INTERFACE
    FFmpeg::avformat FFmpeg::avcodec FFmpeg::swscale FFmpeg::avutil)
endif()

if(PkgConfig_FOUND AND NOT LibSixel_ROOT)
  pkg_check_modules(LibSixel QUIET IMPORTED_TARGET libsixel)
endif()
if(TARGET PkgConfig::LibSixel)
  add_library(LibSixel::LibSixel ALIAS PkgConfig::LibSixel)
else()
  import_sdk_library(LibSixel::LibSixel LibSixel libsixel sixel.h sixel)
endif()
