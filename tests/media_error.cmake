execute_process(
  COMMAND "${program}" "${input}"
  RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err ENCODING NONE)
if(NOT status STREQUAL "1" OR NOT out STREQUAL "" OR NOT err MATCHES "${diagnostic}")
  message(FATAL_ERROR
    "'${input}' exited '${status}' (expected 1)\n"
    "stdout [${out}] (expected empty)\n"
    "stderr [${err}] (expected to match '${diagnostic}')")
endif()
message(STATUS "'${input}' exited 1 with empty stdout and diagnostic [${err}]")
