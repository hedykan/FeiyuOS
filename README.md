# FeiyuOS (肥鱼 OS)

A small, AI-native operating layer for the M5Stack Cardputer Adv (optionally with the Cap LoRa-1262 GPS/LoRa cap). Hold a key and say what you want: 大肥鱼, "the big fat fish" (the community nickname of DeepSeek, the model behind it), turns it into operations on the device's resources: its own screen, LED, buzzer, IMU, TF-card files, timers, infrared, GPS and LoRa, places and routes (AMap), the web, and remote Linux hosts over SSH, including tmux, Codex, Pi and Claude sessions. It can also draw on the screen: canvases with the M5GFX drawing calls (rotation, looping animation, live data), panels with choices, and both saved as local apps. Several steps can be saved as a program on the card and run again, on a schedule or at boot.

FeiyuOS is an application layer on top of ESP-IDF and FreeRTOS, which remain the kernel. What it adds is organised like the user space of an operating system rather than a fixed menu of features:

| OS idea | Here |
|:--|:--|
| File-system namespace | URI resources: `device://` `file://` `sensor://` `timer://` `ir://` `lora://` `wifi://` `place://` `ssh://<host>` `memory://` `prog://` `trigger://` `ui://` |
| Drivers / syscalls | Extension packages claim resources; seven generic tools (`discover` `read` `write` `exec` `observe` `input` `session`) reach them |
| man pages | `discover` |
| Programs | `prog://`: JSON step lists (the six actions plus `if`, `repeat`, `wait`; variables; programs calling programs with arguments) that the model writes and the device runs |
| Scheduling and interrupts | `trigger://` binds a time or the boot to a step; an event bus; waiting runs are saved to the card and resume on time |
| Permissions | A policy layer: local reversible actions run directly, writes and physical actions wait for `Y` on the device; a program's sensitive steps are confirmed once when it is saved |
| Shell | The language model |

Recording, the agent loop, program runs, SSH (libssh), the UI and the settings run on the device, which calls the speech-recognition and model APIs in the cloud directly. No companion computer is involved at run time; USB is only used to flash the firmware.

- [中文说明：构建、刷写、配置、按键](README.zh-CN.md) · [Architecture](docs/ARCHITECTURE.zh-CN.md) · [Agent runtime](docs/AGENT_RUNTIME.zh-CN.md)
- [Programs and triggers](docs/PROGRAMS.zh-CN.md) · [Writing an extension package](docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md) · [Firmware rules](docs/FIRMWARE_RULES.zh-CN.md) · [Contributing](CONTRIBUTING.md)
- [End-to-end acceptance](docs/E2E.zh-CN.md) · [Results per release](docs/evidence/README.md) · [Changelog](CHANGELOG.md)

The documentation is in Chinese for now. The earlier name, Voice Agent Terminal, survives in a few places (the `VAT-XXXX` hotspot, `tools/vat_e2e.py`, the `/vat` directory on the card).

## Hardware

M5Stack Cardputer Adv (ESP32-S3FN8, 8 MB flash, no PSRAM, 240×135 screen, keyboard, microphone). A microSD card is recommended for long context; the Cap LoRa-1262 adds GPS and LoRa.

## Quick start

```sh
make build
make upload PORT=/dev/ttyACM0
```

On first boot the device opens the settings page and a `VAT-XXXX` access point. Join Wi-Fi from the keyboard, then enter the speech-recognition and model API keys and your SSH hosts on the device, from a phone (the settings page shows a QR code that joins the device's hotspot and opens a setup page), or from `http://<device-ip>` on the same LAN with the web token shown on the settings page.

Acceptance runs on the SSH target host and talks to the device over Wi-Fi only:

```sh
VAT_DEVICE=http://<device-ip> python3 tools/vat_e2e.py --phases all
```

API keys, Wi-Fi passwords and the SSH key are stored in plain text on the device in this release; use it on a trusted network only. See [SECURITY](SECURITY.md).

## License

Released under the [MIT License](LICENSE). Third-party libraries, models and services retain their own licenses and terms. The IR decoder in `firmware/standalone_agent/src/third_party/irext/` and the air-conditioner codes in `assets/irdb/` come from [IRext](https://github.com/irext) (MIT). The corner mascot and boot animation art in `assets/mascot/` and `assets/boot/` is fan art and not covered by the MIT License; see [NOTICE](assets/mascot/NOTICE.md) and [NOTICE](assets/boot/NOTICE.md).
