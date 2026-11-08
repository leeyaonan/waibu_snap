#!/usr/bin/env python3
"""生成二维码真实引擎测试夹具。

依赖（仅开发 / 更新夹具时需要，不参与构建与 CI）：
    python3 -m pip install 'qrcode[pil]'

用法：
    python3 tests/tools/make_qr_fixtures.py
"""
from pathlib import Path

import qrcode
from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / "fixtures"
URL = "https://example.com/waibu?q=1"
TEXT = "hello waibusnap"


def make(text: str, box_size: int = 8) -> Image.Image:
    code = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=box_size,
        border=4,
    )
    code.add_data(text)
    code.make(fit=True)
    return code.make_image(fill_color="black", back_color="white").convert("RGB")


def main() -> None:
    ROOT.mkdir(parents=True, exist_ok=True)
    url = make(URL)
    text = make(TEXT)
    url.save(ROOT / "qr-url.png")
    text.save(ROOT / "qr-text.png")
    canvas = Image.new(
        "RGB", (url.width + text.width + 60, max(url.height, text.height) + 40), "white"
    )
    canvas.paste(url, (20, 20))
    canvas.paste(text, (url.width + 40, 20))
    canvas.save(ROOT / "qr-multi.png")
    print(f"已写入 {ROOT}：qr-url.png / qr-text.png / qr-multi.png")


if __name__ == "__main__":
    main()
