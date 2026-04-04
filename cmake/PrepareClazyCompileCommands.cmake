if (NOT DEFINED INPUT_FILE)
    message(FATAL_ERROR "INPUT_FILE is required")
endif()

if (NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

file(READ "${INPUT_FILE}" compile_commands)

set(problematic_flags
    " -mno-direct-extern-access"
)

foreach(flag IN LISTS problematic_flags)
    string(REPLACE "${flag}" "" compile_commands "${compile_commands}")
endforeach()

cmake_path(GET OUTPUT_FILE PARENT_PATH output_directory)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT_FILE}" "${compile_commands}")
