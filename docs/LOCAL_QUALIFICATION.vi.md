# Fixture qualification cục bộ

[English](LOCAL_QUALIFICATION.md) | [Tiếng Việt](LOCAL_QUALIFICATION.vi.md)

Bản ghi này mô tả fixture máy yếu nhất được cung cấp cho việc kiểm thử mức sẵn
sàng của NSTU. Đây là một mục tiêu kiểm thử, tự nó không phải là phê duyệt
production hay một triển khai được hỗ trợ ở trường học.

| Trường | Giá trị báo cáo | Yêu cầu qualification |
|---|---|---|
| CPU | Intel Core i5-7400 | Ghi lại để so sánh workload; danh tính CPU không chặn cài đặt |
| Memory | 8 GiB | Vượt mức tối thiểu 6 GiB; khuyến nghị 8 GiB |
| Windows | Windows 10, báo cáo là “Education/Pro” | Xác định chính xác SKU và build từ báo cáo diagnostics; Education và Pro là hai edition riêng biệt |
| Activation | Chưa kích hoạt | Chỉ ghi nhận; trạng thái kích hoạt không xác lập điều kiện dùng UWF |
| Network | Máy test truy cập được qua Tailscale | Đo link đã thương lượng của adapter vật lý ngay tại chỗ; không dùng địa chỉ Tailscale làm bằng chứng năng lực link |
| Graphics | Chưa xác minh | Ghi lại adapter, phiên bản driver, probe D3D11 và fallback WARP |

## Lượt chạy bắt buộc

Fixture này cố ý sạch. Trước khi bootstrap, nó không có binary NSTU, service,
project checkout hay thư mục diagnostics nào. Hãy chuyển installer hợp nhất đã
được phê duyệt vào VM qua kênh do operator kiểm soát (ví dụ RDP drive
redirection hoặc trang release), kiểm tra chữ ký Authenticode rồi cài đúng một
role. Đừng chỉ chép mỗi `nstu-diagnostics.exe`: installer còn staging các file
runtime và role check cần thiết cho một lượt qualification hợp lệ.

Cổng remote-access tạm thời (nếu lab operator cung cấp) chỉ dành cho phiên tương
tác. Đây không phải cổng control của NSTU. Khi cài client, hãy nhập địa chỉ
server thật và cổng control NSTU (mặc định 47001), hoặc dùng các giá trị server
được cung cấp cho lab.

Sau khi installer hoàn tất và lần restart bắt buộc kết thúc, helper diagnostics
sẽ nằm tại:

```text
C:\Program Files\NSTU\diagnostics\nstu-diagnostics.exe
```

Chạy `nstu-diagnostics.exe` đã staging một cách tương tác trên fixture với quyền
administrator, dùng đường dẫn report cục bộ:

```powershell
& "$env:ProgramFiles\NSTU\diagnostics\nstu-diagnostics.exe" `
  --target=client `
  --report="$env:ProgramData\NSTU\qualification-i5-7400.json" `
  --diagnostics-stay-open
```

Report phải ghi lại chính xác tên sản phẩm Windows, SKU/build sản phẩm, kiến
trúc, trạng thái optional-feature/provider của UWF, trạng thái Safe Mode, trạng
thái service và cài đặt, CPU/RAM, link mạng vật lý, driver đồ họa, thời gian hệ
thống và khả năng truy cập server NSTU đã cấu hình. Không đưa mật khẩu, token,
địa chỉ Tailscale, ảnh chụp màn hình hay tài liệu enrollment vào report hoặc
repository.

## Ranh giới role và UWF

Cài role server trên một máy bền vững. Không bật UWF trên máy đó vì server có
thể chứa package bài thi, tài liệu bài giảng, trạng thái enrollment, bản ghi
audit và báo cáo diagnostics. Một lượt diagnostics server báo UWF là
`not_applicable`.

Dùng một image client riêng cho thử nghiệm reboot-to-restore. Với VM development
3 GiB hiện tại, dùng artifact nội bộ `nstu-<version>-internal-vm-setup.exe`; nó
cho phép cảnh báo RAM thiếu để phục vụ test development. Chi tiết CPU chỉ mang
tính tham khảo ở mọi build. Artifact production vẫn giữ mức tối thiểu cài đặt
6 GiB.

Sau khi cài role client, chạy report client trước bất kỳ thay đổi UWF nào:

```powershell
& "$env:ProgramFiles\NSTU\diagnostics\nstu-diagnostics.exe" `
  --target=client `
  --report="$env:ProgramData\NSTU\qualification-client-before-uwf.json" `
  --diagnostics-stay-open
```

