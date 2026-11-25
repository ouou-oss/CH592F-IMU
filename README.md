# CH592F × BMI323 全量振动采集

一个 BMI323 节点通过 CH592F 私有 2.4G 链路，把 3.2 kHz 三轴原始数据传到 USB CDC 网关。
当前交付包括**节点固件、网关固件、单页串口观测台、宿主测试与离线协议手册**。原 Python PC 工具及其安装配置已移除，既有采集数据保留。

## 使用网页版串口观测台

用桌面版 **Chrome 或 Edge** 打开 [`web/index.html`](web/index.html)，点击“连接网关”，首次连接时选择 **BMI323 Gateway**。
macOS 在项目根目录可以直接执行：

```sh
open -a "Microsoft Edge" "web/index.html"
```

HTML 为独立文件，无 CDN、无安装步骤，也不需要运行服务。Safari、Firefox 当前不能通过此页面连接串口。
页面自动设置 DTR，发现节点后显示三轴波形、直流/振动 AC RMS/峰值/波峰因数、去直流 Hann 频谱和节点健康状态。
节点上电后默认待机。连接网关并选中节点后，点击「恢复采集」。连续静置 10 秒后，页面弹出休眠提示。晃动设备会在原会话继续采集。
横轴默认使用**北京时间（UTC+8）**，由节点采样时钟与电脑时间对齐估算。单击波形放置定位线，滚轮缩放，左键拖动平移。按住 Shift 再拖动会标出蓝色选区，松手放大。点「回到最新」恢复实时跟随。
“导出 CSV”保存当前会话的最新缓存，包含原始 LSB 与未舍入的 g 值。
最近保留 4 个节点/boot 会话，每个最多 96,000 原始点（3.2 kHz 下约 30 s）和最近 1 小时的每秒极值趋势，长时间采集应及时导出原始数据。
CSV 同时保留原始相对时间、节点时钟和北京时间估算。功能、统计口径和开发入口见 [`web/README.md`](web/README.md)。

## 构建与测试

```sh
make test
make firmware
make -C "firmware" ROLE=sensor LOG=1
make -C "firmware" ROLE=gateway LOG=1
```

默认使用已安装的 MounRiver GCC 8.2.0，路径配置在 `firmware/Makefile`，可通过 `TOOLCHAIN` 覆盖。
两种固件输出 `.elf`、`.hex`、`.bin`、`.map`；日志版单独放在 `*-debug` 目录。
`make test` 运行公共协议、节点、网关的 C 测试、离线网页解码器和串口网页协议/分析测试；需要 C 编译器和 Node.js。
固件链接检查使用系统 Python 3 标准库。

| 角色 | 当前版本 | 下载文件 | 接线/运行说明 |
|---|---|---|---|
| 节点 | v1.7（默认上电待机） | `firmware/build/sensor/sensor.hex` | [节点说明](firmware/sensor/README.md) |
| 网关 | v1.5 | `firmware/build/gateway/gateway.hex` | [网关说明](firmware/gateway/README.md) |

MounRiver 可导入工程在 [ide/mounriver](ide/mounriver/README.md)，与命令行共用同一 Makefile。
构建不会自动下载、擦除设备或改动厂家示例。

## 接线与 USB 数据接口

节点接线：SPI0 为 PA12/13/14/15，BMI323 INT1 为 PA4，外部 32.768 kHz 晶振为 PA10/11。
节点 PA8 驱动状态灯，UART1 改用 PB13 TX / PB12 RX，115200 8N1。两端默认 2450 MHz、1M、0 dBm、
access address `0x71764129`、CRCInit `0x123456`。

网关 USB D−/D+ 为 PB10/PB11，枚举为 `BMI323 Gateway`（VID:PID `1A86:8040`）。
主机打开 CDC 数据口并置 DTR 后，网关发送带 CRC 的二进制帧。帧格式、命令及时间字段见
[网关与上位机协议](docs/03_网关与上位机.md)，十六进制帧可用 [离线手册](docs/site/index.html) 中的解码器检查。

既有采集记录、CSV 和 Parquet 文件保留在 `data/`，实板诊断记录保留在 `build/`。

## 当前容量与验收边界

- 节点默认 **24 槽**，按每包 35 点最多缓存 840 点（3.2 kHz 下约 **0.2625 s**）。STATUS 也占槽。
- 网关 **8 个物理槽**，正常反压门限最多积压 7 包（约 76.6 ms）；保留厂家 6 KiB BLE 堆及每工程 2 KiB 栈。
- 固件预留 2 KiB 栈后，v1.7 节点发布版/日志版余 **696 / 676 B**，v1.5 网关发布版/日志版余 **448 / 440 B**。历史基线见 [接板验收文档](docs/11_软件实现与接板验收.md)。
- 网页的近 60 s 缺样取 DATA 编号缺口和 STATUS 的 FIFO／缓存丢样增量之和中的较大值。V1 混合故障无法逐样本唯一归因，长采集停顿仍可能出现未知丢样量。
- 远程 REBOOT 当前明确拒绝，避免命令重复导致重启循环。v1.5 待机电流、启停循环、时间精度、现场覆盖及 24 h 完整性仍需接板验收。

下一步按 [软件实现与接板验收](docs/11_软件实现与接板验收.md) 与 [远程启停与低功耗待机方案](docs/13_远程启停与低功耗待机方案.md) 完成实板烧录与验收。

## 工程位置

- `firmware/sdk/`：厂家快照，保持原件与版权；配置、RAM 链接修正在工程层完成。
- `firmware/common/`：V1 协议偏移、小端序、CRC、主机帧封装与栈水位监控。
- `firmware/sensor/`、`firmware/gateway/`：节点 v1.7 与网关 v1.5 固件工程。
- `tests/`：公共协议、节点及网关的宿主 C 测试。
- `tools/`：固件链接、权限 CSR 与 RAM 检查。
- `web/index.html`：单文件串口观测台；开发源文件与测试同在 `web/`。
- `docs/`：总体设计（00~05）、阶段一与专项方案（10~13）、历史评审（`review/`）及离线协议手册（`site/index.html`）。

固件按采样、队列、无线和 USB 转发分责；协议显式序列化，不依赖结构体填充。
固件不使用动态内存或浮点。节点上电默认低功耗待机（BMI323 Suspend + CH592F 真睡眠，2 s 心跳），启动采集后进入模式 A（调度空闲时浅 Idle），静置后由 BMI323 Any-motion 负责唤醒。
模式 B、事件分类和位置推算未实现。
