#!/usr/bin/env bash
# Assemble a native PS5 title (FSELF eboot + libc.prx shim + sce_sys) that links ps5-opengl.
#
# Runs inside the toolchain image (scripts/ps5env). It follows the same integration ps5-opengl's own
# native test builder uses (heap size, malloc wraps, linker script, AGC import stubs) so we stay on the
# path that project validated on hardware.
#
#   scripts/build-title.sh --title-id PPSA27165 --name "PokeMMO Probe" --sources probe \
#       [--assets DIR] [--heap-mib 256] [--download-mib 256] [--content-suffix PROBE] [--define NAME=VALUE]...
#
# --sources DIR   every .c/.cpp in DIR is compiled (C11 / C++20, -O2) together with the ps5-opengl glue.
# Output: build/titles/<TITLE_ID>/dist/<TITLE_ID>/ (folder title) and .zip next to it.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
template=${PS5_NATIVE_APP_TEMPLATE:?run inside the toolchain image (scripts/ps5env)}
sdk=${PS5_PAYLOAD_SDK:?}
prefix=${PS5_OPENGL_PREFIX:?}
glsrc=${PS5_OPENGL_SOURCE:?}

title_id="" name="" sources="" assets="" heap_mib=256 download_mib=256 suffix="" definitions=()
while (($#)); do
    case $1 in
        --title-id) title_id=$2; shift 2 ;;
        --name) name=$2; shift 2 ;;
        --sources) sources=$2; shift 2 ;;
        --assets) assets=$2; shift 2 ;;
        --heap-mib) heap_mib=$2; shift 2 ;;
        --download-mib) download_mib=$2; shift 2 ;;
        --content-suffix) suffix=$2; shift 2 ;;
        --define) definitions+=("$2"); shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ $title_id =~ ^PPSA[0-9]{5}$ ]] || { echo "--title-id must be PPSA + 5 digits" >&2; exit 2; }
[[ -n $name && -d $root/$sources ]] || { echo "--name and an existing --sources directory are required" >&2; exit 2; }
[[ $heap_mib =~ ^[0-9]+$ && $download_mib =~ ^[0-9]+$ ]] || { echo "sizes must be integers" >&2; exit 2; }
suffix=${suffix:-${title_id:4}}
[[ $suffix =~ ^[A-Z0-9]{1,16}$ ]] || { echo "--content-suffix must be 1-16 of A-Z0-9" >&2; exit 2; }

(cd "$prefix" && sha256sum --check --strict --quiet manifest.sha256)

app="$root/build/titles/$title_id"
rm -rf -- "$app"
mkdir -p "$app"
cp "$template/Makefile" "$app/Makefile"
for directory in runtime sce_sys tooling tools; do
    mkdir -p "$app/$directory"
    cp -a "$template/$directory/." "$app/$directory/"
done

# Process heap: ps5-opengl's builder sets an explicit size instead of the template's "unlimited".
heap_source="$app/tooling/native/sce_module_writer.cpp"
heap_default='write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());'
[[ $(grep -Fc "$heap_default" "$heap_source") == 1 ]] || { echo "boilerplate heap hook changed" >&2; exit 2; }
heap_hex=$(printf '0x%xULL' $((heap_mib << 20)))
sed -i "s/$heap_default/write_u64(result.data, result.heap_size, $heap_hex);/" "$heap_source"

