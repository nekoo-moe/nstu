# Giao thức bảo mật

[English](SECURITY.md) | [Tiếng Việt](SECURITY.vi.md)

## Phạm vi

Lớp bảo mật hiện tại cung cấp phiên control TCP đã xác thực trực tiếp, primitive
packet video UDP đã xác thực, enrollment bootstrap kháng replay, một keyring
phạm vi machine bền vững, và cấu hình runtime client được bảo vệ. Snapshot JPEG
định kỳ, snapshot màn hình giáo viên và stroke overlay đều đi trong các command
frame TCP đã xác thực. NSTU hiện không mã hóa nội dung màn hình.

Lỗ hổng bảo mật và các báo cáo chứa secret, dữ liệu màn hình/bài thi, hoặc thông
tin định danh không được gửi qua GitHub Issue công khai. Hãy dùng một kênh
maintainer riêng tư hoặc GitHub private vulnerability reporting khi repository
bật tính năng đó, và giữ lại chi tiết nhạy cảm cho đến khi một kênh riêng được
thiết lập.

## Control handshake

Mỗi client đã cài có một client ID 128-bit và tham chiếu một key đã provision
bằng `key_id`. Bản thân pre-shared key phải đến từ lưu trữ local machine được bảo
vệ và không bao giờ được commit vào repository. Protocol key ngắn hơn 256 bit bị
từ chối.

```text
Client -> Server: AuthHello(client_id, client_nonce, unix_time, key_id)
Server -> Client: AuthChallenge(server_nonce, unix_time)
Client -> Server: HMAC(PSK, labelled transcript)
Server -> Client: HMAC(session_key, labelled transcript)
```

Cả hai nonce là giá trị 256-bit sinh bằng Windows CNG. Proof của client và server
dùng các domain label khác nhau. Session key cũng được dẫn xuất dưới một label
riêng, ngăn cùng một input HMAC bị tái dùng giữa các vai trò.

Server phải xác minh HMAC proof của client trước khi chèn hello vào
`ReplayProtector`. Việc chèn replay là nguyên tử và có giới hạn. Dung lượng phải
được định cỡ cho tốc độ handshake tối đa được chấp nhận trong khoảng clock-skew,
và TCP listener phải rate-limit riêng các kết nối chưa xác thực.

Các phần triển khai blocking `client_handshake` và `server_handshake` giờ dùng
đường framing `TcpSocket` thật, bao gồm timeout receive/send. Một handshake thành
công trả về một `AuthenticatedSession` move-only; chuyển nó vào
`AuthenticatedControlChannel` sẽ chuyển session key và xóa nguồn. Các kiểu message
handshake bị từ chối sau khi channel đã thiết lập.

Trước command frame có tiền tố độ dài đầu tiên, cả hai peer trao đổi một preamble
kết nối cố định 32 byte. Nó chứa protocol magic/version, role, `client_id` và
`key_id`. Server có thể từ chối các kết nối malformed, sai role, hoặc lệch danh
tính trước khi cấp phát command payload hay chạy HMAC proof. Danh tính preamble
chỉ là gợi ý admission: nó được đối chiếu với `AuthHello` và không bao giờ được
tin nếu chưa có HMAC proof tiếp theo. Việc parse preamble có kích thước cố định và
không cấp phát; triển khai nên áp một read timeout ngắn và rate limit theo nguồn
ở ranh giới này.

`security::HandshakeRateLimiter` cung cấp primitive admission có giới hạn đó. IOCP
listener trực tiếp gọi nó trước khi cho một nguồn vào, ghi lại handshake thất bại
và framing không hợp lệ, xóa trạng thái sau khi thành công, và phát ra các audit
event có cấu trúc. Nó theo dõi một nguồn theo địa chỉ ổn định hoặc danh tính
enrollment, giới hạn số lần thất bại trong một cửa sổ cố định, áp một chặn tạm
thời sau ngưỡng, evict entry cũ nhất khi bảng nguồn đầy, và xóa trạng thái sau
một handshake thành công. Nó phải được gọi trước
`client_handshake`/`server_handshake`; nó không thay thế replay protection hay
xác minh HMAC.

