# CYBER-APEX 仕様書 技術レビュー（実現性・裏取り）

対象: [`SPEC.md`](SPEC.md)（原文）
方針: 仕様の各主張を「公開一次情報で裏が取れるか」「物理的・規約的に成立するか」で判定し、成立しない箇所は**実装可能な代替案**に置き換える。
判定: ✅ 妥当 / ⚠️ 条件付き・要修正 / ❌ 不成立（そのままでは実装しない）

---

## 0. 結論（先に読め）

仕様の**骨格（FHD特化テクセル密度、GPU-Drivenクラスタカリング、プロシージャル都市、ASTC、法線分散のラフネス焼き込み、超解像）は業界で実証済みの技術の組み合わせ**で、方向性は正しい。

ただし以下の4点は現状の記述のままだと成立しない。

1. **「NPUで超解像・フレーム生成・NRCを完全オフロード」** — 2026年の実機トレンドは逆方向。Arm も Qualcomm も**GPU内蔵のニューラル演算器**でやっている。別チップのNPUはGPUとのメモリ往復・同期が毎フレーム発生するため、レンダリングのクリティカルパスに置くべきではない。
2. **「ネイティブ30fps → 表示60〜90fps」** — フレーム生成は入力遅延を改善しない。AMD自身が補間前60fps以上を推奨し、30fps未満は「絶対に避けるべき」としている。30fpsベースはアクション操作性の目標と矛盾する。
3. **「実行時にClaudeが低レイヤコードを生成」** — オフライン動作不可、レイテンシ不可、さらに Google Play の *Device and Network Abuse* ポリシーが Play 外からの実行コード（.so 等）のダウンロードを禁止している。**AIはビルド時限定**にする。
4. **「PC版と同等以上」「工学的に証明される」** — 証明は存在しない。仕様書の数値はすべて**目標値**であり、実機計測で検証するまで主張してはならない。

---

## 1. 主張ごとの判定

### 1.1 ヘテロジニアス構成（§1.2, §2, §4.3）

| 主張 | 判定 | 根拠 / 修正 |
|---|---|---|
| 超解像・フレーム補間をNPUへ完全オフロード | ⚠️ | Arm Mali G2-Ultra NX（2026/09発表）はニューラル演算器を**シェーダコア内**に統合。理由は「GPUのメモリ系・コヒーレントキャッシュを再利用し、データ移動を減らす」ため [1]。Arm NSS は 540p→1080p を約4ms/frame [2]。Qualcomm の Adreno Neural Fusion SDK（SR / FG）も Vulkan レンダラ統合前提で Snapdragon 8 Elite Gen 6 以降対象 [3][4]。→ **SR/FGはベンダーSDK（GPU側）を第一選択**、非対応端末は Snapdragon GSR 2（テンポラル、GPU）[5] にフォールバック。 |
| NRC を NPU 上で推論 | ⚠️ | 原論文（Müller et al. 2021）の本質は**レンダリング中のオンライン学習**で、推論だけの仕組みではない。RTX 3090級でFHD時 約2.6ms [6]。モバイルNPU の推論ランタイムは基本的にオンライン学習向けではない。→ **v1 は非ニューラルGI（プローブ／サーフェスキャッシュ＋SDF）**、NRC は GPU ニューラル演算器搭載端末向けの研究トラックに降格。「計算量1/20」は出典なし→削除。 |
| VK_KHR_ray_query で短距離反射 | ⚠️ | Immortalis-G715 以降は対応 [7]。一方 Adreno 830 は拡張を公称しつつ ray query デモが動作しなかった報告がある [8]。→ **機能フラグ必須、SSR＋SDF反射のフォールバックを常設**。 |

### 1.2 テクスチャ（§3）

