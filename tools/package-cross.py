"""Package ARM64 outputs and shared libraries without executing target code."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

root = Path(os.environ["NVR_HOME"])
dist = root / ".local/dist"
dist.mkdir(parents=True, exist_ok=True)
runtime_names = ("rk-nvr", "rk-nvr-worker", "rk-nvr-server", "rk-model-info")
search = [root / ".local/deps" / name / "lib" for name in ("rga", "rknn", "mpp")]
search += [Path("/usr/lib/aarch64-linux-gnu"), Path("/lib/aarch64-linux-gnu")]
lookup = {}
for directory in search:
    for path in directory.glob("*.so*"):
        if path.is_file():
            lookup.setdefault(path.name, path)
# Some dependencies live in loader-specific subdirectories (e.g. PulseAudio).
for directory in search:
    for path in directory.rglob("*.so*"):
        if path.is_file():
            lookup.setdefault(path.name, path)

# Fresh staging excludes stale assets/test tools; retain the last archive on failure.
with tempfile.TemporaryDirectory(prefix="package-", dir=dist) as temporary:
    stage = Path(temporary) / "rockchip-yolo-cpp"
    binary = stage / ".local/build/bin"
    libraries = stage / ".local/lib"
    binary.mkdir(parents=True)
    libraries.mkdir(parents=True)
    shutil.copy2(root / "nvr", stage / "nvr")
    shutil.copytree(root / ".local/build/web", stage / ".local/build/web")
    pending = []
    for name in runtime_names:
        path = root / ".local/build/bin" / name
        if not path.is_file():
            raise RuntimeError(f"Missing runtime executable: {path}")
        shutil.copy2(path, binary / name)
        pending.append(path)
    seen = set()
    while pending:
        path = pending.pop()
        header = subprocess.check_output(["aarch64-linux-gnu-readelf", "-h", str(path)], text=True)
        if "AArch64" not in header:
            raise RuntimeError(f"Not ARM64: {path}")
        info = subprocess.check_output(["aarch64-linux-gnu-readelf", "-d", str(path)], text=True)
        for name in re.findall(r"\(NEEDED\).*?\[(.*?)\]", info):
            if name in seen:
                continue
            seen.add(name)
            found = lookup.get(name)
            if found is None:
                raise RuntimeError(f"Unresolved target library: {name} (from {path})")
            # Use board glibc/loader. This package targets Ubuntu 26, not older OSes.
            if name in {
                "libc.so.6",
                "libm.so.6",
                "libpthread.so.0",
                "libdl.so.2",
                "librt.so.1",
                "ld-linux-aarch64.so.1",
            }:
                continue
            shutil.copy2(found.resolve(), libraries / name)
            pending.append(found)
    archive_path = Path(temporary) / "runtime.tar.gz"
    with tarfile.open(archive_path, "w:gz") as archive:
        archive.add(stage, arcname="rockchip-yolo-cpp")
    os.replace(archive_path, dist / "rockchip-yolo-cpp-ubuntu26-arm64.tar.gz")
print(
    f"Packaged {len(runtime_names)} executables and {len(seen)} referenced ARM64 shared-library names"
)
