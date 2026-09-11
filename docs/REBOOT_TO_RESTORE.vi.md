# Thiết kế khôi phục trạng thái sau khi khởi động lại của NSTU

Trạng thái: **chỉ là kiến trúc và kế hoạch an toàn. NSTU hiện chưa cung cấp
khôi phục sau reboot và chưa thể thay thế Deep Freeze.** Installer và runtime
hiện tại chưa bật code thay đổi write filter, bảo vệ volume hoặc UWF.

Đây là tài liệu nguồn chuẩn trong repository cho trang GitHub Wiki tương lai.
Tài liệu xác định kế hoạch thận trọng để quản lý tập trung máy phòng học.
Đường monitoring bằng snapshot độc lập với công việc này và vẫn là ưu tiên
production; thiết kế này không yêu cầu H.264 liên tục cho từng client.

Phạm vi UWF chỉ dành cho client. NSTU phải giữ installation server/teacher bền
vững vì server có thể chứa exam package, tài liệu bài giảng, trạng thái
enrollment, audit record và báo cáo diagnostics. Vì vậy diagnostics của server
không qualification hoặc bật UWF; kết quả sẽ là không áp dụng. Qualification UWF
và mọi orchestration reboot-to-restore trong tương lai phải chạy trên image
client riêng.

## Quyết định

NSTU nên quản lý **Unified Write Filter (UWF)** được Microsoft hỗ trợ thay vì
tự viết file-system minifilter, storage filter, boot driver hoặc định dạng đĩa
copy-on-write mới.

UWF chặn các lần ghi không nằm trong exclusion vào volume được bảo vệ và
chuyển chúng vào virtual overlay. Ghi vào exclusion hoặc volume không được bảo
vệ vẫn tồn tại qua reboot, và exclusion không làm giảm mức tiêu thụ overlay.
Với overlay không persistent, restart Windows sẽ xóa các lần ghi bị chặn đó và
đưa volume được bảo vệ về trạng thái đã commit trước đó. NSTU chỉ nên phụ
trách policy, diagnostics, điều phối có xác thực, monitoring và hướng dẫn
recovery; Windows chịu trách nhiệm về filtering ở kernel.

Lựa chọn này giảm đáng kể attack surface ở kernel và tránh để NSTU phải tự xử
lý tính nhất quán khi crash giữa NTFS, BitLocker, paging, hibernation,
antivirus, boot repair và Windows servicing. Tuy vậy vẫn phải kiểm thử VM,
kiểm thử máy thật, giữ recovery image và review bảo mật độc lập.

NSTU không được:

- cài storage filter hoặc file-system filter driver do NSTU tự viết;
- dùng thay đổi registry không được hỗ trợ để mở UWF trên edition không hỗ trợ;
- coi VHD differencing, System Restore, mandatory profile hoặc script dọn file
  là tương đương với bảo vệ toàn volume sau reboot;
- tự động bật protection trong lúc cài NSTU thông thường;
- báo protection đã hoạt động trước khi reboot và kiểm tra sau boot thành công.

## Rào cản Windows edition

Ma trận yêu cầu UWF hiện tại của Microsoft liệt kê các edition sau:

| Windows edition | Microsoft hỗ trợ UWF | Kế hoạch NSTU |
| --- | --- | --- |
| Enterprise, gồm build Enterprise LTSC phù hợp | Có | Chỉ đủ điều kiện sau khi kiểm tra build, feature, storage, driver và recovery |
| Education | Có | Chỉ đủ điều kiện sau cùng các kiểm tra trên |
| IoT Enterprise, gồm build IoT Enterprise LTSC phù hợp | Có | Chỉ đủ điều kiện sau cùng các kiểm tra trên |
| Pro | Không | Chỉ audit; không có điều khiển reboot-to-restore tích hợp |
| Home | Không | Không hỗ trợ |
| Windows Server | Không nằm trong ma trận UWF client | Không hỗ trợ nếu Microsoft chưa công bố đường được hỗ trợ |