| 主張 | 判定 | 根拠 / 修正 |
|---|---|---|
| 1K/512/256 クランプ、FHD基準のテクセル密度 | ✅ | 妥当。ただし「4Kネイティブ以上の解像感」はタイリングディテールでは**固有情報は増えない**ので誇張。「至近距離でのボケ緩和」と言い換える。 |
| Albedo: ASTC 6x6 **HDR** | ❌→修正 | Albedo は LDR（sRGB）データで HDR プロファイルは不要。しかも ASTC HDR は Vulkan 1.3 で**オプション機能**扱い [9]。→ **ASTC 6x6 sRGB (LDR)**。HDR は発光テクスチャ／環境マップ限定。 |
| Normal: ASTC 4x4 "RG" | ⚠️→修正 | ASTC に RG フォーマットは無い。正解は `astcenc -normal`（rrrg 符号化、シェーダでは `.ga` サンプル、Z = √(1−X²−Y²)）[10]。**本リポジトリの `shaders/include/normal_codec.glsl` はこれで実装済み**。 |
| PBR Mask: ASTC 8x8 に4ch詰め | ⚠️ | 8x8 は 2 bpp。相関の無い4チャンネルを詰めると誤差が大きい。→ Roughness は法線分散焼き込み後の値なので**品質要求が高い**。R+G（Roughness/Metallic）を 6x6、AO/Cavity を別 8x8 に分割して A/B 評価する。 |
| Toksvig / LEAN で法線分散をラフネスへ | ✅（手法を具体化） | Toksvig は Blinn-Phong 指数向け。GGX では Karis の vMF 近似 `α' = √(α² + 2/κ)`, `1/κ = (1−r²)/(r(3−r²))` を使う [11]。2ch 法線では実行時に平均法線長が取れないので**オフライン焼き込みが必須**（Self Shadow も同指摘 [12]）。**`engine/src/specular_aa.cpp` に実装・テスト済み**。 |

### 1.3 ジオメトリ（§4.1）

| 主張 | 判定 | 根拠 / 修正 |
|---|---|---|
| 64〜128 tri クラスタ、Compute で視錐台・背面・HZB カリング | ✅ | meshoptimizer のコーン判定 [13] と niagara の球投影 HZB 判定（Mara & McGuire 2013）[14] で実装可能。**`shaders/cluster_cull.comp` に実装、SPIR-V コンパイル確認済み**。CPU 参照実装とテストあり。 |
| ポップ無し連続変形 LOD | ⚠️ | Nanite 型のクラスタ DAG が必要で、構築ツールが最重量級。v1 はクラスタ LOD の離散切替＋ディザ遷移、v2 で DAG。 |

### 1.4 フレームレート・遅延（§4.3, §7）

| 主張 | 判定 | 根拠 / 修正 |
|---|---|---|
| ネイティブ30fps → 表示60〜90fps | ❌→修正 | AMD: FG は補間前 60fps 以上推奨、30fps 未満は絶対に避ける [15]。補間は次フレームを待つため遅延が増え、入力はベースfpsでしか反映されない [16]。→ **ベース 45〜60fps、FG は 90/120Hz 表示向けのオプション**。 |

### 1.5 配布サイズ（§5, §7）

| 主張 | 判定 | 根拠 / 修正 |
|---|---|---|
| 1.8〜2.3GB | ✅ | Google Play 上限はベース 500MB、install-time 累計 4GB、on-demand/fast-follow 累計 30GB、総計 34GB [17]。2GB は規約上余裕。「APK」ではなく **AAB＋Play Asset Delivery**（テクスチャ形式ターゲティング）で配る。 |
| シード＋ルールのみで都市を展開 | ✅ | ステートレスなハッシュ関数で任意タイルを独立展開できる。**`engine/src/citygen.cpp` に実装**。注意: `<random>` の分布クラスは実装依存でホスト（libstdc++）と端末（libc++）で結果が変わるため**使用禁止**、自前ハッシュを使う（実装済み）。 |

### 1.6 電力・帯域・メモリ（§7）

