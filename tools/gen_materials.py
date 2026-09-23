#!/usr/bin/env python3
"""Pack ambientCG (CC0 1.0, https://ambientcg.com/license) material sets into the
compact layers the renderer loads (assets/apk/materials/):

  <layer>_albedo.jpg   512x512 sRGB colour, mean luminance normalised to 0.5 so the
                       shaders can use it as a detail modulator (x2) over their own
                       palette-driven albedo
  <layer>_nrm.jpg      512x512: R,G = OpenGL-convention normal XY, B = roughness

Usage: tools/gen_materials.py <dir with extracted *_1K-JPG sets>
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

LAYERS = [  # order = texture array layer (shaders/include/materials.glsl)
    "Concrete034", "PaintedMetal004", "CorrugatedSteel005", "Asphalt031",
    "PavingStones138", "Metal021", "PaintedPlaster017", "MetalPlates006",
]
SIZE = 512
OUT = Path(__file__).resolve().parent.parent / "assets" / "apk" / "materials"


def load(path, mode):
    return Image.open(path).convert(mode).resize((SIZE, SIZE), Image.LANCZOS)


def main(src):
    OUT.mkdir(parents=True, exist_ok=True)
    for i, name in enumerate(LAYERS):
        d = Path(src) / name
        base = f"{name}_1K-JPG"
        col = np.asarray(load(d / f"{base}_Color.jpg", "RGB")).astype(np.float32) / 255.0
        lin = np.where(col <= 0.04045, col / 12.92, ((col + 0.055) / 1.055) ** 2.4)
        lum = (lin @ np.array([0.2126, 0.7152, 0.0722])).mean()
        lin = np.clip(lin * (0.5 / max(lum, 1e-3)), 0.0, 1.0)
        # Halve the saturation: the city's palette, not the photo, decides the colour.
        grey = (lin @ np.array([0.2126, 0.7152, 0.0722]))[..., None]
        lin = grey + (lin - grey) * 0.5
        srgb = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * lin ** (1 / 2.4) - 0.055)
        Image.fromarray((srgb * 255 + 0.5).astype(np.uint8)).save(OUT / f"{i}_albedo.jpg", quality=88)

        nrm = np.asarray(load(d / f"{base}_NormalGL.jpg", "RGB"))
        rough = np.asarray(load(d / f"{base}_Roughness.jpg", "L"))
        packed = np.dstack([nrm[..., 0], nrm[..., 1], rough])
        Image.fromarray(packed).save(OUT / f"{i}_nrm.jpg", quality=92)
        print(f"layer {i}: {name}")
    (OUT / "CREDITS.txt").write_text(
        "Material textures from ambientCG (https://ambientcg.com), CC0 1.0 Universal.\n"
        + "\n".join(f"layer {i}: {n} (https://ambientcg.com/view?id={n})" for i, n in enumerate(LAYERS)) + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
