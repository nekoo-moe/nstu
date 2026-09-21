# Đánh giá trên máy tính

Giao diện bài thi đầu tiên là web surface độc lập trong `exam/web/`, được thiết
kế để chạy từ gói `.nstuexam` cục bộ qua native WebView2 host trên client có
WebView2. Có thể dùng browser thường hoặc browser nhúng khác để xem UI tĩnh,
nhưng authenticated answer transport và kiosk behavior cần native host. Giao
diện không dùng CDN và không tải runtime từ Internet.

## Ranh giới gói

Gói `.nstuexam` là container ZIP gồm `manifest.json` UTF-8, trang cục bộ bắt
buộc `exam/web/index.html` cùng các asset giao diện, media cục bộ và PDF tùy
chọn. Package release còn có `manifest.p7s`:

```text
ielts-sample-01.nstuexam
|-- manifest.json
|-- manifest.p7s
|-- exam/
|   `-- web/
|       |-- index.html
|       |-- app.js
|       `-- styles.css
|-- documents/
|   `-- reading.pdf
`-- media/
    `-- listening-01.mp3
```

Manifest được kiểm tra theo `exam/schema/manifest.schema.json` trước khi publish
bất kỳ file nào. Validator staging chỉ chấp nhận property đã tài liệu hóa,
trường bắt buộc, giá trị string/integer có giới hạn, dạng câu hỏi được hỗ trợ
và ID document/question không trùng. Các dạng câu hỏi được hỗ trợ là
`multiple_choice`, `short_answer`, `essay`, `listening` và `reading`. Mọi
reference không rỗng trong `documents[].url` và `questions[].audio`/`pdf` phải
trỏ tới regular file trong cùng archive; reference tới thư mục,
`manifest.json`, `manifest.p7s`, traversal, query/fragment, separator đã mã hóa
hoặc URL mạng đều bị từ chối. Manifest phải là strict UTF-8 JSON không có BOM,
phù hợp với parser của native host.

## Staging do deployment quản lý

Native host chỉ nhận package đã giải nén. Administrator phải dùng helper
`docs/deployment/stage-exam-package.ps1` được đóng gói cùng installer.
Helper dùng `System.IO.Compression` của Windows và publish vào thư mục
content-addressed có ACL bảo vệ bên dưới data root bền vững của client:

~~~powershell
$stager = "$env:ProgramFiles\NSTU\docs\deployment\stage-exam-package.ps1"
& $stager -ArchivePath "D:\SecureTransfer\ielts-sample-01.nstuexam" -PublishRoot "$env:ProgramData\NSTU\exams\packages" -ExpectedArchiveSha256 "<release-archive-sha256>" -ExpectedContentSha256 "<unpacked-content-sha256>" -TrustedPublisherThumbprint "<approved-publisher-thumbprint>"
~~~

`PublishRoot` là đường dẫn tuyệt đối bắt buộc do administrator cung cấp. Policy
deployment phải giữ đường dẫn này bên dưới data root bền vững đã cấu hình cho
client; helper không tự dò registry, không tự chọn đường dẫn server và không tự
tải hoặc copy archive. Root phải là thư mục không phải drive root và không nằm
sau reparse point. Cờ `-RequireAuthenticode` là policy bổ sung tùy chọn, kiểm
tra Authenticode cho mọi file `.exe`/`.dll` trong package; cờ này tách biệt với
detached signature `manifest.p7s` bắt buộc.

Cả hai giá trị SHA-256 đều bắt buộc. Content digest dùng đúng thuật toán
sorted relative-path cộng bytes mà `ExamHost` kiểm tra ngay trước khi
map WebView2 virtual host. Helper từ chối path tuyệt đối, traversal, device
name, alternate data stream, duplicate, collision, link, reparse point, entry
mã hóa, archive bị cắt, archive quá lớn và mẫu zip-bomb. Helper giải nén vào
thư mục tạm riêng, kiểm tra toàn bộ cây, rồi dùng rename cùng volume qua
`MoveFileExW` với write-through và fail-if-existing. Lock theo từng package
tuần tự hóa publisher; race tới destination có sẵn chỉ được chấp nhận sau khi
kiểm tra lại toàn bộ cây và metadata. Thư mục content-addressed đã có không
bao giờ bị ghi đè. Sidecar strict ghi pin archive/content, số file, số byte,
trạng thái chữ ký và thời điểm publish ở ngoài package tree. Nếu publish
sidecar lỗi sau khi đã publish thư mục mới, helper chỉ xóa thư mục đó sau khi
kiểm tra lại digest; file tạm luôn được dọn.

Bản release phải có entry detached CMS/PKCS#7 `manifest.p7s` và
thumbprint certificate publisher được phê duyệt. `-AllowUnsigned` và
`-AllowNonElevatedTest` chỉ dành cho disposable test nội bộ, không
được dùng trong deployment trường học. Server vẫn là nơi lưu package và answer
journal bền vững.

## Tích hợp host

Native `ExamHost` đã được triển khai trong `client/src/exam_host.cpp` và chạy
trong `nstu-agent` trên UI/STA thread của agent. Với build bật
`NSTU_ENABLE_WEBVIEW2_HOST=ON`, host sẽ:

1. Nhận package root đã được giải nén và được deployment staging từ
   authenticated agent command. Việc giải nén ZIP và kiểm tra publisher
   signature do helper deployment thực hiện bên ngoài ranh giới host; host
   không tự giải nén archive nhưng vẫn tự kiểm tra content digest được ghim.
2. Kiểm tra manifest, đường dẫn media/document, quota package, reparse point và
   SHA-256 digest được ghim trước khi điều hướng.
3. Tạo cửa sổ Win32 topmost toàn màn hình, map package qua virtual host
   `https://nstu.exam/`, inject manifest và identity context do host cung cấp,
   rồi chỉ điều hướng tới entry page cục bộ đã kiểm tra.