| 主張 | 判定 | 根拠 / 修正 |
|---|---|---|
| メモリ帯域 22〜32GB/s、「サーマル限界 60GB/s」 | ⚠️ | 「サーマル限界60GB/s」という規格値は存在しない（出典なし）。参考として Snapdragon 8 Elite Gen 5 の理論ピークは 84.8GB/s [18]。帯域の問題は上限より**DRAMアクセスのエネルギー**。→ 数値は予算目標として残し、AGI で実測して検証。 |
| SoC 5.0〜6.5W で30分以上熱低下なし | ⚠️ | 出典なし。ファンレス筐体の持続電力は機種依存が大きい。→ ADPF（Thermal API / Performance Hint）で温度余裕を監視し、DRS と FG を**実行時に**調整する設計を必須にする。 |
| テクスチャ 650〜900MB / RAM 4GB | ⚠️ | モバイルは VRAM とシステム RAM が共有。8GB 端末では LMK（Low Memory Killer）の危険域。→ 8GB 端末ティアは 2.5GB 上限。 |
| PC版と同等以上の知覚品質 | ❌ | 測定されていない。→ 基準画像との FLIP 等の客観指標＋実機ユーザーテストで判定する項目にする。 |

### 1.7 AI パイプライン（§6）

| 主張 | 判定 | 根拠 / 修正 |
|---|---|---|
| ビルド時のアセット変換・シェーダ最適化・プロファイル解析 | ✅ | ビルド時なら可能。ただし**生成物は必ず決定的なツール（spirv-val、テスト、画像差分）で検証**してからマージする。LLM の出力をそのまま信用しない。 |
| ワープ発散の「完全排除」 | ⚠️ | 一般には不可能。分岐の平坦化は ALU とレジスタ圧力を増やし、占有率を下げることもある。→ 「発散コストを計測して、得な場合のみ平坦化」。 |
| **実行時**の低レイヤコード生成 | ❌ | ネット必須・遅延大・検証不能、かつ Play 外からの実行コード取得は規約違反 [19]。→ 削除。 |

### 1.8 権利

『Cyberpunk 2077』は品質の**参照ベンチマーク**としての言及に留める。アセット・名称・意匠は一切流用しない。

---

## 2. 修正版バジェット（v1 目標）

| 項目 | 原仕様 | 修正版 | 検証方法 |
|---|---|---|---|
| 内部解像度 | 540〜720p | 540〜720p（DRS） | — |
| 出力 | 1080p | 1080p | — |
| ベース fps | 30 | **45〜60** | AGI フレームタイム |
| 表示 fps | 60〜90 | 60（FG無し）/ 90〜120（FG有り、対応端末のみ） | 同上＋遅延計測 |
| SR / FG 実行先 | NPU | **GPU（ベンダーSDK）**、フォールバック SGSR2 | — |
| GI | NRC（NPU） | **非ニューラル**（プローブ＋SDF）、NRC は研究トラック | — |
| 反射 | SSRT + ray query | SSR + SDF、ray query は機能フラグ | 対応表で端末別に確認 |
| Albedo | ASTC 6x6 HDR | **ASTC 6x6 sRGB** | — |
| Normal | ASTC 4x4 "RG" | **ASTC 4x4 `-normal`（.ga）** | — |
| テクスチャ常駐 | 650〜900MB | 12GB端末 900MB / 8GB端末 600MB | メモリプロファイル |
| 電力 | 5〜6.5W | 目標値（未検証）、ADPF で閉ループ制御 | 30分連続の実機計測 |
| AI 利用 | ビルド時＋実行時 | **ビルド時のみ** | — |

---

## 3. このリポジトリで実装済みの範囲

修正版バジェットに沿って、以下が動くAPKになっている（詳細は [README](../README.md)）。

| 仕様 | 実装 | 検証 |
|---|---|---|
| §3.3 法線 2ch 符号化 | `engine/include/apex/normal_codec.hpp`, `shaders/include/normal_codec.glsl` | ユニットテスト |
| §3.4 法線分散→ラフネス焼き込み | `engine/src/specular_aa.cpp` | ユニットテスト |
| §3.2 トライプラナー・マイクロディテール | `shaders/include/triplanar_microdetail.glsl`, `shaders/gbuffer.frag` | SPIR-V コンパイル（テクスチャ資産がないため本体の描画には未接続） |
| §4.1 クラスタカリング | `shaders/cluster_cull.comp`, `engine/src/cluster_cull.cpp` | ユニットテスト＋SPIR-V（本体は現状インスタンス描画で足りており未接続） |
| §4.3 動的解像度 | `renderer/renderer.cpp`（GPUタイムスタンプで0.5〜0.75倍を制御） | lavapipe上で縮小動作を確認 |
| §5 手続き都市・ストリーミング | `engine/src/citygen.cpp`, `engine/src/world.cpp` | ユニットテスト（決定性、タイル境界、道路非干渉、段状マッシング） |
| 描画（濡れた路面SSR、ブルーム、ACES、雨、霧、ネオン） | `renderer/`, `shaders/` | lavapipe＋検証レイヤーでエラーなし、スクリーンショット |
| ゲーム（歩行・ジャンプ・運転・ギグ）、HUD、手続き音声 | `engine/src/game.cpp`, `hud.cpp`, `audio.cpp` | ユニットテスト |
| Android実行系（スワップチェーンのpre-rotation、タッチ、AAudio） | `android/`, `renderer/presenter.cpp` | present経路をheadless surfaceで検証。**実機は未検証** |

