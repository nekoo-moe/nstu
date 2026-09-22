# Kiến trúc

[English](ARCHITECTURE.md) | [Tiếng Việt](ARCHITECTURE.vi.md)

## Mô hình tiến trình

```text
nstu-server.exe       Giao diện quản trị ImGui/D3D11 và trạng thái client
nstu-service.exe      Service LocalSystem Session 0, policy và vòng đời
nstu-agent.exe        Tray và overlay toàn màn hình của user đã đăng nhập
```

Installer hợp nhất đăng ký `nstu-server.exe` trong `Run` key phạm vi machine để
giao diện giáo viên khởi động khi sign-in tương tác. Nó cố ý không phải một Windows
service: D3D11, ImGui và quyền sở hữu notification-area vẫn nằm trong phiên user đã
đăng nhập. Uninstaller gỡ đăng ký khởi động.

Service và agent tách biệt vì Windows service không thể tương tác trực tiếp với
desktop của user đã đăng nhập. IPC dùng một named pipe local có DACL cho phép
SYSTEM, Administrators và user tương tác, đồng thời từ chối client từ xa. Sau một
kết nối, service lấy PID của peer từ pipe và chỉ chấp nhận khi image tiến trình là
`nstu-agent.exe` đã cài trong một phiên tương tác active.

Installer đăng ký service rõ ràng dưới `LocalSystem`. Service launch agent bằng
token của user active qua `WTSQueryUserToken` và `CreateProcessAsUserW`; agent
không được nâng lên SYSTEM. DACL của service ngăn một tài khoản phòng học chuẩn
dừng hay xóa service, và service thử relaunch agent có giới hạn sau khi một pipe
đã thiết lập bị ngắt. Điều này bảo vệ lõi đặc quyền trong khi vẫn giữ ranh giới
Session 0 của Windows. Nó không ghi đè một local administrator hay một sản phẩm
bảo mật cấp kernel. Các quy trình test disposable và reboot được ghi trong
`docs/VM_TESTING.md`.

Service nạp danh tính client và PSK từ một cấu hình DPAPI phạm vi machine, kết nối
lại tới server giáo viên, hoàn tất handshake HMAC hai chiều, và forward các command
đã xác thực tới agent của phiên active. Status agent được trả về qua cùng pipe rồi
báo cáo tới server. Địa chỉ IPv4 server đã lưu là một cache reconnect, không phải
mỏ neo danh tính.

## Đường đánh giá dựa trên máy tính

Bề mặt bài thi là một web UI local đóng gói trong package dưới `exam/web/`.
`ExamHost` native trong `client/src/exam_host.cpp` được triển khai trong
`nstu-agent` và chạy trên thread UI/STA của agent. Trong các build cấu hình với
`NSTU_ENABLE_WEBVIEW2_HOST=ON`, nó nhận một package root đã được giải nén sẵn, xác
thực manifest và SHA-256 digest đã ghim, kiểm tra lại package digest ngay trước khi
map virtual-host, tạo một cửa sổ Win32 toàn màn hình topmost, map package tới
`https://nstu.exam/`, inject context chỉ-danh-tính, ràng buộc package, và forward
các browser message đã xác thực qua ranh giới agent/service. Host cũng chặn điều
hướng ra ngoài virtual host local, metadata package ngoài web root đã xác thực,
cửa sổ popup, developer tools, context menu mặc định và zoom; nó giữ cửa sổ ở
foreground và chặn các shortcut user-mode phổ biến khi bài thi đang active.

`nstu-service` vẫn là ranh giới transport và độ bền đã xác thực; nó không được tạo
một cửa sổ desktop, và server không bao giờ host browser của học sinh. Đường
response của service publish các message `ExamBridge` có giới hạn, mà host drain
trên thread UI của agent. Các build với `NSTU_ENABLE_WEBVIEW2_HOST=OFF` giữ lại
bridge đó và các protocol test cho dùng offline/CI, nhưng cố ý fail closed khi một
bài thi được bắt đầu. Một triển khai bật WebView2 yêu cầu `WebView2Loader.dll` đã
ký cạnh `nstu-agent.exe` và một WebView2 Runtime tương thích trên client. Agent chỉ
nạp loader đóng gói đó và probe Runtime đã cài trước khi launch; nó không dùng
fallback system-DLL.

Mỗi profile browser bài thi native được dẫn xuất từ package digest, danh tính
client đã provision, và session ID đã xác thực dưới profile root được phê duyệt của
user tương tác. Do đó virtual origin cố định không thể mang một service worker hay
cache từ một package/context khác. Một profile path do server cung cấp chỉ được
chấp nhận khi nó khớp chính xác path dẫn xuất local đó.

```text
Package .nstuexam (server sở hữu; yêu cầu ký release cho production)
  -> staging và giải nén theo phiên do deployment sở hữu với ghim archive/content,
     xác thực detached publisher-signature, và publication nguyên tử
  -> xác thực manifest/path/digest của ExamHost native
  -> bề mặt bài thi WebView2 trong nstu-agent
  -> exam_answer_event / exam_state_request đã xác thực
  -> agent ExamBridge -> service LocalSystem
  -> outbox client DPAPI (chỉ retry)
  -> control channel TCP đã xác thực
  -> AnswerJournal chỉ-ghi-thêm phía server (nguồn chân lý)
```

