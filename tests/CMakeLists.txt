add_executable(sdk_smoke sdk_smoke.cpp)
target_link_libraries(sdk_smoke PRIVATE xtd.core FFmpeg::FFmpeg LibSixel::LibSixel)
add_test(NAME sdk.smoke COMMAND sdk_smoke)
set_tests_properties(sdk.smoke PROPERTIES LABELS cli)

add_executable(sdk_decode sdk_decode.cpp)
target_link_libraries(sdk_decode PRIVATE FFmpeg::FFmpeg)
foreach(format IN ITEMS svg gif)
  add_test(NAME sdk.decode.${format}
    COMMAND sdk_decode ${format} "two-colors.${format}")
  set_tests_properties(sdk.decode.${format} PROPERTIES
    LABELS media
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
endforeach()

add_executable(media_decode media_decode.cpp)
target_link_libraries(media_decode PRIVATE si_media)

set(fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(literal "${CMAKE_CURRENT_BINARY_DIR}/literal")
file(MAKE_DIRECTORY "${literal}")
foreach(name IN ITEMS renamed.bin frame%03d.png frame001.png frame002.png missing001.png)
  configure_file("${fixtures}/alpha.png" "${literal}/${name}" COPYONLY)
endforeach()

function(add_media_case name kind input)
  add_test(NAME media.${name} COMMAND media_decode "${kind}" "${input}")
  set_tests_properties(media.${name} PROPERTIES LABELS media)
endfunction()

add_media_case(png png "${fixtures}/alpha.png")
add_media_case(renamed png "${literal}/renamed.bin")
add_media_case(literal_percent png "${literal}/frame%03d.png")
add_media_case(jpeg jpeg "${fixtures}/oriented.jpg")
add_media_case(mirrored_jpeg mirrored "${fixtures}/mirrored.jpg")
add_media_case(svg_alpha svg_alpha "${fixtures}/alpha-edge.svg")
add_media_case(limited_yuv limited_yuv "${fixtures}/limited-yuv.mkv")
add_media_case(covered_video covered "${fixtures}/covered.mp4")
foreach(format IN ITEMS bmp tiff webp gif svg)
  add_media_case("${format}" "${format}" "${fixtures}/two-colors.${format}")
endforeach()
foreach(format IN ITEMS ppm qoi jxl)
  add_media_case("optional_${format}" "${format}" "${fixtures}/two-colors.${format}")
  set_tests_properties(media.optional_${format} PROPERTIES
    LABELS "media;media-optional" SKIP_RETURN_CODE 77)
endforeach()

function(add_media_error name input diagnostic)
  add_test(NAME media.${name}
    COMMAND "${CMAKE_COMMAND}" "-Dprogram=$<TARGET_FILE:si>"
      "-Dinput=${input}" "-Ddiagnostic=${diagnostic}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/media_error.cmake")
  set_tests_properties(media.${name} PROPERTIES LABELS media)
endfunction()
add_media_error(corrupt_input "${fixtures}/corrupt.bin" "si: .*probe media format")
add_media_error(audio_only "${fixtures}/audio-only.wav" "si: .*no video or image stream")
add_media_error(secondary_input "${fixtures}/secondary-input.ffconcat" "si: .*open media")
add_media_error(missing_pattern "${literal}/missing%03d.png" "si: cannot open")

add_executable(sixel_test sixel_test.cpp)
target_link_libraries(sixel_test PRIVATE si_sixel)
foreach(case IN ITEMS padded short_write midframe_failure flush_failure bad_buffer multiple_bands)
  add_test(NAME sixel.${case} COMMAND sixel_test ${case})
  set_tests_properties(sixel.${case} PROPERTIES LABELS sixel)
endforeach()

add_test(NAME sixel.round_trip COMMAND "${CMAKE_COMMAND}"
  "-Dcase_name=still" "-Dprogram=$<TARGET_FILE:si>" "-Dverify=$<TARGET_FILE:sixel_test>"
  "-Dwork=${CMAKE_CURRENT_BINARY_DIR}/sixel-round-trip"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_test.cmake")
set_tests_properties(sixel.round_trip PROPERTIES LABELS sixel)

if(UNIX)
  add_test(NAME sixel.closed_pipe
    COMMAND sixel_test closed_pipe "$<TARGET_FILE:si>" "${fixtures}/two-colors.gif")
  set_tests_properties(sixel.closed_pipe PROPERTIES LABELS sixel TIMEOUT 10)
endif()

add_test(NAME sdk.missing
  COMMAND "${CMAKE_COMMAND}"
    "-Dsource=${PROJECT_SOURCE_DIR}"
    "-Dbuild=${CMAKE_CURRENT_BINARY_DIR}/missing-sdk"
    "-Dgenerator=${CMAKE_GENERATOR}"
    "-Dplatform=${CMAKE_GENERATOR_PLATFORM}"
    "-Dtoolset=${CMAKE_GENERATOR_TOOLSET}"
    "-Dtoolchain=${CMAKE_TOOLCHAIN_FILE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/missing_sdk.cmake")
set_tests_properties(sdk.missing PROPERTIES LABELS cli)

foreach(case IN ITEMS help no_operand extra_operands unknown_option missing_file
                      directory spaces unicode end_options)
  add_test(NAME cli.${case}
    COMMAND "${CMAKE_COMMAND}"
      "-Dcase_name=${case}" "-Dprogram=$<TARGET_FILE:si>"
      "-Dverify=$<TARGET_FILE:sixel_test>"
      "-Dwork=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/${case}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_test.cmake")
  set_tests_properties(cli.${case} PROPERTIES LABELS cli)
endforeach()

add_test(NAME install.help
  COMMAND "${CMAKE_COMMAND}"
    -Dcase_name=help "-Dbuild=${PROJECT_BINARY_DIR}" "-Dconfig=$<CONFIG>"
    "-Dwork=${CMAKE_CURRENT_BINARY_DIR}/install-test"
    "-Dbindir=${CMAKE_INSTALL_BINDIR}" "-Dexe_suffix=${CMAKE_EXECUTABLE_SUFFIX}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_test.cmake")
set_tests_properties(install.help PROPERTIES LABELS install)
