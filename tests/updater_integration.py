"""Exercise the updater using temporary installs and a local download fixture."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile


WINDOWS = os.name == "nt"
EXE = "TrueCrimeNYC.exe" if WINDOWS else "TrueCrimeNYC"
ASSET = "TrueCrimeNYC.zip" if WINDOWS else "TrueCrimeNYC-linux-x86_64.tar.gz"
BASE = "https://github.com/roz1077/true-crime-new-york-city-recomp/releases/download/v1.0.1/"
if not WINDOWS:
    import fcntl


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def bundle(source, parent, name):
    archive = parent / f"{name}-{ASSET}"
    if WINDOWS:
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zipped:
            for path in source.rglob("*"):
                if path.is_file():
                    zipped.write(path, path.relative_to(source).as_posix())
    else:
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(source, arcname="release")
    release = archive.with_suffix(".json")
    release.write_text(json.dumps([{"assets": [
        {"size": archive.stat().st_size, "uploader": {"name": "fixture"},
         "name": ASSET, "browser_download_url": BASE + ASSET},
        {"name": ASSET + ".sha256", "browser_download_url": BASE + ASSET + ".sha256"}
    ], "tag_name": "v1.0.1"}]), encoding="ascii")
    checksum = archive.with_suffix(".sha256")
    checksum.write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + "\n", encoding="ascii")
    return archive


def run(exe, argument, archive, result):
    env = dict(os.environ, TCNYC_UPDATE_TEST_VERSION="1.0.0",
               TCNYC_FIXTURE_RESTARTED=str(result), TCNYC_FIXTURE_ARCHIVE=str(archive),
               TCNYC_FIXTURE_CHECKSUM=str(archive.with_suffix(".sha256")),
               TCNYC_FIXTURE_RELEASE=str(archive.with_suffix(".json")))
    return subprocess.run([exe, argument], env=env, timeout=18, capture_output=True, text=True)


def wait_for(result, version):
    end = time.monotonic() + 10
    while time.monotonic() < end:
        if result.exists() and result.read_text().strip() == version:
            return
        time.sleep(0.05)
    raise AssertionError(f"Expected restart into {version}.")


def main():
    old, new, tools = map(Path, sys.argv[1:4])
    tools.mkdir(parents=True, exist_ok=True)
    if WINDOWS:
        shutil.copy2(sys.argv[4], tools / "curl.exe")
    else:
        curl = tools / "curl"
        curl.write_text(r"""#!/usr/bin/env python3
import os, shutil, sys
args = sys.argv[1:]
url = args[-1]
key = ("RELEASE" if "api.github.com" in url else "CHECKSUM" if url.endswith(".sha256") else "ARCHIVE")
shutil.copyfile(os.environ["TCNYC_FIXTURE_" + key], args[args.index("-o") + 1])
""")
        curl.chmod(0o755)

    with tempfile.TemporaryDirectory(prefix="tcnyc update é ") as temporary:
        temp = Path(temporary)
        payload = temp / "payload"
        (payload / "lib").mkdir(parents=True)
        (payload / "HDD2").mkdir()
        (payload / "HDD2" / "save").write_bytes(b"package data")
        (payload / "UPDATE-PROTOCOL").write_bytes(b"tcnyc-update-1\n1.0.1\n")
        (payload / "BUILD.txt").write_bytes(b"replacement")
        (payload / "lib" / "libshaderc.so.1").write_bytes(b"fixture")
        if WINDOWS:
            for name in ("clang.exe", "lld-link.exe", "xbe_recomp.exe", "TrueCrimeNYC.lib", "config/game.toml"):
                path = payload / "compiler" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture")
        shutil.copy2(new, payload / EXE)
        manifest = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(payload).as_posix()}\n"
                    for path in payload.rglob("*") if path.is_file()]
        (payload / "SHA256SUMS").write_text("".join(manifest), encoding="ascii", newline="\n")
        valid = bundle(payload, temp, "valid")
        bad_checksum = bundle(payload, temp, "bad-checksum")
        bad_checksum.with_suffix(".sha256").write_text("0" * 64, encoding="ascii")
        (payload / "BUILD.txt").write_bytes(b"modified after manifest")
        bad_manifest = bundle(payload, temp, "bad-manifest")
        (temp / "empty").mkdir()
        empty = bundle(temp / "empty", temp, "empty")

        def install(name):
            root = temp / name
            (root / "hdd2").mkdir(parents=True)
            (root / "hdd2" / "save").write_bytes(b"user save")
            shutil.copy2(old, root / EXE)
            (root / "BUILD.txt").write_bytes(b"initial")
            return root

        root = install("valid install")
        result = root / "restarted"
        check = run(root / EXE, "--stage", valid, result)
        expect(check.returncode == 0, check.stderr)
        expect((root / "BUILD.txt").read_bytes() == b"initial", "Staging changed the live installation.")
        check = run(root / EXE, "--exercise", valid, result)
        expect(check.returncode == 0, check.stderr)
        wait_for(result, "new")
        expect((root / "BUILD.txt").read_bytes() == b"replacement", "Replacement package was not installed.")
        expect((root / "hdd2" / "save").read_bytes() == b"user save" and
               "HDD2" not in [path.name for path in root.iterdir()], "A protected save path was changed.")
        expect(not (root / "update" / "committed").exists(), "Replacement startup did not acknowledge the commit.")

        for name, archive in (("empty", empty), ("bad checksum", bad_checksum), ("bad manifest", bad_manifest)):
            root = install(name)
            check = run(root / EXE, "--stage", archive, root / "restarted")
            expect(check.returncode != 0 and (root / "BUILD.txt").read_bytes() == b"initial",
                   f"The {name} package changed the install.")

        root = install("linked install")
        outside = temp / "outside"
        outside.mkdir()
        try:
            (root / "lib").symlink_to(outside, target_is_directory=True)
        except OSError as failure:
            if not WINDOWS or failure.winerror != 1314:
                raise
        else:
            check = run(root / EXE, "--stage", valid, root / "restarted")
            expect(check.returncode != 0 and not list(outside.iterdir()), "Updater accepted a linked destination.")

        root = install("locked install")
        (root / "update").mkdir()
        with (root / "update" / "install.lock").open("wb") as lock:
            if not WINDOWS:
                fcntl.flock(lock, fcntl.LOCK_EX)
            check = run(root / EXE, "--stage", valid, root / "restarted")
            expect(check.returncode != 0, "Two updaters acquired the install lock.")

        root = install("interrupted install")
        update = root / "update"
        (update / "staged").mkdir(parents=True)
        (update / "backup").mkdir()
        shutil.copy2(new, update / "staged" / EXE)
        shutil.copy2(old, update / "backup" / EXE)
        shutil.copyfile(root / "BUILD.txt", update / "backup" / "BUILD.txt")
        shutil.copy2(new, root / EXE)
        (root / "BUILD.txt").write_bytes(b"partial update")
        (root / "added.txt").write_bytes(b"new file")
        (update / "active").write_text(f"1\tBUILD.txt\n0\tadded.txt\n1\t{EXE}\n", encoding="ascii", newline="\n")
        result = root / "restarted"
        check = run(root / EXE, "--recover-fixture", valid, result)
        expect(check.returncode == 0, check.stderr)
        wait_for(result, "old")
        expect((root / "BUILD.txt").read_bytes() == b"initial" and not (root / "added.txt").exists(),
               "Recovery left a partial install.")


if __name__ == "__main__":
    main()
