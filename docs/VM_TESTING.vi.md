# Kiểm thử vòng đời trên máy ảo

[English](VM_TESTING.md) | [Tiếng Việt](VM_TESTING.vi.md)

Dùng một guest Windows dùng một lần cho các bài kiểm thử service, chống can
thiệp và gỡ cài đặt. Tuyệt đối không chạy harness vòng đời có tính phá hủy trên
máy của giáo viên hoặc học sinh. Harness sẽ xóa một bản cài NSTU thử nghiệm, tạo
một người dùng cục bộ tạm thời và thay đổi cấu hình service.

## Ranh giới đặc quyền

NSTU cố ý dùng hai security context của Windows:

| Tiến trình | Context yêu cầu | Lý do |
|---|---|---|
| `nstu-service.exe` | `LocalSystem`, Session 0 | Sở hữu vòng đời đặc quyền, mạng, cấu hình được bảo vệ và giám sát agent. |
| `nstu-agent.exe` | Người dùng phòng máy đã đăng nhập, session tương tác | Sở hữu tray, chat, capture, overlay và UI nhập liệu trên desktop của người dùng đó. |
| `nstu-server.exe` | Tài khoản tương tác của giáo viên | Hiển thị UI quản trị; nâng quyền không phải là yêu cầu runtime thông thường. |
| Installer/uninstaller | Administrator đã nâng quyền | Tạo/xóa service và các file được bảo vệ. |

Chạy agent tương tác dưới `LocalSystem` không làm thiết kế an toàn hơn. Windows
cô lập service trong Session 0, trong khi thao tác màn hình và nhập liệu phải
chạy trong session tương tác mục tiêu. Cách đó cũng phơi bày một bề mặt tấn công
UI và media lớn hơn nhiều với quyền SYSTEM.

Installer client chỉ đăng ký `nstu-service.exe` là `LocalSystem`. Service DACL
của nó cấp quyền điều khiển service cho SYSTEM và Administrators, và chỉ cấp
quyền truy vấn cho người dùng đã xác thực. Sau khi một pipe agent đã thiết lập
bị ngắt, service SYSTEM cố khởi chạy lại có giới hạn trong session đang hoạt
động. Đây là recovery, không phải cam kết rằng tiến trình user-mode là không thể
kill được. Một administrator cục bộ hoặc sản phẩm cấp kernel luôn giữ quyền kiểm
soát máy. Hãy dùng tài khoản phòng máy tiêu chuẩn, ACL bảo vệ Program Files,
policy service và, khi phù hợp, WDAC hoặc AppLocker làm ranh giới chống can thiệp
thực sự.

## Kiểm thử Windows Sandbox dùng một lần

Windows Sandbox phù hợp cho các kiểm tra cài đặt, tài khoản service, phục hồi
agent, service DACL và gỡ cài đặt trước khi restart. Nó không phù hợp để kiểm
định việc xóa ở lần boot kế tiếp vì đóng hoặc khởi động lại Sandbox sẽ hủy guest.

Điều kiện tiên quyết:

- Windows 10/11 Pro, Enterprise hoặc Education với virtualization đã bật.
- Tính năng tùy chọn Windows Sandbox được administrator bật.
- Một bản build Windows hoàn chỉnh chứa `client/nstu-service.exe` và
  `client/nstu-agent.exe`.
- Không có secret hoặc tài liệu enrollment production trong checkout đã map.

Từ một prompt PowerShell host thông thường tại thư mục gốc repository, khởi chạy:

```powershell
.\tools\virtualization\Start-NstuSandboxTest.ps1 `
  -BuildDirectory .\build-verify-werror
