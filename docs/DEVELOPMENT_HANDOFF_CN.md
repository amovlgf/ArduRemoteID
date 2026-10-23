# AMOV RemoteID 功能清单与开发交接

更新日期：2026-10-23（Asia/Shanghai）。本文记录当前代码能力和本次验证边界，供新的 Codex 对话继续开发。

## 当前基线

- 仓库：`amovlgf/ArduRemoteID`；本机目录：`/home/amovlgf/ArduRemoteID`。
- 开发分支：`codex/rid-device-management`；固件版本：`1.17`。
- 当前目标：FlyCore I225U V1 集成的 AMOV RemoteID，ESP32-C3，`BOARD_ID=13`。
- 测试与发布共用 `make amov-rid`，每台设备的身份独立存储，不编入公共固件。
- 本仓库负责模块固件、协议定义、工厂工具和主机测试；QGC 页面与 PX4 转发桥属于外部工程。
- 使用前先执行 `git status --short --branch`、`git log -1 --oneline`，以实际检出版本为准。

## 已实现的功能

| 功能 | 当前能力 | 主要代码入口 |
| --- | --- | --- |
| 板级支持 | DroneCAN 1 Mbit/s；TX GPIO4、RX GPIO5；GPIO10 状态灯；保留 ESP32-C3 原生 USB；模块 MAVLink 禁用；最高锁级别为 1 | `RemoteIDModule/board_config.h`、`led.cpp`、`Makefile` |
| 中国 RID 数据处理 | 接收指定 CAN 节点的 Location/System，检查来源、数据时效、重复和乱序，处理 UTC 跨小时、未知值及健康状态 | `RemoteIDModule/CNData.cpp`、`RemoteIDModule.ino` |
| 中国载荷编解码 | GB 46750 载荷编码/解码、边界与枚举检查、固定字节测试向量 | `libraries/opendroneidcn/` |
| 中国格式 Wi-Fi 广播 | 载荷进入 beacon/probe response 的 vendor IE；配置 4 字节标识；异常时撤销旧广播并恢复无线接口 | `RemoteIDModule/WiFi_TX.cpp` |
| 出厂身份 | 硬件 UID 绑定的 120 字节 `AMOVFID1` 记录，含 20 字节整机 SN、类别、修订号及可选登记号/UIN；校验 CRC、字段格式及硬件绑定 | `RemoteIDModule/platform_activation.cpp` |
| 首次身份预置 | 专用 `PROVISION` 操作；仅允许确认没有旧记录且 `LOCK_LEVEL=0` 的设备；核对 UID、启动会话；保存并读回；拒绝覆盖既有身份；支持原事务回执查询 | `RemoteIDModule/management_service.cpp`、`platform_activation.cpp` |
| 激活与取消激活 | 接收客户端同步的平台状态；双槽持久化、重启恢复、保存核验后生效；首次写入工厂身份仍为未激活 | `RemoteIDModule/activation_service.cpp`、`platform_activation.cpp` |
| 业务广播限制 | 无有效身份或未激活时停止业务广播；数据过期/未知时广播故障状态与未知值；保留维护 AP；普通参数不能改写身份 | `RemoteIDModule/RemoteIDModule.ino`、`activation_service.cpp`、`parameters.cpp` |
| DroneCAN 设备管理 | `INFO / METADATA / GET / SET / STATUS / ACTIVATE / RESET / REBOOT / PROVISION`；有界队列、事务回执、结果查询与异常恢复 | `RemoteIDModule/management_service.cpp`、`DroneCAN.cpp` |
| 参数管理 | 参数类型、范围、默认值、只读及重启要求元数据；密码掩码；写入后读回核验；恢复普通设置保留工厂身份和激活状态 | `RemoteIDModule/management_parameters.cpp`、`parameter_text.h` |
| Wi-Fi 与网页恢复 | AP 最多 4 台客户端；检查 AP/IP/DHCP 就绪；失败重试；独立网络任务；HTTP 监听恢复 | `RemoteIDModule/WiFi_TX.cpp`、`wifi_network.h`、`webinterface.cpp` |
| 状态网页与 OTA | 展示 CAN、身份、激活和广播状态；签名 OTA；板型/签名/容量检查及上传失败处理；应用 OTA 保留 NVS | `RemoteIDModule/web/`、`ota_upload.h`、`check_firmware.cpp` |
| 工厂生产工具 | 单台/批量生成身份文件、NVS CSV 和可选分区；批内 UID/SN 去重；拒绝覆盖现有输出；失败清理本次新文件 | `scripts/factory_identity.py` |
| 联调工具 | CAN 正常/故障数据输入、抓包/十六进制解码、OTA 校验、PX4 台架 GPS/System 输入 | `scripts/cn_dronecan_test.py`、`cn_rid_decode.py`、`check_ota.py`、`px4_cn_bench.py` |
| 跨工程协议 | MAVLink 61000–61004 身份/UOM 状态/设备请求响应定义；设备管理 DSDL、共享编解码、摘要向量与互通测试 | `protocol/amov_rid_uom/`、`protocol/amov_device_bridge/` |

AMOV_RID 启动时选择中国模式；通用板仍默认 OpenDroneID。原项目的 BLE、Wi-Fi NAN 支持不能等同于“中国格式 BLE/NAN 已实现”，目前中国载荷使用 Wi-Fi vendor IE。

默认维护热点为 `B950_RID`，默认密码 `987654321`，地址为 `http://192.168.4.1`。已有 NVS 中的设置优先。电脑应自动获取 IPv4 地址；正常为 `192.168.4.x`，获取到 `169.254.x.x` 表示 DHCP 尚未成功。

## 身份、激活和升级边界

