#!/usr/bin/env bash
set -euo pipefail
root="${NVR_HOME:?Use ./nvr build}"
web=ON
[[ "${1:-}" != --cli-only ]] || web=OFF
deps="$root/.local/deps"
bash "$root/tools/setup.sh" "${1:-}"
export PATH="$deps/node/bin:$PATH"
# Copied CMake caches retain absolute paths to the previous checkout.
if [[ -f "$root/.local/build/CMakeCache.txt" ]] \
    && ! grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$root" "$root/.local/build/CMakeCache.txt"; then
    backup="$root/.local/cache/migrated-build-$(date +%Y%m%d-%H%M%S)-$$"
    echo "Preserving old build cache at $backup"
    mv "$root/.local/build" "$backup"
fi
toolchain=()
[[ -z "${NVR_TOOLCHAIN_FILE:-}" ]] || toolchain=("-DCMAKE_TOOLCHAIN_FILE=$NVR_TOOLCHAIN_FILE")
cmake -S "$root" -B "$root/.local/build" "${toolchain[@]}" -DCMAKE_BUILD_TYPE=Release \
    -DROCKCHIP_SDK="${ROCKCHIP_SDK:-$deps}" -DMPP_ROOT="${MPP_ROOT:-$deps/mpp}" \
    -DCMAKE_PREFIX_PATH="$deps/web/usr;$deps/web/usr/lib/aarch64-linux-gnu" \
    -DNVR_BUILD_WEB="$web"
compile_start=$(date +%s)
cmake --build "$root/.local/build" --parallel "${JOBS:-4}"
echo "C++ compilation/link: $(($(date +%s) - compile_start)) seconds"
if [[ "$web" == ON ]]; then
    mkdir -p "$root/.local/cache"
    cd "$root/web"
    web_key="$(
        sha256sum package.json package-lock.json
        node -p 'JSON.stringify([process.version, process.platform, process.arch])'
    )"
    stamp="$root/.local/cache/web-dependencies"
    # A copied or incomplete install can have a lockfile but lack native Rollup.
    if [[ ! -f node_modules/.package-lock.json || ! -f "$stamp" || "$(cat "$stamp")" != "$web_key" ]] \
        || ! node -e 'require("rollup")' >/dev/null 2>&1; then
        npm ci --include=dev --include=optional --no-audit --no-fund
        node -e 'require("rollup")'
        printf '%s\n' "$web_key" > "$stamp"
    fi
    npm run build -- --outDir ../.local/build/web --emptyOutDir
fi
