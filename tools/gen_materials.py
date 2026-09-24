#!/usr/bin/env python3
"""Pack ambientCG (CC0 1.0, https://ambientcg.com/license) material sets into the
compact layers the renderer loads (assets/apk/materials/):

  <layer>_albedo.jpg   1024x1024 sRGB colour as photographed, ambient occlusion baked in
  <layer>_nrm.jpg      512x512: R,G = OpenGL-convention normal XY, B = roughness

Also prints each layer's mean linear albedo (materials.glsl kLayerMean), which the
shaders use when a layer only modulates a palette colour.

Usage: tools/gen_materials.py <dir with extracted *_1K-JPG sets>
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

LAYERS = [  # order = texture array layer (shaders/include/materials.glsl)
    "Concrete034", "Metal021", "CorrugatedSteel005", "Asphalt031",
    "PavingStones138", "Rust009", "PaintedPlaster006", "MetalPlates006",
    "CorrugatedSteel007B", "CorrugatedSteel002", "CorrugatedSteel006A", "Metal041B",
    "Concrete042C", "Tiles133A", "Tiles138", "WoodSiding005",
]
SIZE = 1024       # albedo
NRM_SIZE = 512    # normal + roughness
OUT = Path(__file__).resolve().parent.parent / "assets" / "apk" / "materials"


def load(path, mode, size):
    return Image.open(path).convert(mode).resize((size, size), Image.LANCZOS)


def main(src):
    OUT.mkdir(parents=True, exist_ok=True)
    means = []
    for i, name in enumerate(LAYERS):
        d = Path(src) / name
        base = f"{name}_1K-JPG"
        col = np.asarray(load(d / f"{base}_Color.jpg", "RGB", SIZE)).astype(np.float32) / 255.0
        lin = np.where(col <= 0.04045, col / 12.92, ((col + 0.055) / 1.055) ** 2.4)
        ao_path = d / f"{base}_AmbientOcclusion.jpg"
        if ao_path.exists():
            ao = np.asarray(load(ao_path, "L", SIZE)).astype(np.float32)[..., None] / 255.0
            lin = lin * (0.35 + 0.65 * ao)
        means.append(lin.reshape(-1, 3).mean(axis=0))
        srgb = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.clip(lin, 0, 1) ** (1 / 2.4) - 0.055)
        Image.fromarray((srgb * 255 + 0.5).astype(np.uint8)).save(OUT / f"{i}_albedo.jpg", quality=90)

        nrm = np.asarray(load(d / f"{base}_NormalGL.jpg", "RGB", NRM_SIZE)).astype(np.float32)
        # Centre XY: some scans (overlapping siding boards) lean on average, which would
        # tilt the whole wall's shading.
        nrm[..., :2] -= nrm[..., :2].reshape(-1, 2).mean(axis=0) - 127.5
        nrm = np.clip(nrm + 0.5, 0, 255).astype(np.uint8)
        rough = np.asarray(load(d / f"{base}_Roughness.jpg", "L", NRM_SIZE))
        packed = np.dstack([nrm[..., 0], nrm[..., 1], rough])
        Image.fromarray(packed).save(OUT / f"{i}_nrm.jpg", quality=92)
        print(f"layer {i}: {name} mean {means[-1].round(3).tolist()}")
    print("const vec3 kLayerMean[%d] = vec3[%d](%s);" % (len(LAYERS), len(LAYERS), ", ".join(
        "vec3(%.3f, %.3f, %.3f)" % tuple(m) for m in means)))
    (OUT / "CREDITS.txt").write_text(
        "Material textures from ambientCG (https://ambientcg.com), CC0 1.0 Universal.\n"
        + "\n".join(f"layer {i}: {n} (https://ambientcg.com/view?id={n})" for i, n in enumerate(LAYERS)) + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