Không được dùng tên sản phẩm hoặc chữ "LTSC" làm bằng chứng duy nhất. Client
phải nhận diện SKU/build thật, kiểm tra optional feature
Client-UnifiedWriteFilter và xác nhận UWF WMI provider có sẵn. Phạm vi NSTU
có thể hẹp hơn ma trận Microsoft cho đến khi từng image cụ thể vượt qua các
gates kiểm thử của dự án.

### Baseline qualification đầu tiên: Windows 10 Education

Mục tiêu qualification đầu tiên là image Windows 10 Education x64 sạch và đã
cập nhật đầy đủ trên phần cứng tham chiếu yếu nhất của dự án: Intel Core
i5-6400, RAM 8 GiB, Intel HD 530 và link thương lượng 100 Mbps. Báo cáo test
phải ghi lại chính xác build Windows, servicing stack, driver đồ họa, layout
ổ đĩa, trạng thái optional feature UWF và firmware. "Windows 10 Education" chỉ
là mục tiêu test, không có nghĩa mọi build Windows 10 Education đều đã được
qualification. Image Windows 11 Education và Enterprise vẫn phải qualification
riêng.

Trial đầu tiên chỉ kiểm tra capability. Cần chứng minh diagnostics đọc được
SKU/build và trạng thái provider UWF mà không thay đổi filter. Chỉ sau khi
baseline vượt qua test reboot persistent, recovery image, overlay, servicing
và rollback mới được đánh giá controller UWF trong lab.

Giấy phép Windows là trách nhiệm của trường. MIT License của NSTU không cấp
quyền sử dụng Windows Enterprise, Education hoặc IoT Enterprise.

### Phương án thay thế cho Windows Pro

Không có công tắc an toàn nào của NSTU biến UWF thành tính năng được hỗ trợ
trên Windows Pro. Thứ tự lựa chọn là:

1. Chuyển phòng máy sang Windows edition được cấp phép và được hỗ trợ, sau đó
   chạy đầy đủ quy trình đánh giá UWF.
2. Giữ một sản phẩm reboot-to-restore bên thứ ba có license riêng; NSTU chỉ
   cung cấp tích hợp health/inventory dạng read-only qua API chính thức của nhà
   cung cấp, nếu có.
3. Dùng reimage tập trung giữa các học kỳ hoặc sau sự cố. Cách này phục hồi
   chậm hơn nhiều và không tương đương bảo vệ discard-on-reboot.

NSTU không được phát hành bản sao UWF chỉ dành cho Pro dựa trên script,
snapshot, shadow copy hoặc chép baseline lên live system volume. Bản sao như
vậy có tính atomicity, boot recovery, servicing và bảo mật khác hẳn, đồng thời
đưa phần rủi ro nhất của dự án trở lại sản phẩm.

## Phạm vi và threat model

Mục tiêu là xóa các thay đổi thông thường trong phiên học sinh sau reboot,
nhưng vẫn cho phép kỹ thuật viên được ủy quyền bước vào maintenance cycle có
kiểm soát.

Thiết kế có thể xử lý:

- drift vô tình của cấu hình hệ điều hành và ứng dụng;
- file và ứng dụng do tài khoản học sinh tiêu chuẩn ghi vào volume được bảo vệ;
- việc dùng lại image phòng máy đã biết là tốt;
- hiển thị tập trung current/next-session filter state và sức khỏe overlay.

Thiết kế tự nó không xử lý:

- compromise của local Administrator, SYSTEM, kernel, firmware hoặc
  deployment credential của server NSTU;
- đánh cắp dữ liệu, screen capture hoặc hoạt động độc hại trước reboot;
- ghi vào path bị exclude, volume không bảo vệ, thiết bị ngoài, firmware hoặc
  network storage;
- boot từ removable media, tháo đĩa hoặc tấn công offline;
- hỏng sẵn trong committed image;
- backup, disaster recovery, lưu giữ pháp lý hoặc bảo toàn bài làm học sinh.