4. Xử lý message phục hồi câu trả lời từ WebView2, chuyển event đã kiểm tra qua
   ranh giới agent/service tới authenticated control channel của server.
5. Lấy acknowledgement và state chunk đã xác thực trên UI thread của agent rồi
   phục hồi answer state cuối cùng server chấp nhận sau khi client kết nối lại.
   Browser chỉ dùng `localStorage` làm outbox phục hồi có giới hạn; khóa bền
   vững được tạo từ package digest, client ID, session ID và candidate ID do host
   cung cấp. Trước khi có đầy đủ trusted context, trang chỉ dùng namespace tạm
   theo page và không nạp hoặc gửi dữ liệu phục hồi cũ. Khi có trusted context,
   chỉ storage của đúng context đó được nạp và giữ lại. Khi đổi context, giao
   diện và chain cursor được khởi tạo lại; các record giữa các context không bao
   giờ được trộn. Cache của browser không bao giờ là bản ghi authoritative.

Host chặn điều hướng ra ngoài virtual host cục bộ, popup, developer tools,
context menu mặc định và zoom; đồng thời giữ cửa sổ ở foreground và chặn một số
phím tắt phổ biến trong user mode khi bài thi đang chạy. Đây là ranh giới kiosk
ở user mode, không phải cam kết chống secure desktop, local Administrator hay
phần mềm kernel-level. Chính sách clipboard/download, policy certificate-chain
và revocation của publisher cho production, authorization của giáo viên và quy
trình proctor đầy đủ vẫn là release gate.

Build có WebView2 cần WebView2 SDK lúc build, `WebView2Loader.dll` cạnh
`nstu-agent.exe` và WebView2 Runtime tương thích trên client. Agent chỉ nạp
loader được đóng gói cạnh `nstu-agent.exe` (không fallback sang đường dẫn DLL
hệ thống tùy ý), từ chối bản thiếu hoặc nằm sau reparse point, và probe version
của Runtime trước khi tạo environment. Vì vậy máy sạch phải có loader trong
client package và Evergreen WebView2 Runtime được cài hoặc đưa vào image của
trường. Build tắt `NSTU_ENABLE_WEBVIEW2_HOST=OFF` vẫn có protocol và
`ExamBridge` giới hạn để test offline/CI, nhưng sẽ fail closed khi có lệnh bắt
đầu bài thi và không cung cấp exam browser.