Với key material local, `protect_machine_secret` và `unprotect_machine_secret`
dùng Windows DPAPI với `CRYPTPROTECT_LOCAL_MACHINE`. `save_machine_secret` ghi một
file tạm hạn chế ACL, flush nó, và thay thế nguyên tử đích bằng `MoveFileExW`. ACL
của file chỉ cấp quyền truy cập đầy đủ cho LocalSystem, built-in Administrators và
chủ sở hữu file. PSK và DPAPI entropy vẫn là input triển khai và không bao giờ
được sinh vào repository.

`security::KeyStore` cung cấp ranh giới vòng đời trong tiến trình mà một server
key resolver dùng: enrollment từ chối ID yếu hoặc bị tái dùng, rotation cấp một ID
đơn điệu mới trước khi thu hồi các key active cũ hơn, và revocation zeroize key
material trong khi vẫn giữ một tombstone ID. `resolve()` chỉ trả về bản sao key
active. `save_keyring` và `load_keyring` serialize các entry active và tombstone
ID đã thu hồi thành một định dạng nhị phân có version, được bảo vệ bằng DPAPI phạm
vi machine, ACL hạn chế, flush và atomic replacement.

Enrollment ban đầu thường được thực hiện bằng pairing trên màn hình, không cần
bootstrap secret chia sẻ trước. Client chưa enroll tự chạy trao đổi có xác thực
hai chiều và hiển thị một short authentication string sáu chữ số; người vận hành
server chỉ duyệt yêu cầu khi mã hiển thị trên client trùng với mã trong danh sách
chờ của server. Phép so sánh sáu chữ số ngoài băng đó là gốc tin cậy — một kênh mà
kẻ tấn công không thể giả mạo nếu không có mặt vật lý tại cả hai màn hình. Khi
được duyệt, server mint một key mới, dẫn xuất PSK đã cài từ transcript (PSK không
bao giờ được truyền đi), áp kiểm tra clock và replay, persist keyring trước khi
xác nhận, và rollback thay đổi trong bộ nhớ nếu persist thất bại. Một tên phòng
tùy chọn do người vận hành đặt được mang trên pairing beacon thuần túy như một gợi
ý định tuyến để client nhắm đúng phòng học; nó không bao giờ là credential và không
bao giờ làm yếu phép kiểm tra SAS.

Một fallback thủ công đã bị deprecate vẫn còn cho các máy không thể pair trên màn
hình. Nó dùng một bootstrap secret 256-bit dùng một lần: client gửi một nonce
mới, timestamp, danh tính, key ID yêu cầu và HMAC, và cả hai peer dẫn xuất PSK đã
cài từ bootstrap secret và transcript. Tool provision lưu cấu hình runtime client
kết quả dưới DPAPI phạm vi machine. Bản export bootstrap phải được phân phối ngoài
băng và xóa sau khi enroll. Đường này không còn được đóng gói trong installer.

## Discovery endpoint LAN đã xác thực

Một client đã enroll có thể phục hồi từ địa chỉ IPv4 server thay đổi mà không coi
dữ liệu DHCP là danh tính. UDP discovery dùng PSK 256-bit-trở-lên hiện có của
client và hai transcript HMAC-SHA256 phân tách domain:

```text
request  = version, kind, client_id, key_id, client_nonce, client_time, HMAC
response = version, kind, client_id, key_id, client_nonce, server_time,
           TCP control port, HMAC
```

Cả hai wire message chính xác 104 byte. Trường reserved phải bằng zero, kích thước
packet và version là chính xác, timestamp có cửa sổ chấp nhận 120 giây, và
response phải echo lại danh tính request, key ID và nonce 256-bit. Server chấp
nhận mỗi request nonce đã xác thực một lần. Nguồn không hợp lệ bị giới hạn tám lần
thất bại trong 30 giây và bị chặn 60 giây; bảng nguồn có giới hạn.

