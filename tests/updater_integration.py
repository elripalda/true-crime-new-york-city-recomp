"""Exercise the updater using temporary installs and a local download fixture."""

import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
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


def publish_bundle(archive):
    release = archive.with_suffix(".json")
    release.write_text(json.dumps([{"assets": [
        {"size": archive.stat().st_size, "uploader": {"name": "fixture"},
         "name": ASSET, "browser_download_url": BASE + ASSET},
        {"name": ASSET + ".sha256", "browser_download_url": BASE + ASSET + ".sha256"}
    ], "tag_name": "v1.0.1"}]), encoding="ascii")
    checksum = archive.with_suffix(".sha256")
    checksum.write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + "\n", encoding="ascii")
    return archive


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
    return publish_bundle(archive)


def malicious_bundle(source, parent, name, kind):
    """A valid-looking package plus one symlink or traversal member."""
    archive = parent / f"{name}-{ASSET}"
    if WINDOWS:
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zipped:
            for path in source.rglob("*"):
                if path.is_file():
                    zipped.write(path, path.relative_to(source).as_posix())
            if kind == "symlink":
                link = zipfile.ZipInfo("link")
                link.create_system = 3
                link.external_attr = (stat.S_IFLNK | 0o777) << 16
                zipped.writestr(link, "../../outside")
                zipped.writestr("link/evil.txt", "owned")
            else:
                zipped.writestr("../outside.txt", "owned")
    else:
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(source, arcname="release")
            if kind == "symlink":
                link = tarfile.TarInfo("release/link")
                link.type = tarfile.SYMTYPE
                link.linkname = "../../outside"
                tar.addfile(link)
                data = b"owned"
                entry = tarfile.TarInfo("release/link/evil.txt")
                entry.size = len(data)
                tar.addfile(entry, io.BytesIO(data))
            else:
                data = b"owned"
                entry = tarfile.TarInfo("../outside.txt")
                entry.size = len(data)
                tar.addfile(entry, io.BytesIO(data))
    return publish_bundle(archive)


