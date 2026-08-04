#!/usr/bin/env python3
"""Validate the app-owned Pak Rat metadata before a future release."""

import json
import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]


def load(path: str) -> dict:
    return json.loads((ROOT / path).read_text(encoding="utf-8"))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"pakrat-metadata-check: {message}")


manifest = load("pak/pak.json")
metadata = load("pakrat.json")
release_lock = load("release-lock.json")

require(metadata.get("schema") == 1, "Pak Rat schema must be 1")
require(metadata.get("id") == "org.umrk.videofromhell", "unexpected app id")
require("compat" not in metadata, "compatibility packages are not supported")
packages = metadata.get("leaf", {}).get("packages")
require(isinstance(packages, list) and len(packages) == 1, "expected one Leaf package")
package = packages[0]
require(package.get("platform") == "mlp1", "only mlp1 may be published")
require(package.get("version") == manifest.get("pak_version"), "metadata/manifest version mismatch")
require(re.fullmatch(r"\d+\.\d+\.\d+", str(package.get("version", ""))) is not None, "invalid version")
require(package.get("artifact_name") == "VideoFromHell.pak.zip", "unexpected artifact name")
require(package.get("install_name") == "VideoFromHell.pak", "unexpected install name")
require(package.get("runtime_manifest_path") == "pak.json", "unexpected manifest path")
require(package.get("package_dir") == "build/mlp1/package/VideoFromHell.pak", "unexpected package dir")
require(package.get("build_command") == ["make", "package-platform", "PLATFORM=mlp1"], "unexpected build command")

require(re.fullmatch(r"[0-9a-f]{40}", str(release_lock.get("catastrophe_commit", ""))) is not None,
        "Catastrophe commit must be a full SHA")
require(re.fullmatch(r"[0-9a-f]{40}", str(release_lock.get("jawaka_commit", ""))) is not None,
        "Jawaka commit must be a full SHA")
require("@sha256:" in str(release_lock.get("mlp1_toolchain_image", "")),
        "toolchain image must be digest-pinned")

print("pakrat-metadata-check: PASS")
