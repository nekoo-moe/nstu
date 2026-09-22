# Nhật ký công việc hiện tại và lộ trình

[English](ROADMAP.md) | [Tiếng Việt](ROADMAP.vi.md)

## Đã hoàn thành trong MVP thử nghiệm

- [x] Build CMake modular cho common, video, client, server và tests.
- [x] Loại trừ memory, dump, trace, capture và build chỉ ở phạm vi local.
- [x] Protocol nhị phân có version và TCP parser tăng dần có giới hạn.
- [x] Nền tảng Winsock TCP/IOCP và UDP multicast.
- [x] State machine fallback từ multicast sang unicast.
- [x] Bộ ước lượng packet-loss nhận biết reorder và hysteresis sức khỏe server.
- [x] Handshake HMAC hai chiều, replay cache, primitive MAC cho control/video.
- [x] Handshake TCP blocking trực tiếp và control channel đã xác thực.
- [x] Preamble kết nối cố định để lọc sớm role/version/identity.
- [x] Lưu trữ secret phạm vi machine bằng DPAPI với ACL hạn chế và atomic replace.
- [x] Enroll key trong tiến trình, rotation đơn điệu, revocation và zeroization.
- [x] Rate limiter handshake chưa xác thực có giới hạn kèm chặn tạm thời.
- [x] Installer NSIS hợp nhất chọn vai trò với tự động đăng ký client service,
      recovery policy, kích hoạt reboot, khởi động sign-in cho server, kiểm tra
      xung đột và xử lý uninstall do package sở hữu.
- [x] Giao diện server hướng giáo viên với tường snapshot mới nhất phản hồi
      nhanh, interval 5-10 giây điều chỉnh được, tóm tắt sức khỏe, lọc, telemetry
      tập trung, chat và điều khiển; shell chat client Native Win32 nhẹ.
- [x] Bản địa hóa Anh/Việt tại runtime, phủ glyph font tiếng Việt, theme
      dashboard sáng/tối, và icon tray server native bền bỉ với xử lý ẩn, khôi
      phục, tái tạo taskbar và thoát.
- [x] Reassembly frame có giới hạn, deadline, xử lý duplicate/xung đột.
- [x] Capture Desktop Duplication và báo cáo mất thiết bị.
- [x] Chuyển đổi GPU BGRA sang NV12.
- [x] Cấu hình MFT H.264 phần cứng và xử lý sự kiện bất đồng bộ.
- [x] Tách service/session-agent, helper DACL, named pipe, tray, overlay.
- [x] Shell server Dear ImGui/D3D11 và registry trạng thái client thread-safe.
- [x] Registry server mặc định rỗng, không chèn client minh họa.
- [x] Wrapper create, associate socket, wait và post cho IOCP completion-port.
- [x] Test unit/integration và Windows CI.
- [x] README song ngữ hướng triển khai với hướng dẫn cài đặt, mạng, license,
      Deep Freeze và trạng thái MVP trung thực.

## Đánh giá dựa trên máy tính

- [x] Giao diện bài thi đóng gói trong package với trắc nghiệm, trả lời ngắn,
      tự luận, nghe, đọc, tham chiếu PDF/audio, timer, autosave, cài đặt hiển thị
      song ngữ và export response JSON local.
- [x] Answer event đã xác thực với số thứ tự theo phiên, kiểm tra hash-chain,
      state response có giới hạn và export chẩn đoán đóng khung.
- [x] Outbox retry bền vững phía client với bảo vệ DPAPI phạm vi machine,
      migration, thực thi quota, kiểm tra ACK và journal server bền vững an toàn
      với crash-tail.
- [x] Cài đặt host WebView2 native tùy chọn với kiểm tra digest/path đã ghim,
      điều hướng local nghiêm ngặt, safeguard kiosk user-mode, xử lý lỗi tiến
      trình và bridge agent/service đã được test.
- [x] Gắn exam start đã xác thực với package ID trong manifest (EXS2), mang danh
      tính đó vào client host, và từ chối package-ID mismatch trong browser
      recovery, answer event và state request.
- [x] Thêm staging package do deployment sở hữu với giải nén ZIP có giới hạn,
      từ chối traversal/reparse/duplicate/path-collision, ghim digest archive và
      content, publish content-addressed, và cổng detached CMS/PKCS#7 theo
      thumbprint publisher. Helper `packaging/stage-exam-package.ps1` được phủ
      bởi một test đối kháng disposable và được đóng trong payload tài liệu
      triển khai. Host native vẫn kiểm tra lại digest package đã ghim ngay trước
      khi map, từ chối parent reparse point, giới hạn điều hướng trong web root
      đã xác thực, và yêu cầu loader đóng gói cùng một runtime probe.
- [ ] Validate helper staging, policy chain/revocation publisher,
      `WebView2Loader.dll` và WebView2 Runtime trên các Windows image được hỗ trợ
      với certificate production và fixture đã ký.
- [ ] Thêm authorization của instructor/deployment cho binding package, session
       và candidate; xác thực và audit mọi kỳ đánh giá chính thức.