1. 先建立设备自己的工厂身份，再单独同步平台激活状态；写入 SN 不等于激活。
2. 首次预置保留普通参数和 OTA 公钥；不能覆盖已存在、损坏或部分残留的旧身份记录。通信中断后先查原事务和身份，不要盲目换事务重写。
3. `RESET` 重置普通设置，身份与激活状态保留；不能据此认为所有普通参数或 OTA 公钥均会保留。
4. 旧签名激活包、测试授权导入路径已移除；不要恢复 `AMOV_RID_TEST` 或将每台设备身份写进通用固件。
5. 工厂分区工具生成的完整 NVS 镜像仅用于空白设备；现有设备使用专用首次预置或签名应用 OTA。整分区覆盖会替换其他 NVS 数据。
6. AMOV 没有手动 BOOT 恢复路径，不支持 level 2 永久 eFuse 锁定。首刷与维护要求见 [BUILDING.md](../BUILDING.md)。

## 外部工程与尚未验收的事项

- QGC 管理页需支持本协议及首次预置能力；PX4 需启用 `CONFIG_UAVCAN_DEVICE_BRIDGE`。能读取普通 `uavcan param` 不能证明管理桥已启用。
- 本机相关工程路径为 `/home/amovlgf/QGroundControl` 和 `/home/amovlgf/PX4-Autopilot`。本次提交、清理和推送仅覆盖 ArduRemoteID，不代表另外两个仓库同步提交或完成验收。
- 激活接口保存的是客户端同步的平台状态；本仓库没有模块直连真实 UOM 平台的认证、联网和上报实现。协议中的 UOM 状态定义也不代表平台业务已经接通。
- 首次预置依赖可信本地生产连接，没有额外密码/签名认证；QGC 的地面/未解锁限制属于客户端，透明桥自身没有解锁检查。批次之间的 SN 唯一性仍依赖制造台账。
- 中国格式仍为实验实现；接收端互通、实际无线封装、法规合规及认证未由本次测试证明。编码说明和待澄清项见 `libraries/opendroneidcn/SOURCE.md`。
- 后续实机验证：空片首刷与重复 USB 刷写；QGC→PX4→CAN→模块预置/激活全链路；断电和断线恢复；OTA 后身份保留；多客户端 DHCP/网页恢复；真实接收机抓包及广播频率。

## 本次验证

验证对象为本次提交的源码；本次没有刷写设备或执行实机验收。

| 检查 | 结果 |
| --- | --- |
| AMOV_RID 固件编译 | 通过；程序 1,436,914 字节，静态全局数据 76,636 字节 |
| 通用 ESP32C3_DEV 编译 | 通过；程序 1,457,188 字节，静态全局数据 77,636 字节 |
| 中国编码库 CTest | 1/1 通过 |
| `tests/` 主机测试 | 运行 50 项，49 项通过，1 项 NVS 官方生成器集成测试默认跳过 |
| `scripts/tests/` 工具测试 | 22/22 通过 |
| 设备管理协议编解码 | 通过 |
| PX4/QGC MAVLink 方言互通 | 5/5 通过，使用本机两个工程各自的上游 XML 和固定生成器 |
| NVS 官方生成器集成 | 未运行；本机未找到 `nvs_partition_gen.py`，对应上述跳过项 |

本次构建和测试日志保存在本地 `artifacts/handoff-20261023/`，该目录不进入 Git。上述结果不能代替硬件、无线接收端或真实平台验收。

## 清理后的恢复与验证命令

清理范围：本仓库中的 `RemoteIDModule/build*`、生成固件/链接映射、自动生成协议头、ROMFS/git 版本头及 Python 缓存。保留源码、子模块、已安装编译工具、本地签名密钥及历史验证记录。`artifacts/`、工厂输出、编译目录和本地工具均被 Git 忽略，不随源码推送。

已有开发环境中，清理后可在仓库根目录执行：

```sh
make -C RemoteIDModule amov-rid
make -C RemoteIDModule esp32c3dev
PYTHONDONTWRITEBYTECODE=1 bash scripts/test_cn.sh
python3 protocol/amov_device_bridge/test_protocol.py
```

AMOV 构建会自动重建协议头；如果仅运行主机测试，先运行 `PATH="$HOME/.local/bin:$PATH" bash scripts/regen_headers.sh`。全新开发环境先按 [BUILDING.md](../BUILDING.md) 安装工具和初始化子模块。

MAVLink 跨工程测试不在 `scripts/test_cn.sh` 中，需要指定两个工程真实的上游 XML 目录：

```sh
python3 protocol/amov_rid_uom/test_protocol.py \
  --px4-definitions "$PX4_MAVLINK_DEFINITIONS" \
  --qgc-definitions "$QGC_MAVLINK_DEFINITIONS"
```

如两个工程固定了不同版本的生成器，另传 `--pymavlink-root` 和 `--qgc-pymavlink-root`。修改共享协议时也需使用 `--mirror` 检查对应工程的协议副本。

## 给下一次 Codex 对话的起始说明

> 请在 `/home/amovlgf/ArduRemoteID` 的 `codex/rid-device-management` 分支继续开发。先检查 Git 状态，并阅读 `docs/DEVELOPMENT_HANDOFF_CN.md`、`README.md` 和 `BUILDING.md`。当前为 AMOV_RID 1.17，已实现中国格式 Wi-Fi RID、DroneCAN 设备管理、首次工厂身份写入、平台激活状态同步、参数持久化及签名 OTA。工厂身份与激活状态独立，测试和发布使用同一固件。QGC 与 PX4 在另外的仓库；跨工程修改先检查协议副本和各仓库状态。实机全链路和真实平台接入仍需单独确认。编译中间文件已清理，可用 `make -C RemoteIDModule amov-rid` 重新生成。请基于这份基线完成我接下来提出的开发任务。