Thư mục user-data của WebView2 cũng được giới hạn theo từng context. Client tự
derive leaf filesystem-safe dạng `v2-<package-digest>-<client-id>-<session-id>`
bên dưới root `%LOCALAPPDATA%\\NSTU\\exam-webview` được phép của user tương tác.
Điều này ngăn service worker, cache, cookie hoặc artifact trình duyệt của package
hay client khác được dùng lại trên origin cố định `https://nstu.exam/`. Nếu request
trên wire có `user_data_root` khác rỗng, nó chỉ được chấp nhận khi canonicalize
đúng bằng leaf đã derive; server không thể chọn một profile persistent tùy ý.
Reconnect cùng một bộ tuple đã xác thực sẽ cố ý dùng lại leaf để giữ khả năng phục
hồi cục bộ trong browser.

Giao diện có PDF, audio listening, trắc nghiệm, trả lời ngắn, bài luận, đọc
hiểu, timer, autosave, chọn ngôn ngữ, cỡ chữ, tương phản, điều hướng bàn phím
và xuất JSON. Giao diện không tự chấm essay và không nhận JavaScript tùy ý từ
gói bài thi.

## Cầu nối phục hồi câu trả lời

Trước khi điều hướng, host inject context chỉ chứa danh tính. Manifest bài thi
không được tự chọn client hoặc session:

```js
window.NSTU_EXAM_CONTEXT = {
  packageId: "ielts-sample-01",
  packageDigestHex: "64 ký tự thập lục phân",
  clientIdHex: "32 ký tự thập lục phân",
  sessionIdHex: "32 ký tự thập lục phân",
  candidateId: "candidate-id",
  previousEventHashHex: "hash 64 ký tự, tùy chọn",
  nextSequence: 1
};
```

Trang gửi `exam_ready`, sau đó gửi từng `exam_answer_event` theo thứ tự. Câu
trả lời dạng chữ là chuỗi UTF-8; control có cấu trúc được biểu diễn bằng chuỗi
JSON chuẩn. Với mỗi event `upsert`, `clear` và `finalize`, trang tự serialize
định dạng canonical `NEV1` rồi tính SHA-256 thành `eventHashHex`. Hash không
được nhận như một trường tùy ý từ package: event đã lưu phải tính lại đúng
digest, còn native host và server vẫn kiểm tra cả hash-chain. Event vẫn nằm
trong hàng đợi giới hạn ở browser cho đến khi host trả `exam_answer_ack` có
`status` là `accepted` hoặc `duplicate` và đúng `eventHashHex`. Event bị từ
chối hoặc tạm thời không dùng được sẽ được giữ lại để thử lại.

Việc nộp bài fail-closed đối với độ bền cục bộ. Trước khi đánh dấu đã nộp,
trang phải bảo đảm mọi answer đang hiển thị có draft hoặc event pending bền
vững, ghi answer map và lưu marker finalization. Nếu serialize, quota hoặc
quyền truy cập browser storage thất bại, bài không được đánh dấu hoàn tất và
`exam_submit` không được gửi; giao diện báo storage phục hồi không khả dụng
hoặc đã đầy. Chỉ sau checkpoint bền vững đó, `exam_submit` mới được gửi cho
workflow gói kết quả, đồng thời event `finalize` bền vững được xếp sau các
event câu trả lời.

