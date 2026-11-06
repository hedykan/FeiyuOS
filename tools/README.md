# 工具

| 脚本 | 用途 | 文档 |
|:--|:--|:--|
| `vat_e2e.py` | 自动验收：在 SSH 目标主机上运行，只经 Wi-Fi 调用设备 Web API，同时核对主机上的真实副作用。按 git 改动挑选阶段（`--phases auto`） | [E2E 验收](../docs/E2E.zh-CN.md) |
| `prompt_lab.py` | 提示词实验台：把设备的请求原样发给模型 API，并发跑全部意图用例，判定与 E2E 相同。改提示词时先在这里调 | [E2E · 提示词实验台](../docs/E2E.zh-CN.md#提示词实验台) |
| `gen_pcm.py` | 用 TTS 合成 `tests/pcm/` 的语音用例（需要 OpenRouter Key 与 ffmpeg） | [E2E · 语音用例](../docs/E2E.zh-CN.md#语音用例) |
| `pack_mascot.py` | 把吉祥物 PNG 帧（5 个状态各最多 8 帧、32×32、最多 63 色）打包成设备读的 `mascot.bin`，上传到 `/vat/assets/mascot.bin`；素材不在仓库里 | 脚本头部注释 |
| `wait_ready.sh` | 刷写或重启后轮询 `/api/status`，等到新启动（`uptime_s` 不超过已等待时间）且配置已加载；`make flash` 用它代替固定等待 | `make flash` |
| `record_amap_fixtures.sh` | 重新录制 `tests/host/fixtures/amap/`：只查公开地标，Key 从 `../.amap_key` 读取，录完校验文件里不含 Key | [测试用例](../tests/README.md) |
| `build_esp32_libs.sh` | 用 Docker 按板卡包的同一组提交重新编译 ESP-IDF 预编译库，TLS 发送缓冲 16→4 KB | [固件 · 自建 ESP-IDF 库](../firmware/README.md#自建-esp-idf-库更小的-tls-发送缓冲) |
| `esp32_libs.sh` | 在板卡包自带库与自建库之间切换（`make libs-custom` / `libs-stock` / `libs-status`） | 同上 |
| `publish_snapshot.sh` | 从 HEAD 导出公开发布用的单提交仓库：排除 `export-ignore` 的文件，跑 gitleaks 和仓库外的关键词黑名单 | 脚本头部注释 |

设备地址统一用环境变量 `VAT_DEVICE`（如 `http://192.168.1.50`）。API Key 放在仓库**外**的上一级目录（`../.deepseek_key`、`../.openrouter_voice_key`），脚本不会打印它们；运行记录写到 `logs/`（不入库，可能含转写与主机输出）。
