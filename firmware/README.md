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

## 自建 ESP-IDF 库（更小的 TLS 发送缓冲）

板卡包自带的预编译库里，mbedTLS 每个连接的收、发缓冲各 16 KB。设备一次只开一个 TLS 连接，但没有 PSRAM，握手时可用堆只剩二三十 KB。`tools/build_esp32_libs.sh` 按板卡包 `versions.txt` 的同一组提交重新编译 esp32s3 的库，只改两处：

| 配置 | 原值 | 新值 | 效果 |
|:--|:--|:--|:--|
| `MBEDTLS_ASYMMETRIC_CONTENT_LEN` + `SSL_OUT_CONTENT_LEN` | 收发都 16 KB | 收 16 KB / 发 4 KB | 每个连接少 12 KB；请求体超过 4 KB 时自动分成多条记录发送，与服务器无关 |
| `MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` | 开 | 关 | 握手后释放服务器证书（1–2 KB） |

```sh
tools/build_esp32_libs.sh ~/esp-libbuild        # Docker，约 1–2 小时、占几 GB
make libs-custom CUSTOM_LIBS=~/esp-libbuild/lib-builder/out/tools/esp32-arduino-libs
make libs-status                                # 当前用的是哪一套、mbedTLS 关键配置
make libs-stock                                 # 换回板卡包自带的库
```

切换会清空构建目录；换库后要跑全量 E2E。
