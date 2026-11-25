# 节点固件：单节点模式 A 与低功耗待机

在项目根目录运行 `make -C "firmware" sensor`。`make -C "firmware" ROLE=sensor LOG=1`
启用 UART1 每秒累计统计；发布版默认不打印。构建参数和下载方法见根目录说明。

当前节点固件为 **v1.7**：发布版 `firmware/build/sensor/sensor.hex`，日志版
`firmware/build/sensor-debug/sensor.hex`。两目录中保留的历史版本 HEX
（`sensor-v1.4.hex`、`sensor-v1.3.hex`、`sensor-v1.1.hex`）仅作回退留档。
远程启停需配合 **v1.5 网关**。在网页观测台选中节点后，点击「恢复采集」或「停止采集」。操作步骤见 [网页观测台说明](../../web/README.md)，固件行为见 [远程启停与低功耗待机方案](../../docs/13_远程启停与低功耗待机方案.md)。

## 低功耗待机与空闲降耗

- **自动休眠与晃动唤醒（v1.6）**：采集中的三轴读数连续 10 秒未偏离参考值达到约 31 mg 时，节点先排队发送 `STATUS`（`PAUSED=1, mode=2`）。发送队列等待网关序号确认，遇到无线异常最多等 1 秒。
  节点随后关闭周期心跳与 RTC 唤醒。BMI323 以 50 Hz 低功耗模式运行 Any-motion，唤醒参数初值约 94 mg、40 ms。INT1 经 PA4 唤醒 CH592F，同一 `boot_id` 下开启新的采样段并发送采集态 `STATUS`。
  v1.7 的 GPIO 休眠保留 Flash 供电，SRAM 使用正常保持电压，唤醒采用长延时。日志版在启动行打印 `reset`，其中 `2` 表示看门狗复位，`5` 表示低功耗唤醒触发全局复位。
  接板时可先持续晃动超过 10 秒，确认仍采集。再静置 10 秒，确认网关收到休眠状态并且节点停止心跳。保持静置一段时间后晃动，检查状态及样本恢复、`boot_id` 不变，并测量休眠电流。
- **默认上电待机**：`SENSOR_AUTO_START` 默认为 `0`，节点上电后将 BMI323 置于 Suspend，
  禁用 PA4 中断。允许真睡眠且射频空闲时暂停 20 ms `POLL_EVT`。节点每 2 s 发送一次心跳 `STATUS`（`flags |= PROTO_FLAG_PAUSED`，
  `bmi_temp_raw = 0x8000`）。心跳在收到合法 ACK 后即释放；无网关时最多尝试后放弃本次心跳，
  不陷入退避死循环。编译传 `SENSOR_AUTO_START=1` 可切回上电直采。
- **待机真睡眠**：`SENSOR_STANDBY_SLEEP` 默认为 `1`。待机态下发送环为空、
  无射频调度、不在 `IDENTIFY` 闪烁且距下次 RTC 唤醒时间充足时，关闭看门狗后进入
  节点专用的真睡眠入口（保留 RAM24K + RAM2K + EXTEND，由 RTC 提前 `WAKE_UP_RTC_MAX_TIME` 唤醒），
  唤醒后恢复 60 MHz 时钟、重载看门狗并标记射频重初始化。编译传 `SENSOR_STANDBY_SLEEP=0` 可退回纯浅 Idle。
- **采集态浅 Idle**：收到 `STREAM_CTRL(1)` 切入采集态后，TMOS 无待办任务时进入浅 Idle（`_WFE`），
  保留 60 MHz 外设时钟、UART1 和射频配置。PA4 水位中断可提前唤醒；20 ms 兜底采样、串口轮询、
  喂狗和每秒 STATUS 照常调度。临近或已过比较点、PA4 已挂起或本轮调度后发生中断时跳过 Idle。
  SDK 的 `HAL_SLEEP` 开关只用于注册节点回调，本工程不链接厂家深睡眠实现。
- **停采排空与超时兜底**：收到 `STREAM_CTRL(0)` 后先封包保存半包并 Suspend BMI323，
  立即发一帧带 `PAUSED` 与 `cmd_ack` 的 `STATUS`；在浅 Idle 下排空残余队列，若断链超过 1 s
  仍未排空则丢弃残余并直接进入待机真睡眠。

