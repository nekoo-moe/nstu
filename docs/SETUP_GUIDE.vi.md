# Hướng dẫn thiết lập NSTU

[README tiếng Việt](../README.vi.md) | [English](../README.md)

## Installer hợp nhất

Bản phát hành dùng một gói duy nhất: `nstu-<version>-setup.exe`. Màn hình đầu
tiên cho phép chọn **Install for Client** hoặc **Install for Server** và không
bao giờ cài đồng thời hai vai trò trong cùng thư mục. Installer kiểm tra vai trò
đối diện trước khi chép file.

### Bản thử VM nội bộ

CI dành cho developer cũng phát hành artifact riêng
`nstu-<version>-internal-vm-setup.exe`. Artifact được gắn nhãn **Internal VM
Test**, ghi `BuildChannel=InternalVmTest` và cho phép cảnh báo RAM thấp để
kiểm thử cài đặt, diagnostics UWF và hành vi restart trên VM thiếu cấu hình.
Artifact này không thay đổi installer production: bản production vẫn yêu cầu
tối thiểu RAM 6 GiB cùng link vật lý 100 Mbps. Model, kiến trúc và số core CPU
được ghi lại để tham khảo nhưng không chặn cài đặt. Không dùng artifact nội bộ
trong trường học.

### Bootstrap trên máy sạch

