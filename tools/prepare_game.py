#!/usr/bin/env python3
"""Import and compile an owned SLUS-21938 disc; do not assert playability."""
from pathlib import Path
import argparse
import fcntl
import hashlib
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/desktop"))
from import_disc import EXPECTED, import_disc, sha256
from build_native import build


def prepare(source, work, kit, optimize="2", jobs=1):
    work, kit = Path(work).resolve(), Path(kit).resolve()
    work.mkdir(parents=True, exist_ok=True)
    with (work / ".prepare.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        boot = work / "disc/SLUS_219.38"
        if not boot.exists():
            import_disc(source, work / "disc")
        if sha256(boot) != EXPECTED[boot.name][1]:
            raise ValueError("O executável importado foi alterado. Os arquivos foram preservados.")
        template = (ROOT / "config/recomp.template.toml").read_text()
        config = template.replace("@ELF@", str(boot)).replace("@OUT@", str(work / "output") + "/")
        key = hashlib.sha256(config.encode() + (kit / "manifest.json").read_bytes() +
                             (kit / "bin/ps2_recomp").read_bytes()).hexdigest()
        stamp = work / ".translation-key"
        if not stamp.exists() or stamp.read_text() != key:
            (work / "config.toml").write_text(config)
            env = dict(os.environ, LD_LIBRARY_PATH=str(kit / "lib"))
            subprocess.run([str(kit / "bin/ps2_recomp"), str(work / "config.toml")], env=env, check=True)
            stamp.write_text(key)
        return build(kit, work, optimize, jobs)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--kit", type=Path, default=ROOT / "artifacts/compiler")
    parser.add_argument("--work", type=Path, default=ROOT / "work")
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    prepare(args.source, args.work, args.kit, jobs=args.jobs)
