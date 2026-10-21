#!/usr/bin/env python3
"""Gera icon0.png (512x512) e pic1.png (1920x1080) com a identidade do app (verde Kick sobre fundo escuro).
Roda no GitHub Actions depois do get_toolchain.sh. Se falhar, o build segue com os icones do sample."""
import os
from PIL import Image, ImageDraw

GREEN = (83, 252, 24)
os.makedirs("sce_sys", exist_ok=True)

def vgrad(w, h, top, bot):
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return img

def draw_k(d, cx, cy, size, color):
    """Letra K geometrica (barra + duas diagonais), estilo logo da Kick."""
    s = size
    x0, y0 = cx - s * 0.36, cy - s * 0.5
    bar = s * 0.26
    d.rectangle([x0, y0, x0 + bar, y0 + s], fill=color)
    # braco superior
    d.polygon([(x0 + bar, cy + s * 0.04), (x0 + bar + s * 0.36, y0), (x0 + bar + s * 0.66, y0),
               (x0 + bar + s * 0.30, cy + s * 0.12)], fill=color)
    # braco inferior
    d.polygon([(x0 + bar + s * 0.04, cy - s * 0.02), (x0 + bar + s * 0.34, cy - s * 0.10), (x0 + bar + s * 0.66, y0 + s),
               (x0 + bar + s * 0.36, y0 + s)], fill=color)

# ---- icon0.png ----
S = 512
icon = vgrad(S, S, (14, 18, 28), (6, 8, 12))
d = ImageDraw.Draw(icon)
d.rounded_rectangle([96, 96, 416, 416], radius=84, fill=GREEN)
draw_k(d, S // 2, S // 2, 220, (8, 10, 14))
icon.save("sce_sys/icon0.png")

# ---- pic1.png (fundo da tela de detalhes) ----
W, H = 1920, 1080
bg = vgrad(W, H, (9, 11, 16), (21, 26, 40))
d = ImageDraw.Draw(bg)
draw_k(d, W - 420, H // 2, 760, (18, 24, 36))
d.rounded_rectangle([160, 420, 224, 484], radius=18, fill=GREEN)
bg.save("sce_sys/pic1.png")
print("icones gerados")