Trên máy qualification sạch, `nstu-diagnostics.exe` chưa tồn tại trước khi
installer hợp nhất được chuyển vào và chạy. Hãy chuyển
`nstu-<version>-setup.exe` qua kênh do operator kiểm soát, kiểm tra chữ ký rồi
chọn đúng một role. Installer chép diagnostics vào
`C:\Program Files\NSTU\diagnostics\`; với client, Windows phải restart theo
quy trình trước khi service hoạt động.

Endpoint RDP hoặc port-forward tạm thời dùng để truy cập máy không phải cổng
control của NSTU. Ở trang cài client, nhập đúng địa chỉ server NSTU và cổng
control (`47001` mặc định), không nhập cổng remote-access tạm thời.

Với client, nhập IP server và cổng điều khiển (`47001` mặc định). Installer chạy
diagnostics trước khi đăng ký service, lưu địa chỉ server cho diagnostics sau
khi đăng nhập, đăng ký `nstu-service` là service `LocalSystem` tự khởi động
và thực hiện thêm một kiểm tra TCP có timeout giới hạn trên layout client đã
cài. Kiểm tra này phải thành công trước khi installer đến bước restart bắt
buộc. Service chỉ được kích hoạt sau lần restart đó. Địa chỉ nhập tại đây không
phải enrollment credential và chưa phải cấu hình runtime có xác thực của
service.

Cài client là thao tác trong trạng thái thawed. NSTU không bật, tắt hoặc thay
thế Microsoft UWF và không cố điều khiển Deep Freeze bên thứ ba. Hãy cài khi
máy đang thawed rồi cho installer restart Windows. Cách này giữ tương thích
với phần mềm đóng băng hiện có; boot check của client chỉ quan sát trạng thái
sau đó.

### Địa chỉ ổn định và tự phục hồi

Nên cấp DHCP reservation hoặc IP tĩnh ngoài DHCP pool cho máy giáo viên. Một
sơ đồ dễ quản trị là giữ router/gateway ở `.1` và dành địa chỉ như `.10` cho
NSTU Server; không gán địa chỉ gateway cho server. Đây vẫn là cấu hình production
đơn giản và dễ chẩn đoán nhất.

Sau khi provision có xác thực, `nstu-service` chỉ xem IP server đã lưu là cache.
Nếu kết nối TCP hoặc mutual authentication thất bại, client gửi UDP discovery
được xác thực bằng HMAC trên chính control port, kiểm tra response bằng PSK đã
enroll, hoàn tất mutual TCP handshake rồi mới lưu IPv4 mới bằng DPAPI phạm vi
machine. Service cũng cập nhật địa chỉ registry không chứa secret cho diagnostics
sau đăng nhập. NSTU không tin hoặc dùng địa chỉ MAC của server làm yếu tố xác
thực.

Cho phép **TCP và UDP `47001`** inbound tới executable NSTU Server, chỉ từ VLAN
phòng máy được quản lý. TCP mang control và snapshot; UDP `47001` chỉ dùng cho
trao đổi tìm lại endpoint có giới hạn. UDP `47000` vẫn dành cho đường video liên
tục đang trì hoãn và nên đóng khi không thử nghiệm tính năng đó.

Broadcast IPv4 không đi xuyên router. Hai phòng máy trong cùng VLAN có thể tìm
cùng server đã enroll; nếu khác VLAN thì phải dùng IP reservation ổn định hoặc
DNS/cấu hình thủ công do quản trị viên quản lý cho tới khi có relay xác thực.
Khi router, switch, DHCP hoặc server không hoạt động, client vẫn giữ enrollment
và retry có jitter nhưng control phòng học sẽ offline. Khi mất kết nối, trạng
thái exam, lock, broadcast, annotation và remote control tạm thời được dọn thay
vì bị giữ vô thời hạn.

Installer vẫn cần địa chỉ đang truy cập được cho TCP preflight ban đầu; discovery
chỉ hoạt động sau khi enrollment một lần đã cài PSK cho client.

Với server, diagnostics kiểm tra display, link mạng và encoder H.264 phần cứng
trước khi cài file server và data root được bảo vệ. UWF được báo là không áp
dụng cho role server vì dữ liệu server phải bền vững. Chạy qualification UWF
bằng `--target=client` trên image client riêng.

Installer server đồng thời tạo giá trị startup toàn máy `NSTU Server` tại
`HKLM\Software\Microsoft\Windows\CurrentVersion\Run`. Vì vậy ứng dụng desktop
server tự chạy trong session tương tác của giáo viên ở mỗi lần đăng nhập
Windows; nó không được cài thành service trong Session 0. Đóng hoặc thu nhỏ cửa
sổ chính vẫn giữ tiến trình trong khay hệ thống. Chọn **Exit** sẽ dừng server cho
tới khi mở thủ công hoặc đăng nhập lần sau. Unified uninstaller xóa giá trị
startup này.

## Diagnostics tích hợp

`diagnostics\nstu-diagnostics.exe` được installer sử dụng và cũng có thể chạy
riêng bởi kỹ thuật viên. Các kiểm tra hiện tuần tự. Nếu không có
`--auto-close`, cửa sổ giữ nguyên để xem lại. Với `--auto-close`, chỉ run hoàn
toàn sạch mới tự đóng; warning và lỗi đều giữ cửa sổ, còn lỗi trả về exit code
khác 0. Installer truyền `--installer --auto-close`; run có issue giữ cửa sổ
trong sáu giây rồi tự đóng để preflight NSIS chạy đồng bộ không bị treo. Report
được ghi trước khi thoát và exit code lỗi vẫn được giữ, nên NSIS có thể dừng cài
đặt an toàn. Nhánh installer sạch đóng sau một khoảng trễ ngắn. Dùng
`--diagnostics-stay-open` nếu kỹ thuật viên cần giữ cửa sổ của run sạch; không
kết hợp cờ này với preflight installer đồng bộ. Khi dừng cài đặt, installer giữ
report tại `%TEMP%\NSTU-installer-preflight.json`. Dùng `--report=<path>` để lưu
kết quả JSON có cấu trúc; `--log=<path>` vẫn được giữ làm alias tương thích.
Kiểm tra UWF chỉ đọc, không bật filter, sửa registry/service hoặc reboot.
Edition không hỗ trợ và provider không khả dụng sẽ được báo warning, vì vậy
Windows Pro/Home vẫn chỉ ở chế độ audit. Nếu edition được hỗ trợ nhưng truy
vấn optional feature không hoàn tất, kết quả sẽ là `probe unavailable` thay vì
`feature missing`; hãy chạy lại diagnostics với quyền cục bộ cần thiết và
không bật UWF dựa trên kết quả chưa xác định.

Trên image client được hỗ trợ, probe chỉ đọc còn báo trạng thái protected volume
hiện tại/kế tiếp, số lượng exclusion mà không lưu tên path, loại/kích thước tối
đa/mức sử dụng/threshold overlay và sức khỏe event UWF trong bảy ngày gần nhất.
Công việc WMI có timeout giới hạn: các lần đọc exclusion dùng chung budget hai
giây; mỗi event query có budget hai giây và giới hạn 256 event. Kết quả đọc một
phần hoặc chạm giới hạn được báo warning, không được coi là phê duyệt. Các
diagnostics này không triển khai mutation UWF nào.

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

## Payload installer và script vận hành

Installer hợp nhất chỉ đóng gói binary theo vai trò (`nstu-service`,
`nstu-agent`, `nstu-provision` cho client; `nstu-server` cho server), runtime
MinGW đi kèm, helper diagnostics độc lập và tài nguyên runtime bài thi
(`exam/web`, `exam/schema`, `exam/examples`). Nó **không** đóng gói PowerShell
script, tài liệu hay ảnh chỉ dùng cho repository trong `docs\assets\`. Standalone
EXE không đăng ký service và không phải nguồn cài đặt được hỗ trợ.

Các helper script vận hành nằm trong thư mục `packaging\` của repository và được
chạy từ source checkout đúng phiên bản release, không phải từ sản phẩm đã cài.
Helper vai trò client cấu hình service, data root, recovery policy và ACL bảo
vệ. Helper vai trò server kiểm tra role và data root được bảo vệ.

`packaging\stage-exam-package.ps1` staging package bài thi trên client. Chạy
script từ PowerShell elevated trong source checkout sau khi chép archive
`.nstuexam` và metadata release đã được phê duyệt. Phải cung cấp cả archive
SHA-256 và unpacked content SHA-256; package production phải có detached
publisher signature `manifest.p7s` cùng thumbprint certificate được phê duyệt.
Truyền `-PublishRoot` là đường dẫn tuyệt đối bên dưới data root bền vững đã cấu
hình cho client (mặc định là `%ProgramData%\NSTU\exams\packages`); helper không
tự dò root hoặc tải/copy archive từ server. `-RequireAuthenticode` là policy bổ sung
tùy chọn cho các file `.exe` và `.dll` trong package, tách biệt với detached
manifest signature bắt buộc. Chỉ dùng helper để staging trên client, không dùng
server data root. Helper không bật UWF hoặc thay đổi Deep Freeze. Package phải
có `exam/web/index.html`; xem [định dạng package bài thi](EXAM_ASSESSMENT.vi.md).
`-AllowUnsigned` và
`-AllowNonElevatedTest` chỉ dành cho disposable developer test.

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

Sau khi cài server, tạo secret một lần bằng cách chạy
`packaging\new-enrollment-secret.ps1` từ source checkout, sau đó provision từng
client bằng `client\nstu-provision.exe` đã cài. Provisioning ghi cấu hình runtime
được DPAPI bảo vệ mà `nstu-service` sử dụng; IP nhập trong installer chỉ phục vụ
diagnostics cho đến khi trao đổi có xác thực này thành công.

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
