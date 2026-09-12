import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("apple_artifacts", ROOT / "tools/apple_artifacts.py")
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)


class ArtifactsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.out = self.root / "out-release"
        self.out.mkdir()
        self.core = self.root / "core"
        self.core.mkdir()
        (self.core / "LICENSE").write_text("license")
        for group, targets in a.GROUPS.items():
            for target in targets:
                framework = self.out / target / "WebRTC.framework"
                binary = framework / "Versions/A/WebRTC"
                binary.parent.mkdir(parents=True)
                binary.write_text(target)
                (framework / "Versions/Current").symlink_to("A")
                (framework / "WebRTC").symlink_to("Versions/Current/WebRTC")
            with patch.object(a, "revision", return_value="1" * 40), patch.object(
                a.subprocess, "check_output", return_value="Xcode 26.0\nBuild 17A324\n"
            ):
                a.record(self.out, group, "release", self.root, self.core)

    def verify(self):
        return a.verify(self.out, "release", "1" * 40)

    def test_full_platform_set_and_symlinks(self):
        self.assertEqual(sum(map(len, a.GROUPS.values())), 11)
        self.verify()
        self.assertEqual(a.files(self.out / "macOS-x64/WebRTC.framework")["WebRTC"],
                         {"link": "Versions/Current/WebRTC"})

    def test_tampered_bytes_or_missing_architecture_rejected(self):
        binary = self.out / "macOS-x64/WebRTC.framework/Versions/A/WebRTC"
        binary.write_text("wrong binary")
        with self.assertRaisesRegex(ValueError, "changed framework"):
            self.verify()
        binary.unlink()
        with self.assertRaisesRegex(ValueError, "changed framework"):
            self.verify()

    def test_mixed_commits_profiles_toolchains_or_skipped_tests_rejected(self):
        path = self.out / "macos.json"
        original = json.loads(path.read_text())
        for field, wrong in [("wrapper", "2" * 40), ("profile", "debug"),
                             ("core", "2" * 40), ("toolchain", "Xcode 25"),
                             ("ownership_tests_passed", False), ("slices", ["macOS-arm64"])]:
            with self.subTest(field=field):
                path.write_text(json.dumps({**original, field: wrong}))
                with self.assertRaises(ValueError):
                    self.verify()
        path.write_text(json.dumps(original))
        self.verify()

    def test_package_only_does_not_checkout_compile_or_run_gclient(self):
        # Actual shell packaging path, fake lipo/xcodebuild/zip (not native build).
        wrapper = self.root / "wrapper"
        (wrapper / "build").mkdir(parents=True)
        (wrapper / "tools").mkdir()
        script = wrapper / "build/libwebrtc_apple_build.sh"
        script.write_bytes((ROOT / "build/libwebrtc_apple_build.sh").read_bytes())
        script.chmod(0o755)
        (wrapper / "tools/apple_artifacts.py").write_bytes((ROOT / "tools/apple_artifacts.py").read_bytes())
        # Revision verification is real; make a tiny local git checkout.
        subprocess.run(["git", "init", "-q", str(wrapper)], check=True)
        subprocess.run(["git", "-C", str(wrapper), "add", "."], check=True)
        subprocess.run(["git", "-C", str(wrapper), "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                        "commit", "-qm", "test"], check=True)
        revision = a.revision(wrapper)
        import shutil
        dest = wrapper / "build/out-release"
        shutil.copytree(self.out, dest, symlinks=True)
        for manifest in dest.glob("*.json"):
            data = json.loads(manifest.read_text())
            data["wrapper"] = revision
            manifest.write_text(json.dumps(data))
        tools = self.root / "bin"
        tools.mkdir()
        log = self.root / "tools.log"
        stub = tools / "tool"
        stub.write_text("""#!/usr/bin/env python3
import os,sys,shutil
from pathlib import Path
name=Path(sys.argv[0]).name
with open(os.environ['TEST_LOG'],'a') as log: log.write(name+'\\n')
args=sys.argv[1:]
if name in ('git','gclient','gn','ninja'):
    raise SystemExit('Unexpected build work during package-only')
if name=='lipo':
    # Read inputs first: fixture single-device lipo input is also output.
    out=Path(args[args.index('-output')+1]); data=Path(args[-1]).read_bytes()
    out.unlink(); out.write_bytes(data)
elif name=='xcodebuild':
    out=Path(args[-1]); out.mkdir()
    maps={'iOS-device-lib':'ios-arm64','iOS-simulator-lib':'ios-arm64_x86_64-simulator',
          'macOS-lib':'macos-arm64_x86_64','catalyst-lib':'ios-arm64_x86_64-maccatalyst',
          'xrOS-arm64-device':'xros-arm64','xrOS-arm64-simulator':'xros-arm64-simulator',
          'tvOS-arm64-device':'tvos-arm64','tvOS-arm64-simulator':'tvos-arm64-simulator'}
    for i,arg in enumerate(args):
        if arg=='-framework':
            src=Path(args[i+1]);shutil.copytree(src,out/maps[src.parent.name]/src.name,symlinks=True)
elif name=='zip': Path('WebRTC.xcframework.zip').write_text('packaged')
""")
        stub.chmod(0o755)
        for name in ("lipo", "xcodebuild", "zip", "gclient", "gn", "ninja"):
            (tools / name).symlink_to("tool")
        env = {**os.environ, "PATH": str(tools) + os.pathsep + os.environ["PATH"],
               "WEBRTC_BUILD_JOBS": "4", "TEST_LOG": str(log)}
        env.pop("CCACHE_DIR", None)
        result = subprocess.run([str(script), "--profile", "release", "--package-only"],
                                cwd=script.parent, env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(log.read_text().splitlines(), ["lipo"] * 4 + ["xcodebuild", "zip"])
        self.assertTrue((dest / "WebRTC.xcframework.zip").is_file())
        self.assertTrue((dest / "WebRTC.xcframework/macos-arm64_x86_64/WebRTC.framework/WebRTC").is_symlink())


if __name__ == "__main__":
    unittest.main()