未実装: ニューラル超解像・フレーム生成（ベンダーSDK統合）、GI、レイトレース反射、アセットパイプラインCLI（astcenc連携）、Rust I/O ランタイム、ADPF による熱制御。

---

## 出典

1. Unite.AI, "Arm Unveils Mali G2-Ultra NX, Its First AI-Native Mobile GPU" — https://www.unite.ai/arm-unveils-mali-g2-ultra-nx-its-first-ai-native-mobile-gpu/
2. Arm Newsroom, "Arm Neural Technology…" — https://newsroom.arm.com/news/arm-announces-arm-neural-technology
3. Qualcomm OnQ, "Adreno Neural Fusion: AI rendering for mobile gaming" (2026/09) — https://www.qualcomm.com/news/onq/2026/09/adreno-neural-fusion-ai-rendering
4. SnapdragonGameStudios/adreno-neural-fusion — https://github.com/SnapdragonGameStudios/adreno-neural-fusion
5. Qualcomm, "Introducing Snapdragon Game Super Resolution 2" — https://www.qualcomm.com/developer/blog/2024/10/introducing-snapdragon-game-super-resolution-2
6. Müller, Rousselle, Novák, Keller, "Real-time Neural Radiance Caching for Path Tracing", ACM TOG 2021 — https://arxiv.org/abs/2106.12372
7. Arm Learning Path, "Implement ray tracing effects with Vulkan on Android" — https://learn.arm.com/learning-paths/mobile-graphics-and-gaming/ray_tracing/rt02_setup/
8. "Ray Tracing with Bindless Vulkan on Mobile Devices, a Case Study: Performance", SIGGRAPH 2025 Talks — https://dl.acm.org/doi/10.1145/3721239.3734103
9. Khronos, VK_EXT_texture_compression_astc_hdr — https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_texture_compression_astc_hdr.html
10. ARM astc-encoder, "Effective ASTC Encoding" — https://github.com/ARM-software/astc-encoder/blob/main/Docs/Encoding.md
11. B. Karis, "Normal map filtering using vMF (part 3)" — http://graphicrants.blogspot.com/2018/05/normal-map-filtering-using-vmf-part-3.html
12. Self Shadow, "Specular Showdown in the Wild West" — https://blog.selfshadow.com/2011/07/22/specular-showdown/
13. zeux/meshoptimizer README（クラスタのコーンカリング）— https://github.com/zeux/meshoptimizer
14. zeux/niagara `src/shaders/math.h`（projectSphere, MIT）— https://github.com/zeux/niagara
15. AMD GPUOpen, "AMD FSR 3 game integrations out now + more details for developers" — https://gpuopen.com/news/fsr3-in-games-technical-details/
16. TechSpot, "AMD FSR 3 Frame Generation Analyzed" — https://www.techspot.com/article/2747-amd-fsr-3-tech/
17. Play Console Help, "Optimize your app's size and stay within Google Play app size limits" — https://support.google.com/googleplay/android-developer/answer/9859372
18. Jon Peddie Research, "Qualcomm Snapdragon 8 Elite Gen 5" — https://www.jonpeddie.com/news/qualcomm-snapdragon-8-elite-gen-5/
19. Google Play Policy, "Device and Network Abuse" — https://support.google.com/googleplay/android-developer/answer/16559646
