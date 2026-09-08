# Kế hoạch tự động cập nhật NSTU và bảo trì LTSC

Trạng thái: **kiến trúc và bộ thử nghiệm đã được lập kế hoạch; MVP hiện tại
chưa bật cơ chế tự động phân phối gói.**

Tài liệu này dành cho các trường thường bảo trì phần mềm trong kỳ nghỉ hè dài,
thường sau khoảng chín tháng sử dụng. Tài liệu cũng dành cho máy Windows
Long-Term Servicing Channel (LTSC), trong đó phải ghi nhận và kiểm tra chính
xác edition/build thay vì suy đoán chỉ từ tên sản phẩm.

## Mục tiêu

- Cập nhật hơn 50 máy client mà không phải đến từng phòng máy.
- Giữ client và server tương thích trong khi rollout theo từng đợt.
- Bảo đảm bản vá được xác thực, có thể tiếp tục tải, có audit và hoàn tác.
- Giữ nguyên identity đã enroll, cấu hình được bảo vệ và trạng thái service.
- Phối hợp với Deep Freeze, không để bản cập nhật biến mất sau khi reboot.
- Không cần kênh video liên tục hoặc mở inbound firewall cho cập nhật.

## Phạm vi không bao gồm

- Updater không phải cơ chế nâng quyền. MSI, NSIS hay helper tương lai chỉ có
  quyền mà UAC và tài khoản service cấp.
- Updater không được vượt qua Deep Freeze, làm yếu chính sách bảo mật Windows,
  hoặc chạy executable chưa ký từ thư mục người dùng có thể ghi.
- Bản nightly hiện tại không tự cập nhật im lặng. Đây là thiết kế và tiêu chí
  nghiệm thu cho release tương lai.

## Hình dạng triển khai đề xuất

Runtime nên được giữ nguyên tối đa có thể:

```text
Manifest và package release đã ký
              |
              v
nstu-service (LocalSystem, polling HTTPS outbound)
              |
              +--> thư mục staging được bảo vệ
              +--> kiểm tra chữ ký/hash
              +--> chờ cửa sổ bảo trì
              +--> cài đặt gắn với reboot
              +--> health check và rollback
```

Service, không phải agent giao diện, quản lý update state. Dashboard server hiển
thị trạng thái của các client đã enroll, nhưng client vẫn phải tự xác thực
release trước khi cài.

Nên dùng artifact theo role dù vẫn có bootstrapper cho kỹ thuật viên:

- `NSTU-Client-<version>.msi` hoặc role package đã ký tương đương.
- `NSTU-Server-<version>.msi` hoặc role package đã ký tương đương.
- Bootstrapper đã ký cho diagnostics, chọn role và cài offline.

Không để client tự chuyển sang package server hoặc ngược lại. Role đã cài,
role của package và identity của service phải khớp.

## Metadata và độ tin cậy

Manifest phải có schema version, role, version, minimum version, URL, kích
thước, SHA-256, channel (`ltsc-stable`, `security`, `pilot`), thời điểm phát
hành, rollout percentage, Windows build tối thiểu và cờ cần reboot.

Manifest phải được ký bằng release key được giữ ngoài build worker. Mọi
executable/MSI cũng phải có Authenticode hợp lệ kèm trusted timestamp. Client
từ chối manifest/package không ký, hash sai, sai role, downgrade hoặc OS không
được policy hỗ trợ.

Dùng certificate validation bình thường cho HTTPS. Enrollment credential xác
định client với update service; MAC chỉ là gợi ý identity cục bộ, không phải
yếu tố cấp quyền duy nhất.

## State machine cập nhật client

1. **Check**: polling metadata qua HTTPS outbound và ghi kết quả cục bộ.
2. **Defer**: nếu chưa có cửa sổ bảo trì được duyệt, không thay binary nào.
3. **Download**: tải tiếp phần bị gián đoạn vào staging được bảo vệ.
4. **Verify**: kiểm tra chữ ký manifest/package, role, version, kích thước và
   SHA-256.
5. **Stage**: chép package và state record theo version; chưa sửa service/agent.
6. **Apply at reboot**: dừng process NSTU thuộc sở hữu, cài package đã xác
   thực, giữ data root và trả trạng thái cần reboot nếu Windows yêu cầu.
7. **Validate**: kiểm tra service account, automatic start, Session 0, agent,
   enrollment, handshake và package version sau reboot.
8. **Commit hoặc rollback**: giữ package cũ đến khi health check đạt; tự khôi
   phục package cũ khi startup hoặc connectivity thất bại.
9. **Report**: gửi kết quả, error code, boot identity và version cuối về server.
   Không upload screen content hoặc private key trong telemetry.

