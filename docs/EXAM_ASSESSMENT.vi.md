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
`exam/web/index.html`. Host xử lý message `{ type: "exam_submit" }` từ
`window.chrome.webview` và lưu câu trả lời qua control channel đã xác thực.

Server là nguồn dữ liệu chính. `localStorage` trong browser chỉ là cache phục
hồi crash, không phải bản ghi điểm chính thức.

Giao diện có PDF, audio listening, trắc nghiệm, trả lời ngắn, bài luận, đọc
hiểu, timer, autosave, chọn ngôn ngữ, cỡ chữ, tương phản, điều hướng bàn phím
và xuất JSON. Giao diện không tự chấm essay và không nhận JavaScript tùy ý từ
gói bài thi.

Gói bài thi và câu trả lời phải nằm trên server persistent, không đặt trong
đường dẫn client do UWF hoặc phần mềm đóng băng bên thứ ba quản lý. Cần chặn
navigation bên ngoài, download, clipboard injection và developer tools trong
profile WebView2. Manifest mẫu nằm tại `exam/examples/ielts-sample.json`.
