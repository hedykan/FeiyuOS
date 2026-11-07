# 开机动画素材（不在 MIT 授权范围内）

这个目录是开机画面的像素大肥鱼：四个入场动画（`enter_left` 左侧入场、`enter_right` 右侧入场、`enter_top` 上方跳下、`enter_bottom` 底部探头爬出）各播一次，之后循环 `wait`（等待加载中）。每帧 32×32，`frames/<状态>/*.png`；帧时长和每帧位移（`offsets_px`，素材像素，相对落点）在 `animation.json`。

**出处与授权**：与 `assets/mascot` 相同——形象参考社区的 DeepSeek 鲸鱼娘，原型归上善无形「溟月」，女仆版设计归 ZipZipPipe（见 `README.txt`）。本套是用图像生成模型做的像素二创，**没有获得原作者授权**，仅作为本项目的示例形象随仓库提供，请勿用于其他用途或商用。

**如有侵权**：请联系 hedykan@outlook.com，收到后会删除。

固件不依赖它：卡上没有 `/vat/assets/boot.bin` 时，开机画面用角落吉祥物的待机帧滑入；两者都没有时只有气泡。

**用法**：`make mascot` 会一起打包成 `build/boot.bin`，`make mascot-upload DEVICE=http://<设备 IP>` 上传到卡上 `/vat/assets/boot.bin`。
