add_library(nstu_client_platform STATIC
    src/agent_protocol.cpp
    src/client_config.cpp
    src/client_control.cpp
    src/client_freeze.cpp
    src/client_pairing.cpp
    src/client_uwf_request.cpp
    src/exam_bridge.cpp
    src/exam_host.cpp
    src/exam_profile.cpp
    src/named_pipe.cpp
    src/session.cpp
)
target_include_directories(nstu_client_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(nstu_client_platform PUBLIC cxx_std_20)
target_link_libraries(nstu_client_platform PUBLIC nstu_common advapi32 ole32 shlwapi userenv wtsapi32)
nstu_enable_warnings(nstu_client_platform)

# WebView2 is an optional native host dependency.  Keeping the SDK opt-in
# preserves clean/offline CI builds; production packaging must provide the
# matching WebView2Loader.dll beside nstu-agent.exe.
if(NSTU_ENABLE_WEBVIEW2_HOST)
    if(NOT NSTU_WEBVIEW2_SDK_DIR)
        message(FATAL_ERROR
            "NSTU_ENABLE_WEBVIEW2_HOST requires NSTU_WEBVIEW2_SDK_DIR")
    endif()
    find_path(NSTU_WEBVIEW2_INCLUDE_DIR WebView2.h
        PATHS "${NSTU_WEBVIEW2_SDK_DIR}/build/native/include"
        NO_DEFAULT_PATH)
    if(NOT NSTU_WEBVIEW2_INCLUDE_DIR)
        message(FATAL_ERROR
            "WebView2.h was not found under NSTU_WEBVIEW2_SDK_DIR")
    endif()
    target_compile_definitions(nstu_client_platform PUBLIC NSTU_HAS_WEBVIEW2=1)
    target_include_directories(nstu_client_platform PRIVATE
        "${NSTU_WEBVIEW2_INCLUDE_DIR}")
    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(nstu_webview2_loader_dir
                "${NSTU_WEBVIEW2_SDK_DIR}/build/native/x64")
        elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(nstu_webview2_loader_dir
                "${NSTU_WEBVIEW2_SDK_DIR}/build/native/x86")
        endif()
        if(EXISTS "${nstu_webview2_loader_dir}/WebView2Loader.dll")
            install(FILES "${nstu_webview2_loader_dir}/WebView2Loader.dll"
                DESTINATION client COMPONENT client)
        elseif(NSTU_ENABLE_PACKAGING)
            message(FATAL_ERROR
                "NSTU_ENABLE_WEBVIEW2_HOST=ON with packaging enabled requires "
                "WebView2Loader.dll at ${nstu_webview2_loader_dir}. "
                "Install the matching WebView2 SDK loader before configuring "
                "the release package.")
        else()
            message(WARNING
                "WebView2Loader.dll was not found; package it beside nstu-agent.exe")
        endif()
    endif()
else()
    target_compile_definitions(nstu_client_platform PUBLIC NSTU_HAS_WEBVIEW2=0)
endif()

add_executable(nstu-service service/main.cpp)
target_link_libraries(nstu-service PRIVATE
    nstu_client_platform nstu_setup_diagnostics nstu_uwf)
nstu_enable_warnings(nstu-service)

add_executable(nstu-provision tools/provision.cpp)
target_link_libraries(nstu-provision PRIVATE nstu_client_platform)
nstu_enable_warnings(nstu-provision)
if(MINGW)
    target_link_options(nstu-provision PRIVATE -municode)
endif()

add_executable(nstu-agent WIN32 agent/main.cpp)
target_link_libraries(nstu-agent PRIVATE nstu_client_platform shell32 user32 gdi32)
nstu_enable_warnings(nstu-agent)

if(MINGW)
    target_link_options(nstu-agent PRIVATE -municode)
endif()
