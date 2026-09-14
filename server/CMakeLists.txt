add_library(nstu_server_state STATIC
    src/client_registry.cpp
    src/control_plane.cpp
)
target_include_directories(nstu_server_state PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(nstu_server_state PUBLIC cxx_std_20)
target_link_libraries(nstu_server_state PUBLIC nstu_common)
nstu_enable_warnings(nstu_server_state)

option(NSTU_SERVER_USE_IMGUI "Build the Dear ImGui server UI" ON)
if(NSTU_SERVER_USE_IMGUI)
    include(FetchContent)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)

    add_library(nstu_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    )
    target_include_directories(nstu_imgui PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
    )
    target_link_libraries(nstu_imgui PUBLIC
        d3d11 dxgi d3dcompiler dwmapi user32 imm32
    )

    add_executable(nstu-server WIN32 app/main.cpp)
    target_compile_definitions(nstu-server PRIVATE
        NSTU_PROJECT_VERSION="${PROJECT_VERSION}"
        NSTU_BUILD_CHANNEL="${NSTU_INSTALLER_CHANNEL}")
    target_link_libraries(nstu-server PRIVATE nstu_server_state nstu_imgui
        d3d11 dxgi mfplat mfuuid shell32)
    if(MSVC)
        target_compile_options(nstu-server PRIVATE /utf-8)
    endif()
    nstu_enable_warnings(nstu-server)
    if(MINGW)
        target_link_options(nstu-server PRIVATE -municode)
    endif()
endif()