Student standard user là adversary chính. Local administrator có thể thay đổi
UWF cho next session, còn phần mềm ở kernel có thể vượt qua ranh giới này.
Trường vẫn cần Secure Boot, khóa boot order/firmware, BitLocker khi phù hợp,
lưu recovery key an toàn, credential quản trị riêng, firewall policy và image
recovery đã biết là tốt.

## Ranh giới quyền và ủy quyền

Cấu hình UWF cần quyền cao. Nó phải nằm trong process LocalSystem
nstu-service, sau một module typed nhỏ; không được chạy trong nstu-agent.exe
hoặc giao diện tương tác của giáo viên.

Các component phía service được đề xuất:

| Component | Trách nhiệm |
| --- | --- |
| RestoreCapabilityProbe | Đọc SKU/build, optional feature, current/next UWF, protected volume, exclusion, overlay và sức khỏe event UWF |
| RestorePolicyValidator | Kiểm tra policy bất biến, có version, theo luật an toàn cục bộ |
| UwfController | Gọi UWF WMI provider qua allowlist operation cố định |
| OverlayMonitor | Đọc consumption và event warning/critical, không đổi cấu hình |
| MaintenanceCoordinator | Lưu transaction reboot/servicing có giới hạn và chạy kiểm tra sau boot |
| RestoreAudit | Ghi ai yêu cầu, state nào đổi, kết quả đã kiểm tra; không ghi secret/screen data |

Implementation nên dùng UWF WMI provider được tài liệu hóa làm API chính.
uwfmgr.exe get-config có thể hữu ích cho kỹ thuật viên chẩn đoán độc lập,
nhưng service không được tạo command line từ network input hoặc mở generic
process-execution endpoint.

Teacher control authentication hiện tại không đủ để cấp quyền cho storage
policy. Trước khi cho phép mutation từ xa, NSTU cần role và credential riêng
cho deployment administrator. Mỗi request mutation phải là operation typed chặt
chẽ, có intent ID duy nhất, target device, current state kỳ vọng, policy
revision mong muốn, thời hạn, maintenance window và replay protection. Client
phải tự kiểm tra lại mọi điều kiện trước khi đổi state.

Các operation sau tối thiểu phải cần deployment-administrator:

- cài hoặc gỡ optional UWF feature;
- protect hoặc unprotect volume;
- enable hoặc disable filter;
- đổi exclusion, loại/kích thước overlay hoặc persistence;
- vào hoặc rời servicing mode;
- reboot, decommission hoặc chuyển quyền quản lý client được bảo vệ.

Kích hoạt lần đầu trên mỗi tổ hợp hardware/image và mọi thao tác recovery hoặc
decommission cần kỹ thuật viên xác nhận tại máy. Giáo viên thông thường có thể
xem health và yêu cầu reboot phòng học theo policy trường, nhưng không có quyền
sửa UWF policy.

## Ranh giới dữ liệu persistent

Reboot-to-restore chỉ hữu ích khi ranh giới persistence được xác định rõ.
NSTU nên ưu tiên một **control-state volume** không bảo vệ riêng, ACL chỉ cho
LocalSystem và administrator ghi. Volume này chỉ dành cho identity, policy,
transaction, audit và health state của NSTU, không phải nơi lưu bài của học
sinh. Bài học sinh cần một user-data volume riêng hoặc network/cloud path do
trường phê duyệt, với ACL dành cho học sinh như dự kiến. Nếu máy chỉ có một
volume, dùng số exclusion file/registry nhỏ nhất đã được review.

Dữ liệu NSTU được phép tồn tại qua reboot chỉ gồm:

- enrollment identity và key được bảo vệ bằng DPAPI;
- restore policy đã ký và policy revision tăng dần;
- trạng thái transaction update/maintenance có giới hạn;
- audit và health record có giới hạn;
- network configuration được duyệt rõ ràng để enrollment ổn định.

