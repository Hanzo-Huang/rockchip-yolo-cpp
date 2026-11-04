#!/usr/bin/env bash
# Automatic build dependencies; leave system SDK libraries and drivers untouched.
set -euo pipefail
root="${NVR_HOME:?Use ./nvr setup}"
deps="$root/.local/deps"
cache="$root/.local/cache"
mkdir -p "$deps" "$cache"
web=ON
[[ "${1:-}" != --cli-only ]] || web=OFF
command -v apt-get > /dev/null || {
    echo "Automatic setup requires Debian/Ubuntu (apt-get)." >&2
    exit 1
}
packages=(build-essential cmake pkg-config git curl ca-certificates xz-utils python3
    ffmpeg libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libavdevice-dev
    nlohmann-json3-dev libdrm-dev)
if [[ "$web" == ON ]]; then
    # apt resolves release-specific runtime names; do not pin libjsoncpp25 etc.
    packages+=(libjsoncpp-dev libyaml-cpp-dev libsqlite3-dev libarchive-dev zlib1g-dev uuid-dev)
fi
missing=()
for package in "${packages[@]}"; do
    [[ "$(dpkg-query -W -f='${Status}' "$package" 2> /dev/null || true)" == 'install ok installed' ]] || missing+=("$package")
done
if [[ "${NVR_CROSS:-0}" == 1 ]]; then
    missing=() # Target packages are supplied by the multiarch build image.
