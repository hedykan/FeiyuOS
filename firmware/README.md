# 固件

| 目录 | 用途 |
|:--|:--|
| `standalone_agent/` | 产品固件。`standalone_agent.ino` 是启动与主循环，源码在 `src/`（模块地图见 [单机架构 · 源码](../docs/ARCHITECTURE.zh-CN.md#源码)），扩展包在 [`src/packages/`](standalone_agent/src/packages/README.md) |
| `hardware_probe/` | 诊断用：键盘、屏幕、麦克风 |
| `sd_probe/` | 诊断用：TF 卡挂载与读写速度 |

构建与刷写（arduino-cli，板卡包 `m5stack:esp32`）：

```sh
make build                      # 编译 standalone_agent
make upload PORT=/dev/ttyACM0   # 刷写；只覆盖程序分区，设备 /vat 下的配置与历史保留
make serial-log                 # 串口日志写入 logs/serial.log
```

改动固件前先读 [固件开发约束](../docs/FIRMWARE_RULES.zh-CN.md)：没有 PSRAM，内存、音频、存储、线程、超时都有来自真实故障的硬规则。改动后按 [E2E 验收](../docs/E2E.zh-CN.md) 跑受影响的阶段。

## 自建 ESP-IDF 库（省内存、带蓝牙控制器）

板卡包自带的预编译库按通用配置编译：mbedTLS 每个连接收发缓冲各 16 KB，内核和堆的代码放在 IRAM（和堆共用同一块片内 SRAM），还带着 PSRAM、Zigbee、Matter 等用不上的组件。设备没有 PSRAM，这些都直接吃掉可用的堆。`tools/build_esp32_libs.sh` 按板卡包 `versions.txt` 的同一组提交重新编译 esp32s3 的库：

| 配置 | 原值 | 新值 | 效果 |
|:--|:--|:--|:--|
| `MBEDTLS_ASYMMETRIC_CONTENT_LEN` + `SSL_OUT_CONTENT_LEN` | 收发都 16 KB | 收 16 KB / 发 4 KB | 每个连接少 12 KB；请求体超过 4 KB 时自动分成多条记录发送 |
| `MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` | 开 | 关 | 握手后释放服务器证书（1–2 KB） |
| `FREERTOS_` / `HEAP_` / `RINGBUF_PLACE_FUNCTIONS_INTO_FLASH` | IRAM | flash | IRAM 代码 77 → 57 KB，等量变成堆 |
| `SPIRAM` | 开 | 关 | 板子没有 PSRAM；Wi-Fi 发送缓冲随之改为用时分配（空闲时不常驻约 13 KB） |
| `ESP_TIMER_TASK_STACK_SIZE` | 8 KB | 4 KB | 实测只用 1.3 KB |
| `ESP_WIFI_STATIC_RX_BUFFER_NUM` | 8 | 6 | 约 3 KB |
| `BT_CONTROLLER_ONLY` + `BT_CTRL_RUN_IN_FLASH_ONLY` | Bluedroid、BLUFI、Mesh | 只有控制器，代码在 flash | 蓝牙主机用 NimBLE-Arduino；关着蓝牙时只多占约 8 KB 堆 |
| PPP、NAPT、以太网、DTLS、Zigbee、Insights | 开 | 关 | 用不上 |

`tools/esp32_libs_trim.py` 在编译前去掉 19 个用不上的托管组件（相机、tflite、esp-sr、Matter、Zigbee、RainMaker、Insights 等），并且只编 Cardputer 用的一种变体（qio flash、80 MHz、一种引导程序）。首次运行要下载 ESP-IDF 和工具链（约 1 小时），之后改配置重编只要几分钟。**编出来的库只适用于 Cardputer**，同一台机器编别的板子先 `make libs-stock`。

效果（v0.6 固件，2026-11-08）：开机空闲堆 110 → 147 KB，最大连续块 43 → 78 KB。

```sh
tools/build_esp32_libs.sh ~/esp-libbuild        # Docker；首次约 1 小时、占几 GB，之后几分钟
make libs-custom CUSTOM_LIBS=~/esp-libbuild/lib-builder/out/tools/esp32-arduino-libs
make libs-status                                # 当前用的是哪一套、mbedTLS 关键配置
make libs-stock                                 # 换回板卡包自带的库
```

切换会清空构建目录；换库后要跑全量 E2E。
