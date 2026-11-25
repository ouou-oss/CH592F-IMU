# 离线系统手册

双击 `index.html`，用 Chrome、Edge、Firefox 或 Safari 打开。无需安装依赖、运行服务器或联网。页面中的解码器不连接硬件、不发送网络请求，也不把输入保存到磁盘。

## 带到车间电脑

- 复制整个 `site/` 文件夹，保持 `css/`、`js/` 与 `index.html` 的相对位置。
- 要使用页面中的源文档链接，复制整个 `docs/`，仍从 `docs/site/index.html` 打开。浏览器可能直接显示或下载 Markdown 文件，可用本地文本编辑器查看。
- 固件构建入口见根 `README.md`，历史联调结果与接板验收见 `../11_软件实现与接板验收.md`。原 Python PC 工具已移除；本页保留离线协议说明与解码器。现场指标以 `../10_阶段一_单节点全量上传.md` 为准。

## 使用解码器

粘贴一帧十六进制数据，点击“解码”，或按 Ctrl / ⌘ + Enter。支持连续 HEX、空白、逗号、冒号、短横线及 `0x` 前缀，不支持带日志文字的混合输入。

- 主机帧：必须包含 `A5 5A`、完整头部、payload 和两字节 CRC，只接受单帧。CRC 错误时停止解析负载。
- 无线负载：支持 V1 DATA、STATUS、POLL、ACK，输入不含 RF 空口头和硬件 CRC。模式 B 的 SUMMARY / EVENT 见 05 源文档。
- 主机类型 RF_RX 会继续解析内层无线包；TIME_REQ / TIME_RESP / GW_STATUS / CMD_RESP / HELLO / QUEUE_DOWNLINK / GET_INFO 按已定义格式解析。SET_GW_CFG 留待后续阶段，仅展示原始负载。
- 样本按 cfg 中的量程换算 g，时间字段按 cfg.b7 区分 RTC 计数和微秒。示例均为合成数据。

CRC 采用 CRC-16/CCITT-FALSE：poly=0x1021、init=0xFFFF、refin/refout=false、xorout=0；范围是 type 到 payload 末尾，结果小端。精确定义与测试向量见 `../03_网关与上位机.md`。

## 维护与测试

- `index.html`：运行流程、通信说明、可靠性、时间与现场检查。
- `css/style.css`：本地系统字体、响应式布局与打印样式。
- `js/protocol.js`：与 02 / 03 保持一致的字段、命令与主机类型。
- `js/codec.js`：CRC、严格长度校验、编解码和示例生成。
- `js/render.js`：字段联动、解码结果与样本波形。
- `js/app.js`：初始化与章节导航。

协议修改时同步更新源文档、字段定义与测试。无需构建，开发机有 Node.js 即可运行：

```sh
node "docs/site/tests/codec.test.cjs"
```

测试包含独立 CRC 向量、主机封装、内层长度、版本、确认表、样本换算与错误输入。页面无 CDN、外部字体、远程图片、模块导入或 fetch。