- [ ] Triển khai control MAC ràng buộc theo hướng với capability đã xác thực hoặc
       thương lượng protocol-version; giữ tương thích v1 cho đến khi toàn fleet
       được nâng cấp.
- [ ] Thêm chấm điểm phía server, review, export niêm phong và diễn tập recovery
      bao gồm reset client, replay offline và retention được trường phê duyệt.
- [ ] Chạy các production gate riêng cho bài thi trong `PRODUCTION_VALIDATION.md`
      trước khi bật đánh giá chính thức.

## Kỹ thuật production đã hoàn thành

- [x] Keyring DPAPI bền vững, có version với key active và tombstone ID đã thu
      hồi, cùng enrollment bootstrap đã xác thực kháng replay.
- [x] Dispatcher IOCP `AcceptEx`/`WSARecv`/`WSASend` trực tiếp với dung lượng kết
      nối có giới hạn, rate limiting theo nguồn, audit event và test integration
      64 client.
- [x] Control plane server đã xác thực và service client tự kết nối lại với các
      lệnh status, heartbeat, lock/unlock, chat, stream, stop và keyframe.
- [x] Khôi phục endpoint server cùng-VLAN đã xác thực bằng PSK enrollment theo
      từng client, UDP discovery có giới hạn trên control port, revalidate TCP
      hai chiều trước khi cập nhật cache DPAPI nguyên tử, dọn dẹp fail-safe khi
      disconnect và reconnect phòng học có jitter. Địa chỉ IP và MAC phần cứng
      không phải là mỏ neo danh tính.
- [x] Định tuyến named-pipe service-agent trực tiếp và báo cáo status.
- [x] Các hành động dashboard server kết nối với phiên client đã xác thực.
- [x] Snapshot JPEG đã xác thực có giới hạn từ client tới dashboard, với thay thế
      queue theo frame mới nhất, payload bất biến chia sẻ, decode WIC chỉ-JPEG có
      giới hạn, cache generation âm và texture preview D3D11.
- [x] Stroke annotation chuẩn hóa đã xác thực được vẽ bởi overlay client trong
      suốt click-through, cùng điều khiển clear-overlay.
- [x] Broadcast snapshot màn hình giáo viên có giới hạn tới client đã xác thực,
      với thứ tự cửa sổ lock/broadcast/annotation xác định.
- [x] Validate cài đặt installer cho elevation, Windows được hỗ trợ, filesystem
      ACL data-root, trạng thái firewall, port và hướng dẫn autologon user
      chuẩn tùy chọn mà không lưu credential.
- [x] Packetizer video đã xác thực, jitter buffer, NackPolicy có giới hạn và
      keyframe scheduler với test reorder/loss xác định.
- [x] Điều phối recovery duplication/converter/encoder với retry hàm mũ có giới
      hạn và điều khiển keyframe phần cứng.
- [x] Hook Authenticode, workflow production có cổng certificate, data root cấu
      hình được bảo vệ cho không gian thaw của Deep Freeze, và script validate
      triển khai.
- [x] Công cụ benchmark và multicast-matrix tái lập được cùng bản ghi bằng chứng
      production.
- [x] Thêm thu thập chẩn đoán server opt-in với giới hạn 64 event trong bộ nhớ,
      lọc riêng tư local, điều khiển error-prompt độc lập, UI review/copy đầy đủ,
      và submit GitHub Issue công khai chỉ thủ công. Không payload event nào được
      lưu hoặc upload tự động.

## Công việc video liên tục tùy chọn

Quyết định (2026-09-09): giữ công việc này hoãn lại. Đường giám sát production là
snapshot JPEG định kỳ vì stream độc lập hơn 50 feed client tạo áp lực switch,
CPU, decoder và failure-domain tránh được. Không quảng bá, bật, hay biến H.264
thành điều kiện tiên quyết cho việc dùng trong phòng học.

- [ ] Kết nối gửi/nhận UDP đã mã hóa, rotation group-key đã xác thực, decode
      H.264 và texture continuous-preview D3D11. Đường giám sát production là
      snapshot-first; công việc này chỉ cần trước khi quảng bá hoặc bật chế độ
      H.264 liên tục.

## Công việc reboot-to-restore được quản lý

- [x] Tài liệu kiến trúc UWF-first, cổng edition Windows, threat model, ranh giới
      persistence, vòng đời servicing/recovery và kế hoạch test theo giai đoạn.
- [x] Triển khai probe năng lực UWF chỉ-đọc theo SKU/optional-feature/provider/
      current-next, bao gồm trạng thái `probe unavailable` rõ ràng khi một image
      được hỗ trợ không thể phân loại mà không mutation.
- [x] Mở rộng probe năng lực chỉ-đọc sang volume được bảo vệ, đếm exclusion
      không tiết lộ path, cấu hình/tiêu thụ overlay và sức khỏe event UWF gần đây,
      với query có giới hạn và cảnh báo partial hoặc truncated-result rõ ràng.
