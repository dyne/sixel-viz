set(generator_args -G "${generator}")
if(platform)
  list(APPEND generator_args -A "${platform}")
endif()
if(toolset)
  list(APPEND generator_args -T "${toolset}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${build}" ${generator_args}
    "-DCMAKE_TOOLCHAIN_FILE=${toolchain}" "-DFFmpeg_ROOT=${build}/missing-ffmpeg"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)

if(NOT status MATCHES "^[1-9][0-9]*$" OR
   NOT error MATCHES "Required FFmpeg development SDK component avformat was not found")
  message(FATAL_ERROR "Expected CMake to identify the missing FFmpeg SDK.\n${output}\n${error}")
endif()