Thư mục executable NSTU, DLL, script, plugin/search path, startup entry và mọi
thư mục có thể thực thi code không được cho học sinh ghi và không được exclude
rộng. Không bao giờ exclude toàn bộ C:\Users, C:\ProgramData, thư mục Windows
hoặc browser profile chỉ vì tiện. Mỗi exclusion là một đường persistence cho
cả state hợp lệ lẫn kẻ tấn công.

Qualification checklist phải từ chối rõ các exclusion phủ lên file hệ thống,
cấu hình Windows và đường boot quan trọng được Microsoft nêu, gồm
`\Windows`, `\Windows\System32`,
`\Windows\System32\config\{DEFAULT,SAM,SECURITY,SOFTWARE,SYSTEM}`,
`BOOTSTAT.DAT`, volume root, `Drivers` và page file. Các path này phải tiếp tục
được bảo vệ, trừ khi một quy trình Windows servicing đã được tài liệu hóa yêu
cầu thay đổi tạm thời và đã được review.

Bài làm học sinh phải được chuyển tới network location, cloud location hoặc
user-data volume riêng do trường phê duyệt. UI phải cảnh báo rõ bài làm để trên
protected volume sẽ bị xóa sau restart.

## Chính sách overlay cho máy cấu hình thấp

Máy mục tiêu i5-6400/8 GB khiến RAM overlay lớn trở nên không phù hợp. Pilot
ban đầu nên đánh giá **disk overlay với state không persistent** và, nếu được
hỗ trợ rồi kiểm chứng, free-space passthrough. Đây là giả thuyết kiểm thử,
không phải production default.

Không hard-code một kích thước overlay cho mọi trường. Lấy kích thước từ lượng
ghi cao nhất đo được trong một ngày học đầy đủ cộng safety margin có ghi chép.
Kiểm tra Windows, browser, office suite, antivirus, exam mode, printing và
download lớn bất thường. Đặt warning/critical threshold thấp hơn maximum và
báo cả ba giá trị về server.

Persistent overlay trái với kỳ vọng "discard mỗi reboot" và Microsoft mô tả
đây là chế độ thử nghiệm. NSTU phải giữ chế độ này tắt, trừ khi một policy
riêng được review sau này yêu cầu rõ ràng.

Fast Startup phải tắt vì shutdown kiểu Fast Startup không xóa overlay. Side
effect của UWF phụ thuộc vào đường kích hoạt: Microsoft ghi nhận các thay đổi
với paging, System Restore, SysMain, indexing, fast boot, drive optimization,
boot status, Windows Update, Store update và maintenance khi bật UWF bằng
`uwfmgr.exe`/WMI trên installation đang chạy, còn provisioning bằng SMI hoặc
unattend không nhất thiết áp dụng cùng các thay đổi. Installer phải lưu
baseline, ghi rõ activation method và báo chính xác setting nào đã đổi trước
khi bật. Tắt UWF sau đó không tự khôi phục tất cả thiết lập hiệu năng; phải
lưu baseline và giá trị mong muốn sau khi gỡ.

## Mô hình state

Server và client phải hiển thị cả current session và next session state. Setting
đang xếp hàng chưa phải setting đang hoạt động.

~~~text
unsupported
  -> audit-only
  -> eligible/unconfigured
  -> feature-reboot-pending
  -> ready/unprotected
  -> protection-reboot-pending
  -> protected/healthy
  -> servicing-reboot-pending
  -> servicing
  -> validation-reboot-pending
  -> protected/healthy
~~~

Mọi state có thể chuyển sang blocked hoặc recovery-required. Transaction phải
ghi boot identifier và attempt count có giới hạn. NSTU tuyệt đối không được tạo
reboot loop tự động. Sau hai lần boot/health thất bại, thiết bị dừng mutation tự
động, hiện là quarantined khi còn mạng và yêu cầu kỹ thuật viên chạy recovery
runbook.

## Qualification và kích hoạt lần đầu

Protection phải opt-in và tách khỏi cài NSTU thông thường.