# ps5-opengl's app heap replaces the allocator wholesale; partial wrapping is unsafe (see its docs).
link_script="$app/tools/build.sh"
[[ $(grep -Fc -- '--eh-frame-hdr \' "$link_script") == 1 ]] || { echo "boilerplate link step changed" >&2; exit 2; }
sed -i 's/--eh-frame-hdr \\/--eh-frame-hdr --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size \\/' \
    "$link_script"
cp "$template/tooling/native/ps5-pie.ld" "$app/tooling/native/ps5-pie-base.ld"
cp "$glsrc/native-app/ps5-pie.ld" "$app/tooling/native/ps5-pie.ld"
cp "$glsrc/native-app/app-symbols.map" "$app/tooling/native/app-symbols.map"

mkdir -p "$app/src" "$app/include" "$app/vendor"
cp "$glsrc/native-app/runtime_shims.c" "$app/src/runtime_shims.c"
cp "$glsrc/native-app/app_heap.c" "$app/src/app_heap.c"
find "$root/$sources" -maxdepth 1 -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
    -exec cp {} "$app/src/" \;
for headers in EGL GL KHR; do cp -a "$prefix/include/$headers" "$app/include/"; done

if [[ -n $assets ]]; then
    [[ -d $root/$assets ]] || { echo "assets directory missing: $assets" >&2; exit 2; }
    mkdir -p "$app/assets"
    cp -a "$root/$assets/." "$app/assets/"
fi

# Title metadata: ps5-opengl's param.json (address-space and page-table settings) with our identity.
python3 - "$glsrc/native-app/param.json" "$app/sce_sys/param.json" "$title_id" "$name" "$suffix" "$download_mib" <<'PY'
import json, sys
from pathlib import Path
source, target, title_id, name, suffix, download = sys.argv[1:]
param = json.loads(Path(source).read_text())
param["titleId"] = title_id
param["conceptId"] = title_id[4:]
param["contentId"] = f"UP9000-{title_id}_00-" + ("PKMMO" + suffix).ljust(16, "0")[:16]
param["localizedParameters"]["en-US"]["titleName"] = name
param["downloadDataSize"] = int(download)
Path(target).write_text(json.dumps(param, indent=2) + "\n")
PY
python3 "$glsrc/tools/native-display-metadata.py" "$app/sce_sys/param.json" --fps 120

# One linker group: ps5-opengl plus the C++ runtime it needs.
compiler_runtime=$("${PS5_CLANG:-clang-18}" --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a
{
    printf 'SEARCH_DIR("%s")\nSEARCH_DIR("%s")\n' "$sdk/target/lib" "$prefix/lib"
    printf 'EXTERN(ps5_agc_gate2_run)\nGROUP (\n'
    for library in "$prefix/lib/libPS5OpenGL.a" "$sdk/target/lib/libunwind.a" "$sdk/target/lib/libc++abi.a" \
        "$sdk/target/lib/libc++.a" "$compiler_runtime"; do
        [[ -s $library ]] || { echo "missing library: $library" >&2; exit 2; }
        printf '  "%s"\n' "$library"
    done
    printf ')\n'
} > "$app/vendor/libps5_opengl_group.a"
{
    printf 'APP_INCLUDE_PATHS = include\nAPP_STATIC_ARCHIVES = vendor/libps5_opengl_group.a\n'
    printf 'APP_DEFINITIONS = GL_GLEXT_PROTOTYPES=1 %s\n' "${definitions[*]:-}"
} > "$app/.env"

# A private copy of the SDK: the AGC import stubs are written into it.
mkdir -p "$app/.deps"
cp -a "$template/.deps/native" "$app/.deps/native"
app_sdk="$app/.deps/native/ps5-payload-sdk"
mkdir -p "$app/build/native-imports"
for stub in agc_link_stub:libSceAgc agc_driver_link_stub:libSceAgcDriver; do
    source=${stub%%:*} library=${stub##*:}
    PS5_PAYLOAD_SDK="$app_sdk" sh "$app/tooling/prospero-clang18" -std=c11 -O2 -fPIC -ffunction-sections -fdata-sections \
        -c "$glsrc/native-app/$source.c" -o "$app/build/native-imports/$source.o"
    "$app_sdk/bin/prospero-lld" --shared -soname "$library.prx" -o "$app_sdk/target/lib/$library.so" \
        "$app/build/native-imports/$source.o"
done

make -C "$app" --no-print-directory app

dist="$app/dist/$title_id"
for required in eboot.bin sce_module/libc.prx sce_sys/param.json; do
    [[ -s $dist/$required ]] || { echo "missing output: $required" >&2; exit 1; }
done
printf '\nTitle folder: %s\nZip:          %s\n' "${dist#"$root"/}" "${dist#"$root"/}.zip"
