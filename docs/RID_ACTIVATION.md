# RID 身份与离线激活（AMOV 首版）

本实现仅对 AMOV 板强制签名激活，并固定使用 CN 业务路径。原有普通 CN 参数不能配置或替换有效整机身份。其他上游板保留原业务策略。本地编译、主机测试不等于无线国标正式互通认证；现有实验性 Wi-Fi 封装保留，正式接收端互通须另行实测。

## 构建和测试

在仓库根目录：

```sh
bash scripts/test_cn.sh
make -C RemoteIDModule amov-rid
make -C RemoteIDModule amov-rid-test
```

生产配置产物：

- `RemoteIDModule/ArduRemoteID-AMOV_RID.bin`：从 0x0 使用的全量镜像。
- `RemoteIDModule/ArduRemoteID_AMOV_RID_OTA.bin`：应用分区镜像，仍受原 OTA 签名校验规则约束。

测试产物名称含 `AMOV_RID_TEST`，对应目录为 `build-AMOV_RID_TEST`。测试构建只信任公开测试根，网页明显显示“测试固件”。测试密钥不能用于生产；生产默认没有配置公司公钥，因此拒绝所有授权包，不能正常业务广播。仓库没有生产私钥。

生产公钥由公司在外部头文件配置，例如头文件定义：

```c
// KEY_ID 须在公司签发端唯一；65535 保留给测试。
// 这里的 bytes 必须来自公司的真实 EdDSA-BLAKE2b 公钥。
#define RID_ACTIVATION_KEYS {KEY_ID, { /* 32 public-key bytes */ }}
```

随后用 `EXTRA_BUILD_FLAGS='-DRID_ACTIVATION_TRUST_HEADER=\"/absolute/company_activation_public.h\"'` 构建 `amov-rid`。此文件仅含公钥，构建端不接收生产私钥。可列出多个不同 key ID 的公钥支持轮换；保留仍需验证的旧授权对应公钥。激活信任根与 `PUBLIC_KEY1..5`、OTA、SecureCommand 完全独立，不采用 Transport 的“没有公钥则通过”逻辑。

全量镜像烧写会覆盖 NVS 地址范围，不能当作保留授权升级。应用 OTA 不擦除授权 NVS；测试激活密钥不授权 OTA 固件。仓库此次只构建文件，未操作设备、烧写 eFuse 或提交/推送代码。

## 用户操作与结果

AMOV 固件默认管理 Wi-Fi 名称为 `B950_RID`，密码为 `987654321`，
连接后访问 `http://192.168.4.1/`。已保存的 `WIFI_SSID` / `WIFI_PASSWORD`
优先于编译默认值；保留 NVS 的升级不会自动覆盖已有 Wi-Fi 配置。

1. 连接 RID 管理 Wi-Fi，在网页读取设备 UID。
2. 公司签发服务按 UID、整机身份和递增 revision 签发二进制授权包。签发服务不在本仓库；下文是实现接口。
3. 网页上传 `.act` 文件。HTTP 202 只表示接收，查询 `/ajax/activation.json`，直到 `busy=false`。只有 `PENDING_REBOOT` 表示验签和持久化读回完成。
4. 在地面安全状态人工断电后上电，重新查询 `state=1`、有效 revision、身份及 hash。首次上传后、冷启动前，仍是未激活；更新上传后，旧身份继续工作。
5. 普通软件复位、看门狗、掉电检测复位、OTA 重启都不应用 pending。网页没有激活自动重启入口，也不更换运行中身份。

激活后还需按实际总线配置唯一飞控节点及无线封装标识；已有工具仍可设置这两个非身份参数：

```sh
python3 scripts/cn_dronecan_test.py can0 --node-id 42 --target-node 125 --no-stream \
  --set CN_CAN_NODE=1 --set CN_WIFI_ID=02000001
```

其中 RID 节点125、飞控节点1和 `02000001` 仅是封闭台架样例。无线正式互通标识需按已验证方案配置。
不要再通过该工具设置 CN_UAS_ID/CN_UIN/CN_CLASS/RID_PROFILE；这些旧版步骤在当前 AMOV 上被拒绝。