1. Inventory SKU/build, firmware mode, Secure Boot, disk layout, BitLocker,
   nơi giữ recovery key, free space, file system, Storage Spaces, page-file,
   security product, network profile và tất cả local account.
2. Từ chối edition không hỗ trợ, Storage Spaces, UWF feature/WMI provider
   không có, thiếu recovery material, file system lỗi hoặc image chưa qua test
   backup/restore.
3. Tạo và kiểm tra image tốt trước khi bật protection. UWF không phải backup và
   không sửa được baseline đã hỏng.
4. Chỉ bật optional feature của Windows, restart, rồi xác nhận feature/WMI
   provider khỏe.
5. Khi filter đang tắt, áp dụng policy protected-volume, exclusion, overlay,
   threshold và persistence đã review. Hiển thị side effect của UWF và yêu cầu
   local administrator xác nhận.
6. Enable protection cho next session và restart bằng đường Windows/UWF đã
   được phê duyệt.
7. Sau boot, kiểm tra current/next filter, volume identity, exclusion, overlay,
   threshold, Fast Startup, service account, agent session, enrollment, server
   handshake, snapshot, chat và audit persistence.
8. Chạy sentinel test: tạo file thử vô hại trên protected volume, restart,
   chứng minh file biến mất, đồng thời chứng minh persistent state được duyệt
   vẫn còn. Chưa bật fleet control trước khi test này đạt.

Khi có thể, protected volume nên bind bằng identity ổn định thay vì mặc định
C: luôn là volume đúng.

## Vận hành hằng ngày và overlay cạn

Client báo filter state, loại overlay, maximum size, consumption, warning/
critical state, lần reset thành công gần nhất, boot identity và maintenance
đang chờ. Server không thu thập tên file trong overlay khi vận hành bình thường.

Ở warning threshold, báo giáo viên và dừng các ghi không thiết yếu như
diagnostic capture. Event ở critical threshold chỉ là telemetry; tự nó không
cho phép NSTU restart máy. Khi overlay đạt kích thước tối đa, Windows có thể
tự restart trên các build được hỗ trợ; đây là rủi ro exhaustion riêng phải đo
trong qualification matrix. NSTU chỉ được ép restart theo deployment policy và
consent path rõ ràng, sau khi kiểm tra trạng thái kỳ thi và cảnh báo người dùng
lưu bài ở nơi bên ngoài. Khi overlay đầy, shutdown bình thường có thể mất rất
lâu; dùng operation restart UWF đã tài liệu hóa trong recovery path đã kiểm
thử, và audit mọi ngoại lệ khi tiếp tục chạy được đánh giá là nguy hiểm hơn
mất dữ liệu.

## Servicing và cập nhật NSTU

Máy được UWF bảo vệ cần workflow servicing rõ ràng. UWF thường thay đổi
Windows Update và maintenance vì update thông thường sẽ bị xóa. Servicing path
được Microsoft tài liệu hóa có thể xóa overlay, restart, tạm tắt filter, áp
dụng package đã stage cục bộ rồi bật lại protection; phải kiểm chứng cơ chế và
hành vi khi lỗi trên từng image hỗ trợ. NSTU không được ngụ ý rằng servicing
mode sẽ cài một package mạng bất kỳ hoặc luôn bật lại protection sau lỗi.

NSTU nên tích hợp workflow này với thiết kế package đã ký trong
AUTO_UPDATE_LTSC.md:

1. Deployment administrator mở maintenance window; client từ chối khi đang thi
   hoặc chưa đồng bộ xong user data.
2. Trước khi đổi UWF, kiểm tra nguồn điện, recovery artifact, free disk/overlay,
   tương thích server, manifest/package đã ký, role, version và anti-downgrade.
3. Lưu transaction signed có giới hạn ở ngoài overlay bị xóa.
4. Vào servicing path hoặc filter-disabled path được tài liệu hóa cho next
   session rồi restart. Cơ chế phải rõ ràng, đã kiểm thử và có thể recovery
   nếu NSTU không khởi động được. Servicing mode của Microsoft yêu cầu mọi
   local account có password; preflight phải bắt điều kiện này.
