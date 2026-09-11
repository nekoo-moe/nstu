cmake_minimum_required(VERSION 3.25)

project(nstu
    VERSION 0.1.0
    DESCRIPTION "Low-latency classroom lab management and screen broadcasting"
    LANGUAGES CXX
)

option(NSTU_BUILD_CLIENT "Build the Windows client" ON)
option(NSTU_BUILD_SERVER "Build the server UI" ON)
option(NSTU_BUILD_SETUP "Build the unified-installer diagnostics helper" ON)
option(NSTU_BUILD_TESTS "Build unit and integration tests" ON)
option(NSTU_BUILD_VIDEO "Build DXGI and Media Foundation components" ON)
option(NSTU_ENABLE_WERROR "Treat compiler warnings as errors" OFF)
option(NSTU_ENABLE_PACKAGING "Enable unified Windows NSIS packaging" ON)
option(NSTU_INTERNAL_TEST_BUILD
    "Build an explicitly labeled installer that permits undersized VM hardware"
    OFF)

if(NSTU_INTERNAL_TEST_BUILD)
    set(NSTU_INTERNAL_TEST_BUILD_VALUE 1)
    set(NSTU_INSTALLER_DISPLAY_NAME "NSTU Internal VM Test")
    set(NSTU_INSTALLER_FILE_SUFFIX "-internal-vm")
    set(NSTU_INSTALLER_CHANNEL "InternalVmTest")
    set(NSTU_INSTALLER_ROLE_SUFFIX " (Internal VM Test)")
else()
    set(NSTU_INTERNAL_TEST_BUILD_VALUE 0)
    set(NSTU_INSTALLER_DISPLAY_NAME "NSTU")
    set(NSTU_INSTALLER_FILE_SUFFIX "")
    set(NSTU_INSTALLER_CHANNEL "Release")
    set(NSTU_INSTALLER_ROLE_SUFFIX "")
endif()

# Keep release installers self-contained on clean school images.  The static
# MSVC runtime avoids requiring a separately deployed Visual C++ Redistributable
# before the installer diagnostics helper can even start.
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        CACHE STRING "MSVC runtime linkage" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    enable_language(RC)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NstuWarnings)
include(NstuSigning)

add_subdirectory(common)

if(WIN32 AND NSTU_BUILD_VIDEO)
    add_subdirectory(video)
endif()

