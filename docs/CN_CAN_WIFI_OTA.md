# AMOV_RID：CAN 国标广播实验固件与 OTA 教程

本版实现 **DroneCAN 输入 → GB 46750—2025 必选字段编码 → Wi-Fi Beacon**。
串口/MAVLink 输入、蓝牙国标广播、网络 RID、120 小时存储和飞控处置不在本版范围。

这是实验固件：国标载荷有独立测试，但 Wi-Fi 外层尚无已确认的官方互通标识。
`CN_WIFI_ID` 必须显式设置。下文 `02000001` 仅为封闭台架实验约定，不能据此宣称
能被所有国标接收器接收或满足整机认证。实际产品应使用有依据的封装并完成互通和射频测试。

## 1. 交付与升级边界

交付目录：`/home/amovlgf/ArduRemoteID/artifacts/cn-can-wifi/`。

| 文件 | 用途 |
| --- | --- |
| `firmware/ArduRemoteID_AMOV_RID_CN_OTA.bin` | 本版签名应用包，供 Wi-Fi 网页上传 |
| `firmware/ArduRemoteID_AMOV_RID_recovery_OTA.bin` | 用本次密钥签署的上次应用，供网页恢复旧模式 |
| `keys/amov_cn_public_key.dat` | 写入旧设备空闲公钥槽的公钥 |
| `keys/amov_cn_private_key.dat` | 本地后续签名所需的私钥，不上传到设备、不分享、不提交 Git |
| `baseline/` | 上次原始完整固件、应用、分区及校验值 |
| `manifest.json`、`SHA256SUMS`、`TEST_RESULTS.md` | 构建来源、镜像校验值、验证记录 |

本次私钥由工具本地生成，`keys/` 目录权限为700，私钥权限为600，整个 `artifacts/` 被Git忽略。
请单独备份私钥；丢失后不能再以该公钥签署更新。公开分享固件时不要把 `keys/` 整个打包。

目标仍为 **AMOV_RID / ESP32-C3 / BOARD_ID=13**：CAN TX GPIO4、RX GPIO5、1 Mbit/s。
通用开发板的 BOARD_ID 不同，不能将其固件上传到 AMOV。

保留旧bootloader、分区、参数命名/类型、公钥及锁级。应用分区均为 `0x1f0000` 字节。
旧上传器会额外写4097字节，发布检查要求：

```
签名 OTA 文件大小 + 4097 <= 2031616
```

首次 OTA 成功取决于旧设备能访问升级网页、板卡身份匹配且信任签名公钥。
代码和离线检查不能替代实际设备升级。旧bootloader没有自动回滚；如果新程序完全无法启动
或物理Wi-Fi失效，不能保证通过同一无线通道恢复。

## 2. 确认旧设备与信任公钥

先在停桨地面维护状态操作，保持稳定供电。现有升级方式是连接模块管理Wi-Fi后使用网页，
不是互联网云端推送。以下终端命令从仓库根目录运行：

```bash
cd /home/amovlgf/ArduRemoteID
```

使用1 Mbit/s USB-CAN与设备连接，正确接CAN_H/CAN_L并共地，按总线实际布线配置终端电阻。
Linux SocketCAN 示例（仅需在接口尚未配置时执行）：

```bash
sudo ip link set can0 down
sudo ip link set can0 type can bitrate 1000000
sudo ip link set can0 up
```

确认设备实际CAN节点ID。下文用 **125** 表示RID设备，**42** 表示测试发送器；它们不是
强制值，不要和现有节点冲突。若旧设备使用动态节点分配，请先让现有飞控/DroneCAN GUI
分配节点，或保持现有总线上的分配器工作。本脚本不承担动态分配器功能。

```bash
python3 scripts/cn_dronecan_test.py can0 --node-id 42 --target-node 125 \
  --no-stream --get LOCK_LEVEL --get PUBLIC_KEY4 --get WEBSERVER_EN \
  --get WIFI_SSID --get WIFI_CHANNEL
```

默认固件通常是 `LOCK_LEVEL=0`，但应以读回值为准。默认内置的是ArduPilot发布公钥，
它们不会自动信任我们本次生成的私钥。公钥槽4为空时，执行：

```bash
python3 scripts/cn_dronecan_test.py can0 --node-id 42 --target-node 125 \
  --no-stream --install-public-key artifacts/cn-can-wifi/keys/amov_cn_public_key.dat \
  --key-slot 4
```

此命令会检查锁级、公钥格式、空槽并独立读回，拒绝覆盖其他公钥。槽4已占用时先检查槽5，
确认空闲再使用 `--key-slot 5`。不要删除现有厂商公钥。

若 `LOCK_LEVEL>0`，普通参数写入会被拒绝，需要使用设备已信任密钥的DroneCAN安全命令。
没有该密钥时，单靠本次新固件无法绕过旧固件校验。不要把锁级改成-1作为本教程的升级办法。

公钥添加完成后，在地面重启一次，重新读取对应槽，确认公钥持久保存。
同时确认 `WEBSERVER_EN=1`；若为0，先在DroneCAN GUI的设备参数列表中将它设为1并写入，
重启后核对参数和管理网页可访问，再开始升级。脚本的普通 `--set` 仅开放中国新增参数。

## 3. 验证并上传本版应用

先进行不连接设备的包检查：

