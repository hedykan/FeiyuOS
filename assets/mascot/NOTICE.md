# 吉祥物素材（不在 MIT 授权范围内）

这个目录是 Agent 页右下角像素大肥鱼的素材：5 个状态（待机、识别、理解、执行、出错）各 6 帧，32×32，`frames/<状态>/*.png`，帧时长在 `animation.json`。

**出处与授权**：本目录**不适用**仓库的 MIT 许可证。形象参考社区的 DeepSeek 鲸鱼娘，原型归上善无形「溟月」，女仆版设计归 ZipZipPipe（参考资料见 `README.txt`）。本套是用图像生成模型做的像素二创，**没有获得原作者授权**，仅作为本项目的示例形象随仓库提供，请勿用于其他用途或商用。

**如有侵权**：请联系 hedykan@outlook.com，收到后会删除。

固件不依赖它：卡上没有 `/vat/assets/mascot.bin` 时吉祥物不显示，其余功能不受影响。

**用法**：

```sh
make mascot                                   # 打包成 build/mascot.bin
make mascot-upload DEVICE=http://<设备 IP>    # 打包并上传到设备卡上 /vat/assets/mascot.bin
```

换成自己的形象：按同样的目录结构放 PNG（每帧 32×32，全透明或不透明，最多 63 色），改 `animation.json` 里的帧时长即可。
