# 发布流程

每个版本都按这份文档发布，步骤和产物保持一致。开发仓库是私有的（不推送）；公开仓库是 [hedykan/FeiyuOS](https://github.com/hedykan/FeiyuOS)，每个版本追加一个快照提交。下文 `vX` 指版本号（如 `v0.5.4`），`IP` 指设备地址。

## 发布什么

| 渠道 | 内容 |
|:--|:--|
| GitHub Release `vX` | 发布说明 `docs/releases/vX.md`；`feiyuos-vX-cardputer-adv.bin`（首次安装和升级都写 0x0，不覆盖存储分区）、`feiyuos-vX-sd.zip`（卡上资源，按卡上目录排好：`vat/assets/mascot.bin`、`vat/assets/boot.bin`、`vat/ir/ac.irdb`，解压到卡根目录）、`SHA256SUMS.txt`。不再单独发 app.bin 和零散的卡文件（v0.5.4 起的旧版本发布页也已改成这个样子）；发布说明见下 |
| 公开仓库 main | 开发仓库 HEAD 的快照（`export-ignore` 的路径不进） |
| M5Burner 桌面端 | 固件「FeiyuOS for Cardputer Adv」新增版本 `vX`，传 0x0 完整镜像，版本更新说明（`change_log`）取自 `docs/releases/vX.md` |
| M5Burner 网页版（burner.m5stack.com） | 同名固件新增版本 `vX`，设备 Cardputer ADV、分类 AI & Assistants，版本说明（`versionDescription`）取自 `docs/releases/vX.md`；公开后需审核，审核通过前页面仍显示上一个已通过的版本 |

## 发布前

1. `firmware/standalone_agent/src/version.h` 的 `kVersion` 改成新版本号（开机画面显示）。
2. `CHANGELOG.md`：`## 未发布` 改为 `## vX · 日期 · 一句话标题`，每条写现象、原因和改法。
3. **写发布说明** `docs/releases/vX.md`（按下文模板，从 CHANGELOG 挑用户能感知的写）。一处写、三处用：GitHub Release 用全文；M5Burner 桌面端和网页版的「本版更新」由脚本截取「## 下载哪个文件」之前的部分（一句话 + 新增 + 修正）转成纯文字自动填入。没有这个文件，两个上传脚本会拒绝上传。预览商店里会显示什么：`node tools/release_notes.mjs vX`。
4. `make host-test` 全部通过（含说明文字检查）。
5. `make build` 后刷进设备，按改动做实机验证；小版本（0.x）发布前过一遍 E2E（`tools/vat_e2e.py --phases all`，发布档，S0–S11 + S2I，约 15 分钟；改了语音链路、提示词或大改动时跑 `--phases full`，约 25–30 分钟，见 `docs/E2E.zh-CN.md`「跑多少」）。结论写进 `WORK_PROGRESS.md`。
6. 改了公开简介（`tools/m5burner.json`）或 README 时，一并检查措辞和链接。

## 步骤

```sh
cd <开发仓库>
# 1. 提交、打标签（pre-commit 里有 gitleaks）
git add -A && git commit -m "..."
git tag vX

# 2. 发布镜像：去掉路径前缀后编译，切 0x0 镜像，把吉祥物、开机动画、空调码库按卡上目录打成 sd.zip（tools/pack_sd.py），
#    写 SHA256SUMS，并用 gitleaks、../.publish_denylist、$HOME 检查镜像里的字符串
make release VERSION=vX            # 产物在 build/release/vX/；gitleaks 在 ~/.local/bin，非登录 shell 先 export PATH=$HOME/.local/bin:$PATH

# 3. 实机验证首次安装镜像：USB 写 0x0，看开机到 ready
esptool --chip esp32s3 -p /dev/ttyACM0 --baud 921600 write_flash 0x0 build/release/vX/feiyuos-vX-cardputer-adv.bin

# 4. 公开仓库：导出快照（gitleaks + denylist），作为一个普通提交追加到公开仓库 main
#    （不强推：历史和旧版本的标签都保留）
tools/publish_snapshot.sh /tmp/feiyuos-vX "FeiyuOS vX"
git clone https://github.com/hedykan/FeiyuOS.git /tmp/feiyuos-public
rsync -a --delete --exclude .git /tmp/feiyuos-vX/ /tmp/feiyuos-public/
cd /tmp/feiyuos-public && git add -A && git diff --cached --stat   # 审一遍
git -c user.name=hedykan -c user.email=hedykan@outlook.com commit -m "FeiyuOS vX" && git push origin main && cd -

# 5. GitHub Release（说明见下节），发完从公开链接下载回来核对校验和
gh release create vX --repo hedykan/FeiyuOS --target main --title "肥鱼 OS vX" \
  --notes-file docs/releases/vX.md build/release/vX/*.bin build/release/vX/*.zip build/release/vX/SHA256SUMS.txt

# 6. M5Burner 桌面端：新增版本（不公开，带上 docs/releases/vX.md 的本版更新）→ 分享码 → 试烧 → 公开
node tools/m5burner.mjs version vX build/release/vX/feiyuos-vX-cardputer-adv.bin
node tools/m5burner.mjs share <fid> vX       # 得到分享码，再用它下载、比对、写 0x0、看开机：
#   curl https://m5burner-api.m5stack.com/api/firmware/share/<分享码>   → {"file": "<文件名>.bin"}
#   curl -o share.bin https://m5burner-cdn.m5stack.com/firmware/<文件名>.bin
#   sha256sum 与 build/release/vX/feiyuos-vX-cardputer-adv.bin 一致后再写 0x0
node tools/m5burner.mjs publish <fid> vX on

# 7. M5Burner 网页版：新增版本（带本版更新）并申请公开（等审核；list 里 PENDING = 待审核）
node tools/m5burner_web.mjs version vX build/release/vX/feiyuos-vX-cardputer-adv.bin public
node tools/m5burner_web.mjs list
```

首次在某台机器上传时先登录一次：`node tools/m5burner.mjs login`、`node tools/m5burner_web.mjs login`（密码隐藏输入，token 存 `~/.config/feiyuos/`，权限 600）。简介改了以后，网页版用 `node tools/m5burner_web.mjs describe <firmwareId>` 更新；桌面端的简介随 `version` 一起提交。

## 发布说明模板

中文，按这个顺序：

1. 一句话：这个版本是什么（新功能或主题）。
2. **这个版本能做什么 / 新增了什么**：从 CHANGELOG 挑用户能感知的，几条即可，不写实现。
3. **下载哪个文件**：三行表格：固件（0x0）、sd.zip（解压到卡根目录，写明里面有哪些资源）、校验和。
4. **刷写**：`pip install esptool`，一条命令（首次安装和升级相同）；端口在 Windows 是 `COMx`、macOS 是 `/dev/cu.usbmodem*`；刷写不动 TF 卡和存储分区。
4.5. **拷卡上资源**：`unzip -o … -d <卡>`；Windows 解压到卡根目录；提醒 macOS 访达拖 `vat` 默认是「替换」会删掉卡上原有内容。
5. **第一次开机**：固件不含任何 Key；设置页或手机扫码填 Wi-Fi（仅 2.4 GHz）、语音识别 Key、DeepSeek Key、高德 Key（可选）；建议插 TF 卡；Key 明文保存，见 SECURITY。
6. **说明**：自建 ESP-IDF 库、还没有的功能。

## 发布后

- 打开两边商店页面，确认这个版本的更新说明显示出来了（桌面端看版本的 change_log，网页版看版本说明；v0.6 起两边都已验证能写入）。
- `WORK_PROGRESS.md` 记一行：版本、提交、各渠道状态（网页版审核结果出来后补）。
- 下载数：`gh release view vX --repo hedykan/FeiyuOS --json assets`；桌面端看公开接口里该固件的 `download`；网页版 `node tools/m5burner_web.mjs list`。