Server giữ package, record answer journal, state export và dữ liệu chấm điểm ngoài
bất kỳ ranh giới UWF hay Deep Freeze nào. Chỉ outbox retry client có giới hạn mới
được đặt trong vị trí persistence client được phê duyệt. Một response reconnect
server mang hash event cuối cùng và metadata chunk để browser có thể rebase các
event đang chờ mà không phải đoán qua một xung đột.

Server shell trình bày hai chế độ vận hành. `Room screens` dùng một grid phản hồi
nhanh cho mọi client hiển thị, tóm tắt sức khỏe, search/filter, và một interval
snapshot 5-10 giây. `Selected client` cung cấp telemetry tập trung, preview
snapshot 16:9, lock/unlock, điều khiển snapshot, annotation và chat. Registry khởi
đầu rỗng và không chèn record client minh họa. Client agent dùng các control Win32
LISTBOX/EDIT/BUTTON chuẩn cho một cửa sổ chat nhỏ; không framework UI nào được thêm
vào client.

Server UI có bản địa hóa Anh/Việt tại runtime với dải glyph tiếng Việt của Segoe UI
và một palette ngữ nghĩa sáng/tối. Nó sở hữu một icon notification-area native.
Minimize và close ẩn cửa sổ trong khi control plane vẫn active; menu tray khôi phục
cửa sổ hoặc thoát tiến trình. Việc Windows tái tạo taskbar khiến icon được đăng ký
lại.

## Đường snapshot

```text
Màn hình chính của client (GDI)
  -> downscale có giới hạn
  -> encode WIC JPEG, tối đa 60 KiB
  -> named pipe service-agent
  -> control channel TCP đã xác thực
  -> slot registry client theo frame mới nhất
  -> decode WIC và texture snapshot D3D11
```

Snapshot được lên lịch mỗi 5-10 giây. Service thay một snapshot đã queue cũ hơn
bằng cái mới nhất thay vì dựng một backlog cũ. Cùng codec JPEG có giới hạn mang
snapshot màn hình giáo viên theo hướng ngược lại. Stroke annotation chuẩn hóa là
các command control đã xác thực và được vẽ bởi một cửa sổ topmost trong suốt
click-through trong phiên tương tác của học sinh. Cửa sổ lock được giữ rõ ràng
phía trên cửa sổ broadcast và annotation.

Server chấp nhận metadata snapshot không lớn hơn 480x270 và một payload JPEG không
lớn hơn 60 KiB. WIC xác minh rằng payload thực sự là JPEG và rằng kích thước đã
decode của nó khớp metadata đã xác thực trước khi pixel được cấp phát. Byte JPEG đã
publish là lưu trữ chia sẻ bất biến, nên sao chép registry client cho render loop
không sao chép payload ảnh. UI upload một generation lên một texture
shader-resource D3D11 nhiều nhất một lần; một decode hay texture upload thất bại
được cache âm cho đến khi một generation mới hơn tới. Cache bị vô hiệu rõ ràng khi
graphics device mất hay được tạo lại, nên các shader-resource view cũ không thể
chặn một lần khởi tạo device tiếp theo. Điều này giữ các frame malformed hay không
được hỗ trợ khỏi gây một vòng retry theo từng frame trong khi vẫn giữ một ranh giới
cache sạch cho một đường device-recovery tương lai.

## Đường video liên tục tùy chọn

```text
DXGI Desktop Duplication (texture BGRA)
  -> D3D11 Video Processor (texture NV12)
  -> Media Foundation hardware H.264 MFT
  -> packetization của ứng dụng
  -> UDP multicast hoặc fallback UDP unicast
```

Frame thô vẫn nằm trong bộ nhớ GPU. Access unit H.264 đã nén trở nên CPU-visible để
packetization Winsock; do đó kiến trúc tránh sao chép frame thô nhưng không thực sự
copy-free suốt qua NIC.

## Đường control

Frame TCP có tiền tố độ dài và chứa header little-endian được serialize rõ ràng.
Không bố cục bộ nhớ struct C++ nào được đặt trực tiếp lên wire. Parser thực thi
giới hạn payload và byte buffered trước khi cấp phát.

Xác thực control dùng một handshake HMAC hai chiều dựa trên nonce, session key dẫn
xuất, sequence command nghiêm ngặt theo từng hướng, và replay protection. Xem
`docs/SECURITY.md`.

Mỗi kết nối TCP bắt đầu với một preamble role/version kích thước cố định mang gợi ý
danh tính client và key ID. Nó là một bộ lọc admission rẻ và không thay thế xác
thực mật mã.