## Deep Freeze và cửa sổ bảo trì LTSC

Deep Freeze là phần mềm đóng băng dữ liệu của bên thứ ba. Binary ghi vào system
volume được bảo vệ có thể biến mất sau reboot frozen. Vì vậy trường phải có
cửa sổ bảo trì được ghi rõ:

1. Export inventory NSTU, trạng thái enrollment và update state.
2. Ghi nhận image và package version đang hoạt động tốt.
3. Boot hoặc schedule máy ở trạng thái **Thawed** và tắt protection.
4. Xác nhận edition/build Windows LTSC, GPU driver, network policy và
   edition/version Deep Freeze.
5. Cài package đã ký theo từng wave, bắt đầu từ nhóm pilot.
6. Reboot từng wave và chạy boot diagnostics cùng kiểm tra server connectivity.
7. Kiểm tra persistence enrollment, service recovery, snapshot, chat và remote
   control trước khi sang wave tiếp theo.
8. Chỉ refreeze sau khi acceptance check và thời gian rollback kết thúc.

Nếu trường không có cửa sổ thaw hoặc giao diện quản lý Deep Freeze được hỗ trợ,
NSTU phải báo update bị defer. Không được báo thành công chỉ vì package đã tải.

## Lịch rollout chín tháng

### T-8 đến T-6 tuần: chuẩn bị

- Chốt release candidate và publish manifest đã ký.
- Lập ma trận Windows LTSC theo edition/build thực tế của từng trường.
- Test package trên image client/server sạch, bao gồm rollback.
- Xác nhận credential bảo trì Deep Freeze, thaw space và chính sách reboot.

### T-5 đến T-3 tuần: pilot

- Cập nhật một server và nhóm client đại diện nhỏ.
- Bao gồm phần cứng i5-6400/Intel HD 530 và driver có rủi ro cao.
- Đo startup, CPU/RAM/GPU, thời lượng cập nhật và bandwidth.
- Giữ package cũ để rollback ngay.

### T-2 đến T-0 tuần: wave theo trường

- Lập lịch batch phù hợp khả năng switch và reboot của trường.
- Chỉ mở wave tiếp theo khi health report đạt.
- Ghi nhận máy ngoại lệ thay vì retry liên tục khi Deep Freeze hoặc mạng chưa rõ.

### T+1 tuần: kết thúc

- Kiểm tra mọi thiết bị đã enroll báo đúng version và boot identity.
- Đối chiếu inventory cuối với bản export trước bảo trì.
- Chỉ refreeze image đã xác thực và giữ log cho chu kỳ sau.

## Tiêu chí nghiệm thu

Release chỉ sẵn sàng cho trường khi package/manifest có chữ ký và hash hợp lệ;
role conflict pass; install/upgrade/reboot/repair/uninstall được test trên image
LTSC hỗ trợ; enrollment và data root tồn tại; service chạy LocalSystem,
automatic, Session 0; agent chạy ở interactive session; health check lỗi thì
khôi phục package cũ; Deep Freeze được xác thực đúng edition/version; log không
có secret/screen capture/private key; pilot và ít nhất một wave hoàn tất mà
không có rollback không rõ nguyên nhân hoặc reboot loop.

## Bộ thử nghiệm hiện tại

Chạy state-machine trial không phá hủy:

```powershell
pwsh -NoProfile -File packaging/test-update-cycle.ps1
```

Trial tạo payload giả trong thư mục tạm và kiểm tra hash sai, staging thành công,
rollback do health check lỗi và cleanup. Trial không dừng service, không reboot,
không sửa Program Files, không gọi update server và không thay đổi Deep Freeze.
Validation LTSC/Deep Freeze thực tế vẫn là production gate.

### Biên bản trial: 2026-09-08

Trial đầu tiên trong repository đạt trên PowerShell 7 và Windows PowerShell
5.1. Trial từ chối payload đã bị sửa, chấp nhận payload khớp SHA-256, kích hoạt
version staging khỏe, rollback version không đạt health check và giữ version
khỏe gần nhất. Đây chỉ là evidence cho state machine, không phải evidence cho
thay service thật, reboot, image LTSC hoặc chu kỳ bảo trì Deep Freeze.

## Trạng thái hiện tại

MVP hiện có nền tảng package đã ký và lifecycle reboot-safe, nhưng chưa có client
auto-update nền và service manifest phía server. Bước tiếp theo là thêm state
machine cập nhật sau một feature flag do administrator kiểm soát, chạy trial,
rồi chạy test VM disposable trước khi bật cho bất kỳ trường nào.