5. Chỉ áp dụng package đã verify và stage cục bộ (hoặc trên recovery medium
   được tin cậy rõ ràng). Không tải hoặc chạy payload chưa xác thực trong
   session không bảo vệ.
6. Dùng cơ chế được tài liệu hóa để bật lại protection và restart, sau đó chứng
   minh từ current state sau boot rằng filter đang hoạt động trước khi báo
   thành công.
7. Chỉ commit version NSTU mới sau khi health check và bằng chứng protection
   đạt. Nếu thất bại, giữ một state có thể recovery và quarantine rõ ràng, rồi
   chạy signed package rollback/recovery-image; không bao giờ âm thầm để máy ở
   trạng thái không được bảo vệ, và dừng sau số lần thử giới hạn.
8. Báo current/next UWF state và package version cuối về server.

Prototype phải kiểm chứng workflow custom application servicing trên từng build
LTSC hỗ trợ; test state-machine trong repository không chứng minh Windows
servicing mode chạy thành công.

## Decommission và gỡ cài đặt

NSTU không được xóa service quản lý rồi để máy ở trạng thái protected nhưng
không ai quản lý.

1. Cần deployment-administrator authorization, kỹ thuật viên xác nhận tại máy
   và recovery key sẵn sàng.
2. Ghi rõ policy cuối: chuyển UWF sang tool khác, hoặc tắt protection và tùy
   chọn gỡ Windows feature.
3. Disable/unprotect cho next session, restart, rồi xác nhận current session
   đã unprotected.
4. Export audit record cuối, sau đó chạy uninstaller NSTU hiện có vốn gắn với
   reboot.
5. Gỡ Windows UWF feature là hành động riêng cần xác nhận; không tự gỡ Windows
   component chỉ vì NSTU bị gỡ.

Nếu service hỏng khi protection đang bật, recovery phải dùng quy trình Windows/
UWF chính thức từ local recovery đáng tin cậy, known-good system image hoặc
recovery media, cùng một cách đã kiểm thử để disable hoặc unconfigure UWF khi
NSTU không khởi động được. NSTU phải phát hành quy trình đó và offline
diagnostics package đã ký trước khi tính năng rời pilot.

## Hành vi khi lỗi

| Lỗi | Kết quả bắt buộc |
| --- | --- |
| Server không dùng được trong vận hành protected bình thường | Giữ protected; reboot-to-restore cục bộ vẫn chạy; không đổi policy |
| Admin request sai/hết hạn/replay | Từ chối và audit, không đổi next-session state |
| Mất mạng trước maintenance reboot | Hủy hoặc giữ nguyên protected state hiện tại |
| Mất mạng trong servicing session không bảo vệ | Chỉ hoàn tất transaction cục bộ đã verify hoặc dừng để kỹ thuật viên recovery; không fetch payload mới |
| Current/next UWF lệch transaction | Dừng tự động, quarantine, yêu cầu recovery |
| Overlay warning/critical | Thông báo, giảm ghi không cần thiết, theo restart policy đã test |
| Boot/health lỗi lặp lại | Dừng sau số lần giới hạn; không reboot loop |
| ACL persistent state hoặc chữ ký sai | Không mutate UWF; báo recovery-required |

## Test và release gates

Tính năng này có tính destructive. Windows Sandbox không đủ cho qualification
vì không cung cấp lifecycle nhiều reboot có tính persistent và checkpoint cần
thiết. Trước hết dùng VM persistent có checkpoint, sau đó dùng máy vật lý
disposable. Không chạy mutation test sớm trên image production của trường.

### Gate 0: tài liệu và review

- Có trích dẫn và review hành vi UWF cùng ma trận edition của Microsoft.
- Project và IT trường duyệt threat model, persistent-data boundary, exclusion,
  quyền recovery và licensing.