```

Launcher map repository ở chế độ chỉ đọc, map một thư mục kết quả riêng ở chế độ
đọc/ghi, và tự khởi động bài kiểm thử bên trong Sandbox. Guest chép toàn bộ input
executable/script vào ổ đĩa cục bộ của nó trước khi cài đặt. Mặc định, kết quả
được ghi dưới
`%TEMP%\nstu-sandbox-results\run-<UTC timestamp>` trên host. Truyền
`-OutputRoot` để chọn thư mục khác nằm ngoài repository.

Bài kiểm thử được cố ý bảo vệ bằng cả cơ chế phát hiện Windows Sandbox lẫn một
marker chỉ dành cho launcher. Script guest từ chối thực thi khi được gọi trực
tiếp trên host vật lý.

Các assertion mong đợi:

1. Service được đăng ký với `StartName=LocalSystem`, khởi động tự động, các
   recovery action đã cấu hình và service DACL hạn chế. Report cũng ghi lại chủ
   tiến trình service đang chạy khi WMI cho phép truy vấn.
2. Service chạy trong Session 0 và khởi chạy agent trong session desktop đang
   hoạt động; agent nằm trong Session 0 hoặc session active-console không khớp
   sẽ làm bài kiểm thử vòng đời thất bại.
3. Kết thúc agent đang kết nối làm xuất hiện một PID agent khác trong khoảng
   timeout watchdog có giới hạn.
4. Một người dùng tiêu chuẩn tạm thời không thể dừng hoặc xóa `nstu-service`.
5. Một lệnh gọi uninstaller trực tiếp trước khi restart bị từ chối và giữ nguyên
   service, các tiến trình, file package và `PendingFileRenameOperations`.
6. Report ghi lại OS build, tên tài khoản, đường dẫn tiến trình, tài khoản
   service, session ID, exit code và trạng thái pending-delete.

Installer production cố ý chờ Windows restart trước lần sử dụng client bình
thường đầu tiên. Harness Sandbox khởi động service thủ công sau khi cài đặt chỉ
để diễn tập vòng đời trước khi restart. Nó không thể thực thi startup task SYSTEM
vốn hoàn tất việc gỡ bỏ sau một lần restart bền vững.

## Kiểm thử restart bền vững

Dùng một guest Hyper-V Generation 2 bền vững cho bài kiểm thử restart chặn phát
hành. Một baseline thực tế là 4 virtual CPU, 6-8 GiB RAM, một VHDX mở rộng động
64 GiB, Secure Boot và một virtual switch NAT/internal được cô lập. Tạo một
checkpoint sạch trước khi cài NSTU. Dùng hai tài khoản guest:

- một administrator riêng cho việc thiết lập và phục hồi;
- một người dùng phòng máy tiêu chuẩn cho việc đăng nhập bình thường và các lần
  thử can thiệp.

Chạy trình tự sau và giữ lại transcript/ảnh chụp màn hình:

1. Cài trong khi guest đang Thawed hoặc chưa chứa Deep Freeze.
2. Xác nhận rằng installer yêu cầu restart và service không được khởi động bên
   trong transaction của installer.
3. Restart. Xác nhận Windows hiện màn hình đăng nhập bình thường; tự chọn tài
   khoản lớp học tiêu chuẩn hiện có. Xác nhận service `LocalSystem` tự khởi động
   cùng agent trong session đó. NSTU, UWF và Managed mode không được tạo/chọn tài
   khoản, lưu mật khẩu hoặc cấu hình autologon.
4. Đăng nhập một lần bằng administrator và xác nhận boot diagnostics từ chối
   session đó cho vận hành lớp học. Đăng xuất rồi trở lại tài khoản tiêu chuẩn.
5. Diễn tập `End task` của Task Manager với agent và kiểm tra recovery.
5. Xác nhận rằng người dùng tiêu chuẩn không thể dừng/xóa service hoặc sửa đổi
   các binary đã cài và data root được bảo vệ.
6. Giữ mở một file package đã cài, chạy uninstaller đã nâng quyền và ghi lại
   `PendingFileRenameOperations`.
7. Restart, rồi xác nhận service trả về error 1060 từ `sc.exe query`, không còn
   tiến trình NSTU nào, và tất cả file package đã lên lịch đều biến mất.
8. Revert về checkpoint sạch trước lần chạy tiếp theo.

Windows Sandbox không thể thay thế bước 3 và 7. Trên host phát triển hiện tại,
các cmdlet quản lý Hyper-V và một Windows ISO/VHD không khả dụng cho session
này, nên guest bền vững phải được tạo sau khi administrator cung cấp các điều
kiện tiên quyết đó cho host.

## Kiểm định Deep Freeze

Deep Freeze là phần mềm đóng băng ổ đĩa của bên thứ ba. Hãy kiểm thử từng edition
và version được hỗ trợ trong một guest bền vững hoặc một máy lab hy sinh được
cung cấp qua đường virtualization được hãng hỗ trợ:

1. Cài và enroll NSTU khi đang Thawed.
2. Freeze và restart; kiểm tra việc khởi động service/agent và sự tồn tại của
   cấu hình.
3. Thử gỡ cài đặt khi đang Frozen; NSTU phải fail closed khi có một service Deep
   Freeze đang active được nhận diện.
4. Tắt Deep Freeze, restart ở Thawed, gỡ NSTU, và hoàn tất lần restart thứ hai
   cần cho việc xóa file bị khóa.

Đừng coi riêng việc phát hiện tên service là bằng chứng tương thích Deep Freeze
cuối cùng. Ghi lại edition sản phẩm, version, trạng thái policy và kết quả vòng
đời thô trong `docs/PRODUCTION_VALIDATION.md`.
