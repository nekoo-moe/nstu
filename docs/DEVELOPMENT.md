# Project NSTU Development Guide

[Product overview](../README.md) | [Tiếng Việt](../README.vi.md)

NSTU là dự án mã nguồn mở dành cho quản lý phòng máy và classroom lab trên
Windows. Mục tiêu là cung cấp server quản trị tập trung, client nhẹ chạy nền,
và kênh truyền màn hình độ trễ thấp từ một máy giáo viên tới nhiều máy học
sinh.

Repository: <https://github.com/Khoasoma/nstu>

NSTU hiện là engineering MVP dùng để phát triển và kiểm thử kiến trúc, chưa
phải bản triển khai production hoàn chỉnh cho trường học. Các giới hạn thực tế
được ghi trong [ROADMAP.md](ROADMAP.md).

## Tính năng hiện tại

- Protocol nhị phân versioned, little-endian, có giới hạn kích thước và parser
  TCP incremental.
- TCP control channel với connection preamble, mutual HMAC handshake, replay
  protection, sequence guard và rate limiter cho kết nối chưa xác thực.
- `KeyStore` hỗ trợ enrollment, rotation, revocation, tombstone `key_id` và
  zeroization; DPAPI dùng để bảo vệ secret ở local machine.
- UDP multicast cho broadcast và policy fallback sang unicast.
- Packet-loss tracker có reorder window, duplicate/late/wrong-stream/jump
  handling. Xem [PACKET_LOSS.md](PACKET_LOSS.md).
- Bounded video frame reassembly, deadline, duplicate/conflict detection và
  memory-pressure accounting. Xem [VIDEO_REASSEMBLY.md](VIDEO_REASSEMBLY.md).
- DXGI Desktop Duplication, D3D11 BGRA-to-NV12 và Media Foundation H.264 MFT.
- Windows Service, active-session agent, secure named pipe, tray icon, lock
  window, annotation overlay và teacher-broadcast window.
- Server UI dùng Dear ImGui + D3D11 và client registry thread-safe.
- Server UI có room snapshot wall responsive với chu kỳ 5-10 giây, health
  summary, search/filter, selected-client telemetry, annotation, broadcast,
  chat và control actions.
- Server UI chuyển được English/Tiếng Việt, light/dark mode, và có native tray
  icon để hide/restore/exit mà không dừng control plane ngoài ý muốn.
- Client capture dùng GDI + WIC JPEG, giới hạn 60 KiB/frame; service chỉ giữ
  snapshot mới nhất đang chờ gửi để không tạo backlog.
- Client agent có chat window Native Win32 dùng các control hệ thống nhẹ.
- CMake modular, build được bằng MSVC hoặc MinGW-w64, có Windows CI.

## Kiến trúc thư mục

```text
nstu/
|-- common/                  Protocol, auth, TCP/UDP, IOCP, key, rate limit
|   |-- include/nstu/        Public C++ headers
|   `-- src/                 Implementation dùng chung
|-- video/                   DXGI, D3D11 converter, Media Foundation encoder
|-- client/
|   |-- service/             Windows Service entry point
|   |-- agent/               Interactive-session tray/overlay entry point
|   `-- src/                 Named pipe và session integration
|-- server/
|   |-- app/                 Server UI entry point
|   |-- include/nstu/        Client state registry headers
|   `-- src/                 Client registry implementation
|-- tests/                   Protocol, auth, packet-loss, reassembly, IPC tests
|-- docs/                    Architecture, security, packet-loss, roadmap
|-- cmake/                   Compiler warning policy
|-- CMakeLists.txt           Root build configuration
|-- CMakePresets.json        Windows Debug/Release presets
|-- LICENSE                  MIT license
`-- THIRD_PARTY_NOTICES.md   Dependency and license notices
```

## Yêu cầu cài đặt

- Windows 10 hoặc Windows 11 x64.
- Visual Studio 2022 với Desktop development with C++ và Windows SDK, hoặc
  MinGW-w64 hỗ trợ C++20.
- CMake 3.25 trở lên, Git.
- Ninja nếu dùng CMake presets.

Các API video và service là Windows-only. Build đầy đủ và integration tests cần
Windows.

## Clone và build

```powershell
git clone https://github.com/Khoasoma/nstu.git
cd nstu
```

### MSVC / Visual Studio

```powershell
cmake -S . -B build-msvc -A x64 -DNSTU_ENABLE_WERROR=ON
cmake --build build-msvc --config Release --parallel
ctest --test-dir build-msvc -C Release --output-on-failure
```

### MinGW-w64

```powershell
cmake -S . -B build-mingw -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DNSTU_ENABLE_WERROR=ON
cmake --build build-mingw
ctest --test-dir build-mingw --output-on-failure
```

### CMake presets

Preset yêu cầu Ninja và một toolchain C++20 phù hợp:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Release dùng `windows-release` thay cho `windows-debug`.

## Tùy chọn CMake