Trang sẽ gửi lại event không nhận ACK sau năm giây và tăng dần thời gian
chờ trong giới hạn. ACK `accepted` hoặc `duplicate` chỉ được chấp nhận khi
session, sequence, event hash khác zero và contiguous watermark hợp lệ. Khi
reconnect, state có `lastEventHashHex` sẽ đặt lại predecessor cho event đang chờ
và xóa hash cũ của các event tiếp theo; các event đã nằm trong watermark
server sẽ được loại bỏ trong session đó.

Host trả `{ "type": "exam_answer_ack", "ack": { ... } }`; các trạng thái là
`accepted`, `duplicate`, `gap`, `conflict`, `rejected` hoặc `unavailable`. Khi
kết nối lại, host gửi `exam_state_response` gồm câu trả lời đã chấp nhận,
`highestContiguousSequence` và (nếu có) `lastEventHashHex`. Với trusted context
khớp, state từ server là authoritative cho mọi câu không có draft hoặc event
pending cục bộ: trang dựng lại phần đó của answer map, xóa câu bị state bỏ qua
(kể cả kết quả `clear`/reset từ server), rồi chồng các answer đang chờ gửi ở
cục bộ lên trên. Vì vậy answer cũ trong browser không thể tồn tại sau phục hồi
authoritative, còn answer mới chưa gửi vẫn được giữ. Native host phải kiểm tra
danh tính, revision, kích thước và hash-chain trước khi chuyển message sang
protocol C++ đóng gói.

Khi cài client, các thư mục `exam/web`, `exam/schema` và `exam/examples` được
đặt tại `%ProgramFiles%\\NSTU\\client\\exam`. Các nhánh phản hồi của service
chỉ đưa `exam_answer_ack` và `exam_state_response` vào hàng đợi `ExamBridge`,
giới hạn 64 mục và phát tín hiệu `WM_APP` riêng cho cửa sổ agent. Trong build có
WebView2, `ExamHost` lấy hàng đợi trên UI thread của agent; host không đọc trực
tiếp service pipe và không xem việc mất tín hiệu UI là mất dữ liệu. Build không
có WebView2 vẫn giữ bridge giới hạn để test protocol/lifecycle nhưng không thể
mở exam surface.

Trạng thái phục hồi có thể được chia thành nhiều message
`exam_state_response` đã xác thực. Trang chỉ gom các chunk có cùng package,
session, candidate, sequence và state hash; sau đó phải nhận đủ từ index `0`
đến `chunkCount - 1` mỗi index một lần mới ghép câu trả lời. Bộ nhớ
ghép bị giới hạn 64 chunk, 512 câu và hết hạn sau 15 giây. Bộ tách native và
`ExamBridge` dùng cùng giới hạn 64 chunk; snapshot cần nhiều hơn sẽ bị từ
chối toàn bộ thay vì gửi dở dang. Phân trang sẽ dành cho phiên bản protocol
trong tương lai, vì vậy host phải giữ snapshot trong giới hạn này và gửi lại
trọn bộ khi thiếu chunk, không để browser tự suy diễn trạng thái.

Các entry bền vững trong browser được kiểm tra trước khi nạp. Entry không hợp
lệ chỉ được giữ dưới dạng tóm tắt chẩn đoán có giới hạn. Storage key luôn gắn
với đầy đủ trusted context, vì vậy candidate hiển thị trong manifest không thể
tự chọn lịch sử answer được mở. Sau khi host inject context, event hoặc draft
đang chờ có package digest, client ID, session ID hoặc candidate ID khác context
sẽ được chuyển vào quarantine giới hạn và không bao giờ được gửi đi. Khi đổi
context, chain cursor và state phục hồi đang hiển thị được khởi tạo lại cho
context mới; nếu quay lại context khớp, chỉ storage riêng của context đó được
dùng. Không có record của context trước được trộn vào session khác. Cách này
ngăn session cũ chặn hoặc làm nhiễm session mới; giao diện sẽ báo dữ liệu phục
hồi đang được giữ cho context khác. Event
khớp context vẫn có thể được đặt lại predecessor theo watermark
`lastEventHashHex` từ server khi reconnect.

