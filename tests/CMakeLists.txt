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