- Review độc lập xác nhận không cần kernel component tự viết.

### Gate 1: capability probe chỉ đọc

- Unit test dùng WMI adapter giả và không thể đổi host.
- Probe phân biệt đúng supported, unsupported, feature-missing và current/next
  state không nhất quán.
- Server UI đánh dấu tính năng experimental và read-only.

### Gate 2: thử mutation trên VM persistent

- Cài feature, protect, reset, service, disable, uninstall và recovery hoàn
  tất qua reboot thật trên mọi Windows image được tuyên bố hỗ trợ.
- Test power loss đột ngột, overlay đầy, update lỗi, state hỏng, command hết
  hạn, mất server, account không password, BitLocker và rollback.
- Checkpoint VM và image sạch khôi phục được mọi ca cố tình làm lỗi.

### Gate 3: kiểm tra máy vật lý cấu hình thấp

- Test baseline i5-6400/8 GB/Intel HD 530, gồm HDD và SSD nếu trường sử dụng.
- Đo boot time, overlay growth mỗi ngày, RAM, CPU, disk latency, network
  persistence và ít nhất 50 protect/reset cycle liên tiếp.
- Kiểm tra antivirus, Office/browser, printing, exam mode, mất điện và
  shutdown phòng học.

### Gate 4: pilot có kiểm soát

- Một phòng không production và nhóm client nhỏ chạy đủ một chu kỳ giảng dạy,
  review audit mỗi ngày và có recovery path tại chỗ.
- Không chấp nhận persistence không giải thích được, overlay exhaustion,
  reboot loop, mất enrollment hoặc restore thất bại.
- IT trường ký xác nhận user-data handling và emergency recovery.

### Gate 5: đủ điều kiện production

- Feature mặc định tắt và chỉ bật bằng signed policy cho đúng SKU/build/image
  đã validate.
- Hoàn tất code signing, protocol authorization review, fuzzing,
  updater/rollback, UWF servicing, offline recovery và incident runbook.
- Capability hoặc health check thất bại phải loại máy khỏi rollout, không làm
  yếu protection để cố tiếp tục.

## Các giai đoạn bàn giao

1. **Hoàn tất nghiên cứu:** tài liệu này và ma trận nguồn chính thức.
2. **Telemetry read-only:** capability, current/next state, overlay health và
   event log; production build chưa biên dịch mutation method.
3. **Controller local cho lab:** WMI operation typed trong build flag lab-only,
   xác nhận tại máy và test VM persistent.
4. **Servicing coordinator:** signed transaction state, tích hợp update,
   recovery có giới hạn và decommission flow.
5. **Quản trị từ server:** role deployment riêng, fleet wave theo lịch,
   hiển thị current/next state và export audit.
6. **Pilot vật lý và review độc lập:** chỉ sau đó mới cân nhắc policy opt-in
   cho production.

Không nên hứa ngày production trước khi Gate 0-3 đạt. Rủi ro lớn nhất không
phải dashboard mà là giữ máy boot được và có đường bảo trì có thể recovery,
kiểm toán được qua mọi tình huống lỗi.

## Tài liệu chính thức

- Tổng quan UWF và yêu cầu edition:
  https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/
- Bật và cấu hình UWF:
  https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-turnonuwf
- Overlay UWF, kích thước, threshold và exhaustion:
  https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwfoverlay
- Servicing thiết bị được UWF bảo vệ:
  https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/service-uwf-protected-devices
- UWF exclusions:
  https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwfexclusions
- Tài liệu tham chiếu uwfmgr.exe:
  https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwfmgrexe
- Tài liệu tham chiếu UWF WMI provider:
  https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-wmi-provider-reference
- Troubleshooting và event log UWF:
  https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwftroubleshooting

> Các nguồn được xem lại ngày 2026-09-09. Tài liệu Microsoft là nguồn quyết định
> về hỗ trợ nền tảng và có thể thay đổi; phải kiểm tra lại trước mỗi production
> release.
