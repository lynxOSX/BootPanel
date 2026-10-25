#!/usr/bin/env python3
"""mkpkg.py — build a pacman package (pkg.tar.zst) for BootPanel.

A lynxOS rootfs is Arch underneath (see lynxOSX/rootFSBuilder), so the boot
panel ships as a plain pacman package: a zstd-compressed tar holding
`.PKGINFO` (the metadata pacman reads), `.MTREE` (digests pacman verifies the
extracted files against) and the payload.

Everything is generated with the Python stdlib plus the `zstd` binary for the
final compression; no libarchive/makepkg needed. Built to be reproducible:
fixed mtimes, root ownership, sorted entries.

Usage:
  python3 tools/mkpkg.py [--name bootpanel] [--version 0.1.0] [--pkgrel 1]
                         [--arch x86_64] [--binary bootpanel] [--out out]
"""

import argparse
import gzip
import hashlib
import io
import os
import subprocess
import sys
import tarfile
from pathlib import Path

# makepkg's mtree options: '!all,use-set,type,uid,gid,mode,time=0,size,md5,
# sha256,link' — fixed timestamp, these keys only. pacman checks size and the
# digests of what it extracts; the rest is metadata it records.
MTREE_KEYS = "time=0.0"


def digest(path, algo):
    h = hashlib.new(algo)
    h.update(Path(path).read_bytes())
    return h.hexdigest()


def pkginfo(fields, payload_size):
    lines = [
        f"pkgname = {fields['name']}",
        f"pkgbase = {fields['name']}",
        f"pkgver = {fields['version']}",
        f"pkgdesc = {fields['desc']}",
        f"url = {fields['url']}",
        "builddate = 0",
        "packager = lynxOS rootFSBuilder <root@lynxos>",
        f"size = {payload_size}",
        f"arch = {fields['arch']}",
    ]
    for dep in fields["depends"]:
        lines.append(f"depend = {dep}")
    for lic in fields["license"]:
        lines.append(f"license = {lic}")
    return ("\n".join(lines) + "\n").encode()


def mtree_line(name, is_dir, src=None, mode=None):
    """name: archive path; src: filesystem path to hash (None for dirs)."""
    entry = f"./{name} {MTREE_KEYS} type={'dir' if is_dir else 'file'}"
    if mode is not None:
        entry += f" mode={mode:o} uid=0 gid=0"
    if src is not None:
        data = Path(src).read_bytes()
        entry += f" size={len(data)} md5digest={hashlib.md5(data).hexdigest()} " \
                 f"sha256digest={hashlib.sha256(data).hexdigest()}"
    return entry


def build_mtree(pi_bytes, payload):
    """pi_bytes: the .PKGINFO content; payload: list of (arcname, srcpath,
    mode). Returns the gzip bytes of the mtree spec covering .PKGINFO, the
    payload files and their dirs."""
    lines = ["#mtree"]
    lines.append(f"./.PKGINFO {MTREE_KEYS} type=file mode=644 uid=0 gid=0 "
                 f"size={len(pi_bytes)} "
                 f"md5digest={hashlib.md5(pi_bytes).hexdigest()} "
                 f"sha256digest={hashlib.sha256(pi_bytes).hexdigest()}")
    dirs = []
    for arcname, _, _ in payload:
        parts = arcname.split("/")
        for i in range(1, len(parts)):
            d = "/".join(parts[:i])
            if d not in dirs:
                dirs.append(d)
    dirs.sort()
    for d in dirs:
        lines.append(mtree_line(d, True, mode=0o755))
    for arcname, src, mode in payload:
        lines.append(mtree_line(arcname, False, src=str(src), mode=mode))
    text = ("\n".join(lines) + "\n").encode()
    buf = io.BytesIO()
    with gzip.GzipFile(filename=".MTREE", mode="wb", mtime=0, fileobj=buf) as gz:
        gz.write(text)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    here = Path(__file__).resolve().parent.parent
    ap.add_argument("--name", default="bootpanel")
    ap.add_argument("--version", default="0.1.0")
    ap.add_argument("--pkgrel", default="1")
    ap.add_argument("--arch", default="x86_64")
    ap.add_argument("--binary", default="bootpanel")
    ap.add_argument("--out", default=str(here / "out"))
    ap.add_argument("--desc", default="lynxOS boot panel — plymouth-alike "
                                      "boot splash (KMS/DRM + fbdev)")
    ap.add_argument("--url", default="https://github.com/lynxOSX/BootPanel")
    args = ap.parse_args()

    binary = here / args.binary
    lic = here / "LICENSE"
    for f in (binary, lic):
        if not f.is_file():
            sys.exit(f"mkpkg: missing {f} — build first (make)")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    # detect a libdrm dynamic link and depend on the package that provides it
    depends = []
    ldd = subprocess.run(["ldd", str(binary)], capture_output=True, text=True)
    if "libdrm.so" in ldd.stdout:
        depends.append("libdrm")

    # a lynxOS rootfs is glibc (Arch); a musl-built binary will not run in it
    head = binary.read_bytes()[:4096]
    if b"/lib/ld-musl" in head:
        print("mkpkg: WARNING: binary links against musl — it will not run "
              "in a glibc lynxOS rootfs; build for the target libc (e.g. in "
              "CI) before shipping", file=sys.stderr)

    payload = [
        ("usr/bin/bootpanel", binary, 0o755),
        ("usr/share/licenses/bootpanel/LICENSE", lic, 0o644),
    ]

    workdir = out / ".mkpkg"
    workdir.mkdir(exist_ok=True)

    pi = pkginfo({
        "name": args.name,
        "version": f"{args.version}-{args.pkgrel}",
        "desc": args.desc,
        "url": args.url,
        "arch": args.arch,
        "depends": depends,
        "license": ["MIT"],
    }, payload_size=sum(os.path.getsize(src) for _, src, _ in payload))

    mtree = build_mtree(pi, payload)

    pkgname = f"{args.name}-{args.version}-{args.pkgrel}-{args.arch}.pkg.tar"
    pkg = out / pkgname
    tmp = out / (pkgname + ".tmp")
    with tarfile.open(tmp, "w", format=tarfile.GNU_FORMAT) as tar:
        def add_bytes(name, data, mode):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = mode
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            tar.addfile(info, io.BytesIO(data))

        add_bytes(".PKGINFO", pi, 0o644)
        add_bytes(".MTREE", mtree, 0o644)
        for arcname, src, mode in sorted(payload, key=lambda p: p[0]):
            info = tar.gettarinfo(str(src), arcname=arcname)
            info.mode = mode
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            with open(src, "rb") as f:
                tar.addfile(info, f)
    tmp.replace(pkg)

    zst = out / (pkgname + ".zst")
    subprocess.run(["zstd", "-q", "-19", "-f", str(pkg), "-o", str(zst)],
                   check=True)
    pkg.unlink()
    workdir.rmdir()

    size = zst.stat().st_size
    print(f"mkpkg: wrote {zst.name} ({size / (1 << 10):.1f} KiB)"
          + (f", depends: {' '.join(depends)}" if depends else ", no depends"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