Địa chỉ nguồn của response chỉ là một ứng viên kết nối. Trước khi cập nhật cấu
hình DPAPI phạm vi machine, client thiết lập TCP và hoàn tất handshake hai chiều
bình thường, bao gồm proof rằng peer sở hữu cùng PSK enrollment. Xác thực TCP thất
bại để nguyên endpoint đã cache. Việc cập nhật cache dùng đường file-tạm-đã-flush
và atomic-replace hiện có. Sau khi thành công, service cũng làm mới các gợi ý
registry không-secret `ServerAddress` và `ServerPort` mà login diagnostics dùng;
các giá trị đó không bao giờ được chấp nhận làm credential.

Địa chỉ MAC phần cứng của server không phải là material xác thực: chúng có thể
thay đổi theo adapter, ảo hóa, thay NIC, teaming hoặc spoofing, và không có sẵn
qua router. Administrator vẫn có thể dùng một địa chỉ MAC cho DHCP reservation hay
kiểm kê. Sự tin cậy đến từ PSK enrollment và handshake hai chiều, không phải từ
một địa chỉ IP hay MAC.

Discovery tự động bị giới hạn trong broadcast domain IPv4 local. Không forward nó
ra Internet. Chỉ cho phép UDP inbound trên control port từ VLAN phòng học được
quản lý. Metadata discovery và traffic màn hình không được mã hóa; giới hạn bảo
mật trên LAN-tin-cậy hiện có vẫn áp dụng.

## Control frame đã xác thực

Sau handshake, mỗi command mang:

- một sequence 64-bit đơn điệu nghiêm ngặt;
- một tag HMAC-SHA256 cắt ngắn 128-bit;
- envelope và payload command bình thường.

MAC bao phủ một domain label, envelope command đã serialize, sequence và payload.
Vì TCP giữ thứ tự, `ControlSequenceGuard` yêu cầu đúng sequence kế tiếp. Một
reconnect tạo một session key mới và reset cả hai hướng về các sequence khởi tạo
được thương lượng độc lập.

Transport v1 được đóng gói dùng domain control-MAC trung tính legacy để tương
thích wire. Các helper `ControlDirection` V2 được phủ bởi unit test nhưng chưa
được channel trực tiếp bật; bật chúng yêu cầu một rollout capability/version đã
xác thực trên server và client. Không mô tả traffic v1 hiện tại là ràng buộc theo
hướng.

Xác minh MAC trước khi áp sequence guard. Chỉ advance guard sau khi xác minh thành
công. Không thực thi, log là tin cậy, hay xác nhận một command chưa xác thực.

Payload snapshot JPEG bị giới hạn ở 60 KiB trước transport, với metadata đã xác
thực giới hạn ở 480x270. Server chỉ chấp nhận container JPEG, xác minh kích thước
đã decode so với metadata đó, và cấp phát một pixel buffer có giới hạn. Service chỉ
giữ snapshot client đang chờ mới nhất, trong khi server publish lưu trữ JPEG chia
sẻ bất biến và thử decode/upload mỗi generation một lần. Xác thực ngăn sửa đổi
không bị phát hiện nhưng không giấu JPEG khỏi một người quan sát trên LAN. Cùng
giới hạn bảo mật đó áp dụng cho broadcast snapshot màn hình giáo viên.

## Xác thực video UDP

Bố cục datagram là:

```text
[44-byte video header][16-byte authentication tag][H.264 fragment]
```

Tag là một HMAC-SHA256 cắt ngắn trên một domain label riêng cho video, header đã
serialize và payload. Xác minh nó trước khi hạch toán packet-loss hay reassembly
frame; nếu không packet giả mạo có thể làm hỏng cả metric lẫn video.

Một session key dẫn xuất từ client không thể xác thực một stream multicast chia
sẻ. Do đó server cần một video group key ngẫu nhiên, phân phối riêng lẻ qua mỗi
phiên control đã xác thực. Rotate group key khi membership thay đổi hoặc theo
policy, và cấp một `stream_id` và baseline sequence mới cho stream đã rotate.

## Thứ tự reassembly

Thứ tự nhận bắt buộc là:

```text
validate datagram length
  -> decode fixed header
  -> select authenticated stream/key
  -> verify video HMAC
  -> packet-loss tracker
  -> frame reassembler
  -> decoder/jitter buffer
```

