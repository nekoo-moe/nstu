# Hướng dẫn thiết lập NSTU

[README tiếng Việt](../README.vi.md) | [English](../README.md)

## Installer hợp nhất

Bản phát hành dùng một gói duy nhất: `nstu-<version>-setup.exe`. Màn hình đầu
tiên cho phép chọn **Install for Client** hoặc **Install for Server** và không
bao giờ cài đồng thời hai vai trò trong cùng thư mục. Installer kiểm tra vai trò
đối diện trước khi chép file.

Với client, nhập IP server và cổng điều khiển (`47001` mặc định). Installer chạy
diagnostics trước khi đăng ký service, lưu địa chỉ server cho diagnostics sau
khi đăng nhập, đăng ký `nstu-service` là service `LocalSystem` tự khởi động
và đặt cờ restart bắt buộc. Service chỉ được kích hoạt sau lần restart đó.
Địa chỉ nhập tại đây không phải enrollment credential và chưa phải cấu hình
runtime có xác thực của service.

Với server, diagnostics kiểm tra display, link mạng và encoder H.264 phần cứng
trước khi cài file server và data root được bảo vệ. UWF được báo là không áp
dụng cho role server vì dữ liệu server phải bền vững. Chạy qualification UWF
bằng `--target=client` trên image client riêng.

## Diagnostics tích hợp

`diagnostics\nstu-diagnostics.exe` được installer sử dụng và cũng có thể chạy
riêng bởi kỹ thuật viên. Các kiểm tra hiện tuần tự. Nếu không có
`--auto-close`, cửa sổ giữ nguyên để xem lại. Với `--auto-close`, chỉ run hoàn
toàn sạch mới tự đóng; warning và lỗi đều giữ cửa sổ, còn lỗi trả về exit code
khác 0. Dùng `--report=<path>` để lưu kết quả JSON có cấu trúc;
`--log=<path>` vẫn được giữ làm alias tương thích. Thêm
`--diagnostics-stay-open` nếu kỹ thuật viên cần giữ cửa sổ khi toàn bộ kiểm tra
đạt. Kiểm tra UWF chỉ đọc, không bật filter, sửa registry/service hoặc reboot.
Edition không hỗ trợ và provider không khả dụng sẽ được báo warning, vì vậy
Windows Pro/Home vẫn chỉ ở chế độ audit.

```powershell
& "$env:ProgramFiles\NSTU\diagnostics\nstu-diagnostics.exe" --target=client --server-ip=192.168.10.10 --server-port=47001 --installer
& "$env:ProgramFiles\NSTU\diagnostics\nstu-diagnostics.exe" --target=client --boot-check --auto-close --log="$env:ProgramData\NSTU\boot-check.log"
```

Installer đăng ký lệnh này trong machine `Run` key, vì vậy cửa sổ health check
chạy khi user đăng nhập sau boot. Nó xác nhận service đang chạy bằng
`LocalSystem` trong Session 0, agent đã cài đang chạy trong session của user,
adapter mạng hoạt động và TCP tới server. Đây không phải pre-logon check
trong Session 0. Agent cố ý không chạy bằng SYSTEM: tray, overlay, capture và
input phải chạy trong interactive user session.

Diagnostics không enrollment và không tạo key chỉ từ IP. Enrollment authenticated
một lần vẫn thực hiện theo phần dưới.

## Script và installer

Gói đầy đủ chứa lifecycle script trong `client\` và `docs\deployment\`, cùng
các tài liệu Markdown nhưng không đóng gói ảnh chỉ dùng cho repository trong
`docs\assets\` như screenshot và logo đối tác. Standalone EXE không đăng ký
service và không phải nguồn cài đặt được hỗ trợ.
Hai vai trò đều được kiểm tra trước khi cài để tránh xung đột.

## Gỡ cài đặt bắt buộc restart

Lần gọi uninstaller đầu tiên không xóa gì. Script kiểm tra quyền Administrator và
Deep Freeze, ghi kế hoạch gỡ, tạo startup task chạy bằng SYSTEM một lần và yêu
cầu restart Windows. Nếu người dùng bỏ qua restart, service, process và package
file không bị thay đổi. Sau restart, task kiểm tra uptime đã đổi rồi mới dừng
process, xóa service và lên lịch xóa file bị khóa ở boot tiếp theo.

Deep Freeze là phần mềm bên thứ ba đóng băng dữ liệu. Gỡ cài đặt bị chặn khi các
service bảo vệ được nhận diện đang active; hãy boot ở Thawed và tắt bảo vệ trước
khi stage gỡ.

## Enrollment

Sau khi cài server, tạo secret một lần bằng
`docs\deployment\new-enrollment-secret.ps1`, sau đó provision từng client bằng
`client\nstu-provision.exe`. Provisioning ghi cấu hình runtime được DPAPI bảo vệ
mà `nstu-service` sử dụng; IP nhập trong installer chỉ phục vụ diagnostics cho
đến khi trao đổi có xác thực này thành công.

## Build

Target diagnostics là helper kiểm tra độc lập được installer hợp nhất sử dụng:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DNSTU_ENABLE_PACKAGING=ON
cmake --build build --target nstu-diagnostics
cmake --build build --target nstu-package
```

`nstu-package` cần NSIS `makensis.exe`. Dùng [kiểm thử lifecycle trên VM](VM_TESTING.md)
để kiểm tra service, End Task, uninstall và restart thực tế.

Quy trình cập nhật chín tháng được lập tại [Tự động cập nhật và bảo trì
LTSC](AUTO_UPDATE_LTSC.vi.md). MVP hiện tại chưa tự động thay binary im lặng;
hãy chạy trial không phá hủy trước khi implement hoặc bật updater tương lai:

```powershell
pwsh -NoProfile -File packaging/test-update-cycle.ps1
```
