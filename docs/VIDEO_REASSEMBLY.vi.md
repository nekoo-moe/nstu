# Ghép lại frame video

[English](VIDEO_REASSEMBLY.md) | [Tiếng Việt](VIDEO_REASSEMBLY.vi.md)

`FrameReassembler` chỉ chấp nhận các packet có stream và xác thực đã được
control plane thiết lập từ trước.

Giới hạn mặc định:

- 8 frame chưa hoàn chỉnh;
- 4096 fragment mỗi frame;
- 1400 byte mỗi fragment;
- 4 MiB mỗi frame đã mã hóa;
- 8 MiB tổng payload được đệm;
- deadline 150 ms tính từ fragment đầu tiên.

Deadline không bao giờ được kéo dài bởi các fragment đến sau. Điều này ngăn một
bên gửi giữ bộ nhớ của bên nhận vô thời hạn bằng cách gửi từng fragment một.

Các fragment trùng lặp có nội dung giống hệt sẽ được đếm và bỏ qua. Một index
trùng lặp nhưng nội dung khác là xung đột và bị từ chối. Metadata như số lượng
fragment và capture timestamp phải giống nhau ở mọi fragment của một frame. Chỉ
fragment cuối cùng mới được mang `end_of_frame`, và fragment cuối cùng bắt buộc
phải mang nó.

Các frame chưa hoàn chỉnh đã hết hạn là sự kiện mất frame. Chúng khác với mất
packet: một packet bị thiếu có thể làm rớt một frame, trong khi nhiều packet bị
thiếu có thể cùng thuộc về một frame. `missing_fragments(frame_id)` cung cấp dữ
liệu cho `NackPolicy` có giới hạn, vốn chờ một khoảng reorder ban đầu, giới hạn
số fragment mỗi yêu cầu, giới hạn số lần retry, và không bao giờ yêu cầu dữ liệu
sau deadline của frame.

Implementation ghi nhớ các frame ID vừa hoàn thành gần đây để các bản trùng lặp
đến muộn không thể cấp phát một partial frame mới. Các giới hạn khi ghép lại là
hàng rào bảo vệ cấp phát bộ nhớ, không phải gợi ý tinh chỉnh; cấu hình production
phải luôn có giới hạn.