Server dùng một dispatcher IOCP có giới hạn với `AcceptEx` đã post, một `WSARecv`
đang chờ mỗi kết nối, `WSASend` queue được serialize, giới hạn admission theo
nguồn, và các audit callback có cấu trúc. Đường receive thô nạp một state machine
preamble/handshake bất đồng bộ trước khi các frame đã xác thực có thể cập nhật
registry hay thực thi command dashboard.

## Phục hồi endpoint server đã xác thực

Server bind một discovery responder UDP đã xác thực vào cùng port số với TCP control
listener (`47001` mặc định). Khi endpoint đã cache không thể kết nối hay hoàn tất
xác thực hai chiều, client gửi một discovery request 104-byte cố định tới các địa
chỉ directed broadcast của các adapter IPv4 LAN active. Adapter loopback và tunnel
bị loại khỏi việc chọn target tự động; test và triển khai có kiểm soát có thể cung
cấp target rõ ràng.

```text
thử endpoint IPv4 đã cache
  -> khi thất bại, gửi discovery request UDP đã xác thực HMAC
  -> xác minh client ID, key ID, nonce, timestamp, port và HMAC của response
  -> kết nối tới địa chỉ nguồn của response
  -> hoàn tất handshake TCP hai chiều bình thường
  -> thay thế nguyên tử cache endpoint được bảo vệ DPAPI
  -> làm mới gợi ý registry không-secret mà login diagnostics dùng
```

Request và response dùng các domain label HMAC riêng. Responder resolve PSK đã
enroll của client yêu cầu, từ chối nonce cũ hoặc bị replay, và rate-limit request
không hợp lệ theo địa chỉ nguồn. Một response UDP chỉ cung cấp một endpoint ứng
viên: client không cache nó cho đến khi handshake TCP hai chiều hiện có thành công.
Một địa chỉ IPv4 hay MAC phần cứng do đó không bao giờ được dùng làm proof danh
tính server.

Discovery cố ý là link-local trong phạm vi triển khai. Router thường không forward
broadcast IPv4, nên các VLAN riêng yêu cầu một địa chỉ ổn định, DHCP/DNS có kiểm
soát, hoặc một authenticated relay tương lai. Sau một ngắt control-session, service
xóa trạng thái tạm exam, lock, stream, snapshot, teacher-broadcast, annotation và
remote-input, rồi retry sau 3-6 giây jitter mật mã để tránh một reconnect storm
phòng học.

## Packet loss

Video protocol v2 bao gồm một sequence packet đơn điệu độc lập với định danh frame
và fragment. Receiver dùng một cửa sổ reorder có giới hạn và chỉ xác nhận một loss
sau khi sequence thiếu rời khỏi cửa sổ đó. Xem `docs/PACKET_LOSS.md` để biết quy
tắc báo cáo, hysteresis, stream-reset và fallback.

## Giới hạn đã biết

- Dashboard và teacher broadcast dùng snapshot JPEG định kỳ, không phải video liên
  tục. Packetizer, reassembler, jitter buffer, NACK policy và pipeline recovery đã
  xác thực tồn tại, nhưng socket UDP đã mã hóa, rotation group-key, decode H.264 và
  texture continuous-preview ImGui vẫn chưa được nối end to end.
- Codec payload video group-key tồn tại, nhưng sinh và phân phối key theo membership
  vẫn chưa được nối với khởi động stream.
- HMAC video cung cấp toàn vẹn và xác thực nguồn, không phải bảo mật.
- Rate control dài hạn, mất device lặp lại, hành vi multicast/unicast, và mức dùng
  tài nguyên 50 client vẫn cần ma trận validate phần cứng trong
  `PRODUCTION_VALIDATION.md`.
- Host WebView2 native, answer bridge và answer journal được triển khai trong build
  WebView2 opt-in. Test Runtime/loader WebView2 trên máy đích, policy
  chain/revocation certificate publisher production, authorization package/session
  của instructor, workflow chấm điểm và proctoring đầy đủ vẫn là các production
  gate.
- Cửa sổ bài thi chỉ cung cấp safeguard kiosk user-mode. Các đường thoát
  secure-desktop, local-administrator và cấp kernel yêu cầu một policy do OS quản lý
  và nằm ngoài ranh giới của host agent này.
- Payload exam-start đã xác thực version-2 hiện tại mang package ID manifest bắt
  buộc cùng các root package, web và user-data UTF-8 tuyệt đối. Agent canonicalize
  và giới hạn chúng, từ chối parent reparse point, kiểm tra lại content digest
  trước khi map, và ràng buộc profile browser local. Staging triển khai giờ thực
  hiện giải nén ZIP có giới hạn, kiểm tra path/reparse/duplicate, ghim digest
  archive và content, xác thực detached CMS publisher-signature, và publication
  nguyên tử. Các release gate còn lại là validate trên mỗi Windows image được hỗ
  trợ, policy chain/revocation certificate production, và authorization
  instructor/deployment. Host native cố ý không giải nén archive hay lặp lại kiểm
  tra signature triển khai; nó kiểm tra lại content digest đã ghim và giới hạn điều
  hướng trong package root đã xác thực.
