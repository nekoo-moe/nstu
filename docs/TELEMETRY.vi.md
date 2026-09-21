# Chẩn đoán tùy chọn và báo cáo công khai

[English](TELEMETRY.md) | [Tiếng Việt](TELEMETRY.vi.md)

NSTU Server có chế độ thu thập chẩn đoán tùy chọn để hỗ trợ xử lý lỗi đồ họa,
giải mã snapshot và các lỗi server có giới hạn. Tính năng mặc định tắt và không
phải là một dịch vụ telemetry từ xa.

## Đồng ý và luồng dữ liệu

1. Giáo viên hoặc kỹ thuật viên chủ động bật **Thu thập chẩn đoán đã lọc riêng
   tư trên máy** trong `Cài đặt`.
2. NSTU chỉ giữ tối đa 64 sự kiện đã lọc gần nhất trong bộ nhớ tiến trình.
3. Nếu bật **Nhắc khi báo cáo lỗi đã sẵn sàng**, lỗi đầu tiên trong một nhóm dữ
   liệu sẽ mở popup xem lại. Xóa nhóm dữ liệu hoặc bật lại chế độ nhắc sẽ kích
   hoạt lại popup; nhờ vậy lỗi đồ họa lặp lại không mở popup ở mọi frame. Nếu
   không, người vận hành có thể mở báo cáo từ `Cài đặt` hoặc `Chẩn đoán`.
4. Người vận hành đọc toàn bộ báo cáo Markdown rồi mới sao chép.
5. **Sao chép và mở GitHub** chỉ sao chép nội dung và mở biểu mẫu GitHub Issue
   công khai. Người vận hành vẫn phải tự dán, kiểm tra và bấm gửi.

NSTU không nhúng thông tin đăng nhập GitHub, không tạo Issue ở chế độ nền và
không truyền báo cáo tới máy chủ NSTU nào. Khi đóng ứng dụng, các sự kiện trong
RAM bị xóa. Tắt tính năng cũng xóa chúng ngay lập tức.

Nhật ký đồ họa có giới hạn vốn dùng cho cửa sổ `Chẩn đoán` cục bộ và thông báo
lỗi khởi động vẫn hoạt động khi tắt phần thu thập dành cho báo cáo công khai.
Nhật ký này chỉ nằm trong tiến trình, không được đưa vào báo cáo chia sẻ khi
tính năng tắt và bị xóa khi NSTU thoát.

## Trường dữ liệu được đưa vào

Báo cáo chỉ dùng danh sách trường do ứng dụng kiểm soát:

- phiên bản NSTU và kênh build;
- chế độ thiết bị D3D11, adapter/hãng GPU và feature level;
- khả năng Desktop Duplication và số bộ mã hóa H.264 phần cứng đã đăng ký;
- chu kỳ snapshot đang cấu hình;
- số lượng client theo nhóm (`0`, `1-10`, `11-25`, `26-50` hoặc `51+`);
- tối đa 64 sự kiện chẩn đoán nội bộ gần nhất sau khi lọc cục bộ.

Ngoài trường hợp bằng `0`, báo cáo không đưa số client chính xác nhằm hạn chế
việc nhận diện môi trường triển khai.

## Dữ liệu bị loại trừ

Không thêm các dữ liệu sau vào báo cáo công khai:

- địa chỉ IP/MAC, ID client, tên máy, tên người dùng hoặc tên trường;
- đường dẫn local/UNC, mật khẩu, enrollment secret, token hoặc private key;
- ảnh chụp/nội dung màn hình, tin nhắn chat hoặc input điều khiển từ xa;
- package, câu hỏi, câu trả lời, danh tính thí sinh, điểm hoặc journal khôi phục
  bài thi;
- dump hoặc log ứng dụng không giới hạn.

Bộ lọc loại bỏ các dạng IP/MAC, đường dẫn, email, secret, phép gán credential và
tham chiếu client dạng số thường gặp. Đây chỉ là lớp phòng vệ bổ sung, không phải
quyền thu thập nội dung tùy ý của người dùng. Nơi phát sinh sự kiện vẫn phải dùng
thông báo chẩn đoán nội bộ có kiểm soát và không lấy từ nội dung người dùng.

## Điều khiển và lưu trữ

Server chỉ lưu hai lựa chọn đồng ý trong profile của người vận hành tương tác:

```text
HKCU\Software\NSTU\Server\TelemetryEnabled
HKCU\Software\NSTU\Server\TelemetryPromptOnError
```

Cả hai là giá trị `REG_DWORD`; nếu chưa tồn tại thì được hiểu là tắt. Không có
payload sự kiện nào được ghi vào registry hoặc ổ đĩa.

## Phạm vi hiện tại

Implementation hiện chỉ áp dụng cho server. Client service trong Session 0
không được tự hiển thị consent thay cho học sinh hoặc âm thầm công bố chẩn đoán
của máy. Thiết kế fleet reporting trong tương lai phải có policy của quản trị
viên, transport được xác thực, thời hạn lưu giữ rõ ràng, aggregation trên server
có vùng lưu trữ bền vững và một vòng review riêng về quyền riêng tư/bảo mật trước
khi được bật.

## Báo cáo an toàn

GitHub Issues là công khai. Trước khi gửi:

1. đọc từng dòng trong báo cáo;
2. xóa mọi dữ liệu có thể nhận diện người, thiết bị, trường học hoặc mạng;
3. mô tả bước tái hiện mà không ghi tên hay địa chỉ;
4. không đính kèm ảnh chụp, dump, tài liệu thi hoặc log không giới hạn;
5. chỉ gửi khi trường hoặc tổ chức cho phép công bố công khai.

Không dùng biểu mẫu chẩn đoán công khai cho lỗ hổng bảo mật hoặc báo cáo chứa dữ
liệu nhạy cảm. Hãy dùng kênh liên hệ riêng với maintainer hoặc GitHub private
vulnerability reporting khi có; xem [SECURITY.md](SECURITY.md).

## Log audit hoạt động (khác với telemetry)

NSTU cũng giữ một **log audit hoạt động** vận hành, không giống chẩn đoán công
khai tùy chọn ở trên và cũng không giống answer journal của bài thi. Log audit
ghi lại rằng hoạt động NSTU đã xảy ra - enrollment, thay đổi chế độ Managed, thao
tác UWF theo fleet, cấp phép thi, sự kiện phiên và bảo mật - kèm category,
severity, component, action, result và detail đã được làm sạch và giới hạn. Nó
không bao giờ ghi câu hỏi thi, nội dung câu trả lời, ảnh chụp màn hình, nội dung
chat hay remote-input, thông tin đăng nhập, bí mật, khóa, mã SAS, token, đường
dẫn thô, hay định danh mạng thô: mọi trường đều đi qua cùng bộ làm sạch văn bản
công khai cộng với một danh sách chặn cứng trước khi được ghi hoặc gửi.

Hoạt động của client được đưa vào một hàng đợi có giới hạn, bỏ phần tử cũ nhất khi
đầy, và tải lên server theo từng khối nhỏ, có giới hạn tốc độ và được xác nhận
theo sequence. Server lưu trữ bản ghi tập trung vào một sink phân tách theo dòng,
có xoay vòng file. Đây là việc ghi nhận vận hành luôn bật, tách biệt với chẩn
đoán cần đồng ý ở trên.