DATA 和 STATUS 入队共用射频唤发入口；已有重试定时器时只入队，不提前发射或延后该定时器。
默认 3200 Hz、FIFO 时标、全量上传、协议和下行命令保持不变。`DCDC_ENABLE` 默认 1，
由构建参数控制。关闭 Idle/Sleep 的构建可独立生成：
`make -C "firmware" ROLE=sensor SENSOR_IDLE=0 BUILD=build/sensor-no-idle`。

浅 Idle、待机真睡眠和射频退避已通过宿主回归及链接检查；历史 v1.4 浅 Idle 实机电流见
[接板验收](../../docs/11_软件实现与接板验收.md#节点-v14-实机电流)，v1.5 待机电流尚待实板验收。

节点显式关闭 `BLE_CALIBRATION_ENABLE` 和 `TEM_SAMPLE`，避免 SDK 在待机调度额外校准，也避免采集期间执行 Flash 校准。接板时应同时观察温变下的无线首发成功率与重传。样本温度仍来自 BMI323。

## 接线与首次接板

- BMI323：CSB PA12、SCK PA13、SDI PA14、SDO PA15；SPI mode 0、7.5 MHz。
- INT1 PA4，外部 32.768 kHz 晶振 PA10/PA11，UART1 TX PB13/RX PB12，115200 8N1。
  其余未用 PA/PB 引脚上电设为上拉输入（避开 PA10/PA11 晶振与 PB14/PB15 SWD），降低睡眠漏电。
- PA8 状态灯在待机和休眠时熄灭，采集开始时点亮，随后每秒翻转一次。默认低电平点亮（PA8 输出高电平时熄灭），高电平点亮的板型使用 `SENSOR_LED_ACTIVE_LOW=0` 构建。需要原 UART1 PA9 TX / PA8 RX 的板型可使用 `SENSOR_PA8_LED=0` 构建。
- PA5 为 RF 交换高电平测试点。PA6 采集态每秒翻转，待机态拉低（`IDENTIFY` 除外）。
- 节点默认 ID 1，3.2 kHz、±16 g、800 Hz 带宽，2450 MHz、1M PHY、0 dBm。
- `SENSOR_RING_SLOTS` 默认 **24**。固件保留 6 KiB BLE 堆和 2 KiB 栈，发布版和日志版的剩余 RAM 分别为 **696 B** 和 **676 B**。
- 加载 DataFlash 配置和处理 `SET FREQ` 时均拒绝 BLE 广播中心频点 2402、2426、2480 MHz。默认 2450 MHz 可用。

先确认芯片 ID、静止 Z 约 2048 LSB、每秒样本增量约 3200，再进行无线链路测试。
`irq` 为累计水位中断计数，目标约 91 次/s；`first=成功数/尝试数` 表示 DATA/STATUS
首发成功比例，不包含 POLL；`tx` 为全部合法 ACK 物理交换数，包含 POLL。
STATUS 的 `ack_rssi_avg` 为最近 8 次合法 ACK 的整数均值（不足 8 次按已有次数，
除法向零截断，尚无合法 ACK 时为 0）。CRC、类型或长度错误的 ACK 不更新均值。
`fin` 表示实际收到的 TX_FINISH 回调数。AUTO 模式不产生该回调时，sync 使用
RF_Tx 前 RTC 时刻，时间模型需要以逻辑分析仪测得的固定延迟标定。

## 可靠性与恢复

采集由 PA4 上升沿和 20 ms 兜底驱动，FIFO 每次最多三轮、每轮 48 帧，
PA4 仍高则重置采集事件。每帧的 FIFO 时间与 RTC 锚点用于检测缺口和时间段变化，
积压数据使用完整无符号 16 位时间差回算时间。时间参考在 FIFO burst 完成后读取，保证
`SENSOR_TIME_0` 不早于本批最新样本；否则新样本可能被误算为早了一个 2.56 s 回绕周期，
造成假分段和每次 8192 样本的假缺口。该顺序修复从节点 v1.1 起生效，STATUS 会报告版本。
1 s 无新样本时重新初始化 IMU。

超过 2 s 未处理采样且 FIFO 已满时，旧 FIFO 的 16 位传感器时间可能已跨越多圈，
无法精确恢复未知丢失量。此时清除旧 FIFO、重新初始化 IMU、开启新 segment，
下一包置 `GAP|NEW_SEGMENT|FIFO_FULL`，UART `unknown` 累计此类异常次数。
`sample_idx` 只继续累计已知样本和可确定的丢样，不插入猜测的未知样本数；
PC 必须把此段边界记为未知损失，不能作为精确缺样区间验收通过。
正常 20 ms 兜底、150 ms FIFO 暂停和仅射频断链不进入此边界。

数据包每包最多 35 个连续样本。当前默认 24 个静态槽包含待发和等待确认的包；仅收到
确认表中的本节点序号才释放，硬件 ACK 本身不会释放数据。满队列丢弃最老包，
STATUS 累计报告丢包/丢样数。启动先做两次 POLL（握手期间忽略 ACK 命令区），配合网关按 boot_id 清除旧确认表与旧待发命令，
防止重启后旧确认项或旧命令误作用于新会话。POLL 不消耗 seq，也不更新同步配对字段。
24 槽全用于 DATA 时可缓存 840 点，在 3.2 kHz 下约 0.2625 s。STATUS 也占槽，实际缓存时间略短。

节点使用硬件看门狗，运行 `POLL_EVT` 时每 20 ms 喂狗。CHIP_ID 连续五次错误或 1 s 无新样本
会重启 BMI323、开启新 segment，sample_idx 不归零。看门狗整机复位则 boot_id 改变。

DataFlash 分配：0x0000 配置；0x0100、0x0200 两页追加 boot 计数和 CRC，
交替擦页时保留另一页的有效记录。boot 写入校验失败会停止启动，不复用会话。

UART1 配置命令：`SET NODE 1`、`SET FREQ 2450000`、`SAVE`，每条以换行结束。
SET 仅暂存，SAVE 写配置并复位。待机真睡眠期间 20 ms 轮询暂停，UART 改参请在采集态操作或使用 `SENSOR_STANDBY_SLEEP=0` 台架构建。未提供模式 B、漫游或太阳能策略。
网关可排队的下行支持加速度配置、开始/停止、功率和 IDENTIFY（PA6 闪烁输出）。
节点可幂等接纳模式 A，网关当前不排队模式切换；模式 B 和检测参数命令不确认。
阶段一保留 REBOOT（code 0x05）但不执行、不回显 cmd_ack；网关拒绝排队并返回
CMD_RESP result=2（不支持）。当前尚未实现复位前的确认闭环，直接执行会造成下一 boot 重复复位。需要复位时使用物理复位，
或 UART SAVE（保存当前暂存配置并复位）。

## 本机与硬件验收

本机纯 C 测试源码位于 `tests/sensor/`（共 7 套）：`test_sensor.c` 覆盖时间回绕、FIFO 缺样、
新段、长暂停、RTC 锚点、队列满、误确认、确认超时和 seq 回绕；
`test_radio.c` 覆盖启动握手（含跳过旧命令）、待机心跳不死循环、停采排空超时、持续入队不打断退避及到期恢复发送；
`test_idle.c` 覆盖 RTC 回绕、临界截止点、PA4 竞态及 真睡眠门禁、RTC 触发竞态与看门狗切换；
`test_startup.c` 覆盖启动看门狗使能顺序；
`test_boot.c` 覆盖 boot 页切换、
破损记录及写入/擦除中断电恢复；`test_command.c` 覆盖命令幂等、`STREAM_CTRL` 状态切换和 REBOOT 重复
下发不会复位；`test_sampler.c` 直接运行采集器，覆盖读取 FIFO 水位期间新增样本、
真实缺样、传感器时间及 RTC 回绕，防止假 2.56 s 锚点和假 8192 样本缺口。
射频活动窗口从 RF_Tx 到 RF_Shut 全程在 RAM 中。
以下项目需接板执行：1 h 无采集 GAP、0.2 s/2 s 断链、150 ms FIFO 暂停、
TX_FINISH 行为、重新挂接间隔、24 h 完整性及沿线 10 圈覆盖测试。
`SENSOR_TEST_FIFO_PAUSE_MS=150` 会在启动 5 s 后只暂停一次 FIFO 处理，
不阻塞 TMOS 或喂狗，用于验证溢出缺样。发布版不定义该宏。

寄存器值已按 BST-BMI323-DS000-13 的 ACC_CONF、FIFO_CONF、IO_INT_CTRL、
INT_CONF、INT_MAP2 描述核对；初始化读回 ACC_CONF 与 ERR_REG。
