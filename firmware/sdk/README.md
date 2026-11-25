# 厂家 SDK 快照

本目录原样取自本机 `/Users/chanyan/Code/CH592F/EVT/EXAM`：

- `SRC/Ld`、`SRC/RVMSIS`、`SRC/Startup`、`SRC/StdPeriphDriver`
- `BLE/HAL`、`BLE/LIB`（V1.4 配套头文件与 `libCH59xBLE.a`）

保留厂家版权及使用限制：源代码、修改版本及二进制仅用于南京沁恒微电子制造的 MCU。二进制库不是开源实现，不另行授予许可。

不在此目录改配置。项目预处理宏在 `firmware/Makefile`，无线默认参数在 `firmware/common/CONFIG.h`。项目级 `firmware/Link.ld` 派生自此处 `SRC/Ld/Link.ld`，用于 2 KiB 栈与 RF 活动路径的 RAM 放置。