Mọi state response, kể cả response chỉ có một chunk, phải có metadata nhất
quán về package, digest, client, session, candidate, chunk và finalized. Nếu
`highestContiguousSequence` lớn hơn 0 thì bắt buộc có `stateHashHex` và
`lastEventHashHex` khác zero. Với watermark bằng 0, hai trường này có thể bỏ
trống; nhưng nếu gửi last-event hash thì giá trị phải toàn zero. Response không
đạt kiểm tra sẽ bị bỏ qua và không được sửa câu trả lời cục bộ.

Nếu watermark reconnect bao phủ một event vẫn đang chờ ở browser, trang chỉ
loại event đó khi có thể đối chiếu hash của từng event. Nếu thiếu hash hoặc hash
khác nhau, phục hồi chuyển sang trạng thái cần kiểm tra thủ công và giữ nguyên
bản ghi đang chờ; trang không tự đoán hay ghi đè câu trả lời. Kỹ thuật viên cần
xuất response cục bộ và đối chiếu với journal trên server trước khi tiếp tục
phiên.

Định dạng response v3 là ranh giới triển khai cần đồng bộ client/server. Native
decoder vẫn đọc được response v1 và v2 để chuyển đổi từng bước hoặc kiểm tra
archive, nhưng các phiên bản đó không có `lastEventHashHex`; browser không thể
đặt lại hash-chain của event đang chờ một cách an toàn từ response cũ. Hãy chạy
server hỗ trợ v3 cùng client hỗ trợ v3 trước khi bật phục hồi câu trả lời và
không công bố hỗ trợ phục hồi mixed-version. Negotiation capability được hoãn
cho một phiên bản protocol sau.

Mỗi state-response payload đã xác thực bị giới hạn ở
`kMaximumExamPayloadBytes` (65.512 byte với giới hạn command 64 KiB hiện tại,
command sequence 8 byte và authentication tag 16 byte). Một response hoàn chỉnh
có tối đa 64 chunk và 512 answer entry. Browser giữ bộ chunk chưa đủ trong 15
giây và yêu cầu mỗi index xuất hiện đúng một lần; host phải gửi lại cả bộ khi
mất chunk.

### Outbox client bền vững (`OBX1` bên trong `EOB1`)

Write-ahead outbox của client tách biệt với server journal có tính authoritative.
Payload nhị phân logic của outbox là `OBX1` version 4. Lịch sử định dạng:

| Version | Phần được lưu thêm |
| --- | --- |
| 1 | Chỉ các event record theo định dạng cũ. |
| 2 | Watermark theo session và last event hash. |
| 3 | Ranh giới sequence/hash đã được ACK. |
| 4 | Marker finalization, ngăn event xuất hiện sau `finalize`. |

Ghi mới được bọc trong `EOB1` version 1 và bảo vệ bằng DPAPI phạm vi machine của
Windows (`CRYPTPROTECT_LOCAL_MACHINE`). Khi mở, client có thể kiểm tra plaintext
cũ và các version `OBX1` cũ, sau đó migrate nguyên tử sang dạng v4 đã mã hóa
trước khi cung cấp event cho runtime. Lỗi xác thực hoặc toàn vẹn là lỗi mở file;
client không được chuyển sang gửi một file chưa kiểm tra. Outbox giới hạn 512
event và 8 MiB plaintext logic, cộng allowance 64 KiB riêng cho wrapper DPAPI.
Đây chỉ là hàng đợi retry: event chỉ bị xóa sau ACK bền vững khớp, còn server
journal vẫn là nguồn dữ liệu chính thức.

### State export có framing (`SEX1`)

