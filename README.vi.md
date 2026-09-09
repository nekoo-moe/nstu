<img width="1700" height="1000" alt="nstu" src="https://github.com/user-attachments/assets/16236a59-5fd7-4a58-8985-482ba0f31a06" />

---

# Project NSTU

[English](README.md) | [Tiếng Việt](README.vi.md) | [Development guide](docs/DEVELOPMENT.md) | [Hướng dẫn thiết lập](docs/SETUP_GUIDE.vi.md) | [Thiết kế khôi phục sau reboot](docs/REBOOT_TO_RESTORE.vi.md) | [Tự động cập nhật và bảo trì LTSC](docs/AUTO_UPDATE_LTSC.vi.md) | [Kiểm thử VM](docs/VM_TESTING.md)

[![C++](https://img.shields.io/badge/C++-21%2B-blue?logo=c++&logoColor=white)](https://en.wikipedia.org/wiki/C%2B%2B)
[![License](https://img.shields.io/badge/license-mit%20license-lightgrey)](#licensing)
[![Windows CI](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

[![CI](https://img.shields.io/github/actions/workflow/status/nekoo-moe/nstu/production.yml)](https://github.com/nekoo-moe/nstu/actions)
[![Contributors](https://img.shields.io/github/contributors/nekoo-moe/nstu)](https://github.com/nekoo-moe/nstu/graphs/contributors)
[![Commit activity](https://img.shields.io/github/commit-activity/m/nekoo-moe/nstu)](https://github.com/nekoo-moe/nstu/commits)
[![Last commit](https://img.shields.io/github/last-commit/nekoo-moe/nstu)](https://github.com/nekoo-moe/nstu/commits)
[![Issues](https://img.shields.io/github/issues/nekoo-moe/nstu)](https://github.com/nekoo-moe/nstu/issues)

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
| Server | Intel Core i5-6400, RAM 8 GB, trống 512 MB, Windows 10/11 x64 | Khuyến nghị Gigabit Ethernet có dây |
| Client | Intel Core i5-6400, RAM 8 GB, trống 512 MB, Windows 10/11 x64 | Khuyến nghị Ethernet có dây |
| Router/switch | Hỗ trợ UDP multicast, IGMPv2 hoặc IGMPv3, IGMP snooping và IGMP querier | Một LAN/VLAN được kiểm soát cho lần triển khai đầu |

Với server quản lý từ 50 máy trở lên, RAM 16 GB và SSD là lựa chọn thận trọng
cho đến khi mục tiêu 8 GB vượt qua kiểm thử phần cứng dài hạn. Intel HD Graphics
530 là baseline cho hardware acceleration, không phải cam kết hoạt động với mọi
phiên bản driver.

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
`--auto-close`, `--diagnostics-stay-open` và `--report=...`. `--log=...` vẫn là
alias của `--report=...` để tương thích với boot-check hiện có. Report là JSON
local chỉ chứa kết quả kiểm tra; không chứa enrollment secret, key, token hoặc
tài liệu người dùng. Kiểm tra UWF hoàn toàn chỉ đọc: edition không hỗ trợ hoặc
provider bị thiếu sẽ tạo warning; diagnostics không bật UWF, sửa registry hoặc
service, hay reboot máy.

Popup `Diagnostics` riêng bên trong `nstu-server.exe` hiển thị adapter/hãng
DXGI, feature level, khả dụng Desktop Duplication, trạng thái dự phòng WARP
và các HRESULT gần đây có giới hạn. Nếu khởi tạo đồ họa thất bại
trước khi server UI mở, Windows hiển thị hộp thoại gốc với lỗi đã ghi.

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
    <td align="center" valign="top" width="180">
      <img src="docs/assets/partners/vung-tau-junior-high.png" alt="Logo Trường THCS Vũng Tàu" width="112"><br>
      <sub><b>Trường THCS Vũng Tàu</b><br>Đã hoàn thành kiểm thử; giáo viên thực hiện và cho phép</sub>
    </td>
  </tr>
</table>

#### Đối tác đang chờ xác minh

<table>
  <tr>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/vo-truong-toan-junior-high.png" alt="Logo Trường THCS Võ Trường Toản" width="112"><br><sub><b>Trường THCS Võ Trường Toản</b><br>Đã đồng ý phối hợp; chờ kiểm thử</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/dinh-tien-hoang-high-school.png" alt="Logo Trường THPT Đinh Tiên Hoàng" width="112"><br><sub><b>Trường THPT Đinh Tiên Hoàng</b><br>Đã đồng ý phối hợp; chờ kiểm thử</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/le-quy-don-gifted-high-school.png" alt="Logo Trường THPT Chuyên Lê Quý Đôn" width="112"><br><sub><b>Trường THPT Chuyên Lê Quý Đôn</b><br>Đã đồng ý phối hợp; chờ kiểm thử</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/ptnk-vnu-hcm.png" alt="Logo PTNK ĐHQG-HCM" width="112"><br><sub><b>PTNK, ĐHQG-HCM</b><br>Đã đồng ý phối hợp; chờ kiểm thử</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/ben-cat-high-school.png" alt="Logo Trường THPT Bến Cát" width="112"><br><sub><b>Trường THPT Bến Cát</b><br>Đang xem xét; chưa xác nhận đối tác</sub></td>
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
Managed switch/router có IGMP snooping + một IGMP querier
     |            |             |
 Client 01     Client 02      Client 50+
```

Trước khi triển khai production:

1. Đặt server và client trong cùng VLAN hoặc subnet tin cậy ở lần triển khai
   đầu tiên.
2. Bật IGMP snooping trên managed switch và đảm bảo chỉ một router hoặc switch
   Layer 3 làm IGMP querier cho VLAN đó.
3. Không expose trực tiếp control/video traffic của NSTU ra Internet.
4. Ưu tiên Ethernet có dây. Nếu thử nghiệm bằng Wi-Fi, tắt AP client isolation
   và kiểm tra multicast không bị ép xuống legacy data rate.
5. Không nên dùng unmanaged switch cho phòng máy lớn. Nếu không có IGMP
   snooping, multicast có thể bị flood đến mọi port. Nếu multicast bị chặn,
   fallback unicast dự kiến sẽ làm băng thông server và switch tăng theo từng
   client.
6. Luôn bật Windows Firewall. TCP `47001` là control port mặc định và UDP `47000`
   dành cho video transport. Chỉ mở rule cho VLAN phòng học và executable cần
   thiết; không expose rule rộng ra Internet.

Multicast qua nhiều VLAN cần multicast routing được cấu hình có chủ đích. Không
nên bật tính năng này chỉ để discovery hoạt động.

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
3. Khởi động:

   ```powershell
   & "$env:ProgramFiles\NSTU\server\nstu-server.exe"
   ```

Server không được đăng ký thành Windows service và mặc định không tự chạy khi
Windows khởi động; kỹ thuật viên mở thủ công hoặc tạo shortcut/task do trường
quản lý trên máy giáo viên. Helper diagnostics đi kèm installer có thể chạy lại
để kiểm tra phần cứng và mạng khi máy đang ở trạng thái thawed.

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
3. Restart Windows khi installer yêu cầu. Ở lần boot tiếp theo, service tự chạy
   và khởi động đúng một instance `nstu-agent.exe` trong user session đang active
   khi đăng nhập hoặc mở khóa.
4. Sau khi restart, Administrator có thể kiểm tra service bằng:

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

Quy trình enrollment hiện dùng command line và phải thực hiện khi máy đang
thawed, trong PowerShell chạy bằng quyền Administrator. Chạy cùng installer hợp
nhất trên mỗi máy, chọn Server cho máy giáo viên và Client cho từng máy học
sinh. Đặt các máy trong cùng VLAN tin cậy và cho phép TCP port `47001` giữa
client với server.

Các lệnh dưới đây dùng script được đóng gói cùng installer. Installer đầy đủ
đặt script tại `C:\Program Files\NSTU\docs\deployment`; file tải riêng
`nstu-server.exe` hoặc `nstu-client.exe` không chứa PowerShell script. Nếu chỉ
có binary riêng, hãy tải installer hợp nhất hoặc checkout source đúng phiên bản
trước khi tiếp tục. Trong source checkout, các file tương ứng
nằm trong thư mục `packaging\`.

Nếu dùng source checkout, thay `$deployment` trong ví dụ bằng thư mục
`packaging` của checkout, ví dụ:

```powershell
$deployment = Join-Path (Get-Location) "packaging"
& (Join-Path $deployment "configure-data-root.ps1") -DataRoot "D:\NSTUData"
& (Join-Path $deployment "new-enrollment-secret.ps1") `
  -ExportPath "D:\SecureTransfer\nstu-enrollment.bin"
```

### 1. Chuẩn bị server

Chạy các lệnh sau trên máy giáo viên bằng quyền Administrator. Lệnh đầu tạo
data root được bảo vệ; lệnh thứ hai cài enrollment secret đã mã hóa cho server
và xuất secret dùng một lần để provision client:

```powershell
$deployment = Join-Path $env:ProgramFiles "NSTU\docs\deployment"
if (-not (Test-Path (Join-Path $deployment "configure-data-root.ps1"))) {
  throw "Thiếu script triển khai NSTU; hãy cài lại NSTU và chọn vai trò Server."
}
New-Item -ItemType Directory -Path "D:\SecureTransfer" -Force | Out-Null
& (Join-Path $deployment "configure-data-root.ps1") `
  -DataRoot "$env:ProgramData\NSTU"
& (Join-Path $deployment "new-enrollment-secret.ps1") `
  -ExportPath "D:\SecureTransfer\nstu-enrollment.bin"
```

Restart `nstu-server.exe` để nạp enrollment secret đã bảo vệ. Giữ file export
trong vị trí removable/thawed được bảo vệ cho đến khi provision xong toàn bộ
client. `new-enrollment-secret.ps1` chỉ chạy trên server, không cần chạy trên
máy học sinh.

### 2. Provision từng client

Trên từng máy học sinh, khi server đang chạy, dùng identity 128-bit và key ID
riêng. `nstu-provision.exe` được cài khi chọn vai trò Client:

```powershell
$clientId = [guid]::NewGuid().ToString("N")
& "$env:ProgramFiles\NSTU\client\nstu-provision.exe" `
  192.168.10.10 47001 $clientId 1 "D:\SecureTransfer\nstu-enrollment.bin"
```

Tool xác thực enrollment transcript, derive PSK mà không truyền PSK trên mạng,
và lưu cấu hình client bằng machine-scope DPAPI. Sau khi lệnh thành công, restart
client service hoặc Windows. Khi đã enroll toàn bộ client, xóa mọi bản copy của
file export dùng một lần. Quy trình client tin cậy:

```text
Cài client
  -> cấp danh tính riêng và enrollment credential được bảo vệ
  -> xác thực với server qua TCP
  -> đăng ký thiết bị và nhận room policy
  -> nhận cấu hình video group đã được xác thực
  -> join multicast, đo packet loss và fallback unicast có giới hạn khi cần
```

Connection preamble chỉ giúp loại nhanh peer sai rõ ràng. Danh tính máy chỉ
được chấp nhận sau khi cryptographic handshake thành công. Installer là cách
phân phối được hỗ trợ cho các script này; chỉ chép riêng file EXE là không đủ
để thiết lập enrollment.

## Triển khai cùng Deep Freeze

- Cài binary vào vị trí Windows được bảo vệ thông thường.
- Dành riêng một thawed location có ACL chặt cho identity đã enroll, key material
  được bảo vệ, cấu hình, audit log và update state.
- Cấu hình vị trí đó trước enrollment bằng `configure-data-root.ps1 -DataRoot
  "D:\NSTUData"`.
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

## Bản quyền

Project NSTU được phát hành theo [MIT License](LICENSE). Bạn có thể sử dụng, sao
chép, sửa đổi, công bố, phân phối, cấp phép lại và bán bản sao nếu tuân thủ yêu
cầu giữ thông báo license. Thông báo dependency nằm tại
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