- `NSTU_BUILD_CLIENT=ON|OFF`: build service và agent client.
- `NSTU_BUILD_SERVER=ON|OFF`: build server UI.
- `NSTU_BUILD_SETUP=ON|OFF`: build the `nstu-diagnostics` helper used by the
  unified installer and client boot checks.
- `NSTU_BUILD_VIDEO=ON|OFF`: build DXGI/Media Foundation trên Windows.
- `NSTU_BUILD_TESTS=ON|OFF`: build test suite.
- `NSTU_SERVER_USE_IMGUI=ON|OFF`: bật/tắt server UI Dear ImGui.
- `NSTU_ENABLE_WERROR=ON|OFF`: coi warning là error.
- `NSTU_ENABLE_PACKAGING=ON|OFF`: bật/tắt target installer NSIS hợp nhất.

Ví dụ chỉ build core protocol/test:

```powershell
cmake -S . -B build-core -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DNSTU_BUILD_VIDEO=OFF `
  -DNSTU_BUILD_CLIENT=OFF -DNSTU_BUILD_SERVER=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

## Artifact sau khi build

- `nstu-server.exe`: server administration UI.
- `nstu-service.exe`: Windows Service process.
- `nstu-agent.exe`: interactive session agent.
- `nstu_video_probe.exe`: kiểm tra khả năng video/MFT trên máy hiện tại.
- Các executable trong `tests/`: test protocol, auth, networking và video.

## Chạy thử giao diện

Sau khi build trên Windows, chạy `nstu-server.exe` để mở server shell. Màn hình
server mặc định mở `Room screens`: health summary ở đầu trang, chu kỳ snapshot
5-10 giây, search/filter và screen wall tự đổi số cột theo kích thước cửa sổ.
`Selected client` cung cấp danh sách gọn, telemetry, preview 16:9, snapshot
control, lock/unlock, vẽ overlay và chat. Chu kỳ mặc định là 7 giây.

Nút `EN`/`VI` và `Sáng`/`Tối` đổi ngôn ngữ, theme ngay khi chạy. Có thể chọn
default từ command line bằng `--language=vi`, `--language=en` và `--dark`.
Minimize hoặc đóng cửa sổ sẽ đưa server xuống notification area; double-click
tray icon để mở lại, hoặc dùng menu chuột phải để hide/show và exit.

Chạy `nstu-agent.exe` để mở chat client Native Win32 và tray icon. Double-click
tray icon để ẩn/hiện chat. Agent có các window riêng cho lock, annotation
click-through và teacher-screen broadcast; lock luôn được xếp trên hai lớp còn
lại.

Các lệnh snapshot, annotation, broadcast, lock/unlock và chat đi qua control
channel đã xác thực, service và named pipe tới agent. Nền tảng `Start stream`
và `Request keyframe` của H.264 vẫn còn cho continuous-video mode tùy chọn chưa
được nối end to end.

## Cài đặt và kiểm thử

Installer hợp nhất tự gọi service lifecycle script với quyền Administrator.
Cài đặt client đăng ký service ở chế độ automatic, cấu hình recovery và đặt cờ
bắt buộc restart; service bắt đầu ở lần boot tiếp theo. Lần gọi uninstaller đầu
chỉ stage startup task và không thay đổi service, process hoặc file; việc dừng,
xóa và xử lý file bị khóa chỉ chạy sau restart.

Lifecycle helper chạy `test-system-setup.ps1` để kiểm tra Windows x64,
quyền Administrator, data root NTFS/ReFS có thể ghi, Windows Firewall và
cấu hình autologon. Vai trò Server còn kiểm tra xung đột control/video port.
Script không bật autologon hoặc lưu credential; built-in Administrator
autologon bị từ chối, còn dedicated standard-user autologon chỉ được nêu
như lựa chọn. Khi upgrade server, listener thuộc đúng binary NSTU đã cài
được cảnh báo thay vì bị nhầm với ứng dụng chiếm port khác.

Kiểm thử nhanh sau build:

```powershell
ctest --test-dir build-msvc -C Release --output-on-failure
.\build-msvc\video\Release\nstu_video_probe.exe
```

`nstu_video_probe.exe` chỉ báo khả năng adapter/MFT trên máy đang chạy, không
thay thế soak test hoặc kiểm tra multicast trên switch thật.

## Đóng gói installer

Trên Windows, CMake tạo một target NSIS hợp nhất:

```powershell
cmake --build build-msvc --config Release --target nstu-package
```

