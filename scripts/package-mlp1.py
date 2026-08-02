#!/usr/bin/env python3
"""Create a deterministic Video From Hell pak archive."""

import argparse
import pathlib
import stat
import zipfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True)
    parser.add_argument("--archive", required=True)
    args = parser.parse_args()

    package = pathlib.Path(args.package)
    archive = pathlib.Path(args.archive)
    if not package.is_dir() or package.name != "VideoFromHell.pak":
        raise SystemExit(f"invalid package directory: {package}")

    files = sorted(path for path in package.rglob("*") if path.is_file())
    if not files:
        raise SystemExit("package contains no files")
    archive.parent.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for path in files:
            relative = pathlib.PurePosixPath(package.name) / path.relative_to(package)
            info = zipfile.ZipInfo(str(relative), date_time=(1980, 1, 1, 0, 0, 0))
            mode = 0o755 if relative.as_posix().endswith(("/launch.sh", "/bin/videofromhell")) else 0o644
            info.external_attr = (stat.S_IFREG | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            output.writestr(info, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)

    print(f"Archive: {archive}")


if __name__ == "__main__":
    main()
