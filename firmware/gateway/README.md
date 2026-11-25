# 单节点采集网关

把网关通过 USB 接到 PC，打开 CDC 串口并置 DTR 后，第一帧为 `HELLO`；随后收到原样转发的
`RF_RX`，每秒收到 `GW_STATUS`。串口波特率可设 115200，实际传输速度由 USB 全速 Bulk 决定。
网关单独连接 USB 时就应出现 CDC 串口；节点、BMI323 及其 INT1 接线不参与 USB 枚举。

## 构建与接线

在项目根目录执行：

```sh
make -C "firmware" ROLE=gateway all
```

产物为 `firmware/build/gateway/gateway.{elf,hex,bin,map}`。烧录前检查板子角色；本工程不自动烧录。
加 `LOG=1` 生成独立 `gateway-debug`，在RF启动前输出UART1启动标识及初始化阶段；运行期统计通过
`GW_STATUS` 上报，不在收包期间阻塞打印。网关日志不引入stdio，队列和堆容量与发布版一致。

| 用途 | CH592F 引脚 |
|---|---|
| USB D− / D+ | PB10 / PB11 |
| UART1 调试 TX | PA9，115200 |
| 外部 32.768 kHz 晶振 | PA10 / PA11 |
| RF 回调测点 | PA5，每次回调期间为高 |
| RX 重挂测点 | PA6，`RF_Rx` 调用期间为高 |

默认 `gw_id=1`，2450 MHz、1M PHY、0 dBm、AUTO、白化开启，接入地址 `0x71764129`、CRC 初值
`0x123456`。这些参数与节点一致。`APP_GW_ID` 可用构建预处理宏覆盖，USB 序列号也按该 ID 生成（`BMIGW001` 至 `BMIGW254`）。运行期 `SET_GW_CFG` 尚无
完整线格式，明确返回不支持，不写 DataFlash。

## 队列与确认的实际边界

`GW_QUEUE_SLOTS=8`，不是方案估计的16槽：官方链接脚本只开放26 KiB RAM，需要同时保留6 KiB
厂家协议栈堆、完整RAM运行路径与2 KiB栈。队列使用单调计数器，可物理保存8包；正常重挂要求
至少2个空槽，通常在积压7包时停止接收。按3200 Hz、35样本/包计算，这约为76.6 ms；STATUS、POLL
也占队列，实际缓存时间更短。预留 2 KiB 栈后，发布版余 448 B，日志版余 440 B。

- 只转发合法的 v1 DATA、STATUS、POLL；CRC或格式错误不进入确认表。
- 只有已放入队列的 DATA/STATUS 才加入最近8条确认表，最新项在前。POLL不占用序号，也不被确认。
- 同一节点出现新 `boot_id` 时清除其旧确认项与旧待发命令（v1.5 起），包括收到新 boot 的 POLL 时。节点启动须先完成两次 POLL 握手。
- ACK双缓冲只在RF停止后构造；接收期间不改活动缓冲。
- CDC发送拆为≤64字节。整帧最后一次IN传输完成后才弹出队列；USB复位或关闭串口会保留尚未完整
  交付的RF包，重连后先发HELLO，再完整重发。PC仍需去重。
- USB `CLEAR_FEATURE` 复位对应的 EP1 IN 忙标志或 OUT 游标。EP1 OUT 每包最多向解析器交付 64 B。
- 队列不足时不重新挂接RF。节点拿不到硬件ACK，会从自身发送缓存重传。
- 网关掉电仍会丢失已确认但尚未交付PC的RAM包；本阶段没有持久化网关队列。

## 主机命令

帧头、长度、CRC使用 `docs/03_网关与上位机.md` 的格式。TIME_REQ、GET_INFO与QUEUE_DOWNLINK已实现。
网关入站解析缓冲为 96 B（最大负载 88 B）。超长帧计入 `length_errors`，随后重新搜索帧头。
HELLO负载固定为8字节：`gw_id/u8, fw_major/u8, fw_minor/u8, frequency_khz/u32, phy/u8`，当前PHY为1。

`CMD_RESP.result`：`0=已排队`、`1=参数格式错误`、`2=不支持`、`3=下行命令槽已满`。
排队成功不表示节点已执行；命令会继续附在 ACK 中，直到合法 DATA 的 `cmd_ack` 或 STATUS 的 `last_cmd_id`
匹配。命令 id 使用 1~255，每个节点同时保留一个待执行命令槽；自 **v1.5** 起，对同一 `node_id`
下发新命令会直接覆写其未确认的旧待发槽并返回 `0`，仅 8 个槽被不同节点占满时返回 `3`。
命令入队成功后立即调用 `gateway_radio_refresh_ack()` 触发重挂，刷新硬件 ACK 缓冲（< 1 ms 生效）。

支持SET_ACC_CFG（ODR码12~14、range码0~3、BW码0~1）、STREAM_CTRL、SET_TX_POWER、IDENTIFY。
SET_TX_POWER仅接纳SDK实际功率码（0 dBm=`0x15`、+4 dBm=`0x3B`、+9 dBm=`0xBF`等）。
REBOOT明确拒绝，避免复位前无法报告cmd_ack而导致反复重启；需重启时使用物理复位。
模式B相关命令与SET_CH_LIST不在本阶段实现。

