add_library(nstu_setup_diagnostics STATIC
    src/diagnostics.cpp
    src/hardware_scan.cpp
    src/driver_scan.cpp
)
target_include_directories(nstu_setup_diagnostics PUBLIC include)
target_link_libraries(nstu_setup_diagnostics PUBLIC
    nstu_common d3d11 dxgi iphlpapi mf mfplat mfuuid netapi32 ole32 shell32
    user32 wbemuuid ws2_32
)
nstu_enable_warnings(nstu_setup_diagnostics)

add_executable(nstu-diagnostics WIN32
    resources/nstu-setup.rc
    src/main.cpp
)
target_link_libraries(nstu-diagnostics PRIVATE nstu_setup_diagnostics)
if(MSVC)
    target_compile_options(nstu-diagnostics PRIVATE /utf-8)
    # The RC file carries the application manifest. Suppress MSVC's generated
    # manifest so it does not emit a second RT_MANIFEST resource at link time.
    target_link_options(nstu-diagnostics PRIVATE /MANIFEST:NO)
endif()
nstu_enable_warnings(nstu-diagnostics)
if(MINGW)
    target_link_options(nstu-diagnostics PRIVATE -municode)
endif()
