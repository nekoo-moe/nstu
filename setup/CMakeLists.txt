add_executable(nstu-diagnostics WIN32
    resources/nstu-setup.rc
    src/main.cpp
    src/hardware_scan.cpp
    src/driver_scan.cpp
)
target_include_directories(nstu-diagnostics PRIVATE include)
target_link_libraries(nstu-diagnostics PRIVATE
    nstu_common iphlpapi mf mfplat mfuuid ole32 shell32 user32 ws2_32
)
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
