# CAN → 中国 RID → Wi-Fi 测试说明

> AMOV 当前签名激活版本请先阅读 [RID_ACTIVATION.md](RID_ACTIVATION.md)。
> 下文是此前 CN 实验版的历史配置/交付记录。当前 AMOV 固定 CN 模式，
> CN_UAS_ID/CN_UIN/CN_CLASS 改为只读；必须先通过网页上传签名授权包并在地面冷上电生效，
> 再配置 CN_CAN_NODE/CN_WIFI_ID。旧发布包路径不代表本次新构建产物。


本版只提供广播载荷、CAN适配及实验Wi-Fi封装。通过软件测试不等于真实设备OTA、无线互通、
导航精度、射频或整机合规已经验证。每次交付的实际结果保存在 `artifacts/cn-can-wifi/TEST_RESULTS.md`。

## 一、无需硬件的验证

在已完成仓库依赖准备与消息头生成的环境运行：

```bash
cd /home/amovlgf/ArduRemoteID
bash scripts/test_cn.sh
```

此命令运行三组测试：

- 中国库：人工黄金字节、必选72字节包/全选78字节包、空登记标志、未知值、边界和坏包。
- 真实CAN适配代码：源节点隔离、冻结与乱序、首次零秒、跨小时与计时器回绕、时间重建、失效恢复。
- OTA状态机/签名检查与独立Python工具：失败不激活、失败后可重试、签名/板号/容量及PCAP格式解析。

需要额外检查内存/整数转换错误时：

```bash
CN_TEST_SANITIZE=1 ASAN_OPTIONS=detect_leaks=0 python3 tests/test_cn_data.py
cmake -S libraries/opendroneidcn -B /tmp/arduremoteid-cn-sanitize -DCN46750_SANITIZE=ON
cmake --build /tmp/arduremoteid-cn-sanitize
(cd /tmp/arduremoteid-cn-sanitize && ASAN_OPTIONS=detect_leaks=0 ctest --output-on-failure)
```

仅预览模拟输入，不访问CAN：

```bash
python3 scripts/cn_dronecan_test.py --dry-run --start-unix-ms 1735693199500
```

## 二、CAN台架与Wi-Fi抓包

先按 [OTA及配置教程](CN_CAN_WIFI_OTA.md) 设置测试节点42、设备节点示例125及中国模式。
抓包使用另外一块支持2.4GHz监听模式的Wi-Fi网卡，固定到模块管理AP使用的频道。
保留管理连接所用的网卡，避免切换监听模式后失去升级连接。

可以用Wireshark保存PCAP/PCAPNG，或在已经建立的监听接口 `mon0` 上：

```bash
sudo tcpdump -i mon0 -s 0 -w /tmp/cn-rid.pcap
```

完成抓包后执行：

```bash
python3 scripts/cn_rid_decode.py --pcap /tmp/cn-rid.pcap --wifi-id 02000001
```

解析器只匹配指定vendor IE，不会在整帧里盲搜0xFF。实验IE布局为：

```
DD | IE长度 | 02 00 00 01 | FF 20 42 DF E5 FE | 66字节必选内容
```

`02000001`是本实验约定；载荷中的 `FF 20` 是当前V1.0实现。IE外层不是已证明的官方约定。
解码器也支持 `--hex HEX` 和 `--bin packet.bin`，不依赖Scapy；发送CAN数据需要python-dronecan。

| 场景 | 操作 | 预期 |
| --- | --- | --- |
| 正常输入 | `--duration 60` | 17个必选项齐全；66字节内容/72字节载荷；样例位置31/121，高程100m，航迹90°，地速12.3m/s；时间精度0 |
| 位置断流 | `--scenario drop-location --fault-after 5 --duration 15` | 停更后不把旧样本伪装为更新；失效状态4；重启正常流后恢复 |
| System断流 | `--scenario drop-system --fault-after 5 --duration 15` | 遥控站/完整时间变未知并报告故障；重复整数秒不应无限维持有效性 |
| 未知位置 | `--scenario no-position --fault-after 5 --duration 15` | 坐标NULL语义保留，解码输出null，不显示真实0°位置 |
| 未知时间 | `--scenario no-time --fault-after 5 --duration 15` | 时间编码0，故障；不使用模块开机时长冒充Unix时间 |
| 紧急状态 | `--scenario emergency --fault-after 5 --duration 15` | 新鲜样本状态3；随后停止流，超过时效后状态5 |
| 错误节点 | 设备仍选42，仅运行发送器 `--node-id 43` | 不接受43的位置和时间；正确节点恢复后正常 |
| 空登记标志 | CAN设 `CN_UIN=` 后运行正常输入 | 第002项仍存在，八字节NULL，不拒绝整个包 |
| 管理共存 | 正常广播同时刷新网页状态 | 页面可用，完整载荷更新/发送最大间隔≤1秒；OTA上传期不属于运行验收 |
| 慢网页客户端 | 保持一个不完整HTTP请求，或模拟很慢的网页读取，同时持续CAN输入和抓包 | 记录最大样本/广播间隔；当前同步网页服务可能超过1秒，这项尚不能判定通过 |

