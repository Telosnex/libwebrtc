#!/usr/bin/env python3
"""Validate split Apple artifacts before producing a single XCFramework.

The full release still includes EVERY original platform. Parallel jobs must
share source revisions/profile and each must supply exactly its assigned slices.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

GROUPS = {
    "macos": ["macOS-x64", "macOS-arm64"],
    "ios-device": ["iOS-arm64-device"],
    "ios-simulator": ["iOS-x64-simulator", "iOS-arm64-simulator"],
    "catalyst": ["catalyst-arm64", "catalyst-x64"],
    "tvos": ["tvOS-arm64-device", "tvOS-arm64-simulator"],
    "visionos": ["xrOS-arm64-device", "xrOS-arm64-simulator"],
}


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def revision(root):
    return subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()


def files(root):
    """Hash regular bytes and symlink destinations without following links."""
    result = {}
    for path in sorted(root.rglob("*")):
        name = str(path.relative_to(root))
        if path.is_symlink():
            result[name] = {"link": str(path.readlink())}
        elif path.is_file():
            result[name] = {"sha256": sha(path)}
    return result


def record(out, group, mode, wrapper, core):
    # No standalone slice is marked good until tests (macos) and compiles pass.
    payload = {"schema": 1, "group": group, "profile": mode,
               "wrapper": revision(wrapper), "core": revision(core),
               "toolchain": subprocess.check_output(["xcodebuild", "-version"], text=True).strip(),
               "slices": GROUPS[group], "frameworks": {},
               "ownership_tests_passed": group == "macos"}
    for target in GROUPS[group]:
        framework = out / target / "WebRTC.framework"
        if not (framework / "WebRTC").is_file():
            raise ValueError(f"Missing framework binary: {target}")
        payload["frameworks"][target] = files(framework)
    license_path = out / f"{group}.LICENSE"
    license_path.write_bytes((core / "LICENSE").read_bytes())
    payload["license_sha256"] = sha(license_path)
    (out / f"{group}.json").write_text(json.dumps(payload, indent=2) + "\n")


def verify(out, mode, wrapper_revision):
    common = None
    for group, slices in GROUPS.items():
        payload = json.loads((out / f"{group}.json").read_text())
        if payload.get("schema") != 1 or payload.get("group") != group or payload.get("slices") != slices:
            raise ValueError(f"Wrong or missing slices for {group}")
        if payload.get("profile") != mode or payload.get("wrapper") != wrapper_revision:
            raise ValueError(f"Mixed profile/source revision for {group}")
        if not isinstance(payload.get("core"), str) or len(payload["core"]) != 40:
            raise ValueError(f"Missing core revision for {group}")
        identity = (payload["core"], payload["toolchain"], payload["license_sha256"])
        if common is not None and identity != common:
            raise ValueError(f"Mixed core/Xcode/license in {group}")
        common = identity
        if group == "macos" and payload.get("ownership_tests_passed") is not True:
            raise ValueError("Required native/Objective-C ownership tests have not passed")
        if sha(out / f"{group}.LICENSE") != payload["license_sha256"]:
            raise ValueError(f"Changed license: {group}")
        for target in slices:
            framework = out / target / "WebRTC.framework"
            if not (framework / "WebRTC").is_file() or files(framework) != payload["frameworks"][target]:
                raise ValueError(f"Incomplete or changed framework: {target}")
    return common


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["record", "verify"])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--mode", choices=["release", "debug"], required=True)
    parser.add_argument("--wrapper", type=Path, required=True)
    parser.add_argument("--core", type=Path)
    parser.add_argument("--group", choices=GROUPS)
    args = parser.parse_args()
    if args.action == "record":
        if not args.group or not args.core:
            parser.error("record requires --group and --core")
        record(args.out, args.group, args.mode, args.wrapper, args.core)
    else:
        verify(args.out, args.mode, revision(args.wrapper))
        print("All 11 Apple slices verified; source, Xcode, profile and required tests match.")


if __name__ == "__main__":
    main()