`AnswerJournal::export_state` ghi state export tại một thời điểm trong container
`SEX1` version 1 có framing. Header gồm magic `u32` little-endian (`SEX1`),
format version `u16` và số chunk `u16`. Sau đó là các record lặp lại theo dạng
độ dài `u32` little-endian + payload `NSR1` hoàn chỉnh. Mỗi record là một state
response chunk có thể decode độc lập; consumer phải tôn trọng boundary độ dài,
kiểm tra từng chunk và từ chối dữ liệu bị cắt, metadata trùng hoặc byte thừa.
Không được nối các payload lại rồi parse như một response duy nhất. Toàn bộ
export bị giới hạn 256 MiB.

Export là góc nhìn phục vụ chẩn đoán/sao lưu, không thay thế append-only journal
và bản thân container không được mã hóa. Trên Windows, atomic writer áp dụng
DACL giới hạn cho SYSTEM/Administrators/owner, nhưng người vận hành vẫn phải
lưu trữ và truyền export như dữ liệu câu trả lời nhạy cảm.

## Yêu cầu bảo mật và phục hồi

- Giữ package bài thi và bản ghi câu trả lời trên server persistent; không đặt
  chúng trong đường dẫn client do UWF hoặc phần mềm đóng băng bên thứ ba quản
  lý.
- Host hiện chặn external navigation, popup, developer tools, context menu mặc
  định và zoom trong profile WebView2. Cần bổ sung policy download và clipboard
  rõ ràng trước khi dùng production.
- Package phải được ký ở deployment/release pipeline và phải ghim digest dự kiến
  trước khi bắt đầu bài thi. Staging helper kiểm tra detached publisher
  signature; native host cố ý kiểm tra lại digest và boundary path thay vì lặp
  lại việc xác thực CMS/certificate.
- Coi answer log trên server là append-only, có package version, question
  revision, candidate identity và event time.
- Đóng bài thi bằng command server đã xác thực hoặc thao tác kỹ thuật viên tại
  chỗ. Reboot client không được xóa response đã lưu trên server.

### Ranh giới authorization

Control plane hiện xác thực client trong handshake và tạo active-exam context
ở server khi `start_exam` thành công. `ExamStartRequest` version 2 mang trường
`package_id` của manifest và bắt buộc phải có; mỗi answer event và state request
phải khớp chính xác client identity, package ID, package digest, session ID và
candidate ID trong context. Native client host cũng so sánh package ID do server
cấp với manifest trước khi mở WebView2. Context vẫn tồn tại qua reconnect bình
thường và bị xóa bởi `stop_exam` hoặc khi server shutdown. Cơ chế này ngăn
browser tự đổi tuple của journal, nhưng **chưa** chứng minh package, session và
candidate đã được giáo viên hoặc deployment administrator cấp quyền. Binding
authorization có chữ ký
và audit vẫn là security blocker trước production.

Manifest mẫu nằm tại `exam/examples/ielts-sample.json`. Giao diện này chỉ là
lớp tương tác; quy tắc chấm điểm, công cụ soạn đề của giáo viên, điều khiển
giám thị và bước hoàn tất ở phía server vẫn là các mốc riêng trong roadmap.

## Cổng bảo vệ khôi phục sau reboot

Chế độ Thi chỉ được server cấp phép khi client mục tiêu đã chứng minh được bảo vệ
UWF (khôi phục sau reboot) ở phiên hiện tại. Cổng nằm phía server và mặc định
đóng: client không bao giờ khẳng định cờ bảo vệ, và không có gì về bảo vệ nằm
trong yêu cầu bắt đầu thi. Xem [REBOOT_TO_RESTORE.vi.md](REBOOT_TO_RESTORE.vi.md)
để biết cách thiết lập bằng chứng gắn với boot và vì sao kết nối lại sẽ thu hồi
nó.

Bản dựng công khai `NSTU DEV (UNPROTECTED)` chỉ bỏ qua duy nhất cổng này để thử
nghiệm và không bỏ qua gì khác; mỗi lần bắt đầu bị bỏ qua đều được audit ở
`severity=warning` và bản dựng được gắn nhãn rõ ràng trong giao diện lẫn
installer. Bản Release không thể bắt đầu thi trên client chưa được bảo vệ.