Ghi lại report, tạo recovery image hoặc checkpoint, và chỉ sau đó mới theo quy
trình UWF của Windows đã được review để bật tính năng và cấu hình filter.
Restart client, chạy đúng lệnh đó với đường dẫn report mới, và xác minh trạng
thái UWF hiện tại và kế tiếp khớp nhau. Thử một file dùng-một-lần và một path đã
biết là excluded/persistent, rồi tắt UWF và khôi phục checkpoint trước khi lặp
lại chu kỳ. NSTU không tự bật hoặc thay đổi UWF.

## Ranh giới chấp nhận

- 6 GiB RAM là mức tối thiểu để cài đặt; 8 GiB vẫn là mức khuyến nghị.
- i5-7400 được ghi lại như một kết quả so sánh workload, nhưng danh tính CPU và
  topology core không chặn installer. Hãy kiểm định hiệu năng bộ xử lý bằng một
  workload NSTU thực.
- Link đã thương lượng dưới 100 Mbps không đạt ranh giới mạng tối thiểu;
  100 Mbps đạt mức tối thiểu và khuyến nghị 1 Gbps.
- Windows 10 Education có khả năng đủ điều kiện dùng UWF sau khi qualification về
  đúng build, feature, provider, storage, driver và recovery.
- Windows 10 Pro chỉ ở chế độ audit đối với UWF; không control NSTU nào được bật
  hoặc cấu hình reboot-to-restore trên Pro.
- Một image chưa kích hoạt không được NSTU “sửa”. Kích hoạt và cấp phép vẫn là
  trách nhiệm của quản trị viên/trường học.

## Giới hạn remote-access hiện tại

Tài khoản test được cấp có thể truy cập host qua mạng test riêng, nhưng truy cập
WMI từ xa và Service Control Manager trả về `Access is denied`, và không có SMB
share nào được mở. Do đó fixture cần một lượt chạy local/RDP tương tác bởi một
operator có đủ quyền Windows cần thiết. Diagnostics NSTU không làm yếu
remote-UAC, firewall hay account policy để lách việc này.

## Bàn giao cho operator tương tác

Khi lab operator cung cấp một endpoint RDP được forward ra ngoài tạm thời, hãy
kết nối tương tác bằng host và port được cấp ngoài băng:

```powershell
mstsc /v:<host>:<port>
```

Không đưa endpoint, credential, địa chỉ Tailscale/riêng tư hay chứng chỉ RDP vào
repository này. Xác minh chứng chỉ và danh tính host với lab operator trước khi
đăng nhập. Sau khi một administrator đã đăng nhập trên fixture, hãy chạy lệnh
diagnostics ở trên tại chỗ và chỉ trả về report JSON đã làm sạch. Report là bằng
chứng qualification; chỉ riêng khả năng truy cập TCP không xác lập mức sẵn sàng
về OS, UWF, đồ họa, service hay hiệu năng.

Endpoint test được forward ra ngoài hiện tại đã được xác nhận truy cập được trên
cổng TCP được cấp, nhưng tooling NSTU chưa thực hiện đăng nhập tương tác hay
thay đổi máy nào.

## Report nhận từ VM

Report role server đã làm sạch đầu tiên không qualify fixture máy yếu nhất như
quảng bá:

- Windows báo **Windows 10 Pro Education 22H2**, nên Microsoft UWF đúng là chỉ ở
  chế độ audit cho image này.
- VM cho thấy **3 GiB RAM** và **3 bộ xử lý vật lý / 4 logic**. Bộ nhớ dưới mức
  tối thiểu 6 GiB và không phải cấu hình test 8 GiB đã mô tả trước đây; topology
  bộ xử lý được báo chỉ mang tính tham khảo.
- Link **100000 Mbps Tailscale** được báo là kết quả overlay/tunnel và không
  phải bằng chứng link vật lý. NSTU nay loại tunnel adapter khỏi cổng kiểm tra
  link vật lý; hãy chạy lại diagnostics với một adapter Ethernet hoặc Wi-Fi hoạt
  động mà VM nhìn thấy.
- Phần cứng D3D11 và WARP đều có sẵn. Thiếu mã hóa H.264 phần cứng chỉ là warning
  vì chế độ snapshot vẫn là baseline được hỗ trợ.
- Windows Time không chạy. Hãy sửa việc này qua policy image của lab và chạy lại
  kiểm tra; NSTU không tự thay đổi thời gian hệ thống.

Report này chỉ là bằng chứng cho baseline của lab; nó không phải phê duyệt
production. Không ghi VM là mục tiêu 8 GiB hay có khả năng UWF cho đến khi phần
cấp phát bộ nhớ VM, kết nối mạng vật lý và edition Windows được sửa và test lại.
