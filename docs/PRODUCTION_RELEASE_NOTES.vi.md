# Bản phát hành production của Project NSTU

[English](PRODUCTION_RELEASE_NOTES.md) | [Tiếng Việt](PRODUCTION_RELEASE_NOTES.vi.md)

Khi được tạo bởi workflow production có cổng certificate, bản phát hành này chứa
một installer Windows chọn vai trò đã ký để triển khai server hoặc client. Các
ghi chú này không áp dụng cho build nightly chưa ký. Bản phát hành chỉ được công
bố sau khi bản ghi production validation trong `docs/PRODUCTION_VALIDATION.md` đã
có bằng chứng cho mọi môi trường bắt buộc.

Xác minh signature Authenticode và file `SHA256SUMS.txt` đi kèm trước khi triển
khai. Việc cài đặt và gỡ bỏ client yêu cầu quyền administrator và một lần restart
Windows. Hệ thống Deep Freeze phải được boot ở Thawed để cài đặt, enrollment,
nâng cấp, validation và gỡ bỏ.
