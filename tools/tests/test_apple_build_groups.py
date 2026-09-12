"""Execute group recipe against inert tool stubs: no download/compile/device."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
GROUPS = {"macos": ["macOS-x64", "macOS-arm64"], "ios-device": ["iOS-arm64-device"],
          "ios-simulator": ["iOS-x64-simulator", "iOS-arm64-simulator"],
          "catalyst": ["catalyst-arm64", "catalyst-x64"],
          "tvos": ["tvOS-arm64-device", "tvOS-arm64-simulator"],
          "visionos": ["xrOS-arm64-device", "xrOS-arm64-simulator"]}


class BuildGroupsTest(unittest.TestCase):
    def test_each_group_builds_only_its_targets_and_cache_wraps_every_compile(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = root / "build"
            build.mkdir()
            for d in ["src", "test", "include", "tools"]:
                (root / d).mkdir()
            (root / "BUILD.gn").write_text("")
            (build / "depot_tools").mkdir()
            (build / "src").mkdir()
            (build / "src/LICENSE").write_text("test")
            for file in [".gclient", "libwebrtc_apple_build.sh"]:
                shutil.copy2(ROOT / "build" / file, build / file)
            shutil.copy2(ROOT / "tools/apple_artifacts.py", root / "tools/apple_artifacts.py")
            tools = root / "bin"
            tools.mkdir()
            log = root / "calls.jsonl"
            stub = tools / "tool"
            stub.write_text("""#!/usr/bin/env python3
import os,sys,json
from pathlib import Path
name=Path(sys.argv[0]).name
args=sys.argv[1:]
with open(os.environ['TEST_LOG'],'a') as log: log.write(json.dumps({'tool':name,'args':args})+'\\n')
if name=='git' and args[-1]=='HEAD': print('1'*40)
elif name=='xcodebuild': print('Xcode 26.0\\nBuild 17A324')
elif name=='ninja':
    out=Path(args[args.index('-C')+1]);out.mkdir(parents=True,exist_ok=True)
    if out.name=='ownership-tests':
        for test in ['external_recording_demand_objc_unittests','pcm_factory_unittests','pcm_playout_unittests']:
            p=out/test;p.write_text('#!/bin/sh\\necho test:$0 >> "$TEST_LOG"\\nexit 0\\n');p.chmod(0o755)
    else:
        f=out/'WebRTC.framework';f.mkdir(exist_ok=True);(f/'WebRTC').write_text('binary')
""")
            stub.chmod(0o755)
            for name in ("git", "gclient", "gn", "ninja", "ccache", "xcodebuild"):
                (tools / name).symlink_to("tool")
            env = {**os.environ, "PATH": str(tools) + os.pathsep + os.environ["PATH"],
                   "WEBRTC_BUILD_JOBS": "4", "TEST_LOG": str(log), "CCACHE_DIR": str(root / "cache")}
            for group, expected in GROUPS.items():
                with self.subTest(group=group):
                    log.write_text("")
                    result = subprocess.run([str(build / "libwebrtc_apple_build.sh"), "--profile", "release", "--group", group],
                                            cwd=build, env=env, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    entries = [json.loads(l) for l in log.read_text().splitlines() if l.startswith("{")]
                    generated = [e["args"] for e in entries if e["tool"] == "gn"]
                    actual = [Path(args[1]).name for args in generated]
                    self.assertEqual(actual, (["ownership-tests"] if group == "macos" else []) + expected)
                    for args in generated:
                        self.assertNotIn("--ide=xcode", args)
                        gn = next(a for a in args if a.startswith("--args="))
                        self.assertIn('cc_wrapper="'+str(tools / 'ccache')+'"', gn)
                    for e in entries:
                        if e["tool"] == "ninja": self.assertEqual(e["args"][-2:], ["-j", "4"])
                    self.assertEqual(sum(l.startswith("test:") for l in log.read_text().splitlines()), 3 if group == "macos" else 0)
                    self.assertTrue((build / "out-release" / (group + ".json")).is_file())

    def test_missing_or_invalid_cli_values_fail_before_checkout(self):
        for args in [["--profile"], ["--profile", "invalid"], ["--profile", "release", "--group", "bad"],
                     ["--profile", "release", "--group"], ["--profile", "release", "--group", "macos", "--package-only"]]:
            result = subprocess.run(["sh", str(ROOT / "build/libwebrtc_apple_build.sh"), *args], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("Checkout ref:", result.stdout)


if __name__ == "__main__":
    unittest.main()