- [x] Giữ phần mở rộng này local, chỉ-client và chỉ-đọc: nó không triển khai hay
      cho phép mutation UWF, và vai trò server bỏ qua UWF nên dữ liệu của nó vẫn
      bền vững.
- [x] Thêm contract độc lập cho credential deployment-administrator riêng và các
      maintenance intent kiểu chặt, đã xác thực HMAC, kháng replay. Codec chuẩn
      ràng buộc ID intent/nonce, client mục tiêu, trạng thái restore mong đợi
      chính xác, revision policy, cửa sổ hiệu lực tối đa 15 phút và tham số theo
      thao tác có giới hạn. Nó chỉ trả về một capability mờ sau khi authorization
      thành công và cố ý không nối với control channel giáo viên, WMI UWF, reboot,
      UI hay thực thi helper; trạng thái replay/transaction bền vững vẫn thuộc
      milestone tiếp theo.
- [ ] Triển khai controller WMI UWF kiểu chặt sau một feature flag chỉ-lab, với
      xác nhận local cho lần kích hoạt đầu, recovery và decommission.
- [ ] Thêm maintenance transaction bền vững có giới hạn, giám sát overlay, tích
      hợp update/servicing, xác minh post-boot, quarantine và runbook recovery
      offline mà không có vòng lặp reboot tự động.
- [ ] Test event warning/critical UWF tách biệt với hành vi tự khởi động lại của
      OS khi overlay tối đa, bao gồm mất điện, cạn kiệt overlay và một đảm bảo
      rằng mất mạng hoặc mất điện không thể để servicing âm thầm không được bảo vệ.
- [ ] Hoàn tất test mutation VM bền vững, ma trận image LTSC chính xác, validate
      phần cứng i5-6400/8 GB, ít nhất 50 chu kỳ reset, review bảo mật độc lập và
      một pilot trường không-production trước khi phát hành opt-in.

Windows Pro và Home không hỗ trợ Microsoft UWF. Trên các edition đó công việc này
phải giữ audit-only; fallback được hỗ trợ là nâng cấp edition, một sản phẩm bên
thứ ba được quản lý riêng, hoặc reimaging quản lý tập trung. NSTU sẽ không triển
khai một mô phỏng UWF dạng script hay custom-kernel.

## Công việc update LTSC được quản lý

- [x] Tài liệu mô hình bảo trì LTSC/Deep Freeze chín tháng và cung cấp một harness
      thử nghiệm hash, staging, health-check và rollback không phá hủy.
- [ ] Triển khai release manifest đã ký và kiểm tra update outbound đã xác thực
      trong `nstu-service`, mặc định tắt sau policy administrator.
- [ ] Thêm staging được bảo vệ có thể tiếp tục, validate role/version/OS, kiểm tra
      anti-downgrade và kích hoạt package ràng buộc reboot.
- [ ] Thêm status fleet server, điều khiển pilot/wave, báo cáo sức khỏe post-reboot,
      rollback tự động và retention audit không có dữ liệu màn hình hay secret.
- [ ] Validate toàn bộ chuỗi thaw, update, reboot, health-check, rollback và
      refreeze với từng edition Windows LTSC và Deep Freeze được hỗ trợ trước khi
      bật triển khai cho trường.

## Các cổng phát hành production còn lại

- [ ] Chạy workflow production với certificate ký code thật và xác minh
      signature/timestamp trên installer hợp nhất và các binary đã cài.
- [ ] Validate hành vi cài/gỡ/data-root Deep Freeze bảo thủ với mọi edition và
      version dự án tuyên bố hỗ trợ.
- [ ] Thực thi và đính kèm bằng chứng cho soak snapshot 50 client, dung lượng
      snapshot-network/uplink TCP đã xác thực, đổi địa chỉ server cùng-VLAN và
      reconnect storm, ma trận build Windows, ma trận driver Intel và benchmark
      CPU/RAM/network.
- [ ] Nếu H.264 liên tục được bật trong một release tương lai, thực thi và đính
      kèm bằng chứng ma trận switch multicast riêng và fallback unicast bắt buộc
      trước khi quảng bá chế độ đó.
- [ ] Hoàn tất review protocol độc lập và fuzzing. Nơi threat model LAN yêu cầu
      bảo mật màn hình, thêm authenticated encryption trước khi triển khai; định
      dạng video hiện tại xác thực nhưng không mã hóa.
- [ ] Validate sanitizer chẩn đoán tùy chọn và UI consent/deletion với fixture
      test được trường phê duyệt trước khi cân nhắc bất kỳ báo cáo client/fleet
      nào. Client Session 0 không được publish chẩn đoán hay suy diễn consent.

## Ghi chú ngữ cảnh

Repository giờ chứa các phần triển khai control, enrollment, persistence,
snapshot có giới hạn, annotation, teacher-broadcast, packetization và recovery mà
trước đây chỉ là stub trong lộ trình. Một build xanh vẫn không chứng minh độ tin
cậy production, sự tin cậy certificate, hay tính tương thích với phần cứng trường
học không đồng nhất. Các cổng bằng chứng ở trên vẫn là điều kiện chặn phát hành.
