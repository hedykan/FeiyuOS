# Cardputer 技术调研记录

更新时间：2026-11-04

## 硬件基线

**M5Stack Cardputer Adv（K132-Adv）+ Cap LoRa-1262（U214）**。2026-11-04 用 `M5.getBoard()` 实机确认为 `cardputer_adv`（此前文档误按初代 Cardputer 记录）；`/api/status` 的 `board`、`cap_io_expander` 字段可随时核对。

| 能力 | 基线 |
|---|---|
| SoC | ESP32-S3FN8（Stamp-S3A），双核 240 MHz，8 MB Flash，无 PSRAM |
| 显示 | ST7789V2，1.14 英寸，240×135 |
| 键盘 | 56 键，TCA8418 I2C 键盘控制器（0x34，中断 G11） |
| 音频 | ES8311 编解码器（I2C 0x18 + I2S），MEMS 麦克风，NS4150B 功放 + 1 W 扬声器，3.5 mm 耳机口 |
| IMU | BMI270 六轴（I2C 0x68） |
| 内部 I2C | SDA G8 / SCL G9：键盘、IMU、编解码器，以及 Cap 的 IO 扩展芯片 |
| 存储 | 8 MB Flash（LittleFS 1.5 MB）+ microSD（SPI：CS G12，MOSI G14，CLK G40，MISO G39） |
| 其他 | RGB 指示灯 G21（Stamp-S3A 上的 WS2812，M5Unified 引脚表）、红外发射 G44（只发不收）、电池 ADC G10、1500 mAh 电池（官方 2026-07-13 由 1750 mAh 更正）、Grove 口、EXT 14 针扩展口 |
| 网络 | 2.4 GHz Wi-Fi + BLE |

Cap LoRa-1262（插在 EXT 14 针口上）：

| 部分 | 要点 |
|---|---|
| LoRa | Semtech SX1262，868–923 MHz，最大 +22 dBm；SPI 与 TF 卡**共用** SCK/MOSI/MISO，片选 G5，RST G3，DIO1 G4，BUSY G6；TCXO 由 DIO3 供 1.8 V，DIO2 控制收发切换 |
| 射频开关 | PI4IOE5V6408 IO 扩展（I2C 0x43，本机在内部总线 G8/G9 应答），P0 置高才通射频：`0x03=0x01`、`0x07=0x00`、`0x05=0x01` |
| GNSS | ATGM336H-6N（GPS/北斗/伽利略/GLONASS/QZSS），UART 115200 8N1：设备 RX=G15，TX=G13；冷启动定位 30–60 s，室内基本无法定位 |
| 注意 | **没装天线绝不能发射**（会烧功放）；频段只覆盖 868–923 MHz，不含国内 470–510 MHz；LoRa 发射约 163 mA |

来源：[Cardputer Adv 官方文档](https://docs.m5stack.com/en/core/Cardputer-Adv)、[Cap LoRa-1262 官方文档](https://docs.m5stack.com/en/cap/Cap_LoRa-1262)、[m5stack-skills](https://github.com/iot-forge/m5stack-skills)（cardputer-adv、cap-lora-1262）。
## 构建环境

```text
arduino-cli 1.4.1
m5stack:esp32 3.2.5
M5Cardputer 1.1.1 / M5Unified 0.2.13 / M5GFX 0.2.19
LibSSH-ESP32 5.7.0 / RadioLib 7.8.1
板型 m5stack:esp32:m5stack_cardputer
选项 FlashSize=8M, PartitionScheme=default_8MB, PSRAM=disabled
```

USB 枚举为 `303a:1001 Espressif USB JTAG/serial debug unit`（`/dev/ttyACM0`）。

## 单机实测（2026-11-03）

| 项 | 数据 |
|:--|:--|
| 固件体积 | 1.96 MB，程序分区 58%（含 libssh 与 mbedTLS） |
| 静态内存 | 64 KB（19%） |
| 启动到 READY | 约 4.5 s（含 Wi-Fi） |
| 联网后空闲可用堆 | 约 90 KB（TF 卡驱动约 28 KB、worker 栈 32 KB、TLS 预留 35 KB 已扣除）；长时间运行后稳定在约 78 KB |
| 高内存阶段开始时可用堆 | 最低约 84 KB（150 轮混合压测） |
| worker 栈最深使用 | 约 15.7 KB |
| SSH 握手（ed25519，局域网） | 约 0.5 s |
| 文字指令 → DeepSeek → SSH `whoami` | 约 3.3 s |
| Wi-Fi RSSI（同屋） | −27 ~ −29 dBm，关闭省电 |

语音链路、意图准确率和延迟分布见 [E2E](E2E.zh-CN.md) 与 [证据](evidence/README.md)。

## 关键实现结论

- **语音不落第二份**：PCM 写入存储后，上传时由自定义 `Stream` 现场加 WAV 头并 Base64 编码，直接写进 TLS 连接。旧方案先生成 WAV 再生成请求体文件，需要约 2.3 倍录音大小的空闲空间和约 2.8 s 额外耗时。
- **取消要关 socket**：把 TLS 与 SSH 的 fd 登记到取消令牌，Esc 时 `shutdown()`，阻塞读立即返回，不必等超时。
- **网络与 UI 分任务**：所有网络和 SSH 只在 worker 任务里跑，UI 任务在等待期间仍能切页和响应按键。
- **SSH 会话按任务复用**：同一任务内的多次远端命令共用一个会话；任务结束立即关闭，把堆还给下一次 TLS。
- **tmux 输入合并成一条命令**：粘贴、按键和回车合成一次 SSH exec，一次往返完成。
- **扬声器与麦克风分时**：两者经同一个 ES8311 编解码器和 I2S，开机关闭扬声器，只在提示音的瞬间切换，结束后立即恢复麦克风。
- **碎片化比总量更要命**：长时间运行后空闲堆还有 90 KB，最大连续块却只剩约 31 KB，mbedTLS 第二块缓冲区分配失败（`-32512 SSL - Memory allocation failed`），此后所有请求失败直到重启。解决：开机时预留两块 TLS 缓冲区、请求体从 Flash 流式发送、对话历史和任务记录移到存储。修复后 150 轮压测 0 失败。
- **低内存下的 cJSON 失败是静默的**：打印失败返回空，曾导致请求体变成 `null`、或残缺的历史记录落盘后让后续请求全部 422。现在写入和读回历史都做结构校验。
- **M5Unified 麦克风任务会在内存低谷后卡死**：录音预热超时时自动 `Mic.end()/begin()` 重启一次。
- **TF 卡与 LoRa 共用 SPI**：Cap LoRa-1262 的 SX1262 片选（G5）悬空时会占用 MISO，TF 卡低层能响应但读不出类型（`type=0`），表现像坏卡、换卡插拔偶尔又好。挂载前把 G5 拉高后稳定识别为 16 GB（2026-11-04）。此前“重新插拔后恢复”多半也是这个原因。
- **数据只存一处**：插卡开机时把板载 `/vat` 搬到卡上（板载的一定比卡上新），全部复制成功后清空板载副本；曾因“卡上已有配置就不复制”读回了一天前的旧配置。

## 局域网注意事项

测试环境中设备偶尔对 ARP 无响应，表现为 1–2 s 内连接被拒、随后恢复。E2E 客户端对传输错误自动重试；人工使用时表现为配置页偶尔需要刷新。
