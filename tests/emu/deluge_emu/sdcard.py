"""SD card images for the emulator.

A card is described by a *spec*:

* ``None`` / ``"none"``  - no card inserted
* a fixture name         - a directory under tests/emu/fixtures/sd/ (see README there)
* ``"factory"``          - the Synthstrom factory card contents (downloaded once into .emu/)
* a path to a directory  - its contents become the card root
* a path to a ``.img``   - used as-is (copied, so the original is never written)

Images are built with delugemu's scripts/mksd.sh (FAT32, 32 KiB clusters,
power-of-two size, sparse) and cached by a hash of the content. Every session
gets its own sparse copy, so tests can save to the card without interfering.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

from . import paths

FIXTURE_SD_DIR = paths.FIXTURES_DIR / "sd"

FACTORY_URL = (
    "https://synthstrom-audible-deluge.s3.us-east-2.amazonaws.com/"
    "Deluge+OLED+V4p1p0+factory+card+contents.zip"
)


def list_fixtures() -> list[str]:
    return sorted(p.name for p in FIXTURE_SD_DIR.iterdir() if p.is_dir())


def _hash_tree(root: Path) -> str:
    h = hashlib.sha256()
    for p in sorted(root.rglob("*")):
        if p.is_file():
            st = p.stat()
            h.update(str(p.relative_to(root)).encode())
            h.update(f"{st.st_size}:{int(st.st_mtime)}".encode())
    return h.hexdigest()[:16]


def _materialise_fixture(fixture_dir: Path, out: Path) -> None:
    """Copy a fixture into `out`, resolving `links.txt` entries.

    links.txt lines are `<path on card> = <path relative to repo root>`; this
    lets fixtures reuse songs/presets already in the repo (e.g. contrib/).
    """
    for p in fixture_dir.rglob("*"):
        rel = p.relative_to(fixture_dir)
        if p.name in ("links.txt", "README.md") and p.parent == fixture_dir:
            continue
        dst = out / rel
        if p.is_dir():
            dst.mkdir(parents=True, exist_ok=True)
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)
    links = fixture_dir / "links.txt"
    if links.exists():
        for line in links.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            card_path, src = (s.strip() for s in line.split("=", 1))
            dst = out / card_path
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(paths.FW_ROOT / src, dst)


def _factory_dir() -> Path:
    root = paths.EMU_HOME / "factory_sd"
    marker = root / ".complete"
    if marker.exists():
        return root
    root.mkdir(parents=True, exist_ok=True)
    zpath = paths.EMU_HOME / "factory_sd.zip"
    print(f"[emu] downloading factory SD contents -> {zpath}", flush=True)
    urllib.request.urlretrieve(FACTORY_URL, zpath)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(root)
    # The zip wraps everything in one top-level folder; flatten it.
    entries = [e for e in root.iterdir() if e.name not in ("__MACOSX",)]
    if len(entries) == 1 and entries[0].is_dir():
        inner = entries[0]
        for e in inner.iterdir():
            shutil.move(str(e), root / e.name)
        inner.rmdir()
    shutil.rmtree(root / "__MACOSX", ignore_errors=True)
    marker.write_text("ok\n")
    return root


def _mksd(content: Path, out: Path) -> None:
    mksd = paths.delugemu_root() / "scripts" / "mksd.sh"
    out.parent.mkdir(parents=True, exist_ok=True)
    # Unique temp name: concurrent builders of the same image must not interleave.
    fd, tmp = tempfile.mkstemp(dir=out.parent, prefix=out.stem + ".", suffix=".tmp")
    os.close(fd)
    try:
        subprocess.run(
            [str(mksd), str(content), tmp], check=True, stdout=subprocess.DEVNULL
        )
        os.replace(tmp, out)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def build_image(spec: str | os.PathLike | None) -> Path | None:
    """Return the path of a (cached, pristine) image for `spec`, building it if needed."""
    if spec is None or str(spec) == "none":
        return None
    spec_s = str(spec)
    p = Path(spec_s)
    if p.suffix == ".img" and p.is_file():
        return p.resolve()

    if spec_s == "factory":
        content, name = _factory_dir(), "factory"
    elif "/" not in spec_s and (FIXTURE_SD_DIR / spec_s).is_dir():
        content, name = None, spec_s
    elif p.is_dir():
        content, name = p.resolve(), p.name
    else:
        raise FileNotFoundError(
            f"SD spec {spec_s!r} is not a fixture ({', '.join(list_fixtures())}), "
            "'factory', 'none', a directory or an .img file"
        )

    with tempfile.TemporaryDirectory(prefix="deluge-sd-") as tmp:
        if content is None:
            content = Path(tmp) / "card"
            content.mkdir()
            _materialise_fixture(FIXTURE_SD_DIR / spec_s, content)
        # mksd.sh copies `content/*`, which fails on an empty folder.
        if not any(content.iterdir()):
            (content / "SONGS").mkdir()
        img = paths.SD_CACHE_DIR / f"{name}-{_hash_tree(content)}.img"
        if not img.exists():
            print(f"[emu] building SD image {img.name}", flush=True)
            _mksd(content, img)
    return img


def working_copy(image: Path, dest: Path) -> Path:
    """Sparse copy of a pristine image that the firmware may write to."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    if sys.platform == "darwin":
        # APFS clone (instant, copy-on-write); BSD cp has no --sparse.
        subprocess.run(["cp", "-c", str(image), str(dest)], check=True)
    else:
        subprocess.run(["cp", "--sparse=always", str(image), str(dest)], check=True)
    return dest


def extract(image: Path, card_path: str, dest: Path) -> Path:
    """Copy a file off a card image (e.g. a song the firmware just saved)."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["mcopy", "-n", "-i", str(image), "::" + card_path.lstrip("/"), str(dest)],
        check=True,
    )
    return dest


def listdir(image: Path, card_path: str = "/") -> str:
    """`mdir` listing of a directory on a card image."""
    return subprocess.run(
        ["mdir", "-i", str(image), "::" + card_path.lstrip("/")],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
