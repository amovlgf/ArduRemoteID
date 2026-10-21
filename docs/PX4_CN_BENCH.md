# 无 GNSS 模块时的 PX4 → CAN → RID 台架联调

> AMOV 当前签名激活版本请先阅读 [RID_ACTIVATION.md](RID_ACTIVATION.md)。
> 下文是此前 CN 实验版的历史配置/交付记录。当前 AMOV 固定 CN 模式，
> CN_UAS_ID/CN_UIN/CN_CLASS 改为只读；必须先通过网页上传签名授权包并在地面冷上电生效，
> 再配置 CN_CAN_NODE/CN_WIFI_ID。旧发布包路径不代表本次新构建产物。


本步骤承接已启用的中国实验模式：RID 节点125、`CN_CAN_NODE=1`，
产品码 `TESTCN00000000000001`，实验广播标识 `02000001`。
测试链路为：电脑 USB → PX4 模拟 GNSS / System → 飞控 CAN1 → RID → Wi-Fi。

模拟飞机和遥控站位置均为北纬31°、东经121°、高度100m，速度为0。
这些是假数据，仅用于已确认的封闭台架；保持停桨、未解锁。

## Windows 操作

1. 在 QGC 控制台记录原值，并启用模拟 GNSS 输入：

   ```text
   param show MAV_USEHILGPS
   param set MAV_USEHILGPS 1
   param show MAV_USEHILGPS
   ```

   若参数不存在或不能读回1，停止本步骤并提供 `ver all` 输出。
   该参数允许接收 HIL_GPS，不需要设置 SYS_HITL 或启动整机 HIL。

2. 将 `px4_cn_bench.py` 复制到 Windows 的一个目录（仓库中位于 `scripts/`；测试包内可直接解压得到）。
   在此目录打开 PowerShell，安装依赖并查看串口：

   ```powershell
   py -m pip install pymavlink pyserial
   py .\px4_cn_bench.py --list-ports
   ```

3. 完全退出 QGC，使其释放飞控 USB 串口，也避免其发送的空 System 覆盖测试数据。
   保持整板正常供电和飞控 USB 连接。将下方 `COM7` 替换为实际飞控串口：

   ```powershell
   py .\px4_cn_bench.py --port COM7 --duration 120
   ```

   程序核对 PX4 心跳、未解锁状态、`MAV_USEHILGPS=1`，再开始发送。
   HIL_GPS 使用发现的飞控 MAVLink system ID（它和 CAN 节点号是不同概念）。
   程序不修改参数，不发解锁、航点或运动控制命令；心跳丢失或解锁时停止。

4. 在发送期间，用手机连接 RID Wi-Fi 并查看中国状态区。
   目标为位置 `31 / 121`、UTC持续更新、健康状态 `OK`。
   保存界面和脚本文字输出；不能只凭脚本计数判定 RID 收到数据或射频互通成功。

5. 两分钟后自动停止，也可 Ctrl+C。程序会尽力发送未知/无效样本结束模拟。
   重新打开 QGC，将 `MAV_USEHILGPS` 恢复为步骤1记录的原值。例如原值为0：

   ```text
   param set MAV_USEHILGPS 0
   param show MAV_USEHILGPS
   ```

   随后整板断电（包含USB）重启，清除模拟数据和可能形成的模拟返航点。
   恢复参数仍需手工完成，脚本异常退出也要执行。

## 结果判断与限制

- 没有GNSS时，飞机位置为空、home_position未发布是当前缺少输入的表现。
  仅设置 QGC 的固定遥控站位置，不会生成飞机 GNSS 数据。
- RID 返回 `open_drone_id_arm_status` 表明 RID → PX4 的 CAN 通道有数据；
  不能单独证明 PX4 → RID 的 Location / System 完整、及时。
- 当前本地 PX4 将 System 的2019年起UTC秒数直接保存到 uORB `timestamp`。
  `listener` 把它当成开机微秒显示“seconds ago”，因此该年龄值没有意义。
  用户读到的243074697实际对应北京时间2026-09-14 16:44:57。
- 本地 PX4 的 DroneCAN RID 发送频率是1Hz，而 RID 输入新鲜度阈值为1000ms。
  电脑以5Hz/2Hz输入不会提高这个 CAN 输出频率；调度抖动可能引起状态反复。
  若页面在OK与stale之间切换，记录现象，后续处理发送调度与接收期限，不能当成联调通过。
- 本地 PX4 还存在将MSL高度填入geodetic字段的映射问题。本测试的两种模拟高度均为100m，
  因而无法验证真实GNSS的高度基准转换，需在真实定位测试中单独检查。
- 工具只完成离线验证，尚未连接用户实物运行；本实验封装的射频接收仍需另行验证。

2026-09-14 验证记录：8项单元检查通过；使用虚拟串口和模拟PX4端验证了设备发现、
参数读取、正确的发送方system ID、GPS/System发送与结束时的无效样本。未使用真实飞控。

参考：[PX4 MAV_USEHILGPS](https://docs.px4.io/main/en/advanced_config/parameter_reference.html#MAV_USEHILGPS)、
[MAVLink HIL_GPS](https://mavlink.io/en/messages/common.html#HIL_GPS)。

离线检查：

```bash
python3 scripts/px4_cn_bench.py --dry-run
python3 -m unittest discover -s tests -p test_px4_cn_bench.py -v
```