def run(exe, argument, archive, result):
    env = dict(os.environ, TCNYC_UPDATE_TEST_VERSION="1.0.0",
               TCNYC_FIXTURE_RESTARTED=str(result), TCNYC_FIXTURE_ARCHIVE=str(archive),
               TCNYC_FIXTURE_CHECKSUM=str(archive.with_suffix(".sha256")),
               TCNYC_FIXTURE_RELEASE=str(archive.with_suffix(".json")),
               TCNYC_UPDATER_NO_DIALOG="1")
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
        (payload / "hdd-pal").mkdir()
        (payload / "hdd-pal" / "save").write_bytes(b"package data")
        (payload / "regions").mkdir()
        pal_exe = "regions/TrueCrimeNYCPAL.exe" if WINDOWS else "regions/TrueCrimeNYCPAL"
        (payload / pal_exe).write_bytes(b"new PAL executable")
        pal_eu_exe = "regions/TrueCrimeNYCPALEU.exe" if WINDOWS else "regions/TrueCrimeNYCPALEU"
        (payload / pal_eu_exe).write_bytes(b"new PAL EU executable")
        (payload / "hdd-pal-eu").mkdir()
        (payload / "hdd-pal-eu" / "save").write_bytes(b"package data")
        (payload / "UPDATE-PROTOCOL").write_bytes(b"tcnyc-update-1\n1.0.1\n")
        (payload / "BUILD.txt").write_bytes(b"replacement")
        (payload / "lib" / "libshaderc.so.1").write_bytes(b"fixture")
        (payload / "locked.txt").write_bytes(b"replacement")
        (payload / "lockdir").mkdir()
        (payload / "lockdir" / "file").write_bytes(b"replacement")
        if WINDOWS:
            for name in ("clang.exe", "lld-link.exe", "xbe_recomp.exe", "TrueCrimeNYC.lib", "config/game.toml",
                         "TrueCrimeNYCPAL.lib", "config/game-pal.toml", "TrueCrimeNYCPALEU.lib", "config/game-pal-eu.toml"):
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
        symlink_archive = malicious_bundle(payload, temp, "symlink", "symlink")
        traversal_archive = malicious_bundle(payload, temp, "traversal", "traversal")
        (temp / "empty").mkdir()
        empty = bundle(temp / "empty", temp, "empty")

        def install(name):
            root = temp / name
            (root / "hdd2").mkdir(parents=True)
            (root / "hdd2" / "save").write_bytes(b"user save")
            (root / "hdd-pal").mkdir()
            (root / "hdd-pal" / "save").write_bytes(b"PAL user save")
            (root / "regions").mkdir()
            (root / pal_exe).write_bytes(b"old PAL executable")
            (root / pal_eu_exe).write_bytes(b"old PAL EU executable")
            (root / "hdd-pal-eu").mkdir()
            (root / "hdd-pal-eu" / "save").write_bytes(b"PAL EU user save")
            shutil.copy2(old, root / EXE)
            (root / "BUILD.txt").write_bytes(b"initial")
            return root

        root = install("valid install")
        previous_library = root / "LiB" / "LibShaderc.so.1"
        previous_library.parent.mkdir()
        previous_library.write_bytes(b"old library")
        removed = root / "obsolete.txt"
        removed.write_bytes(b"old package file")
        (root / "update").mkdir()
        (root / "update" / "installed").write_text("LiB/LibShaderc.so.1\nobsolete.txt\n", encoding="ascii")
        result = root / "restarted"
        check = run(root / EXE, "--stage", valid, result)
        expect(check.returncode == 0, check.stderr)
        expect((root / "BUILD.txt").read_bytes() == b"initial", "Staging changed the live installation.")
        check = run(root / EXE, "--busy", valid, result)
        expect(check.returncode == 0, "Updater began replacing files while a game held them.")
        expect((root / pal_exe).read_bytes() == b"old PAL executable", "Staging changed the PAL executable.")
        check = run(root / EXE, "--exercise", valid, result)
        expect(check.returncode == 0, check.stderr)
        wait_for(result, "new")
        expect((root / "BUILD.txt").read_bytes() == b"replacement", "Replacement package was not installed.")
        expect((root / pal_exe).read_bytes() == b"new PAL executable", "PAL executable was not updated.")
        expect((root / "hdd-pal" / "save").read_bytes() == b"PAL user save", "PAL save was overwritten.")
        expect((root / pal_eu_exe).read_bytes() == b"new PAL EU executable", "PAL EU executable was not updated.")
        expect((root / "hdd-pal-eu" / "save").read_bytes() == b"PAL EU user save", "PAL EU save was overwritten.")
        library = root / "lib" / "libshaderc.so.1"
        expect(library.is_file() and library.read_bytes() == b"fixture", "Pruning removed the case-renamed file.")
        expect(not previous_library.exists() or previous_library.samefile(library), "The old library was not pruned.")
        expect(not removed.exists(), "An obsolete package file was not pruned.")
        expect((root / "hdd2" / "save").read_bytes() == b"user save" and
               "HDD2" not in [path.name for path in root.iterdir()], "A protected save path was changed.")
        expect(not (root / "update" / "committed").exists(), "Replacement startup did not acknowledge the commit.")

        for name, archive, reasons in (
                ("empty", empty, ("archive is empty", "An update file is missing")),
                ("bad checksum", bad_checksum, ("did not match its published size or checksum",)),
                ("bad manifest", bad_manifest, ("does not match the update manifest",))):
            root = install(name)
            check = run(root / EXE, "--stage", archive, root / "restarted")
            expect(check.returncode != 0 and (root / "BUILD.txt").read_bytes() == b"initial",
                   f"The {name} package changed the install.")
            expect(any(reason in check.stderr for reason in reasons),
                   f"The {name} package failed for the wrong reason: {check.stderr.strip()}")

        # A symlink member and a traversal member must be refused before tar
        # extracts anything, so neither can write outside update/unpacked.
        for name, archive, reason in (("symlink archive", symlink_archive, "link or special file"),
                                      ("traversal archive", traversal_archive, "unsafe filename")):
            root = install(name)
            check = run(root / EXE, "--stage", archive, root / "restarted")
            expect(check.returncode != 0 and (root / "BUILD.txt").read_bytes() == b"initial",
                   f"The {name} was accepted.")
            expect(reason in check.stderr, f"The {name} failed for the wrong reason: {check.stderr.strip()}")
            expect(not (root / "outside").exists() and not (root / "update" / "outside.txt").exists(),
                   f"The {name} wrote outside the unpack directory.")

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
            expect("symbolic link" in check.stderr or "reparse point" in check.stderr or
                   "unsupported filesystem entry" in check.stderr,
                   f"The linked install failed for the wrong reason: {check.stderr.strip()}")

        root = install("locked install")
        (root / "update").mkdir()
        with (root / "update" / "install.lock").open("wb") as lock:
            if not WINDOWS:
                fcntl.flock(lock, fcntl.LOCK_EX)
            check = run(root / EXE, "--stage", valid, root / "restarted")
            expect(check.returncode != 0, "Two updaters acquired the install lock.")
            expect("Another launcher" in check.stderr,
                   f"The locked install failed for the wrong reason: {check.stderr.strip()}")

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

        # An active journal without its staged installer must roll back in-process
        # instead of refusing to start the game.
        root = install("active without staged")
        update = root / "update"
        (update / "staged").mkdir(parents=True)
        (update / "backup").mkdir()
        shutil.copyfile(root / "BUILD.txt", update / "backup" / "BUILD.txt")
        (root / "BUILD.txt").write_bytes(b"partial update")
        (update / "active").write_text("1\tBUILD.txt\n", encoding="ascii", newline="\n")
        result = root / "restarted"
        check = run(root / EXE, "", valid, result)
        expect(check.returncode == 0, f"A missing staged installer blocked startup: {check.stderr.strip()}")
        wait_for(result, "old")
        expect((root / "BUILD.txt").read_bytes() == b"initial" and not (update / "active").exists(),
               "The in-process rollback did not restore the install.")

        # A package that fails inspection before the journal must clear
        # pending/staged so every launch does not offer the same broken update.
        root = install("broken staged")
        check = run(root / EXE, "--stage", valid, root / "restarted")
        expect(check.returncode == 0, check.stderr)
        (root / "update" / "staged" / "BUILD.txt").unlink()
        check = run(root / EXE, "--exercise", valid, root / "restarted")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and (root / "update" / "pending").exists():
            time.sleep(0.05)
        expect(not (root / "update" / "pending").exists(),
               "A failed apply left the update pending for every later launch.")
        expect((root / "BUILD.txt").read_bytes() == b"initial",
               "A failed apply changed the live installation.")

        # A failure partway through publication (an unwritable destination) must
        # roll the already-published files back. Linux root can write through the
        # permission bit, so the case is covered on Windows and unprivileged Linux.
        if WINDOWS or os.geteuid() != 0:
            root = install("partway apply")
            check = run(root / EXE, "--stage", valid, root / "restarted")
            expect(check.returncode == 0, check.stderr)
            # A file the previous package installed and this one drops is removed under
            # the journal, so the failed apply must bring it back.
            dropped = root / "obsolete.txt"
            dropped.write_bytes(b"old package file")
            (root / "update" / "installed").write_text("obsolete.txt\n", encoding="ascii")
            if WINDOWS:
                locked = root / "locked.txt"
                locked.write_bytes(b"original")
                os.chmod(locked, stat.S_IREAD)
            else:
                locked = root / "lockdir"
                locked.mkdir()
                os.chmod(locked, 0o555)
            check = run(root / "update" / "staged" / EXE, "--apply-update=999999", valid, root / "restarted")
            expect(check.returncode != 0, "A package that cannot be published reported success.")
            if WINDOWS:
                expect(locked.read_bytes() == b"original", "A failed apply did not restore the replaced file.")
                os.chmod(locked, stat.S_IWRITE)
            else:
                expect(not (locked / "file").exists(), "A failed apply left a partially published file.")
                os.chmod(locked, 0o755)
            expect(not (root / "update" / "committed").exists(), "A failed apply committed the update.")
            expect(dropped.read_bytes() == b"old package file", "A failed apply did not restore a pruned file.")


if __name__ == "__main__":
    main()