上述场景命令都接在 `python3 scripts/cn_dronecan_test.py can0 --node-id 42` 后。
硬件AP会以自身Beacon周期重复同一份载荷；时序验收要同时看抓包时间和载荷样本时间。
区分接收端丢包、重复Beacon和发送端未更新，不只计算平均Hz。
500ms软件调度周期不是最坏延迟保证。慢网页请求可能阻塞同一循环中的CAN处理和广播更新；
发布正式产品前需隔离网络处理，并通过上述并发场景。

进一步用已知轨迹移动平台、实际飞控和独立接收器比对字段；按标准8.2.1.9验证真实精度需要
时空基准、导航模拟条件及各等级不少于10分钟的统计。模拟器只能证明编码/映射。

## 三、实机OTA验收矩阵

所有场景在地面维护、稳定供电的台架上做。涉及断电的测试使用可恢复测试板，先验证恢复方法。

| 场景 | 必须记录 |
| --- | --- |
| 上次实际固件→本版签名包 | 旧/新版本、板号、公钥指纹、镜像SHA256、参数前后对照 |
| 本版→同一本版再次OTA | app0/app1切换、重启成功、配置保留，证明下一次更新路径可用 |
| 中国参数缺失、无CAN时OTA | 故障提示存在，升级网页仍可访问并完成下一次OTA |
| 错误板号/签名、损坏、截断、超大包 | 不能启动坏镜像；不用有线重刷即可重传正确包 |
| 上传开始/中途/验签前断网 | 当前程序可继续运行，下一次上传可重试 |
| 上传中断电 | 重启后的实际镜像和参数仍可用；启动切换临界点单独记录结果 |
| 本版→已签名旧版恢复包 | 回到1.14，管理连接和公钥可用；记录新增参数仍保留 |

旧bootloader未启用自动回滚，因此不能预设新应用崩溃后会自动回到旧槽。
上传百分比、HTTP 200均不是单独的成功证据；以重启后实际版本和运行槽为准。

## 四、重新构建和签名

保持ESP32 Arduino core 2.0.3和原分区布局：

```bash
cd /home/amovlgf/ArduRemoteID/RemoteIDModule
make amov-rid
cd ..
cp RemoteIDModule/ArduRemoteID_AMOV_RID_OTA.bin /tmp/amov-cn-candidate.bin
python3 scripts/sign_fw.py /tmp/amov-cn-candidate.bin \
  artifacts/cn-can-wifi/keys/amov_cn_private_key.dat 13
python3 scripts/check_ota.py /tmp/amov-cn-candidate.bin \
  --public-key artifacts/cn-can-wifi/keys/amov_cn_public_key.dat --board-id 13
```

签名脚本修改目标文件，所以先复制原应用；不要对已经签名的包重复签名。
源代码变更后应重跑主机测试、构建和包检查，实际硬件变化则补齐相应实机测试。

## 五、下一步验收与上游提交

| 优先级 | 工作 | 完成依据 |
| --- | --- | --- |
| P0 | 对实际旧设备完成首次OTA和再次OTA | 使用其真实公钥/锁级，重启后版本、运行槽和参数均正确 |
| P0 | 确认正式Wi-Fi封装和接收端约定 | 获得有出处的标识/帧格式，并与独立接收器抓包互通；实验标识不作为通过依据 |
| P1 | 将同步网页处理与CAN/广播调度隔离 | 正常浏览与慢客户端条件下，实测完整数据更新及广播最大间隔≤1秒 |
| P1 | 完成断流、时钟、掉电和恢复矩阵 | 保存原始CAN日志、PCAP和OTA前后状态，逐项记录通过/失败 |
| P2 | 拆分上游PR | 先提交可独立验证的中国编码库修复、OTA健壮性修复，再提交默认关闭的CAN中国模式 |

发布级中国模式应等P0/P1证据齐全后提出；编码器或OTA修复可先单独评审。
本次只在本地准备实现和测试，尚未向官方仓库发起PR。
