#!/usr/bin/env python3
"""Validate the Video From Hell MLP1 package and archive."""

import argparse
import hashlib
import json
import pathlib
import stat
import struct
import zipfile


def fail(message: str) -> None:
    raise SystemExit(f"package-smoke: {message}")


FFMPEG_LIBS = {
    "libavformat.so.58", "libavcodec.so.58", "libavutil.so.56",
    "libswresample.so.3", "libswscale.so.5",
}
# MPP stays the firmware's, so the binary keeps /usr/lib after its own lib/.
BINARY_RUNPATH = "$ORIGIN/../lib:/usr/lib"


def elf_dynamic(path: pathlib.Path) -> tuple[list[str], str | None]:
    """Return (DT_NEEDED names, DT_RUNPATH) of a little-endian ELF64 file."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        fail(f"{path.name} is not a little-endian ELF64 file")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    for _name, kind, _flags, _addr, offset, size, link, _info, _align, entsize in sections:
        if kind != 6:  # SHT_DYNAMIC
            continue
        strtab = sections[link]
        def string(at: int) -> str:
            start = strtab[4] + at
            return data[start:data.index(b"\0", start)].decode()
        needed, runpath = [], None
        for at in range(offset, offset + size, entsize):
            tag, value = struct.unpack_from("<qQ", data, at)
            if tag == 1:
                needed.append(string(value))
            elif tag == 29:
                runpath = string(value)
        return needed, runpath
    fail(f"{path.name} has no dynamic section")


def check_ffmpeg_runtime(package: pathlib.Path, binary_path: pathlib.Path) -> None:
    lib_dir = package / "lib"
    shipped = {path.name for path in lib_dir.iterdir()} if lib_dir.is_dir() else set()
    if shipped != FFMPEG_LIBS:
        fail(f"lib/ must hold exactly {sorted(FFMPEG_LIBS)}, found {sorted(shipped)}")
    if not (package / "licenses" / "FFmpeg" / "COPYING.LGPLv2.1").is_file():
        fail("missing licenses/FFmpeg/COPYING.LGPLv2.1")
    if not (package / "licenses" / "FFmpeg" / "FFMPEG-SOURCE.txt").is_file():
        fail("missing licenses/FFmpeg/FFMPEG-SOURCE.txt")
    objects = [(binary_path, BINARY_RUNPATH)] + [(lib_dir / name, "$ORIGIN") for name in sorted(shipped)]
    for path, wanted in objects:
        needed, runpath = elf_dynamic(path)
        if runpath != wanted:
            fail(f"{path.name} RUNPATH is {runpath!r}, want {wanted!r}")
        missing = [name for name in needed if name.startswith(("libav", "libsw")) and name not in shipped]
        if missing:
            fail(f"{path.name} needs FFmpeg libraries the pak does not ship: {missing}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()

    package = pathlib.Path(args.package)
    archive = pathlib.Path(args.archive)
    manifest_path = package / "pak.json"
    binary_path = package / "bin" / "videofromhell"
    launch_path = package / "launch.sh"
    icon_path = package / "res" / "icon.png"

    if not package.is_dir() or package.name != "VideoFromHell.pak":
        fail("expected one VideoFromHell.pak package directory")
    if not all(path.is_file() for path in (manifest_path, binary_path, launch_path, icon_path)):
        fail("package is missing pak.json, launch.sh, res/icon.png, or bin/videofromhell")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("pak_version") != args.version:
        fail(f"manifest version {manifest.get('pak_version')!r} != {args.version!r}")
    if manifest.get("platform") != "mlp1":
        fail("manifest platform must be mlp1")
    if binary_path.read_bytes()[:4] != b"\x7fELF":
        fail("videofromhell is not an ELF binary")
    check_ffmpeg_runtime(package, binary_path)
    if not archive.is_file():
        fail("archive is missing")

    installed_size = 0
    with zipfile.ZipFile(archive) as source:
        names = source.namelist()
        if len(names) != len(set(names)):
            fail("archive contains duplicate paths")
        expected_files = {
            f"{package.name}/{path.relative_to(package).as_posix()}"
            for path in package.rglob("*")
            if path.is_file()
        }
        if set(names) != expected_files:
            fail("archive file set does not match the package")
        for info in source.infolist():
            path = pathlib.PurePosixPath(info.filename)
            if path.is_absolute() or ".." in path.parts or path.parts[0] != package.name:
                fail(f"unsafe archive path: {info.filename}")
            mode = info.external_attr >> 16
            if not stat.S_ISREG(mode):
                fail(f"non-regular archive member: {info.filename}")
            wanted = 0o755 if info.filename.endswith(("/launch.sh", "/bin/videofromhell")) else 0o644
            if stat.S_IMODE(mode) != wanted:
                fail(f"wrong mode for {info.filename}: {oct(stat.S_IMODE(mode))}")
            installed_size += info.file_size
        archive_manifest = json.loads(source.read(f"{package.name}/pak.json"))
        if archive_manifest != manifest:
            fail("archive manifest differs from package manifest")

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    print(f"package-smoke: PASS version={args.version} size={archive.stat().st_size} "
          f"installed={installed_size} sha256={digest}")


if __name__ == "__main__":
    main()