Cả `PacketLossTracker` lẫn `FrameReassembler` đều không tự chọn một stream từ một
datagram đầu tiên không tin cậy. Cả hai phải được reset từ metadata control-plane
đã xác thực trước khi bắt đầu nhận UDP.

## Phục hồi câu trả lời bài thi

Answer event bài thi dùng một sequence theo phiên riêng và một hash chain SHA-256.
`AnswerJournal` chỉ-ghi-thêm của server là nguồn chân lý: nó xác thực chain, flush
mỗi record trước khi xác nhận, và chỉ có thể phục hồi một tail chưa hoàn chỉnh sau
một crash. Client giữ một retry buffer `exam-answer-outbox.bin` có giới hạn được
bảo vệ bởi DPAPI phạm vi machine. Outbox không phải nguồn chân lý và một entry chỉ
bị xóa sau khi hash event tương ứng đã được server xác nhận bền vững.

Browser và native bridge xác thực package ID, package digest, danh tính client,
session, candidate, revision câu hỏi, sequence, kích thước và state hash trước khi
forward event. State response có giới hạn và được chia chunk; một hash thiếu hoặc
xung đột dẫn tới review thủ công thay vì ghi đè tự động. Outbox phải nằm trên ranh
giới persistence client được phê duyệt rõ ràng khi UWF hoặc freezing bên thứ ba
được dùng. Package bài thi, journal server, export và dữ liệu chấm điểm phải ở lại
trên lưu trữ server bền vững.

Staging triển khai được thực hiện bởi `packaging/stage-exam-package.ps1` (cài dưới
`docs/deployment`). Nó giữ archive mở với write/delete sharing bị từ chối, yêu cầu
ghim SHA-256 của archive và unpacked-tree, xác thực schema manifest được hỗ trợ và
mọi asset local được khai báo, từ chối traversal, duplicate không phân biệt hoa
thường, va chạm file/directory, link, reparse point, entry quá cỡ hoặc tỉ lệ cao,
và chỉ publish qua một rename `MoveFileExW` cùng-volume với `MOVEFILE_WRITE_THROUGH`
và không có `MOVEFILE_REPLACE_EXISTING`, giữ nguyên bất kỳ đích hiện có. Một
detached CMS/PKCS#7 `manifest.p7s` và một thumbprint certificate publisher đã ghim
được yêu cầu mặc định. Helper dùng một lock publication theo từng package và
revalidate một content directory và sidecar hiện có sau bất kỳ va chạm move; nó
không bao giờ mù quáng thay thế byte của một publisher đồng thời. Các switch
`-AllowUnsigned` và `-AllowNonElevatedTest` chỉ-phát-triển không bao giờ hợp lệ cho
một triển khai trường học. Sidecar metadata content nghiêm ngặt nằm ngoài cây
package, nên nó không thể âm thầm thay đổi digest mà `ExamHost` kiểm tra; nếu commit
của nó thất bại sau khi một directory mới được publish, helper chỉ gỡ directory
vừa được xác minh đó và dọn các file tạm của nó.

Control plane hiện tại giờ giữ một active-exam context phía server cho mỗi client
đã xác thực. Answer event và state request chỉ được chấp nhận khi client ID,
package ID, package digest, session ID và candidate ID của chúng trùng khớp chính
xác context do `start_exam` phát; context sống sót qua một reconnect thông thường
và bị gỡ bởi một `stop_exam` đã xác thực hoặc khi server tắt. Native client cũng
kiểm tra package ID do server phát so với manifest trước khi mở WebView2. Điều này
ngăn một client chuyển journal bằng cách đổi trường trong một browser message,
nhưng nó không chứng minh rằng giáo viên hoặc deployment administrator đã cho phép
lệnh start ban đầu. Cho đến khi một workflow authorization instructor/deployment đã
ký, có thể audit được triển khai, phục hồi câu trả lời không được dùng làm kiểm
soát toàn vẹn duy nhất cho một kỳ đánh giá chính thức.

