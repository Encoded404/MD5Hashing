find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)

function(target_clang_tidy TARGET_NAME)
    if(NOT CLANG_TIDY_EXECUTABLE)
        message(WARNING "Clang-Tidy not configured: 'clang-tidy' executable not found.")
        return()
    endif()
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/.clang-tidy")
        message(STATUS "Clang-Tidy disabled for '${TARGET_NAME}': no .clang-tidy in ${PROJECT_SOURCE_DIR}.")
        return()
    endif()
    set_target_properties(${TARGET_NAME} PROPERTIES
            CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE};-p=${CMAKE_BINARY_DIR}"
    )
    message(STATUS "Clang-Tidy enabled for target '${TARGET_NAME}'.")
endfunction()