仅 `esp_reset_reason()==ESP_RST_POWERON` 时应用 pending。AMOV 保留 brownout 检测。硬件无法区分人工完全断电与意外完全断电，因此应在地面进行上传和冷上电验收；这不是飞机真实未解锁状态的证明。本版不使用 DroneCAN 的 ESC ArmingStatus 作为维护许可，不新增维护 CAN 服务。

普通 `TO_DEFAULTS` 仅清除 `storage` 配置命名空间和 pending 授权，保留 active A/B。不会清除或降级有效整机身份。生产私钥泄漏、物理整片 Flash 重写、改固件等抵抗能力不属于此次离线授权实现。

## 固定授权包

长度恰为 **204 字节**；HTTP 文件缓冲同样只有 204 字节，超长、短包、中断不提交。多字节整数均小端，保留字段为零。前 140 字节被签名，后 64 字节为签名。

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 8 | ASCII `AMOVACT1`（签名域） |
| 8 | 1 | schema = 1 |
| 9 | 1 | algorithm = 1 |
| 10 | 2 | 签发 key ID |
| 12 | 2 | board ID，AMOV = 13 |
| 14 | 2 | 权限 = 1，仅 CN 业务 |
| 16 | 4 | 非零 revision |
| 20 | 6 | RID 出厂 MAC 原始 6 字节 |
| 26 | 1 | 飞行器类别 0..4 |
| 27 | 1 | 完整登记号长度 0..64 |
| 28 | 1 | UIN 长度 0 或 8 |
| 29 | 3 | 保留零 |
| 32 | 16 | 非全零签发 token |
| 48 | 20 | 产品识别码，恰为 20 字节 |
| 68 | 64 | 完整登记号，尾部补零 |
| 132 | 8 | UIN，尾部补零 |
| 140 | 64 | EdDSA 签名 |

algorithm 1 是仓库 Monocypher 3.1.3 使用的 **Curve25519 + BLAKE2b EdDSA**，不是常见 SHA-512 Ed25519，不能混用签名库默认算法。直接对全部前 140 字节调用 `crypto_sign/crypto_check`，不做额外预哈希。

身份字符串只允许 ASCII 33..126。完整登记号未知可空，不能用 UIN 补造；完整登记号非空时至少 8 字节，UIN 必须明确提供其最后 8 字节。RID_UID 绑定模块，首版不绑定 FC_UID。签发端须对同一 UID 分配唯一、递增 revision，并保存原始签发包；相同 revision 重新随机生成 token 是冲突，不是幂等重试。

测试工具（仅含公开测试种子 `bytes(range(32))`）：

```sh
python3 scripts/activation_package.py sign-test \
  --uid AABBCCDDEEFF --product AMOV0123456789012345 \
  --registration REG-12345678 --uin 12345678 --class 2 \
  --revision 1 --output /tmp/amov-test.act
```

依赖 `pymonocypher==3.1.3.2`。key ID 65535，测试公钥为
`F65333FA6303B6A23DEFD7DE2AF8AA461CB047CCBF12D4EDD29EF3B1EBA6706B`。
工具不接收生产私钥，拒绝覆盖已有文件。

## 持久化与状态

`rid_activation` 独立 NVS 命名空间有 `active_a`、`active_b`、`pending` 三个 blob。每个记录保留完整已签名包。启动验证全部 active 记录，取最高有效 revision；升级写另一槽、提交并读回后才启用，再清 pending。单槽损坏可回退另一有效槽。完整写入后、清 pending 前断电可在下次启动幂等恢复。

暂存失败不改变当前有效身份。旧 revision 拒绝，同 revision 同字节文件幂等，同 revision 不同字节拒绝。两个 active 槽同 revision 却内容不同属于签发/存储冲突，关闭业务并需检修；普通恢复默认不会抹除该证据。