Kết quả là `nstu-0.1.0-setup.exe` trong build directory. Installer mở đầu bằng
lựa chọn Client hoặc Server, kiểm tra vai trò đối diện trước khi cài và gọi
`nstu-diagnostics` theo từng bước. Cần cài [NSIS](https://nsis.sourceforge.io/)
và đặt `makensis.exe` trong `PATH`. Nếu môi trường build không có NSIS, vẫn có
thể kiểm tra layout bằng script staging:

```powershell
cmake -DNSTU_BUILD_DIR=build-msvc `
  -DNSTU_STAGE_DIR=build-msvc/unified-installer-stage `
  -P packaging/PrepareUnifiedPackage.cmake
```

Package staging chứa client, server, diagnostics và tài liệu. Các lifecycle
script yêu cầu Administrator; quy trình hỗ trợ là chạy installer/uninstaller
hợp nhất thay vì gọi helper trực tiếp. Lần gọi uninstall đầu tiên không thay
đổi service, process hoặc file cho đến sau restart bắt buộc; quá trình fail
closed khi service Deep Freeze được nhận diện (`DFServ` hoặc `DeepFrz`) đang
active.

Mỗi commit push lên `main` sẽ chạy Windows CI, build/test, tạo một unified
installer và phát hành GitHub pre-release tự động dạng
`nightly-<run-number>`. Pull Request vẫn được build/test nhưng không tạo
release. Các release thủ công như `v0.2.0-dev` vẫn được giữ độc lập.

Nightly CI chưa ký artifact; workflow production có signing nhưng chỉ chạy khi
có certificate và evidence gate đầy đủ. MVP chưa tự động cấp key hoặc
cấu hình multicast. Service recovery đã được installer cấu hình, nhưng vẫn
cần kiểm thử upgrade/reboot trên image thật.

## Secret, memory và Deep Freeze

Không commit PSK, DPAPI entropy, certificate, dump, memory snapshot, capture,
log hoặc file runtime. Các thư mục local được ignore gồm `local/`, `memory/`,
`dumps/`, `logs/`, `runtime/` và các build tree.

Secret runtime nên được lưu bằng DPAPI machine scope hoặc certificate store.
Named pipe và memory-mapped/local runtime data chỉ giảm disk I/O; chúng không
thay thế ACL, authentication, key rotation hoặc policy của Deep Freeze.

## Bảo mật và protocol

Connection preamble là admission filter nhanh cho magic, version, role,
`client_id` và `key_id`; nó không chứng minh danh tính máy con. Danh tính chỉ
được chấp nhận sau HMAC handshake thành công.

Luồng video xác thực HMAC trước packet-loss accounting và frame reassembly.
HMAC không mã hóa nội dung màn hình; nếu LAN không được tin cậy cần AES-GCM
hoặc cơ chế confidentiality tương đương.

Chi tiết xem [ARCHITECTURE.md](ARCHITECTURE.md),
[SECURITY.md](SECURITY.md), [PACKET_LOSS.md](PACKET_LOSS.md),
[VIDEO_REASSEMBLY.md](VIDEO_REASSEMBLY.md) và [ROADMAP.md](ROADMAP.md).

## Fork và phát triển

1. Mở repository trên GitHub và chọn **Fork**.
2. Clone fork, sau đó cấu hình repository gốc làm `upstream`:

   ```powershell
   git clone https://github.com/<your-account>/nstu.git
   cd nstu
   git remote add upstream https://github.com/Khoasoma/nstu.git
   ```

3. Tạo branch cho thay đổi:

   ```powershell
   git fetch upstream
   git switch -c feature/<short-name> upstream/main
   ```

4. Build với `NSTU_ENABLE_WERROR=ON`, chạy test và kiểm tra diff:

   ```powershell
   cmake --build build-msvc --config Release --parallel
   ctest --test-dir build-msvc -C Release --output-on-failure
   git diff --check
   ```

5. Commit nhỏ, có mục đích rõ ràng, rồi push lên fork:

   ```powershell
   git add <files>
   git commit -m "Describe the change"
   git push -u origin feature/<short-name>
   ```

6. Mở Pull Request từ fork tới `Khoasoma/nstu:main`. PR phải nêu test đã chạy,
   thay đổi protocol/ABI nếu có, rủi ro Windows compatibility và ảnh hưởng
   tới memory/runtime artifact.

Giữ thay đổi tập trung theo module và cập nhật roadmap khi hoàn tất milestone.

## License và dependency

NSTU phát hành theo [MIT License](../LICENSE). Dependency runtime phải tương thích MIT,
Apache-2.0 hoặc license permissive tương đương; không thêm GPL dependency.
Dear ImGui được fetch ở thời điểm configure và dùng MIT License. Windows SDK,
Winsock, DXGI, D3D11 và Media Foundation là thành phần của Windows SDK, không
được redistribute như source dependency. Xem [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Trạng thái production

IOCP dispatcher, persisted keyring, enrollment transport, snapshot routing,
annotation/broadcast, packetizer, jitter, NACK, keyframe scheduling,
service-agent routing, signing automation và device-loss recovery đã có
implementation và automated test. Green CI vẫn chỉ chứng minh build và local
correctness. Trước khi triển khai thật phải chạy production signing bằng
certificate thật, review/fuzz protocol, rồi hoàn tất 50-client snapshot soak,
Windows/Deep Freeze matrix và benchmark theo
[PRODUCTION_VALIDATION.md](PRODUCTION_VALIDATION.md). Continuous H.264/UDP là
enhancement tùy chọn và cần multicast/driver matrix riêng trước khi bật.
