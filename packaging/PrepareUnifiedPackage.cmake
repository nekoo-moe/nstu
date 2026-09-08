if(NOT DEFINED NSTU_BUILD_DIR OR NOT DEFINED NSTU_STAGE_DIR)
    message(FATAL_ERROR "NSTU_BUILD_DIR and NSTU_STAGE_DIR are required")
endif()

set(nstu_install_config_args)
if(DEFINED NSTU_INSTALL_CONFIG AND NOT NSTU_INSTALL_CONFIG STREQUAL "")
    list(APPEND nstu_install_config_args --config "${NSTU_INSTALL_CONFIG}")
endif()

file(REMOVE_RECURSE "${NSTU_STAGE_DIR}")
file(MAKE_DIRECTORY "${NSTU_STAGE_DIR}")
foreach(component IN ITEMS client server diagnostics docs)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${NSTU_BUILD_DIR}"
            --prefix "${NSTU_STAGE_DIR}" --component "${component}"
            ${nstu_install_config_args}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Installing component ${component} failed: ${output}${error}")
    endif()
endforeach()
