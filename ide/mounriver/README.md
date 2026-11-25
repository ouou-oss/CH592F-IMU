# 在 MounRiver Studio 中构建与调试

在 MounRiver Studio 选择 **File → Import → Existing Projects into Workspace**，
选择本目录，导入 `BMI323_sensor` 和 `BMI323_gateway`。不要复制项目到 workspace，
否则相对的源码链接和统一 Makefile 路径会失效。

两个项目分别通过本目录下的 Makefile 调用 `firmware/Makefile`，与命令行构建使用
同一套 GCC8、宏、RAM 链接脚本和内存检查。默认 Build Project 生成发布版；
添加 `debug` Make Target 可生成 UART 日志版。不要让 IDE 自动生成 Makefile。

调试器选 WCH-LinkE、目标 CH592，ELF 使用：

- 节点：`firmware/build/sensor/sensor.elf`
- 网关：`firmware/build/gateway/gateway.elf`
- 日志版目录为 `sensor-debug`、`gateway-debug`

PB14/TIO、PB15/TCK 接下载器，电压与板卡一致。工程只构建，不自动下载、
擦除 EEPROM 或复位正在运行的设备。更换已有设备用途前先备份其 DataFlash。