```bash
python3 scripts/check_ota.py \
  artifacts/cn-can-wifi/firmware/ArduRemoteID_AMOV_RID_CN_OTA.bin \
  --public-key artifacts/cn-can-wifi/keys/amov_cn_public_key.dat --board-id 13
```

检查应显示签名有效、板号13、空间足够。这也检查旧校验器能否在正确位置找到签名描述符。

1. 连接设备原有管理Wi-Fi，沿用原密码。未改过时密码通常为 `ArduRemoteID`。
2. 打开 [升级网页](http://192.168.4.1/)，记录旧版本、板型和身份配置。
3. 在 Firmware Update 选择 **`ArduRemoteID_AMOV_RID_CN_OTA.bin`**，等待完整上传和重启。
4. 重新连接Wi-Fi、刷新页面，核对版本为 **1.15**、板型AMOV，并记录 `Running application slot`。
5. 确认公钥、Wi-Fi信息、原身份仍保留。升级后默认仍是OpenDroneID，不会自动启用缺参数的中国模式。

不要上传 `ArduRemoteID-AMOV_RID.bin` 完整镜像，也不要把未签名的编译产物当作本教程的发布包。
旧网页存在失败响应不可靠的问题，上传100%或显示OK不是充分证据，必须核对重启后的实际版本。
新版已修正失败清理和重复上传逻辑。

## 4. 配置中国 CAN 输入与实验广播

| 参数 | 含义及本版规则 |
| --- | --- |
| `RID_PROFILE` | 0=原模式；1=中国CAN模式；重启生效 |
| `CN_CAN_NODE` | 唯一接受的飞控/测试发送器节点，1～127 |
| `CN_UAS_ID` | 整机产品码，必须完整20个ASCII字符，不含空格（33～126）；本版仅做编码形态检查，合法身份由制造商负责 |
| `CN_UIN` | 登记号码后8个ASCII字符，不含空格（33～126）；未填写可为空，仍发送8字节NULL |
| `CN_CLASS` | 0微型、1轻型、2小型、3中型、4大型；初始255表示尚未配置 |
| `CN_WIFI_ID` | 8个十六进制字符，前三字节vendor标识＋一字节type；初始为空；禁止复用ASTM的FA0BBC0D |

下列产品码和登记标志仅为台架样例，不属于真实登记信息：

```bash
python3 scripts/cn_dronecan_test.py can0 --node-id 42 --target-node 125 --no-stream \
  --set CN_UAS_ID=12345678901234567890 \
  --set CN_UIN=00000001 --set CN_CLASS=2 --set CN_CAN_NODE=42 \
  --set CN_WIFI_ID=02000001 --set RID_PROFILE=1
```

每项写入均核对设备返回值。先配齐字段，再设 `RID_PROFILE=1`；此时网页提示需要重启。
地面重启后，网页应显示 China GB 46750 实验模式。没有CAN输入时应显示故障，但管理网页仍可访问。

```bash
python3 scripts/cn_dronecan_test.py can0 --node-id 42 --duration 60
```

工具每秒发送2组现有 `Location` / `System` 消息。预期网页China区域显示位置31°N、121°E，
运行状态1，健康OK；这是模拟位置，不要把测试发送器接到运行中的航空器总线。

真实飞控接入时，设置其实际节点ID；两种消息必须来自同一节点。
时间重建用System的2019年起UTC秒确定日期/小时，再结合Location的UTC小时内秒。
无可靠一致时间时发送未知；本版时间精度保守报0，不承诺真实导航或授时精度。
WGS84沿用现有CAN字段定义；固定遥控站位置映射为国标遥控站位置。

中国配置只开放CAN参数与CAN安全配置入口；串口专用代码未修改。若之后锁定了参数，
需要使用已有 `scripts/secure_command.py` 和对应可信私钥经CAN修改，不能用普通GetSet越过锁定。

## 5. 再次 OTA 与恢复

中国模式保持管理AP。参数缺失、CAN断流或编码失败不会触发自动重启，也不会主动关闭管理网页。
无线驱动故障可能短暂重建同名AP，重新连接即可继续检查；物理无线故障需要另外恢复。

在地面再次上传同一份已验证的新包，重启后核对运行槽从 `app0`/`app1` 切换到另一个槽，
并确认中国配置保留。无需为了覆盖两个槽再生成一份无实际变化的不同版本。

需要回到上次程序时，先读回并确认 `WEBSERVER_EN=1`；中国模式会强制保留管理AP，
但旧程序遵从此参数，为0时恢复后可能失去网页。必要时按第2节先用CAN参数工具启用。
然后用同样的离线检查方法验证签名恢复包，再通过网页上传
`ArduRemoteID_AMOV_RID_recovery_OTA.bin`。恢复后版本应回到1.14；旧固件忽略新增中国参数，
管理信息和公钥仍在。以后再升1.15时先读回 `RID_PROFILE`，它可能仍保存为1。

OTA本身会同步写Flash并重启，不保证上传期间持续产生新的RID样本；只在地面维护期间执行。
现有同步网页服务处理慢请求时也可能阻塞CAN和广播更新，因此本版尚未保证网页并发时的
1秒更新硬上界；正常负载和慢客户端都需实机测试，后续需将网络处理与广播调度分开。
详细场景与需要实机补测的项目见 [测试说明](CN_TESTING.md)。
