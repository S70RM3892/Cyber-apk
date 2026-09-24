#!/usr/bin/env bash
# Compile every shader stage to SPIR-V for Vulkan 1.3 and validate it.
# Requires glslangValidator (Vulkan SDK / `apt install glslang-tools`);
# spirv-val is used when available.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${root}/build/spirv"
mkdir -p "${out}"

command -v glslangValidator >/dev/null || { echo "glslangValidator not found" >&2; exit 1; }

status=0
for src in "${root}"/shaders/*.{comp,frag,vert}; do
    [[ -e "${src}" ]] || continue
    [[ "$(basename "${src}")" == rt_light.frag ]] && continue  # ray-query only (below)
    spv="${out}/$(basename "${src}").spv"
    if glslangValidator -V --target-env vulkan1.3 -I"${root}/shaders" -o "${spv}" "${src}"; then
        if command -v spirv-val >/dev/null; then
            spirv-val --target-env vulkan1.3 "${spv}" || status=1
        fi
    else
        status=1
    fi
done
# Ray-query variants (CMakeLists.txt APEX_RT_SHADERS).
for name in detail.frag buildings.frag ground.frag resolve.frag rt_light.frag; do
    spv="${out}/${name%.frag}_rt.frag.spv"
    if glslangValidator -V --target-env vulkan1.3 -DAPEX_RT=1 -I"${root}/shaders" -o "${spv}" "${root}/shaders/${name}"; then
        if command -v spirv-val >/dev/null; then
            spirv-val --target-env vulkan1.3 "${spv}" || status=1
        fi
    else
        status=1
    fi
done
exit "${status}"