未激活时保留 Wi-Fi 管理、CAN 心跳和 GetSet，停发 CN 业务 IE，ArmStatus 报 `RID not activated`。授权记录无效报 `RID activation invalid`。已有有效授权且新上传失败，不影响当前广播；其后 CN 数据、无线故障仍走现有健康策略。

网页服务是单独 FreeRTOS 任务。主循环每 500ms 投递长度有界的状态 JSON，队列长度 1；网络任务所有上传与响应均不在 CAN 主循环执行。上传请求队列长度 1，处理未完成时拒绝新请求。验签/NVS 事务在该任务处理，网页读身份使用加锁副本。网页任务栈为 12288 字节，两个8192字节状态缓冲位于静态区，状态队列另占8192字节堆空间，均不放在任务栈上；授权队列只存204字节。状态API额外给出 web_stack_min_free_bytes 和 free_heap_bytes，供实机压力验收。Flash 提交仍可能造成芯片级短暂暂停，实机需要量测 CAN/广播时序。

## 只读身份 GetSet

所有键不超过 16 字节，仅按名称读取；写请求返回当前值，不修改身份。旧 `CN_UAS_ID/CN_UIN/CN_CLASS` 写入在 AMOV 被拒绝。旧参数读回反映有效签名身份。

| 键 | 类型 | 含义 |
| --- | --- | --- |
| RID_ID_SCHEMA | integer | 1 |
| RID_ID_REV | integer | 有效 revision；未激活为 0 |
| RID_BOOT_ID | integer | 每次启动非零随机 uint32 |
| RID_ACT_STATE | integer | 0 未激活 / 1 已激活 / 2 无效记录 |
| RID_UAS_ID | string | 有效身份的 20 字节产品码，未激活为空 |
| RID_REG_ID | string | 完整登记号 0..64 字节 |
| RID_UIN | string | 0 或 8 字节 |
| RID_CLASS | integer | 0..4 |
| RID_UID | string | 出厂 UID，12 位大写 hex |
| RID_ID_HASH | string | 有效身份 BLAKE2b-256 大写 hex64；未激活空 |
| RID_PENDING | integer | 待生效 revision，0 表示没有 |
| RID_ACT_ERR | integer | 最近处理结果，见下表 |

读取方先后读取 `BOOT_ID + ID_REV`，中间读字段、schema 和 UID，首尾一致且校验通过才发布整包快照。不能用 pending 版本替代有效 revision。

hash 是下列 **115 字节**的 BLAKE2b-256：
`"RID-ID-V1\0"`（10字节） + revision LE32 + UID原始6字节 + class1字节 +
product20字节 + registration_len1字节 + registration补零到64字节 +
uin_len1字节 + uin补零到8字节。
不含 boot、pending、状态、token、key ID 或签名，所以重启和暂存不改变当前身份 hash。

| RID_ACT_ERR | 含义 |
| --- | --- |
| 0 | OK |
| 1 | PENDING_REBOOT，已持久化、待人工冷启动 |
| 2 | ALREADY_ACTIVE，原包重复提交 |
| 3 | INVALID_FORMAT |
| 4 | NO_TRUST |
| 5 | INVALID_SIGNATURE |
| 6 | WRONG_DEVICE（含错误 board） |
| 7 | INVALID_IDENTITY |
| 8 | OLD_REVISION |
| 9 | VERSION_CONFLICT |
| 10 | STORAGE_ERROR |
| 11 | BUSY（保留错误码，HTTP忙返回503） |

## 仍需用户实机验收

- 未激活能读 UID/boot/schema，业务广播关闭，PX4 可见预解锁失败。
- 正确包暂存、错误设备/错误签名/超长/截断拒绝，网页断连后可查询 durable 结果。
- 上传期间 CAN 心跳/ArmStatus 继续，广播无超期；并量测 NVS提交时长。
- 首次和升级的冷启动生效；软件、看门狗和 brownout 复位不生效；核对真实复位原因。
- NVS写入/掉电/单槽损坏的恢复和普通恢复默认保留有效授权。
- 用独立接收端比对产品码、UIN、飞行位置/时间与无线帧，验证正式 CN 无线互通；当前主机 golden codec 测试不能替代此项。
