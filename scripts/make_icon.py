#!/usr/bin/env python3
"""Gera icon0.png (512x512) e pic1.png (1920x1080) com a identidade do app (K pixel verde sobre preto, igual a tela inicial).
Roda no GitHub Actions depois do get_toolchain.sh. Se falhar, o build segue com os icones do sample."""
import os
from PIL import Image, ImageDraw

GREEN = (83, 252, 24)
os.makedirs("sce_sys", exist_ok=True)

# "K" em blocos (mesmas proporcoes de uiKMark em src/ui.hpp): x0, x1, y0, y1 em fracao da caixa
K_RECTS = [(0, .370, 0, 1), (.370, .491, .245, .764), (.491, .620, .136, .882), (.620, 1, 0, .318),
           (.620, .861, .318, .445), (.620, .741, .445, .564), (.620, .861, .564, .691), (.620, 1, .691, 1)]

def draw_k(d, x, y, w, h, color):
    for x0, x1, y0, y1 in K_RECTS:
        d.rectangle([x + x0 * w, y + y0 * h, x + x1 * w - 1, y + y1 * h - 1], fill=color)

def glow(img, box, color, spread, peak):
    """brilho suave ao redor de uma caixa"""
    ov = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    x0, y0, x1, y1 = box
    for i in range(spread, 0, -1):
        a = int(peak * (spread - i + 1) / (spread * 3))
        d.rounded_rectangle([x0 - i, y0 - i, x1 + i, y1 + i], radius=i + 8, fill=color + (a,))
    return Image.alpha_composite(img.convert("RGBA"), ov).convert("RGB")

# ---- icon0.png ----
S = 512
icon = Image.new("RGB", (S, S), (0, 0, 0))
kw, kh = 230, 236
box = (S // 2 - kw // 2, S // 2 - kh // 2, S // 2 + kw // 2, S // 2 + kh // 2)
icon = glow(icon, box, GREEN, 40, 150)
d = ImageDraw.Draw(icon)
draw_k(d, box[0], box[1], kw, kh, GREEN)
icon.save("sce_sys/icon0.png")

# ---- pic1.png (fundo da tela de detalhes) ----
W, H = 1920, 1080
bg = Image.new("RGB", (W, H), (0, 0, 0))
d = ImageDraw.Draw(bg)
draw_k(d, W - 900, H // 2 - 380, 740, 760, (10, 26, 8))
bg = glow(bg, (160, 500, 224, 564), GREEN, 20, 120)
d = ImageDraw.Draw(bg)
draw_k(d, 160, 500, 64, 66, GREEN)
bg.save("sce_sys/pic1.png")
print("icones gerados")
