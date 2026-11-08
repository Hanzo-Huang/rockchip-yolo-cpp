#!/usr/bin/env bash
set -euo pipefail
root="${NVR_HOME:?Use ./nvr cross-build}"
if [[ "${1:-}" != --inside ]]; then
    [[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || {
        echo "Cross-build requires an x86_64 Linux host with Docker." >&2
        exit 1
    }
    command -v docker > /dev/null || {
        echo "Install Docker on the x86 build host first." >&2
        exit 1
    }
    image=rockchip-yolo-cpp-cross:ubuntu26
    recipe=$(sha256sum "$root/tools/cross.Dockerfile" | cut -d' ' -f1)
    installed=$(docker image inspect --format '{{index .Config.Labels "org.rockchip-yolo-cpp.cross-recipe"}}' "$image" 2> /dev/null || true)
    if [[ "$installed" != "$recipe" || "${CROSS_REBUILD_IMAGE:-0}" == 1 ]]; then
        docker build -t "$image" --label "org.rockchip-yolo-cpp.cross-recipe=$recipe" \
            --build-arg "UBUNTU_MIRROR=${UBUNTU_MIRROR:-http://archive.ubuntu.com/ubuntu}" \
            --build-arg "UBUNTU_PORTS_MIRROR=${UBUNTU_PORTS_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}" \
            -f "$root/tools/cross.Dockerfile" "$root/tools"
    fi
    workspace="$root/.local/cross/project"
    mkdir -p "$workspace/.local/cache/home"
    tar -C "$root" --exclude=./.git --exclude=./.local --exclude=./tmp --exclude=./.cross-build.log \
        --exclude=./web/node_modules -cf - . | tar -C "$workspace" -xf -
    docker run --rm --user "$(id -u):$(id -g)" \
        -e HOME=/project/.local/cache/home -e NVR_HOME=/project -e JOBS="${JOBS:-$(nproc)}" \
        -v "$workspace:/project" "$image" bash /project/tools/cross-build.sh --inside
    mkdir -p "$root/.local/dist"
    cp "$workspace/.local/dist/rockchip-yolo-cpp-ubuntu26-arm64.tar.gz" "$root/.local/dist/"
    echo "Board package: $root/.local/dist/rockchip-yolo-cpp-ubuntu26-arm64.tar.gz"
    exit
fi
export NVR_CROSS=1
export NVR_TOOLCHAIN_FILE="$root/tools/aarch64.cmake"
export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
export PKG_CONFIG_PATH=
start=$(date +%s)
bash "$root/tools/build.sh"
echo "Build phase (setup + C++ + web, excludes packaging): $(($(date +%s) - start)) seconds"
mkdir -p "$root/.local/dist"
python3 "$root/tools/package-cross.py"