fi
if ((${#missing[@]})); then
    echo "Installing missing build dependencies: ${missing[*]}"
    elevate=()
    if [[ "$EUID" != 0 ]]; then
        command -v sudo > /dev/null || {
            echo "Install these packages as root: ${missing[*]}" >&2
            exit 1
        }
        elevate=(sudo)
    fi
    "${elevate[@]}" apt-get update
    "${elevate[@]}" apt-get install -y --no-install-recommends "${missing[@]}"
fi
export PATH="$deps/node/bin:$PATH"
if [[ "$web" == ON ]] && ! (command -v npm > /dev/null && node -e '
  const [major,minor]=process.versions.node.split(".").map(Number);
  process.exit((major===20&&minor>=19)||(major===22&&minor>=12)||major>22?0:1);
' 2> /dev/null); then
    echo "Installing private Node.js 22.22.0 for the web build"
    [[ "$(uname -m)" == aarch64 ]] || {
        echo "Install a current native Node.js on the cross-build host/image." >&2
        exit 1
    }
    archive="$cache/node-v22.22.0-linux-arm64.tar.xz"
    curl -fL --retry 3 --connect-timeout 15 -o "$archive" \
        https://nodejs.org/download/release/v22.22.0/node-v22.22.0-linux-arm64.tar.xz
    printf '%s  %s\n' 1bf1eb9ee63ffc4e5d324c0b9b62cf4a289f44332dfef9607cea1a0d9596ba6f "$archive" | sha256sum -c -
    mkdir -p "$deps/node"
    tar -xJf "$archive" --strip-components=1 -C "$deps/node"
fi

# Official source snapshots pinned by commit, cached for subsequent builds.
fetch_source()
{
    local name="$1" repo="$2" revision="$3" destination="$cache/$1-$3"
    if [[ ! -f "$destination/.nvr-ready" ]]; then
        echo "Downloading $name ($revision)" >&2
        mkdir -p "$destination" || return 1
        if [[ "$name" == rknn || "$name" == rga ]]; then
            # Fetch only ARM64 runtime/headers, not SDK docs, wheels or other CPUs.
            [[ -d "$destination/.git" ]] || git init "$destination" >&2 || return 1
            git -C "$destination" config remote.origin.url "https://github.com/$repo.git" || return 1
            git -C "$destination" config remote.origin.promisor true || return 1
            git -C "$destination" config remote.origin.partialclonefilter blob:none || return 1
            git -C "$destination" fetch --depth 1 --filter=blob:none \
                origin "$revision" >&2 || return 1
            if [[ "$name" == rknn ]]; then
                git -C "$destination" sparse-checkout set \
                    rknpu2/runtime/Linux/librknn_api/include \
                    rknpu2/runtime/Linux/librknn_api/aarch64 >&2 || return 1
            else
                git -C "$destination" sparse-checkout set include libs/Linux/gcc-aarch64 >&2 || return 1
            fi
            git -C "$destination" checkout --detach "$revision" >&2 || return 1
        else
            curl -fL --retry 3 --connect-timeout 15 -o "$cache/$name-$revision.tar.gz" \
                "https://codeload.github.com/$repo/tar.gz/$revision" || return 1
            tar -xzf "$cache/$name-$revision.tar.gz" --strip-components=1 -C "$destination" || return 1
        fi
        touch "$destination/.nvr-ready" || return 1
    fi
    printf '%s\n' "$destination"
}
copy_sdk()
{
    local name="$1" headers="$2" libraries="$3"
    [[ -d "$headers" && -d "$libraries" ]] || {
        echo "Invalid $name SDK paths: $headers, $libraries" >&2
        exit 1
    }
    mkdir -p "$deps/$name/include" "$deps/$name/lib"
    cp -a "$headers/." "$deps/$name/include/"
    cp -a "$libraries/." "$deps/$name/lib/"
}
sdk="${ROCKCHIP_SDK:-${RK_SDK_PROJECT:+$RK_SDK_PROJECT/third_party}}"
mpp="${MPP_ROOT:-${RK_SDK_PROJECT:+$RK_SDK_PROJECT/build-rk3576/deps/mpp}}"
toolchain=()
[[ -z "${NVR_TOOLCHAIN_FILE:-}" ]] || toolchain=("-DCMAKE_TOOLCHAIN_FILE=$NVR_TOOLCHAIN_FILE")
if [[ ! -f "$deps/rga/include/im2d.hpp" || ! -e "$deps/rga/lib/librga.so" ]]; then
    if [[ -n "$sdk" ]]; then
        copy_sdk rga "$sdk/rga/include" "$sdk/rga/lib"
    else
        source=$(fetch_source rga airockchip/librga 2b32edcb97b601b25683e2941d888c8515da6d55)
        copy_sdk rga "$source/include" "$source/libs/Linux/gcc-aarch64"
    fi
fi
if [[ ! -f "$deps/rknn/include/rknn_api.h" || ! -e "$deps/rknn/lib/librknnrt.so" ]]; then
    if [[ -n "$sdk" ]]; then
        copy_sdk rknn "$sdk/rknn/include" "$sdk/rknn/lib"
    else
        source=$(fetch_source rknn airockchip/rknn-toolkit2 42aa1d426c0a9e0869b6374edba009f7208a1926)
        runtime="$source/rknpu2/runtime/Linux/librknn_api"
        copy_sdk rknn "$runtime/include" "$runtime/aarch64"
    fi
fi
if [[ ! -f "$deps/mpp/include/rockchip/rk_mpi.h" && ! -f "$deps/mpp/include/rk_mpi.h" ]] \
    || [[ ! -e "$deps/mpp/lib/librockchip_mpp.so" ]]; then
    if [[ -n "$mpp" ]]; then
        copy_sdk mpp "$mpp/include" "$mpp/lib"
    else
        source=$(fetch_source mpp rockchip-linux/mpp 7916c3cc000fefc0bdf96636b7d54fe174724f44)
        cmake -S "$source" -B "$cache/mpp-build" "${toolchain[@]}" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$deps/mpp" \
            -DCMAKE_INSTALL_LIBDIR=lib -DRKPLATFORM=ON -DHAVE_DRM=ON -DBUILD_TEST=OFF
        cmake --build "$cache/mpp-build" --parallel "${JOBS:-4}"
        cmake --install "$cache/mpp-build"
    fi
fi
(cd "$deps" && find rga rknn mpp -type f -exec sha256sum {} + > SHA256SUMS.txt)
prefix="$deps/web"
if [[ "$web" == ON && ! -f "$prefix/usr/lib/cmake/Drogon/DrogonConfig.cmake" && ! -f "$prefix/usr/lib/aarch64-linux-gnu/cmake/Drogon/DrogonConfig.cmake" ]]; then
    [[ -d "$cache/drogon" ]] || git clone --branch v1.9.11 --depth 1 --recurse-submodules --shallow-submodules \
        https://github.com/drogonframework/drogon.git "$cache/drogon"
    cmake -S "$cache/drogon" -B "$cache/drogon-build" "${toolchain[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$prefix/usr" \
        -DBUILD_CTL=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_ORM=OFF \
        -DBUILD_BROTLI=OFF -DBUILD_YAML_CONFIG=OFF -DBUILD_SHARED_LIBS=OFF
    cmake --build "$cache/drogon-build" --parallel "${JOBS:-4}"
    cmake --install "$cache/drogon-build"
fi
echo "Dependencies ready."
