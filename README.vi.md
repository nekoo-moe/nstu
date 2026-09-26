<img width="1280" height="640" alt="photo_2026-09-11_19-20-47" src="https://github.com/user-attachments/assets/4b6c679a-a19a-47ec-9c8e-20b851e3ffe2" />

---

# Project NSTU

[English](README.md) | [Tiếng Việt](README.vi.md) | [Development guide](docs/DEVELOPMENT.md) | [Hướng dẫn thiết lập](docs/SETUP_GUIDE.vi.md) | [Chẩn đoán tùy chọn](docs/TELEMETRY.vi.md) | [Thiết kế khôi phục sau reboot](docs/REBOOT_TO_RESTORE.vi.md) | [Tự động cập nhật và bảo trì LTSC](docs/AUTO_UPDATE_LTSC.vi.md) | [Kiểm thử VM](docs/VM_TESTING.md)

[![C++](https://img.shields.io/badge/C++-21%2B-blue?logo=c++&logoColor=white)](https://en.wikipedia.org/wiki/C%2B%2B)
[![License](https://img.shields.io/badge/license-mit%20license-lightgrey)](#licensing)
[![Windows CI](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

[![CI](https://img.shields.io/github/actions/workflow/status/nekoo-moe/nstu/production.yml)](https://github.com/nekoo-moe/nstu/actions)
[![Contributors](https://img.shields.io/github/contributors/nekoo-moe/nstu)](https://github.com/nekoo-moe/nstu/graphs/contributors)
[![Commit activity](https://img.shields.io/github/commit-activity/m/nekoo-moe/nstu)](https://github.com/nekoo-moe/nstu/commits)
[![Last commit](https://img.shields.io/github/last-commit/nekoo-moe/nstu)](https://github.com/nekoo-moe/nstu/commits)
[![Issues](https://img.shields.io/github/issues/nekoo-moe/nstu)](https://github.com/nekoo-moe/nstu/issues)

> **Phát triển có hỗ trợ AI:** NSTU được phát triển với hỗ trợ AI; mọi thay đổi vẫn được con người rà soát và có thể kiểm tra trong lịch sử mã nguồn công khai.

NSTU là dự án quản lý lớp học và phòng máy Windows miễn phí, mã nguồn mở. Dự
án hướng đến một máy giáo viên quản lý tập trung, client nhẹ trên máy học sinh,
lệnh điều khiển được xác thực, chat, snapshot màn hình tiết kiệm băng thông và
broadcast màn hình giáo viên trong mạng nội bộ.

![Dashboard NSTU Server ở chế độ sáng tiếng Anh và chế độ tối tiếng Việt](docs/assets/server-dashboard-preview.png)

*Dashboard server: chế độ sáng tiếng Anh và chế độ tối tiếng Việt.*

> **Trạng thái phát triển:** NSTU hiện là engineering MVP, chưa phải bản
> production. Persisted enrollment, dispatcher nhiều client, authenticated
> control routing, JPEG snapshot định kỳ, annotation overlay, teacher-screen
> snapshot broadcast, packetization và device recovery đã có implementation/test;
> continuous H.264 decode end-to-end và validation matrix trên phòng máy thật
> chưa hoàn thành.
> Không nên dùng bản nightly hiện tại như một biện pháp bảo mật trong trường học
> thật.

## Câu chuyện bắt đầu

NSTU bắt nguồn từ sự tiếc nuối khi chứng kiến các trường học phải dựa vào phần
mềm quản lý phòng máy không có bản quyền. Phần mềm đến từ nguồn không tin cậy có
thể bị sửa đổi, bị khai thác, hoặc âm thầm biến máy tính trong phòng học thành
hạ tầng đào tiền mã hóa cho kẻ tấn công. Đồng thời, khi Việt Nam ngày càng siết
chặt vấn đề bản quyền, chi phí phần mềm có thể khiến một số trường có ngân sách
hạn chế khó trang bị và quản lý phòng tin học đúng pháp luật.

Vì vậy, chúng tôi xây dựng NSTU như một lựa chọn thực tế: miễn phí, có thể kiểm
tra mã nguồn, dễ cài đặt, và được thiết kế cho cả an toàn lẫn phần cứng cấu hình
thấp. NSTU dùng MIT License. Trường học, giáo viên, doanh nghiệp và cá nhân có
thể sử dụng, nghiên cứu, sửa đổi và phân phối lại, kể cả cho mục đích thương mại,
miễn là giữ lại thông báo bản quyền và nội dung giấy phép.

Mã nguồn mở không tự động đồng nghĩa với an toàn. Vì vậy NSTU công khai threat
model và các hạng mục production chưa hoàn thành tại
[SECURITY.md](docs/SECURITY.md) và [ROADMAP.md](docs/ROADMAP.md).

## Bản đồ tài liệu

- [Hướng dẫn thiết lập](docs/SETUP_GUIDE.vi.md): installer hợp nhất chọn vai
  trò, diagnostics, script enrollment và các cờ build/runtime.
- [Kiến trúc](docs/ARCHITECTURE.md): process model, đường dữ liệu client/server,
  video pipeline, control channel và giới hạn hiện tại.
- [Bảo mật](docs/SECURITY.md): authentication, enrollment, secret và ranh giới
  triển khai.
- [Chẩn đoán tùy chọn](docs/TELEMETRY.vi.md): consent rõ ràng, thu thập cục bộ
  có giới hạn, lọc riêng tư và quy trình gửi báo cáo công khai thủ công.
- [Đánh giá trên máy tính](docs/EXAM_ASSESSMENT.vi.md): native WebView2 tùy chọn,
  ranh giới gói bài thi, phục hồi câu trả lời có xác thực, outbox bền vững và
  định dạng state export.
- [Development guide](docs/DEVELOPMENT.md): tùy chọn build, test, đóng gói và
  quy trình CI.
- [Production validation](docs/PRODUCTION_VALIDATION.md): checklist kiểm thử
  phần cứng, mạng, Deep Freeze và thời gian dài.
- [Tự động cập nhật và bảo trì LTSC](docs/AUTO_UPDATE_LTSC.vi.md): chu kỳ cập
  nhật chín tháng, trust của release, rollout theo đợt và điều kiện rollback.
- [Thiết kế khôi phục sau reboot](docs/REBOOT_TO_RESTORE.vi.md): kế hoạch nghiên
  cứu quản lý UWF của Microsoft, giới hạn edition, servicing, recovery và test.
- [Kiểm thử vòng đời trên VM](docs/VM_TESTING.md): kiểm tra bằng Sandbox,
  validation qua restart, ranh giới quyền và evidence cần lưu.

## Năng lực hướng đến

- Quản lý từ 50 máy Windows trở lên bằng một máy giáo viên.
- Theo dõi phòng máy bằng JPEG snapshot có giới hạn, chụp mỗi 5-10 giây để
  tránh tải video liên tục trên switch.
- Lock/unlock client, vẽ lên màn hình một máy học sinh bằng click-through
  overlay và broadcast snapshot màn hình giáo viên.
- Có thể bật/tắt điều khiển từ xa cho một client đã chọn. Chuột được gửi từ
  preview bằng tọa độ chuẩn hóa; ô bàn phím riêng gửi văn bản ASCII khi nhấn
  Enter.
- Giữ nền tảng allowlist IPv4 native bằng Windows Filtering Platform (WFP) cho
  policy quản trị viên trong tương lai. Installer hợp nhất và diagnostics
  hiện chưa cung cấp control cho policy này.
- Giữ nền tảng H.264 multicast/unicast cho chế độ broadcast liên tục tùy chọn
  trong tương lai, không dùng làm đường monitoring mặc định.
- Hiển thị screen wall responsive của các snapshot mới nhất, cùng telemetry,
  điều khiển và chat tập trung cho một máy.
- Có thể giữ lịch sử chẩn đoán server đã lọc riêng tư và có giới hạn trong RAM,
  sau đó để kỹ thuật viên xem lại trước khi tự tạo GitHub Issue công khai. Thu
  thập và popup nhắc lỗi có thể bật/tắt riêng; không có báo cáo nào tự tải lên.
- Snapshot được giới hạn ở ảnh JPEG tối đa 480x270 và 60 KiB. Giao diện giáo
  viên decode mỗi generation một lần, chỉ giữ payload immutable mới nhất và báo
  frame lỗi thay vì thử lại ở mọi render frame.
- Chạy Windows Service nhỏ và Win32 tray/chat agent native trên client.
- Xác thực lệnh điều khiển và video mà không cần JSON, XML, Electron hay driver
  kernel tùy biến.
- Giữ secret, dump, capture và runtime state ở local, không đưa lên Git.

## Cấu hình hệ thống

Đây là mục tiêu cấu hình của dự án, chưa phải kết quả đã được xác nhận bằng soak
test trên phòng máy 50 client thật.

| Vai trò | Cấu hình mục tiêu | Mạng |
| --- | --- | --- |
| Server | Windows 10/11 x64, RAM tối thiểu 6 GiB, khuyến nghị 8 GiB, trống 512 MiB; CPU tương đương i5-6400 chỉ là mốc tham chiếu hiệu năng | Khuyến nghị Gigabit Ethernet có dây |
| Client | Windows 10/11 x64, RAM tối thiểu 6 GiB, khuyến nghị 8 GiB, trống 512 MiB; CPU tương đương i5-6400 chỉ là mốc tham chiếu hiệu năng | Khuyến nghị Ethernet có dây |
| Router/switch | Switching/routing Ethernet thông thường cho snapshot TCP đã xác thực; IGMP snooping và IGMP querier chỉ là yêu cầu tùy chọn cho video tương lai | Một LAN/VLAN được kiểm soát cho lần triển khai đầu |

Với server quản lý từ 50 máy trở lên, RAM 16 GiB và SSD là lựa chọn thận trọng
cho đến khi baseline khuyến nghị 8 GiB vượt qua kiểm thử phần cứng dài hạn. Intel HD Graphics
530 là baseline cho hardware acceleration, không phải cam kết hoạt động với mọi
phiên bản driver.

Diagnostics vẫn báo model CPU, kiến trúc và số core vật lý/logical cho kỹ thuật
viên, nhưng danh tính và topology của CPU không bao giờ chặn installer. Hãy xác
nhận CPU bằng workload NSTU thực tế vì topology VM, firmware và thông tin do
Windows báo có thể không khớp với tên bộ xử lý.

## Cờ build và setup

Preset Windows có cấu hình Debug và Release. Debug giữ symbol và build test;
Release là cấu hình pre-release dùng để đóng gói.

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug

cmake -S . -B build-release -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DNSTU_ENABLE_WERROR=ON `
  -DNSTU_BUILD_CLIENT=ON -DNSTU_BUILD_SERVER=ON `
  -DNSTU_BUILD_SETUP=ON -DNSTU_BUILD_VIDEO=ON -DNSTU_BUILD_TESTS=ON
cmake --build build-release --parallel 4
```

| Cờ | Mặc định | Mục đích |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE=Debug\|Release` | tùy generator | Chọn symbol debug hoặc binary release tối ưu |
| `NSTU_BUILD_CLIENT=ON\|OFF` | `ON` | Build `nstu-service`, `nstu-agent` và tool provision |
| `NSTU_BUILD_SERVER=ON\|OFF` | `ON` | Build server và giao diện giáo viên |
| `NSTU_BUILD_SETUP=ON\|OFF` | `ON` trên Windows | Build helper `nstu-diagnostics` cho installer và boot check |
| `NSTU_BUILD_VIDEO=ON\|OFF` | `ON` | Build DXGI, Media Foundation và video transport |
| `NSTU_BUILD_TESTS=ON\|OFF` | `ON` | Build các bài test unit/integration |
| `NSTU_SERVER_USE_IMGUI=ON\|OFF` | `ON` | Bật/tắt executable UI Dear ImGui |
| `NSTU_ENABLE_WERROR=ON\|OFF` | `OFF` | Coi warning là error; nên bật khi kiểm tra release |
| `NSTU_ENABLE_PACKAGING=ON\|OFF` | `ON` | Bật các target đóng gói NSIS hợp nhất |

Để chỉ build diagnostics helper cho kỹ thuật viên:

```powershell
cmake -S . -B build-setup -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DNSTU_BUILD_SETUP=ON `
  -DNSTU_BUILD_CLIENT=OFF -DNSTU_BUILD_SERVER=OFF `
  -DNSTU_BUILD_VIDEO=OFF -DNSTU_BUILD_TESTS=OFF `
  -DNSTU_ENABLE_WERROR=ON
cmake --build build-setup --target nstu-diagnostics
```

Server nhận các cờ runtime `--language=en`, `--language=vi`, `--dark` và
`--graphics-debug`. Các cờ này chọn ngôn ngữ/giao diện ban đầu và yêu cầu lớp
debug Direct3D 11. Trong giao diện, Language, Appearance và Screen refresh nằm
trong phần `Settings`; Screen refresh là chu kỳ snapshot, giới hạn từ 5 đến
10 giây.

Installer hợp nhất mở đầu bằng hai lựa chọn: `Install for Client` hoặc
`Install for Server`. Không hỗ trợ cài `Both` vì hai vai trò có vòng đời và
trách nhiệm port khác nhau. Installer gọi `nstu-diagnostics` theo từng bước.
Kỹ thuật viên cũng có thể chạy riêng với `--target=client|server`,
`--server-ip=...`, `--server-port=...`, `--installer`, `--boot-check`,
`--auto-close` và `--log=...`.

Popup `Diagnostics` riêng bên trong `nstu-server.exe` hiển thị adapter/hãng
DXGI, feature level, khả dụng Desktop Duplication, trạng thái dự phòng WARP
và các HRESULT gần đây có giới hạn. Nếu khởi tạo đồ họa thất bại
trước khi server UI mở, Windows hiển thị hộp thoại gốc với lỗi đã ghi.

Thu thập chẩn đoán tùy chọn được cấu hình trong `Cài đặt` của server. Tính năng
mặc định tắt, chỉ giữ tối đa 64 sự kiện đã lọc trong RAM và xóa chúng khi tắt
hoặc khi server thoát. Người dùng có thể bật popup khi có lỗi. Nút `Sao chép và
mở GitHub` không tự gửi dữ liệu: nó chỉ sao chép báo cáo đã xem lại và mở biểu
mẫu Issue công khai để kỹ thuật viên tự dán rồi gửi. Xem
[Chẩn đoán tùy chọn](docs/TELEMETRY.vi.md) để biết danh sách trường và ranh giới
quyền riêng tư.

#### Chẩn đoán đồ họa của server

Mở `Diagnostics` trên thanh điều hướng server để kiểm tra môi trường đồ họa
mà không cần bắt đầu stream từ client. Popup hiển thị mọi bộ điều hợp DXGI
(bao gồm NVIDIA, AMD, Intel và bộ điều hợp phần mềm Microsoft), adapter đang
chọn, feature level, khả dụng của Desktop Duplication và số bộ mã hóa H.264
phần cứng Media Foundation mà Windows đã đăng ký. Popup cũng giữ nhật ký có
giới hạn, kèm thời gian, cho các HRESULT khi tạo thiết bị, đổi kích thước
swap-chain và lỗi `Present`/mất thiết bị. Nút `Refresh` chạy lại việc kiểm tra;
không thay đổi policy hệ thống và không cài driver.

Server thử lần lượt các adapter phần cứng không phải phần mềm rồi dùng WARP
làm dự phòng nếu tạo thiết bị phần cứng thất bại. Nhờ vậy popup chẩn đoán vẫn
có thể mở trên máy thiếu driver hoặc driver không tương thích. Khi truyền
`--graphics-debug`, NSTU yêu cầu lớp debug D3D11 và ghi lại trường hợp Windows
không cung cấp lớp này trước khi thử lại mà không có debug layer. Nếu driver
reset hoặc loại bỏ thiết bị, HRESULT lỗi và giá trị
`ID3D11Device::GetDeviceRemovedReason()` được ghi vào nhật ký, đồng thời thanh
trạng thái hướng kỹ thuật viên đến `Diagnostics`.

Intel Core i5-11400F là CPU dòng F nên không có GPU tích hợp. Server cần một
GPU rời có driver hoạt động (hoặc WARP, chỉ nên dùng để chẩn đoán và chậm hơn).
Tăng tốc NVIDIA và AMD sử dụng đường dẫn DXGI/D3D11 và Windows Media
Foundation nguyên bản khi driver cung cấp; không cần phụ thuộc runtime CUDA,
NVENC SDK hay AMD AMF. Khả năng mã hóa phụ thuộc driver và đăng ký Media
Foundation, vì vậy hãy chụp popup Diagnostics trước khi báo cáo lỗi stream.

## Benchmark tài nguyên cục bộ

Các số liệu dưới đây chỉ là phép đo tham khảo trên máy phát triển, không thay
thế benchmark production với 50 client. Phép đo được thực hiện ngày 1 tháng 9
năm 2026 từ commit `f183e42`, dùng
`build-verify/server/nstu-server.exe`, registry client trống và không có traffic
snapshot hoặc broadcast.

Máy thử nghiệm: AMD Ryzen 7 5700X (8 core/16 logical processor), RAM 15,9 GiB,
ổ SSD, NVIDIA GeForce RTX 2060 SUPER và Windows 11 Pro build 26200.

| Tình huống | CPU trên tổng năng lực máy | Working set | Private memory | Phương pháp |
| --- | ---: | ---: | ---: | --- |
| Dashboard hiển thị, idle | trung bình 0,725%, p95 2,336% | trung bình 53,54 MiB, tối đa 54,50 MiB | trung bình 117,95 MiB, tối đa 118,83 MiB | 120 mẫu, mỗi mẫu cách 1 giây, sau warm-up 10 giây |
| Đóng cửa sổ xuống tray, idle | trung bình 0,023%, tối đa 0,289% | trung bình 52,48 MiB, tối đa 53,00 MiB | trung bình 120,60 MiB, tối đa 121,06 MiB | 60 mẫu, mỗi mẫu cách 1 giây, sau warm-up 5 giây |

Lần đo phòng trống không tạo snapshot payload của ứng dụng, vì vậy đây không
phải benchmark băng thông trực tiếp. Bảng dưới là mô hình payload JPEG trường
hợp xấu nhất từ giới hạn 60 KiB/frame đã được triển khai. Mô hình giả định mọi
frame đều đạt giới hạn và chưa tính overhead của authenticated command, TCP/IP,
Ethernet, retransmission và control traffic khác.

| Snapshot traffic | Chu kỳ 5 giây | Chu kỳ 7 giây | Chu kỳ 10 giây |
| --- | ---: | ---: | ---: |
| Một client, một chiều | 0,098 Mbps | 0,070 Mbps | 0,049 Mbps |
| Theo dõi phòng 50 client, chiều vào server | 4,92 Mbps | 3,51 Mbps | 2,46 Mbps |
| Broadcast màn hình giáo viên tới 50 client qua TCP riêng cho từng client hiện tại | 4,92 Mbps | 3,51 Mbps | 2,46 Mbps |
| Theo dõi và broadcast giáo viên cùng lúc | 9,83 Mbps | 7,02 Mbps | 4,92 Mbps |

Các giá trị này là trần payload của chế độ snapshot định kỳ hiện tại, không
phải throughput đo trực tiếp trên switch. JPEG thực tế có thể nhỏ hơn, trong khi
wire overhead làm mỗi lần truyền lớn hơn một chút. Đường continuous H.264 trong
tương lai cần benchmark rate control và multicast/unicast riêng.

Quyết định cho bản phát hành hiện tại là tiếp tục hoãn monitoring H.264 liên
tục. Snapshot định kỳ là đường được hỗ trợ trong lớp học; không bật hoặc quảng
bá H.264 trước khi ma trận kiểm tra switch nhiều client, decoder, recovery và
bảo mật đạt yêu cầu.

Mười lần khởi chạy trong cùng một phiên Windows đạt trạng thái cửa sổ phản hồi
với median 191,8 ms và trung bình 215,4 ms. Giá trị thấp nhất là 187,4 ms, cao
nhất là 435,1 ms, và lần chạy đầu tiên đo được là 435,1 ms. Đây là độ trễ mở ứng
dụng khi Windows đã chạy, không phải thời gian từ lúc reboot Windows đến khi
manager sẵn sàng.

Để tham khảo, số liệu do chủ dự án cung cấp cho các phần mềm quản lý lớp học
khác trên cùng cấu hình Ryzen 7 5700X là CPU trung bình 11-12%, peak có thể đạt
20%, RAM khoảng 820 MB-1,1 GB với peak 1,3 GB, 30-40 Mbps khi streaming và
khoảng 32 giây cho quá trình boot và mở manager. Các số liệu này không được tái
kiểm chứng độc lập trong lần đo hiện tại; khác biệt về workload và phương pháp
đo vẫn khiến đây chưa phải phép so sánh hoàn toàn cùng điều kiện. Các hệ thống
tham khảo cũng được mô tả là học sinh có thể gỡ cài đặt. Khả năng gỡ NSTU không
được kiểm thử trong benchmark tài nguyên này; nội dung đó vẫn thuộc validation
matrix về policy quản trị, reboot và triển khai Windows.

### Kết quả bổ sung được báo cáo trên i5-6400

Báo cáo một lần chạy NSTU gần đúng trên hệ thống Intel Core
i5-6400 mục tiêu. Kết quả không kèm raw sample, thời lượng, số client hoặc mô tả
workload, vì vậy đây là số liệu sơ bộ chứ chưa phải kết luận chính thức.

| Chỉ số | NSTU trên i5-6400 | So sánh và mức giảm |
| --- | ---: | --- |
| CPU | 4-12% | Chưa có baseline CPU của phần mềm khác trên cùng máy i5. Nếu chỉ dùng mức trung bình 11-12% trên Ryzen trước đó làm tham khảo, mức thấp 4% của NSTU thấp hơn 7-8 điểm phần trăm, tương đương 63,6-66,7%; mức cao 12% trùng với baseline, nên chưa thể khẳng định CPU luôn giảm. |
| RAM | Xấp xỉ tương tự lần chạy NSTU trên Ryzen | Kết quả giữa các máy tương tự nhau, nhưng chưa ghi nhận số RAM i5 đủ chính xác để kết luận. |
| GPU tích hợp | 11% | Phần mềm khác dùng 34% iGPU. NSTU thấp hơn 23 điểm phần trăm, tương đương giảm 67,6%, và dùng khoảng 32,4% mức tải GPU của phần mềm so sánh. |

### Số liệu client được báo cáo tại Trường THCS Vũng Tàu

Giáo viên tại Trường THCS Vũng Tàu đã thực hiện và cho phép ghi nhận các kiểm
tra phía client dưới đây trên ba thiết bị cùng cấu hình Intel Core i5-6400.
Đây là số liệu trung bình gần đúng từ ba lần thử, chưa phải benchmark
production chính thức; workload, cảnh chụp, driver và network có thể làm kết
quả thay đổi.

| Trạng thái client | RAM gần đúng | CPU | GPU / chi tiết capture |
| --- | ---: | ---: | --- |
| Chụp snapshot và service đang chạy | 44 MB | không quá 7% | 11% iGPU / 6% CPU trong mẫu GPU/CPU tương ứng |
| Xem stream từ server có overlay (1080p/60 fps) | 96 MB | 11% | 27% iGPU / 19% CPU |
| Vẽ trực tiếp trên máy học sinh | 72 MB | chưa ghi nhận | Workload vẽ overlay |
| Stream màn hình học sinh về server (720p/30 fps) | 65 MB | chưa ghi nhận | 25% iGPU / 16% CPU |

Kết quả phía client bổ sung cho các phép đo server ở trên. Không nên hiểu đây là
cam kết mọi máy i5-6400 hoặc mọi workload trong phòng học sẽ có đúng các giá trị
này.

### Quan sát về gỡ cài đặt với Deep Freeze

Kiểm tra gỡ client NSTU tại Vũng Tàu cho thấy cần thực hiện đúng quy trình quản
trị Deep Freeze. Phải boot máy ở trạng thái Thawed, mở console Deep Freeze theo
cách thông thường, tắt protection và restart Windows trước khi gỡ hoàn toàn
client; sau đó uninstaller vẫn yêu cầu thêm một lần restart để dừng service và
xóa file. Nếu không mở Deep Freeze và tắt protection, việc gỡ gần như bị chặn
hoàn toàn. Đây là báo cáo quan sát thực địa, không phải khẳng định mọi edition
Deep Freeze đều hoạt động giống nhau.

### Trường phối hợp và địa điểm kiểm thử

Các trường dưới đây đang phối hợp với NSTU với vai trò đối tác dự án hoặc đang
được đánh giá để phối hợp. Kết quả kiểm thử bổ sung vẫn đang được thu thập.

#### Đối tác đã xác minh (hiện tại)

<table>
  <tr>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/le-quy-don-gifted-high-school.png" alt="Logo Trường THPT Chuyên Lê Quý Đôn" width="112"><br><sub><b>Trường THPT Chuyên Lê Quý Đôn</b><br>Đã xác nhận hợp tác</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/ptnk-vnu-hcm.png" alt="Logo Trường Phổ thông Năng khiếu, ĐHQG-HCM" width="112"><br><sub><b>Trường Phổ thông Năng khiếu, ĐHQG-HCM</b><br>Đã xác nhận hợp tác</sub></td>
  </tr>
</table>

#### Đối tác đang chờ xác minh

<table>
  <tr>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/dinh-tien-hoang-high-school.png" alt="Logo Trường THPT Đinh Tiên Hoàng" width="112"><br><sub><b>Trường THPT Đinh Tiên Hoàng</b><br>Đang xác minh tình trạng hợp tác</sub></td>
  </tr>
</table>

Các file logo trong `docs/assets/partners/` là bản đã làm sạch nền từ các hình
ảnh được cung cấp cho báo cáo này.

Dùng `tools/production/collect-benchmarks.ps1` cho các lần đo dài hơn. Vẫn cần
kết quả ở chu kỳ snapshot 5, 7 và 10 giây, với client thật, network traffic,
raw CSV và workload được mô tả rõ trên phần cứng i5-6400 mục tiêu trước khi đưa
ra tuyên bố tài nguyên production.

## Sơ đồ mạng khuyến nghị

```text
Máy giáo viên (NSTU Server)
          |
     Gigabit Ethernet
          |
Ethernet switch/router (snapshot TCP; IGMP tùy chọn cho H.264 tương lai)
     |            |             |
 Client 01     Client 02      Client 50+
```

Trước khi triển khai production:

1. Đặt server và client trong cùng VLAN hoặc subnet tin cậy ở lần triển khai
   đầu tiên.
2. Nên reserve IP server ổn định bên ngoài DHCP pool. Giữ router/gateway ở `.1`
   và dùng địa chỉ khác như `.10` cho máy giáo viên.
3. Nên dùng managed switch hoặc router khi có thể. IGMP snooping và một IGMP
   querier chỉ bắt buộc cho thử nghiệm H.264/multicast liên tục trong tương
   lai; đường snapshot được hỗ trợ dùng kết nối TCP xác thực thông thường.
4. Không expose trực tiếp control/video traffic của NSTU ra Internet.
5. Ưu tiên Ethernet có dây. Nếu thử nghiệm bằng Wi-Fi, tắt AP client isolation
   và kiểm tra switch/AP đáp ứng được lưu lượng snapshot TCP dự kiến.
6. Với triển khai chỉ dùng snapshot, switch thông thường là đủ nếu đáp ứng số
   client và dung lượng uplink đã đo. Không bật multicast chỉ để discovery hoạt
   động. Nếu sau này bật thử nghiệm H.264 tùy chọn, phải kiểm tra riêng IGMP
   snooping, querier, flooding và unicast fallback.
7. Luôn bật Windows Firewall. TCP `47001` là control và snapshot port đã xác
   thực bắt buộc; UDP `47001` dùng để tìm lại địa chỉ server có xác thực trong
   cùng VLAN. UDP `47000` dành cho continuous video tùy chọn trong tương lai và
   nên đóng nếu chưa bật tính năng. Chỉ mở rule cho VLAN phòng học và executable
   cần thiết; không expose rule rộng ra Internet.

Sau khi enroll, client chỉ xem IPv4 server đã lưu là cache. Nếu endpoint đó lỗi,
client broadcast yêu cầu discovery được xác thực bằng PSK trong cùng VLAN, thực
hiện mutual TCP handshake với candidate rồi mới lưu địa chỉ mới. Router có thể
dùng MAC server cho DHCP reservation, nhưng NSTU không xem IP hoặc MAC là bằng
chứng danh tính server. Broadcast recovery không đi xuyên router, vì vậy các
VLAN tách biệt vẫn cần địa chỉ ổn định hoặc routing được quản lý.

Multicast qua nhiều VLAN không thuộc triển khai snapshot được hỗ trợ. Nếu thử
continuous video trong tương lai, phải cấu hình multicast routing có chủ đích
và kiểm tra như một thay đổi mạng riêng.

## Cài bản thử nghiệm hiện tại

Nightly installer được phát hành tại
[trang Releases](https://github.com/Khoasoma/nstu/releases). Đây là artifact
development chưa được ký số nên Microsoft Defender SmartScreen có thể cảnh báo.
Hãy xác minh nguồn release và SHA-256 trước khi chạy:

```powershell
Get-FileHash .\nstu-*-setup.exe -Algorithm SHA256
```

### Server

1. Tải `nstu-<version>-setup.exe` từ pre-release mới nhất.
2. Chạy installer hợp nhất, chọn **Install for Server** và chấp nhận UAC nếu
   Windows yêu cầu.
3. Nếu muốn dùng ngay mà không đăng xuất, hãy mở server một lần:

   ```powershell
   & "$env:ProgramFiles\NSTU\server\nstu-server.exe"
   ```

Installer đăng ký `NSTU Server` là ứng dụng startup toàn máy. `nstu-server.exe`
tự mở trong session tương tác của giáo viên mỗi khi người dùng đăng nhập
Windows; server vẫn là ứng dụng desktop, không phải Windows service chạy trong
Session 0. Minimize hoặc đóng cửa sổ chỉ đưa server xuống notification area.
Chọn **Exit** sẽ dừng server cho tới khi mở thủ công hoặc đăng nhập Windows lần
sau. Helper diagnostics đi kèm installer có thể chạy lại để kiểm tra phần cứng
và mạng khi máy đang ở trạng thái thawed.

Để gỡ server, dùng **Installed apps** của Windows hoặc server uninstaller.
Lần gọi đầu chỉ stage việc gỡ và yêu cầu restart; không xóa service, process hay
package file. Sau reboot, trình gỡ mới buộc dừng `nstu-server.exe`, xóa file
server và diagnostics, rồi lên lịch xóa file còn khóa ở boot sau. Hãy hoàn tất
restart được yêu cầu trước khi cài vai trò client trên máy đó.

Dashboard không tự chèn dữ liệu demo. Registry client khởi động ở trạng thái
trống và chỉ hiển thị record do runtime registry cung cấp. Giáo viên có thể
chuyển giữa `Room screens`, hiển thị toàn bộ client phù hợp trong screen wall
responsive, và `Selected client`, tập trung telemetry, điều khiển snapshot,
annotation, lock/unlock và chat cho một máy. Khoảng chụp điều chỉnh từ 5-10
giây. Mỗi JPEG bị giới hạn ở 60 KiB và queue chỉ giữ snapshot mới nhất để tránh
tích tụ frame cũ. Broadcast màn hình giáo viên dùng cùng đường snapshot có giới
hạn. Dashboard có nút chuyển English/Tiếng Việt và chế độ sáng/tối. Khi minimize
hoặc đóng cửa sổ, server tiếp tục chạy trong notification area của Windows; dùng
menu tray để mở lại hoặc thoát. Continuous H.264/UDP preview vẫn là hạng mục tùy
chọn trong tương lai.

Các lựa chọn Language, Appearance và Screen refresh được gom trong phần
`Settings` trên thanh điều hướng. Screen refresh là chu kỳ snapshot 5-10 giây.

Trong `Selected client`, chỉ bấm `Start remote` khi quản trị viên thực sự cần
điều khiển máy. Preview gửi chuyển động và click chuột trái; ô bàn phím bên
cạnh gửi văn bản ASCII khi nhấn Enter. Remote control tự dừng khi đổi client,
client offline, đổi view hoặc server thoát.

Repository vẫn giữ nền tảng WFP native, nhưng installer hợp nhất và diagnostics
hiện tại chưa có panel allowlist. WFP làm việc với IP đích và port nên policy
tương lai phải tự duy trì việc phân giải domain. NSTU không giải mã HTTPS và
không dùng MITM.

Xem [Hướng dẫn thiết lập](docs/SETUP_GUIDE.vi.md) để biết đầy đủ tính năng
installer, vị trí script, tùy chọn diagnostics và sự khác nhau giữa tùy chọn
build CMake `NSTU_BUILD_SETUP` với tham số runtime. Không có cờ runtime
`--setup`; khi cần kiểm tra riêng, chạy
`diagnostics\nstu-diagnostics.exe`.

Server và client là hai vai trò cài đặt loại trừ lẫn nhau trên cùng một máy
Windows. Installer hợp nhất kiểm tra registry vai trò và layout đã cài trước
khi chép file, rồi dừng với thông báo rõ ràng nếu vai trò đối diện đã có mặt.
Hãy dùng máy riêng cho server giáo viên và client học sinh.

Cũng có thể chọn sẵn Tiếng Việt và dark mode khi khởi động:

```powershell
& "$env:ProgramFiles\NSTU\server\nstu-server.exe" --language=vi --dark
```

### Client

1. Tải và chạy `nstu-<version>-setup.exe` bằng quyền Administrator, chọn
   **Install for Client**, rồi nhập IP server và cổng điều khiển.
2. Installer chạy diagnostics client, đăng ký `nstu-service` để khởi động cùng
   Windows và cấu hình
   service recovery. Service không được khởi động ngay bên trong phiên cài đặt.
3. Restart Windows khi installer yêu cầu. Windows dừng ở màn hình đăng nhập bình
   thường; tự chọn tài khoản lớp học tiêu chuẩn hiện có. NSTU, UWF và Managed mode
   không tạo hoặc chọn tài khoản, không lưu mật khẩu và không cấu hình tự động
   đăng nhập. Sau khi đăng nhập, service mở đúng một instance `nstu-agent.exe`
   trong user session đó.
4. Administrator có thể kiểm tra service sau bằng:

   ```powershell
   Get-Service nstu-service
   ```

Thành phần tự khởi động là client service: installer đăng ký service với
`start= auto`; lần restart bắt buộc sẽ kích hoạt service, sau đó service mở
`nstu-agent.exe` trong user session đã đăng nhập.

`nstu-service.exe` được đăng ký rõ ràng bằng tài khoản `LocalSystem` và chạy ở
Session 0. `nstu-agent.exe` phải chạy trong interactive session của user đang
đăng nhập để Windows cho phép tray, capture, overlay và input. Standard user
không được cấp quyền stop/delete service; sau khi kết nối pipe của agent bị
ngắt, service sẽ thử mở lại agent với retry có giới hạn. NSTU không tuyên bố có
thể chống lại local Administrator hoặc phần mềm kernel-level. Xem
[hướng dẫn kiểm thử vòng đời trên VM](docs/VM_TESTING.md)
để biết các assertion bảo mật cụ thể.

Để gỡ client, dùng **Installed apps** của Windows hoặc NSTU uninstaller. Lần gọi
đầu chỉ xác minh yêu cầu và stage startup task chạy một lần bằng SYSTEM. Nếu
quản trị viên không chấp nhận restart ngay, service, process, file package và
pending-delete state đều không thay đổi. Sau reboot, task mới buộc dừng process
NSTU, xóa service, xóa package và lên lịch file còn khóa cho lần boot kế tiếp.
Gỡ service thủ công không phải quy trình được hỗ trợ.

## Kết nối một phòng máy

Chạy cùng installer hợp nhất trên mỗi máy, chọn Server cho máy giáo viên và
Client cho từng máy học sinh. Đặt các máy trong cùng VLAN tin cậy và cho phép
TCP port `47001` giữa client với server, đồng thời cho phép UDP `47001` để tìm
lại endpoint. Bản thân việc enroll diễn ra trên màn hình: không chép file secret
nào lên máy học sinh, và không chạy lệnh nào cho từng client.

Server vẫn cần data root được bảo vệ, do các helper script của người vận hành
nằm trong thư mục `packaging\` của repository tạo ra. Installer hợp nhất chỉ đóng
gói binary theo vai trò, runtime đi kèm, helper diagnostics và tài nguyên runtime
bài thi; nó không cài PowerShell script hay tài liệu. Hãy chạy các helper này từ
source checkout đúng phiên bản release và đặt `$deployment` là thư mục
`packaging` của checkout.

### 1. Chuẩn bị server

Chạy lệnh sau trên máy giáo viên bằng quyền Administrator để tạo data root được
bảo vệ, rồi khởi động `nstu-server.exe`:

```powershell
$deployment = Join-Path (Get-Location) "packaging"
if (-not (Test-Path (Join-Path $deployment "configure-data-root.ps1"))) {
  throw "Hãy chạy từ source checkout: helper trong packaging không nằm trong sản phẩm đã cài."
}
& (Join-Path $deployment "configure-data-root.ps1") `
  -DataRoot "$env:ProgramData\NSTU"
```

Luồng pairing trên màn hình không tạo enrollment secret nào — server tự mint và
lưu key của từng client khi người vận hành duyệt yêu cầu.

### 2. Enroll từng client bằng pairing

Trên server, mở cửa sổ pairing ("Thêm máy"). Agent trên mỗi máy học sinh quét
LAN tìm server đang mở cửa sổ pairing, tự chạy trao đổi có xác thực hai chiều, và
hiển thị mã sáu chữ số ngay trên màn hình máy đó. Cùng mã đó xuất hiện trong danh
sách chờ của server; người vận hành chỉ duyệt yêu cầu khi hai mã trùng nhau.
**Phép so sánh sáu chữ số đó là gốc tin cậy** — không có gì được chép giữa các
máy để thiết lập nó.

Khi được duyệt, client dẫn xuất protocol key từ transcript (key không bao giờ
được truyền đi), lưu cấu hình DPAPI phạm vi machine mà `nstu-service` dùng, rồi
kết nối. Quy trình client tin cậy:

```text
Cài client
  -> agent quét LAN và hiển thị mã sáu chữ số
  -> người vận hành duyệt mã trùng khớp trên server (gốc tin cậy)
  -> server mint key cho client; client lưu cấu hình được bảo vệ
  -> xác thực với server qua TCP
  -> đăng ký thiết bị và nhận snapshot schedule cùng room policy đã xác thực
  -> chụp JPEG có giới hạn qua kết nối TCP đã xác thực
```

Trên VLAN dùng chung nơi nhiều server cùng trả lời, gán nhãn phòng cho mỗi server
trong giao diện server và gán cho mỗi máy học sinh phòng của nó qua trường
**Room name** tùy chọn của installer hoặc `/ROOM=` khi cài im lặng (ghi
`HKLM\Software\NSTU\PreferredRoom`). Agent khi đó tự chọn server quảng bá đúng
phòng, quay về menu chọn — hoặc pair im lặng khi chỉ một server trả lời — nếu
không đặt phòng hoặc không có server đơn lẻ nào mang phòng đó. Tên phòng chỉ là
gợi ý định tuyến; phép duyệt sáu chữ số vẫn kiểm soát mọi lần pairing, nên phòng
sai hoặc thiếu chỉ hạ xuống menu, không bao giờ dẫn tới pairing nhầm âm thầm.

Đường continuous H.264 tùy chọn chưa nằm trong quy trình enrollment này. Sau
này có thể bổ sung group membership và multicast/unicast đã xác thực, chỉ sau
khi vượt qua các gate kiểm tra switch, decoder và loss-recovery riêng.

Connection preamble chỉ giúp loại nhanh peer sai rõ ràng. Danh tính máy chỉ
được chấp nhận sau khi cryptographic handshake thành công.

### Enrollment thủ công dự phòng (nâng cao)

Đường enroll bằng chép file trước đây đã bị deprecate và không còn được đóng gói
trong installer, nhưng các tool vẫn ở trong repository để phục hồi khi pairing
trên màn hình không khả dụng (ví dụ máy không có phiên tương tác). Từ một source
checkout, xuất secret bootstrap một lần trên server, rồi chạy tool provision trên
từng máy với identity 128-bit và key ID riêng:

```powershell
$deployment = Join-Path (Get-Location) "packaging"
New-Item -ItemType Directory -Path "D:\SecureTransfer" -Force | Out-Null
& (Join-Path $deployment "new-enrollment-secret.ps1") `
  -ExportPath "D:\SecureTransfer\nstu-enrollment.bin"
# Restart nstu-server.exe để nạp enrollment secret đã bảo vệ, rồi trên từng
# client (nstu-provision.exe không được cài; hãy build hoặc chép từ source
# checkout):
$clientId = [guid]::NewGuid().ToString("N")
& ".\nstu-provision.exe" 192.168.10.10 47001 $clientId 1 "D:\SecureTransfer\nstu-enrollment.bin"
```

Tool xác thực enrollment transcript, derive PSK mà không truyền PSK trên mạng,
và lưu cùng cấu hình DPAPI phạm vi machine đó. Giữ file export trong vị trí
removable/thawed được bảo vệ và xóa mọi bản copy khi enroll xong.
`new-enrollment-secret.ps1` chỉ chạy trên server, không cần chạy trên máy học
sinh.

### Staging package bài thi

Archive bài thi chỉ được staging trên client bằng helper do administrator quản
lý `stage-exam-package.ps1`, chạy từ thư mục `packaging\` của source checkout.
Server vẫn giữ package gốc và answer journal trên storage bền vững. Helper bắt
buộc có cả archive SHA-256 và unpacked content SHA-256, từ chối ZIP không an
toàn và zip bomb, rồi publish vào thư mục content-addressed bên dưới data root
bền vững của client:

~~~powershell
$stager = Join-Path (Get-Location) "packaging\stage-exam-package.ps1"
& $stager -ArchivePath "D:\SecureTransfer\exam.nstuexam" -PublishRoot "$env:ProgramData\NSTU\exams\packages" -ExpectedArchiveSha256 "<archive-sha256>" -ExpectedContentSha256 "<content-sha256>" -TrustedPublisherThumbprint "<publisher-thumbprint>"
~~~

Package production phải có entry detached CMS/PKCS#7 `manifest.p7s`
được ký bởi publisher đã cấu hình. `-AllowUnsigned` và
`-AllowNonElevatedTest` chỉ dành cho internal test. Helper không tự
tải package và không thay đổi server, UWF hoặc Deep Freeze.

## Triển khai cùng Deep Freeze

Thử nghiệm UWF/reboot-to-restore và bảo vệ Deep Freeze bên thứ ba chỉ áp dụng
cho máy client. Tuyệt đối không đặt server giáo viên dưới UWF hoặc Deep Freeze:
gói bài thi, `exams/answer-journal.bin`, trạng thái enrollment, audit record và
dữ liệu chẩn đoán phải nằm trên storage bền vững. Outbox câu trả lời của client
được bảo vệ bằng DPAPI chỉ là hàng đợi retry; server journal mới là nguồn chính
thức.

- Cài binary vào vị trí Windows được bảo vệ thông thường.
- Dành riêng một thawed location có ACL chặt cho identity đã enroll, key material
  được bảo vệ, cấu hình, audit log và update state.
- Cấu hình vị trí đó trước enrollment bằng helper trong `packaging\`, ví dụ
  `packaging\configure-data-root.ps1 -DataRoot "D:\NSTUData"`.
- Tuyệt đối không đưa PSK, certificate, dump, screen capture hay runtime secret
  vào repository.
- Không đóng băng image production trước khi đã kiểm thử persistence của
  enrollment, service recovery, upgrade và rollback.
- Boot máy ở trạng thái Thawed và tắt bảo vệ Deep Freeze trước khi cài, nâng cấp
  hoặc gỡ NSTU. Client uninstaller kiểm tra các service `DFServ`/`DeepFrz` đã
  biết và dừng thao tác khi bảo vệ vẫn active; cần xác minh cơ chế kiểm tra thận
  trọng này với đúng edition Deep Freeze được trường sử dụng.

Named pipe và memory-mapped file chỉ giảm I/O tạm thời, không thay thế persistent
protected storage. Deep Freeze sẽ hủy mọi state nằm ngoài thawed space sau reboot.

Phương án tích hợp để thay thế Deep Freeze của NSTU hiện **chỉ ở giai đoạn
nghiên cứu**. Thiết kế an toàn dùng Unified Write Filter của Microsoft, không
dùng kernel driver do NSTU tự viết. Microsoft hỗ trợ UWF trên Enterprise,
Education và IoT Enterprise, nhưng không hỗ trợ Windows Pro hoặc Home. Bản NSTU
hiện tại không bật, cấu hình hay điều khiển UWF và không được mô tả như sản phẩm
thay thế Deep Freeze. Xem [thiết kế khôi phục sau reboot](docs/REBOOT_TO_RESTORE.vi.md)
để biết mô hình quyền, quy trình maintenance/recovery, phương án Windows Pro và
các release gate.

## Lưu ý bảo mật

- Control session hiện tại dùng mutual HMAC authentication và replay protection.
- JPEG snapshot và video datagram được xác thực, nhưng nội dung màn hình chưa
  được mã hóa. Không thử nghiệm màn hình nhạy cảm trên LAN không tin cậy.
- Remote control bị tắt cho đến khi giáo viên bấm `Start remote`; chỉ dùng với
  client đã enroll trong mạng tin cậy. Website policy hiện chỉ hỗ trợ IPv4 và
  TCP port 80/443.
- Automation Authenticode đã có, nhưng artifact nightly vẫn chưa ký; production
  release phải chạy workflow với certificate thật.
- Review/fuzz độc lập, validation từng edition Deep Freeze, ký bằng certificate
  thật và hardware/network matrix vẫn là production blocker.
- CI xanh chỉ chứng minh build và automated test đạt, không chứng minh an toàn
  hoặc độ bền trên phần cứng trường học thật.

## Đóng góp và phát triển

Hướng dẫn build, cấu trúc repository, tùy chọn CMake, test và quy trình fork/PR
nằm trong [development guide](docs/DEVELOPMENT.md). Dependency mới phải tương
thích license permissive; dự án không chấp nhận GPL dependency.

## Người đóng góp

- **Bùi Hồ Hải Đăng (`yanji`)**: đóng góp ý tưởng, tham gia xây dựng NSTU,
  cung cấp thiết bị kiểm thử và review đảm bảo chất lượng cho output cuối.
- **Lê Anh Tuấn (`ssdarealest`)**: chịu trách nhiệm quản lý dự án, hỗ trợ pháp
  lý, xây dựng ý tưởng, quản lý tiến trình và phát triển dự án.
- **Nguyễn Thị Hồng Quyên**: giáo viên môn Tin học tại Trường THPT Chuyên Lê
  Quý Đôn, Thành phố Hồ Chí Minh.

## Bản quyền

Project NSTU được phát hành theo [MIT License](LICENSE). Bạn có thể sử dụng, sao
chép, sửa đổi, công bố, phân phối, cấp phép lại và bán bản sao nếu tuân thủ yêu
cầu giữ thông báo license. Thông báo dependency nằm tại
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