if(NSTU_BUILD_CLIENT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/client/CMakeLists.txt")
    add_subdirectory(client)
endif()

if(NSTU_BUILD_SERVER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server/CMakeLists.txt")
    add_subdirectory(server)
endif()

if(WIN32 AND NSTU_BUILD_SETUP AND EXISTS
   "${CMAKE_CURRENT_SOURCE_DIR}/setup/CMakeLists.txt")
    add_subdirectory(setup)
endif()

if(NSTU_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

foreach(nstu_signable_target
        nstu-server nstu-service nstu-agent nstu-provision nstu-diagnostics
        nstu_video_probe)
    nstu_sign_target(${nstu_signable_target})
endforeach()

if(WIN32 AND NSTU_ENABLE_PACKAGING)
    include(GNUInstallDirs)

    set(nstu_mingw_runtime_files)
    set(NSTU_MINGW_RUNTIME_FILES_PRESENT 0)
    if(MINGW)
        set(NSTU_MINGW_RUNTIME_FILES_PRESENT 1)
        get_filename_component(nstu_compiler_bin_dir
            "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(nstu_runtime_name IN ITEMS
                libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
            set(nstu_runtime_path
                "${nstu_compiler_bin_dir}/${nstu_runtime_name}")
            if(NOT EXISTS "${nstu_runtime_path}")
                execute_process(
                    COMMAND "${CMAKE_CXX_COMPILER}"
                        "-print-file-name=${nstu_runtime_name}"
                    OUTPUT_VARIABLE nstu_runtime_path
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE nstu_runtime_lookup_result)
                if(NOT IS_ABSOLUTE "${nstu_runtime_path}")
                    set(nstu_runtime_path
                        "${nstu_compiler_bin_dir}/${nstu_runtime_path}")
                endif()
            endif()
            if(NOT EXISTS "${nstu_runtime_path}")
                message(FATAL_ERROR
                    "Required MinGW runtime was not found: ${nstu_runtime_name}")
            endif()
            list(APPEND nstu_mingw_runtime_files "${nstu_runtime_path}")
        endforeach()
    endif()

    if(TARGET nstu-server)
        install(TARGETS nstu-server RUNTIME DESTINATION server COMPONENT server)
        if(nstu_mingw_runtime_files)
            install(FILES ${nstu_mingw_runtime_files} DESTINATION server COMPONENT server)
        endif()
    endif()
    if(TARGET nstu-diagnostics)
        install(TARGETS nstu-diagnostics
            RUNTIME DESTINATION diagnostics COMPONENT diagnostics)
        if(nstu_mingw_runtime_files)
            install(FILES ${nstu_mingw_runtime_files}
                DESTINATION diagnostics
                COMPONENT diagnostics)
        endif()
    endif()
    if(TARGET nstu-service)
        install(TARGETS nstu-service nstu-agent nstu-provision
            RUNTIME DESTINATION client
            COMPONENT client)
        install(FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/install-client-service.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/uninstall-client-service.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/configure-data-root.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/test-system-setup.ps1"
            DESTINATION client
            COMPONENT client)
        if(nstu_mingw_runtime_files)
            install(FILES ${nstu_mingw_runtime_files}
                DESTINATION client
                COMPONENT client)
        endif()
    endif()
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.vi.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        DESTINATION docs
        COMPONENT docs)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/configure-data-root.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/new-enrollment-secret.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/test-production-deployment.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/test-system-setup.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/test-install-role.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/test-update-cycle.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/stage-uninstall.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/finalize-uninstall.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/uninstall-server.ps1"
        DESTINATION docs/deployment
        COMPONENT docs)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs/"
        DESTINATION docs/project
        COMPONENT docs)

    set(NSTU_UNIFIED_STAGE_DIR "${CMAKE_BINARY_DIR}/unified-installer-stage")
    set(NSTU_UNIFIED_INSTALLER
        "${CMAKE_BINARY_DIR}/nstu-${PROJECT_VERSION}${NSTU_INSTALLER_FILE_SUFFIX}-setup.exe")
    set(NSTU_PACKAGE_INSTALL_CONFIG "Release" CACHE STRING
        "Configuration staged by the unified installer target")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/nstu-installer.nsi.in"
        "${CMAKE_BINARY_DIR}/nstu-installer.nsi" @ONLY)
    find_program(NSTU_MAKENSIS_EXECUTABLE makensis)
    if(NSTU_MAKENSIS_EXECUTABLE)
        set(nstu_unified_dependencies)
        foreach(nstu_target IN ITEMS nstu-server nstu-service nstu-agent
                nstu-provision nstu-diagnostics)
            if(TARGET ${nstu_target})
                list(APPEND nstu_unified_dependencies ${nstu_target})
            endif()
        endforeach()
        add_custom_target(nstu-package-build
            COMMAND "${CMAKE_COMMAND}"
                -DNSTU_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DNSTU_STAGE_DIR=${NSTU_UNIFIED_STAGE_DIR}
                -DNSTU_INSTALL_CONFIG=${NSTU_PACKAGE_INSTALL_CONFIG}
                -DNSTU_INSTALLER_CONFIG=${CMAKE_BINARY_DIR}/nstu-installer.nsi
                -P "${CMAKE_CURRENT_SOURCE_DIR}/packaging/PrepareUnifiedPackage.cmake"
            COMMAND "${NSTU_MAKENSIS_EXECUTABLE}"
                /WX
                "${CMAKE_BINARY_DIR}/nstu-installer.nsi"
            DEPENDS ${nstu_unified_dependencies}
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Building unified NSTU role-selecting installer")
        if(NSTU_ENABLE_SIGNING)
            add_custom_target(nstu-package
                COMMAND "${NSTU_SIGNTOOL_EXECUTABLE}" sign
                    /sha1 "${NSTU_SIGN_CERT_SHA1}"
                    /fd SHA256
                    /tr "${NSTU_SIGN_TIMESTAMP_URL}"
                    /td SHA256
                    "${NSTU_UNIFIED_INSTALLER}"
                DEPENDS nstu-package-build
                WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                COMMENT "Signing unified NSTU installer"
                VERBATIM)
        else()
            add_custom_target(nstu-package DEPENDS nstu-package-build)
        endif()
    else()
        message(WARNING "makensis was not found; nstu-package will not be available")
    endif()
endif()
