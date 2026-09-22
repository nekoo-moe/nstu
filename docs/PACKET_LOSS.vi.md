# Phát hiện mất packet

[English](PACKET_LOSS.md) | [Tiếng Việt](PACKET_LOSS.vi.md)

Mất packet được đo theo từng stream video đã xác thực bằng trường `packet_sequence`
đơn điệu do video protocol version 2 giới thiệu. `frame_id` và `fragment_index`
không được dùng thay thế: kích thước frame thay đổi, frame có thể bị encoder bỏ
qua, và fragment có thể đến không đúng thứ tự.

## Yêu cầu của sender

- Sinh một `stream_id` ngẫu nhiên mỗi khi pipeline sender khởi động lại.
- Bắt đầu `packet_sequence` ở giá trị bất kỳ và tăng nó đúng một lần cho mỗi
  datagram UDP phát ra, kể cả retransmission gửi dưới dạng datagram mới.
- Không bao giờ reset một sequence counter khi vẫn giữ cùng `stream_id`.
- Xác thực thiết lập stream qua control channel TCP. Một receiver không được reset
  tracker của nó chỉ vì một stream UDP bất ngờ xuất hiện.
- Bao gồm packet sequence mong đợi đầu tiên trong thiết lập đã xác thực đó và gọi
  `reset(stream_id, initial_packet_sequence)` trước khi tham gia stream. Nếu không
  có baseline này, mất hoặc reorder trước datagram được quan sát đầu tiên về cơ bản
  là không quan sát được.

Ở 625 packet/giây, một sequence 64-bit không wrap trong bất kỳ vòng đời hệ thống
thực tế nào. Wraparound do đó được coi là một stream mới, không phải logic thứ tự
modular đặc biệt.

## Thuật toán của receiver

`PacketLossTracker` duy trì một bitmap có giới hạn trên một cửa sổ reorder, mặc
định 256 packet.

1. Một gap ban đầu là pending, không phải lost.
2. Một packet thiếu chỉ trở thành `confirmed_lost` sau khi sequence của nó rời khỏi
   cửa sổ reorder.
3. Một packet lấp một gap pending là `reordered` và ngăn false loss.
4. Một sequence đã có trong cửa sổ là `duplicate`.
5. Một packet đến sau khi sequence của nó đã được finalize là `too_late`. Nó không
   giảm confirmed loss, vì các báo cáo lịch sử phải giữ ổn định.
6. Một packet từ stream khác là `wrong_stream` và không thể reset trạng thái.
7. Một bước nhảy tiến lớn không hợp lý bị từ chối mà không advance cửa sổ. Điều này
   ngăn một datagram bị hỏng hoặc giả mạo tạo ra một sự kiện loss khổng lồ.

`flush()` chỉ có thể finalize phần cửa sổ còn lại khi một stream được kết thúc rõ
ràng. Không flush theo timer hoặc khi im lặng tạm thời; làm vậy sẽ biến jitter bình
thường thành false loss.

## Metric được báo cáo

Mẫu số loss là:

```text
finalized_received + confirmed_lost
```

Các packet vẫn nằm trong cửa sổ reorder bị loại trừ. `unique_received` là một
counter chẩn đoán và không phải mẫu số thích hợp vì nó bao gồm các packet chưa
finalize và sẽ làm lệch phần trăm loss xuống dưới.

Telemetry nên báo cáo delta counter theo các khoảng cố định, không phải tỉ lệ tích
lũy suốt vòng đời. Dùng `packet_loss_delta(newer, older)` và bỏ khoảng đó nếu nó
không trả về giá trị. Snapshot delta bao gồm cả stream ID lẫn generation của
tracker, nên việc reset kể cả với một stream ID vô tình bị tái dùng cũng không thể
tạo ra một khoảng hợp lý nhưng không hợp lệ.

Khoảng báo cáo khuyến nghị:

- Snapshot counter một giây.
- Tối thiểu 200 packet đã finalize trước khi ra một quyết định sức khỏe.
- Giữ lại số đếm thô cùng với phần trăm per-mille.
- Báo cáo riêng các counter reorder, duplicate, too-late, wrong-stream và
  invalid-jump.

## Policy sức khỏe và fallback

Policy server ban đầu dùng hysteresis:

- Degraded: ít nhất 5% confirmed loss trong ba cửa sổ đủ điều kiện.
- Recovered: nhiều nhất 2% confirmed loss trong năm cửa sổ đủ điều kiện.
- Mẫu dưới 200 packet đã finalize không thay đổi trạng thái sức khỏe.
- Các giá trị giữa 2% và 5% reset cả hai streak và giữ nguyên trạng thái hiện tại.

Không chuyển từ multicast sang unicast chỉ vì một client báo cáo một khoảng xấu.
Việc chọn delivery nên kết hợp:

- thành công của multicast probe;
- một số khoảng packet-loss;
- việc thất bại ảnh hưởng tới một client, một segment switch, hay hầu hết client;
- sức khỏe TCP heartbeat;
- dung lượng NIC server và switch cho unicast fallback.

Dùng backoff ngẫu nhiên và một thời gian cư trú tối thiểu trước khi chuyển chế độ
delivery lần nữa. Nếu không nhiều client có thể dao động giữa multicast và unicast
đồng thời. `DeliveryModeSelector` hiện tại cũng yêu cầu năm multicast probe thành
công trước khi recover từ unicast theo mặc định.

## Mất packet so với mất frame

Loss transport và loss frame nhìn thấy được là các metric khác nhau.

- Một datagram UDP thiếu là mất packet transport.
- Một frame là incomplete khi deadline fragment của nó hết hạn với một hoặc nhiều
  fragment thiếu.
- Một retransmission được recover không khôi phục counter transport-loss lịch sử,
  nhưng nó có thể ngăn frame loss hiệu quả.
- Cố ý drop cả một frame trễ là một quyết định playout, không phải mất packet.
- Việc encoder bỏ qua frame không phải là mất mạng.

Reassembler tương lai phải theo dõi frame incomplete và deadline drop tách biệt với
`PacketLossTracker`.

## Validate vận hành

Trước khi triển khai, replay các capture chứa:

- traffic đúng thứ tự;
- reorder có giới hạn và cực đoan;
- duplicate;
- burst loss và loss cô lập;
- packet trễ đến sau khi finalize;
- sender khởi động lại với một stream ID mới;
- packet cũ từ stream trước;
- bước nhảy sequence lớn bị hỏng;
- gap sequence trong quá trình chuyển multicast-sang-unicast.

Phân tích packet capture nên so sánh counter của receiver với một script
sequence-number độc lập. Overflow receive-buffer của Windows, NIC drop, switch drop
và radio loss đều xuất hiện dưới dạng gap sequence, nên tìm nguyên nhân gốc cần
ETW, adapter counter và switch telemetry ngoài các metric ứng dụng.
