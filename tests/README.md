# 测试用例

| 路径 | 内容 | 谁在用 |
|:--|:--|:--|
| `cases/ssh_intent_cases.json` | 41 条 SSH 意图：一句话 → 期望的工具调用 | E2E S2I、实验台 |
| `cases/daily_intent_cases.json` | 17 条日常意图 | E2E S2I、实验台 |
| `cases/v05_intent_cases.json` | 15 条 v0.5 本机扩展包意图 | 实验台 |
| `cases/runtime_cases.json` | 9 条多轮对话，按回答中应出现的文字判定；`{user}`、`{hostname}` 取自运行脚本的主机 | E2E S2 |
| `pcm/manifest.json` | 23 条语音意图：音频文件、转写关键词、期望调用 | E2E S1、实验台（只用文字与期望） |
| `pcm/standalone_manifest.json` | 5 条本机设置语音 | E2E S1 |
| `pcm/*.pcm` | TTS 合成的音频（PCM16 小端、单声道、16 kHz），不是人声录音；参数见 `pcm/generated.json` | E2E S1 |
| `host/host_test.cpp` | 开发机单元测试：可移植固件源码（`common_text.cpp`、`place_format.cpp`）的解析与格式化，`make host-test` 几秒跑完 | 开发时、改解析逻辑后 |
| `host/shim/Arduino.h` | 开发机上的最小 `String`（只实现被测源码用到的部分，语义与 Arduino 一致） | `make host-test` |
| `host/fixtures/amap/*.json` | 高德的真实返回（公开地标天安门→故宫，不含设备位置与 Key），由 `tools/record_amap_fixtures.sh` 录制 | `make host-test` |
| `cases/v053_program_cases.json` | 8 条程序与触发意图：一句话 → 第一个调用 | 实验台 |
| `programs/*.json`、`programs/cases.json` | 测试程序（灯、分支、循环、调用与参数、容错、等待、问答、天气、需确认、无效）及期望结果；E2E S10 经单个工具调用保存、运行、核对，结果不依赖模型 | E2E S10 |
| `host/prog_test.cpp` | 程序引擎的开发机测试：校验、分支、循环、等待的保存与恢复、调用与返回值、容错、上限 | `make host-test` |
| `e2e_profiles.example.json` | E2E 要求设备上存在的 SSH 主机清单（地址、用户是占位符） | E2E S0 检查是否齐全 |

**期望的写法**（`expect`）：`call_names` 是工具名的完整顺序；`arguments` 逐个调用列出要逐键相等的参数，没列出的参数不检查。判定函数是 `tools/vat_e2e.py` 的 `validate_expect`，实验台与设备 E2E 共用。

新增扩展包时，按[扩展包开发手册](../docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md) §6 在这里补意图用例，并在 `tools/vat_e2e.py` 补设备上的用例。