## RF监听期间从RAM运行

主循环、USB ISR及控制请求链、描述符、队列/打包/CRC/时间换算均放RAM。项目级`firmware/Link.ld`
按网关角色把厂家 AUTO RX（含硬件 ACK）和 TMOS 射频处理闭包放入 RAM，SDK文件本身不改。不要在监听期间加入PRINT、
Flash持久化或普通Flash任务。

网关关闭HAL周期校准任务与温度采样回调；每120秒先`RF_Shut`，在受控停收窗口调用`BLE_RegInit`，
然后重新挂接。挂接后1秒没有回调也会停止并重挂。硬件看门狗由正常任务循环喂狗。

`rx_us`为60 MHz SysTick换算的u32微秒，约71.6分钟回绕。读取高/低字稳定后用32位整数分解换算，
不在RAM运行期间调用64位除法库。PC负责展开回绕。

## 验证与接板验收

在项目根目录执行：

```sh
sh "tests/gateway/run.sh"
```

宿主测试共 4 套（均含 ASan/UBSan）：`test_gateway`、`test_host_link`、`test_usb` 与 `test_radio`，
覆盖队满不误确认、队列计数器回绕、最新 8 条确认、重复包、新 boot / POLL 清确认表与旧命令、
同节点命令覆写与即时刷新 ACK、命令确认、CRC/超长帧重同步、10 万组时间换算、主动背压重挂延迟统计，
以及 CDC 分包末块提交、断线重发、HELLO/TIME_RESP。
`test_usb` 直接编译 USB ISR 与控制端点代码，按手册模拟 SETUP 不更新 RX_LEN、旧 IN/OUT 与新 SETUP
共存时的两次写 1 清零，以及旧 EP0 IN 重填缓冲时保护新请求。另覆盖设备描述符、配置描述符 64+3 B
分包、SET_ADDRESS 状态阶段后生效、CDC 类请求及 Bulk 收发。

## 串口出现但文本窗口没有数据时

网关发送带 CRC 的二进制帧。主机串口工具需置 DTR，并按 `docs/03_网关与上位机.md` 的格式处理数据。
原 Python PC 工具已移除。检查十六进制帧可使用 `docs/site/index.html` 的离线协议解码器；该页面不连接串口。
v1.2 已在 macOS 实际枚举并收到节点三轴样本，普通文本窗口的显示不能用来判断无线链路是否有数据。

## 刷写后没有 CDC 串口时

当前板卡使用 BOOT 按键进入 USB ISP：按住 BOOT 接通 USB，下载设备识别后松开 BOOT，再执行：

```sh
wchisp -u flash "firmware/build/gateway/gateway.hex"
# 如需查看 UART1 启动日志，可刷写日志版：
wchisp -u flash "firmware/build/gateway-debug/gateway.hex"
```

该命令下载、校验代码 Flash 后自动复位；`-u` 选择 USB 传输，不操作 DataFlash/EEPROM。
刷写成功后松开 BOOT，拔插一次 USB，正常上电启动应用。按住 BOOT 上电会再次进入 ROM 下载程序。

当前网关版本为 **v1.5**（在 v1.4 基础上新增同节点命令覆写、跨 boot 清旧命令和入队即时刷新 ACK，见 `docs/13`）。
历史记录：v1.3 新增的栈涂色入口曾误在用户态访问 `mstatus`，会在进入 `main()` 前异常导致无 CDC；
v1.4 起已改用厂家 SDK 的用户态 CSR `0x800`，并通过 `tools/check_firmware.py` 强制校验。

macOS 正常应在 USB 树中看到 `BMI323 Gateway`（VID:PID `1A86:8040`），并创建 `/dev/cu.usbmodem*`。
这个数据口对应 PB10/PB11，WCH-Link 的下载口是另一个设备。

```sh
ioreg -p IOUSB -w 0
ls /dev/cu.*
```

v1.2 按 CH592DS1 第 17 章（纸页 107–108）修正控制器语义：`RB_UIS_SETUP_ACT` 已保证收到完整 8 B
请求，SETUP 不更新 RX_LEN，不能据其旧值拒绝请求。若旧 IN/OUT 和 SETUP 同时待处理，必须先处理旧事务、
清一次 TRANSFER，再重新检查 SETUP 并处理，最后再清一次 TRANSFER。处理前保存新请求，防止旧 EP0
IN 回调重填共享 DMA 缓冲将其覆盖。

如果仍未出现设备，将 USB 转串口 RX 接 PA9、GND 共地，设置 115200 8N1。
日志依次为版本/网关编号、`BLE init`、`HAL init (32K)`、`CDC init`、`CDC ready`、`RF init`。
`CDC ready` 表示 USB 外设初始化完成。若最后停在 `HAL init (32K)`，检查 PA10/PA11 的外部
32.768 kHz 晶振：当前构建 `CLK_OSC32K=0`，厂家 RTC 初始化会等待其时钟，USB 初始化尚未执行。

后续仍需验证：USB反复拔插、约91包/秒连续接收、PA5/PA6重挂延迟、RF AUTO回调行为、
受控校准时的重传、1秒无回调恢复及24小时稳定性。PC暂停读取时应看到节点重试/缓存计数上升，
恢复后能从原始日志重组，超出缓存的缺口需与节点计数对齐。
