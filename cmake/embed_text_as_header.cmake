if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE OR NOT DEFINED VAR_NAME)
    message(FATAL_ERROR "INPUT_FILE, OUTPUT_FILE, and VAR_NAME are required")
endif()

file(READ "${INPUT_FILE}" CONTENTS)

# Keep leading/trailing newlines consistent for stable diffs in generated headers.
set(HEADER "#pragma once\n\ninline constexpr const char ${VAR_NAME}[] = R\"MGVWR_EMBED(\n${CONTENTS}\n)MGVWR_EMBED\";\n")

file(WRITE "${OUTPUT_FILE}" "${HEADER}")
