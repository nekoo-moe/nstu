# Đánh giá trên máy tính

Giao diện bài thi đầu tiên là web surface độc lập trong `exam/web/`, chạy từ
gói `.nstuexam` cục bộ bên trong WebView2 hoặc browser nhúng tương đương. Giao
diện không dùng CDN và không tải runtime từ Internet.

## Ranh giới gói

Gói `.nstuexam` là container ZIP gồm `manifest.json` UTF-8, media cục bộ và
PDF tùy chọn:

```text
ielts-sample-01.nstuexam
|-- manifest.json
|-- documents/
|   `-- reading.pdf
`-- media/
    `-- listening-01.mp3
```

Manifest được kiểm tra theo `exam/schema/manifest.schema.json`. Các dạng câu
hỏi được hỗ trợ là `multiple_choice`, `short_answer`, `essay`, `listening` và
`reading`.

## Tích hợp host

Host WebView2 cần giải nén gói đã ký vào thư mục tạm riêng của phiên, kiểm tra
manifest và digest, cấp manifest qua `window.NSTU_EXAM_MANIFEST`, rồi mở
`exam/web/index.html`. Host xử lý các message phục hồi câu trả lời bên dưới từ
`window.chrome.webview`, sau đó lưu qua control channel đã xác thực.

Server là nguồn dữ liệu chính. `localStorage` trong browser chỉ là cache phục
hồi crash, không phải bản ghi điểm chính thức.

Giao diện có PDF, audio listening, trắc nghiệm, trả lời ngắn, bài luận, đọc
hiểu, timer, autosave, chọn ngôn ngữ, cỡ chữ, tương phản, điều hướng bàn phím
và xuất JSON. Giao diện không tự chấm essay và không nhận JavaScript tùy ý từ
gói bài thi.

## Cầu nối phục hồi câu trả lời

Trước khi điều hướng, host phải inject context chỉ chứa danh tính. Manifest bài
thi không được tự chọn client hoặc session:

```js
window.NSTU_EXAM_CONTEXT = {
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
JSON chuẩn. Event vẫn nằm trong hàng đợi giới hạn ở browser cho đến khi host
trả `exam_answer_ack` có `status` là `accepted` hoặc `duplicate` và đúng
`eventHashHex`. Event bị từ chối hoặc tạm thời không dùng được sẽ được giữ lại
để thử lại. Khi nộp bài, `exam_submit` vẫn được gửi cho workflow gói kết quả,
đồng thời một event `finalize` bền vững được xếp sau các event câu trả lời.

Host trả `{ "type": "exam_answer_ack", "ack": { ... } }`; các trạng thái là
`accepted`, `duplicate`, `gap`, `conflict`, `rejected` hoặc `unavailable`. Khi
kết nối lại, host gửi `exam_state_response` gồm câu trả lời đã chấp nhận,
`highestContiguousSequence` và (nếu có) `lastEventHashHex`; trang chỉ hợp nhất
trạng thái đó, không ghi đè event mới đang chờ. Native host phải kiểm tra danh
tính, revision, kích thước và hash-chain trước khi chuyển message sang protocol
C++ đóng gói.

Gói bài thi và câu trả lời phải nằm trên server persistent, không đặt trong
đường dẫn client do UWF hoặc phần mềm đóng băng bên thứ ba quản lý. Cần chặn
navigation bên ngoài, download, clipboard injection và developer tools trong
profile WebView2. Manifest mẫu nằm tại `exam/examples/ielts-sample.json`.
