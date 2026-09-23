# CYBER-APEX

モバイル（Android / Vulkan 1.3）向けハイブリッドレンダリングエンジンのプロトタイプ。

- 原仕様: [`docs/SPEC.md`](docs/SPEC.md)
- **実現性レビューと修正版バジェット: [`docs/FEASIBILITY_REVIEW.md`](docs/FEASIBILITY_REVIEW.md)** — 実装はこちらに従う

## 現状

ホストでビルド・テストできるコア部分のみ。Android ランタイムはまだ無い。

| 仕様 | 場所 |
|---|---|
| §3.3 2ch 法線（ASTC `-normal` / `.ga`） | `engine/include/apex/normal_codec.hpp`, `shaders/include/normal_codec.glsl` |
| §3.4 法線分散→ラフネス焼き込み（vMF） | `engine/src/specular_aa.cpp` |
| §3.2 トライプラナー・マイクロディテール | `shaders/include/triplanar_microdetail.glsl`, `shaders/gbuffer.frag` |
| §4.1 クラスタカリング（視錐台・コーン・HZB） | `shaders/cluster_cull.comp`, CPU 参照 `engine/src/cluster_cull.cpp` |
| §5 シード駆動プロシージャル都市 | `engine/src/citygen.cpp` |

## ビルド

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
scripts/validate_shaders.sh   # glslangValidator が必要
```
