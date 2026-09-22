# Hồ sơ xác nhận production

[English](PRODUCTION_VALIDATION.md) | [Tiếng Việt](PRODUCTION_VALIDATION.vi.md)

Test của repository và đóng gói là cần thiết nhưng không thay thế bản ghi phòng
lab này. Với mỗi lần chạy, hãy đính kèm CSV thô, cấu hình switch, phiên bản
driver, ảnh chụp màn hình và link issue. Không phê duyệt production tag khi có
hàng bắt buộc còn trống hoặc thất bại.

Chỉ đặt `Result` đúng bằng `Passed` sau khi đã đính kèm đường dẫn hoặc link bằng
chứng không rỗng. Production workflow từ chối các hàng bắt buộc bị thiếu, trùng
lặp, đang chờ hoặc để trống.

| Gate | Bằng chứng bắt buộc | Result | Đường dẫn/link bằng chứng |
|---|---|---|---|
| Authenticode | Chữ ký hợp lệ và trusted timestamp trên installer hợp nhất và các executable đã cài | Pending | |
| 50-client soak | Ít nhất 8 giờ, chu kỳ reconnect/lock/chat/snapshot/annotation/broadcast, không crash hoặc tăng bộ nhớ không giới hạn | Pending | |
| CPU/RAM/network | CSV từ `collect-benchmarks.ps1` ở chu kỳ snapshot 5, 7 và 10 giây cùng thông số phần cứng server/client | Pending | |
| Snapshot network | Lần chạy TCP control/snapshot có xác thực với 50 client, đo dung lượng uplink của switch, có reconnect, và không tăng hàng đợi kéo dài | Pending | |
| Server address recovery | Đổi địa chỉ IPv4 server đã enroll trong cùng VLAN; xác minh UDP discovery có xác thực, mutual TCP revalidation, cập nhật cache client theo kiểu atomic, dọn transient-control, và phục hồi có jitter cho 50 client | Pending | Đã bao gồm test loopback tự động và wrong-key/tamper/replay; vẫn cần bằng chứng VLAN vật lý |
| Windows matrix | Các build Windows 10/11 được hỗ trợ, kết quả setup-check, cài sạch, nâng cấp, reboot, gỡ cài đặt | Pending | |
| Intel driver matrix | Các model GPU và phiên bản driver được hỗ trợ, phục hồi capture/encode/device-loss | Pending | |
| Deep Freeze | Mọi edition/version được hỗ trợ, cài/enroll/nâng cấp/gỡ ở trạng thái Thawed và vận hành ở trạng thái Frozen | Pending | |
| Security review | Review protocol/code độc lập và kết quả fuzzing | Pending | |
| Confidentiality decision | Threat model LAN đã được tài liệu hóa; triển khai encryption trước khi dùng ở nơi cần bảo mật màn hình | Pending | |
| Exam journal durability | Phục hồi crash-tail, rollback khi append, replay khi restart/offline, và bằng chứng journal có thẩm quyền trên server | Pending | |
| Exam outbox protection | Migration DPAPI `EOB1`, quota/rollback, xác thực ACK hash, và bằng chứng dữ liệu tồn tại qua reboot client | Pending | |
| Exam state export | Export/import `SEX1` nhiều chunk với việc từ chối trùng lặp, cắt cụt, xung đột metadata, và trailing-byte | Pending | |
| Exam compatibility | Phục hồi client/server v3 có phối hợp cùng việc từ chối/bằng chứng rõ ràng cho rebasing lẫn phiên bản không được hỗ trợ | Pending | |
| Exam authorization | Ràng buộc package/session/candidate của giáo viên, chống replay, và bằng chứng audit | Blocked | Bắt buộc trước các kỳ đánh giá chính thức; runtime hiện đã thực thi context client/digest/session/candidate do server cấp, nhưng vẫn thiếu bản ghi authorization của giáo viên/triển khai |
| Optional H.264 network | Bằng chứng sender/receiver chỉ cho tương lai với mỗi cấu hình switch/VLAN/IGMP được hỗ trợ và forced fallback | Deferred | Không bắt buộc khi H.264 liên tục vẫn đang trì hoãn |

Chạy `packaging/test-production-deployment.ps1 -RequireSignedArtifacts` trên mỗi
máy trước và sau soak. Đo gate snapshot network bằng chính switch/uplink dùng cho
lần triển khai phòng học. Harness `tools/production/test-multicast.ps1` được dành
riêng cho thử nghiệm video liên tục tùy chọn và không chặn bản phát hành snapshot
khi tính năng đó đang trì hoãn. Chạy harness lifecycle Windows Sandbox có kiểm
soát để lấy bằng chứng service/gỡ cài đặt trước reboot, rồi hoàn tất chuỗi reboot
bền vững và Deep Freeze trong `docs/VM_TESTING.md`; chỉ riêng kết quả Sandbox
không thể pass các gate đó.

Các gate bài thi ưu tiên server: server phải giữ được package và journal có thẩm
quyền qua các lần reset UWF/Deep Freeze của client. Test replay outbox trên client
chỉ minh họa hành vi retry; nó không thể thay thế bản backup journal server, bản
ghi authorization, hay bản export chấm điểm.
