#!/usr/bin/env python3
"""Cover image for the M5Burner listing: the idle mascot frame scaled up next to the title.

    python3 tools/m5burner_cover.py assets/mascot build/m5burner_cover.png
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONTS = [
    "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/System/Library/Fonts/PingFang.ttc",
]


def font(size):
    for path in FONTS:
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    sys.exit("no CJK font found, add one to FONTS")


def main(mascot_dir, out):
    width, height = 640, 360
    cover = Image.new("RGB", (width, height), (16, 22, 30))
    frame = Image.open(Path(mascot_dir) / "frames/idle/00.png").convert("RGBA")
    scale = min((height - 80) // frame.height, 200 // frame.width)
    big = frame.resize((frame.width * scale, frame.height * scale), Image.NEAREST)
    cover.paste(big, (width - big.width - 24, (height - big.height) // 2), big)
    draw = ImageDraw.Draw(cover)
    draw.text((36, 92), "FeiyuOS", font=font(64), fill=(240, 244, 248))
    draw.text((40, 176), "肥鱼 OS · Cardputer Adv", font=font(26), fill=(94, 200, 180))
    draw.text((40, 222), "按住 Enter 说一句话，大肥鱼帮你办事", font=font(20), fill=(170, 180, 190))
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    cover.save(out)
    print(out, cover.size)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
