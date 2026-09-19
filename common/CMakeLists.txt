add_library(nstu_common STATIC
    src/auth.cpp
    src/control_channel.cpp
    src/control_messages.cpp
    src/deployment.cpp
    src/discovery.cpp
    src/exam_control.cpp
    src/exam_sync.cpp
    src/answer_journal.cpp
    src/enrollment.cpp
    src/frame_reassembler.cpp
    src/iocp_dispatcher.cpp
    src/key_store.cpp
    src/keyring.cpp
    src/maintenance_intent.cpp
    src/multicast.cpp
    src/network.cpp
    src/packet_loss.cpp
    src/pairing.cpp
    src/protocol.cpp
    src/rate_limiter.cpp
    src/screen_snapshot.cpp
    src/secret_store.cpp
    src/telemetry.cpp
    src/video_transport.cpp
    src/wfp_allowlist.cpp
)

target_include_directories(nstu_common
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(nstu_common PUBLIC cxx_std_20)
nstu_enable_warnings(nstu_common)

if(WIN32)
    target_link_libraries(nstu_common PUBLIC
        advapi32 bcrypt crypt32 fwpuclnt gdi32 iphlpapi ole32 windowscodecs
        ws2_32)
endif()
