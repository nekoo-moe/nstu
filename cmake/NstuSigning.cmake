option(NSTU_ENABLE_SIGNING "Sign Windows binaries and installers" OFF)
set(NSTU_SIGNTOOL_EXECUTABLE "" CACHE FILEPATH "Path to signtool.exe")
set(NSTU_SIGN_CERT_SHA1 "" CACHE STRING "Signing certificate SHA-1 thumbprint")
set(NSTU_SIGN_TIMESTAMP_URL "http://timestamp.digicert.com" CACHE STRING
    "RFC 3161 timestamp service URL")

if(NSTU_ENABLE_SIGNING)
    if(NOT WIN32)
        message(FATAL_ERROR "NSTU signing is supported only on Windows")
    endif()
    if(NOT NSTU_SIGNTOOL_EXECUTABLE)
        find_program(NSTU_SIGNTOOL_EXECUTABLE signtool REQUIRED)
    endif()
    if(NOT EXISTS "${NSTU_SIGNTOOL_EXECUTABLE}")
        message(FATAL_ERROR "NSTU_SIGNTOOL_EXECUTABLE does not exist")
    endif()
    if(NOT NSTU_SIGN_CERT_SHA1)
        message(FATAL_ERROR "NSTU_SIGN_CERT_SHA1 is required for signing")
    endif()
endif()

function(nstu_sign_target target)
    if(NSTU_ENABLE_SIGNING AND TARGET ${target})
        set(signing_target "nstu-sign-${target}")
        add_custom_target(${signing_target} ALL
            COMMAND "${NSTU_SIGNTOOL_EXECUTABLE}" sign
                /sha1 "${NSTU_SIGN_CERT_SHA1}"
                /fd SHA256
                /tr "${NSTU_SIGN_TIMESTAMP_URL}"
                /td SHA256
                "$<TARGET_FILE:${target}>"
            DEPENDS ${target}
            VERBATIM
            COMMENT "Signing ${target}")
    endif()
endfunction()