Công việc reboot-to-restore giờ có một contract maintenance-intent độc lập. Nó
dùng một view credential deployment-administrator và một domain HMAC-SHA-256 riêng
thay vì domain MAC teacher/control-channel. Payload chuẩn có giới hạn của nó xác
thực client mục tiêu, intent ID duy nhất, nonce, trạng thái restore mong đợi chính
xác, revision policy, cửa sổ hiệu lực và một biến thể tham số cố định theo thao
tác. Authorization local từ chối các request cũ, tương lai, sai mục tiêu, lệch
trạng thái, bị replay và quá dung lượng. Chỉ đường thành công đó trả về một
capability mờ chứa một bản sao intent đã xác minh. Code này không được đăng ký làm
một network command và không thể thực thi WMI, tiến trình, reboot, hay thay đổi
UWF. Provision key deployment, trạng thái replay bền vững qua reboot, xác nhận của
kỹ thuật viên, IPC helper cô lập, audit và review độc lập vẫn bắt buộc trước khi
bất kỳ đường mutation nào được bật.

## Báo cáo chẩn đoán tùy chọn

Báo cáo chẩn đoán của NSTU Server là opt-in rõ ràng và mặc định tắt. Khi bật, nó
giữ tối đa 64 internal event đã lọc riêng tư trong bộ nhớ tiến trình. Payload
event không được ghi ra đĩa, gửi tới một endpoint NSTU, hay post lên GitHub tự
động. Registry chỉ lưu hai lựa chọn của người vận hành kiểm soát việc thu thập và
error prompt.

Bộ dựng báo cáo công khai chỉ chấp nhận các trường cấu hình do ứng dụng sở hữu và
sanitize các địa chỉ IP/MAC phổ biến, tham chiếu client, path, địa chỉ email, gán
credential và các token giống-secret. Nội dung màn hình, chat, remote input, nội
dung/câu trả lời bài thi, enrollment secret, private key, tên máy/người dùng/trường
và log không giới hạn nằm ngoài contract thu thập. Producer phải dùng các message
chẩn đoán được kiểm soát; sanitization là một ranh giới defense-in-depth, không
phải sự cho phép thu thập văn bản tùy ý.

Người vận hành phải review toàn bộ báo cáo trước khi tự dán nó vào form GitHub
Issue công khai. Không có GitHub token nào được nhúng trong NSTU. Xem
[TELEMETRY.md](TELEMETRY.md) để biết luồng dữ liệu đầy đủ và hành vi xóa.

## Các blocker còn lại

- Installer hợp nhất và helper diagnostics không áp policy Task Manager, Command
  Prompt, Control Panel, hay hiển thị ổ đĩa. Việc hardening tài khoản học sinh
  production do đó vẫn yêu cầu Group Policy quản lý tập trung hoặc một cơ chế
  triển khai nhận biết target-SID được review riêng.
- IPC service-agent local từ chối client từ xa và xác minh rằng pipe peer là
  `nstu-agent.exe` đã cài trong một phiên tương tác active. Nó vẫn thiếu một
  bootstrap và heartbeat đã xác thực theo từng lần launch. Thêm cả hai trước khi
  coi việc cố ý đua launch cùng-user hay treo agent thật là đã được giảm thiểu
  hoàn toàn.
- Sinh, rotation và phân phối video group-key theo membership được nối vào khởi
  động stream trực tiếp và nhận UDP.
- Bảo mật: HMAC xác thực nhưng không mã hóa nội dung màn hình. Mã hóa group
  AES-GCM hoặc một thiết kế tương đương được yêu cầu nơi người dùng LAN không được
  phép xem traffic đã capture.
- Authorization bài thi: active context ràng buộc client đã xác thực với package
  ID, package digest, session và candidate chính xác trong suốt vòng đời một bài
  thi đã bắt đầu. Nó vẫn không phải một bản ghi authorization instructor/deployment;
  thêm các binding đã ký, kháng replay và audit chúng trước khi dùng cho đánh giá
  production.
- Staging package bài thi: host revalidate content digest trước khi map, kiểm tra
  parent reparse point, và chỉ load loader WebView2 đóng gói. Stager triển khai
  offline giờ cung cấp giải nén ZIP có giới hạn và publication content-addressed.
  Provision certificate production, policy chain/revocation, fixture đã ký và host
  I/O ghim handle vẫn cần validate trước các kỳ đánh giá chính thức.
- Review protocol độc lập và fuzzing trước khi triển khai.
