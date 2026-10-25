#!/usr/bin/env python3
"""Bounded native-game probe with private saves and reliable child cleanup."""
from pathlib import Path
import argparse
import json
import os
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def probe(game, boot, destination, kit, seconds=45, visible=False, buttons=""):
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "memcards").mkdir(exist_ok=True)
    (destination / "pad-control").write_text("0\n")
    env = dict(os.environ)
    for name in ("PYTHONHOME", "PYTHONPATH", "GI_TYPELIB_PATH", "GIO_EXTRA_MODULES", "GTK_PATH"):
        env.pop(name, None)
    env.update(LD_LIBRARY_PATH=str(Path(kit).resolve() / "lib"), LP_NUM_THREADS="2",
               GOW_WINDOW="960x540", COSMIC_SAVE_DIR=str(destination / "memcards"),
               COSMIC_SHOT_DIR=str(destination / "frames"), COSMIC_EXIT_SECONDS=str(seconds),
               COSMIC_SHOT_INTERVAL="10",
               COSMIC_PAD_CONTROL=str(destination / "pad-control"),
               # Compatibility with the first boot binary built from the base kit.
               GOW2_SHOT_DIR=str(destination / "frames"), GOW2_EXIT_SECONDS=str(seconds),
               PS2X_IOP_NATIVE=env.get("PS2X_IOP_NATIVE", "1"), PS2X_GS_WORKER="1")
    if not visible:
        env["PS2X_HIDDEN_WINDOW"] = "1"
    if buttons:
        env["COSMIC_PAD_SCRIPT"] = buttons
    started = time.monotonic()
    forced = False
    with (destination / "game.log").open("w") as output:
        process = subprocess.Popen([str(Path(game).resolve()), str(Path(boot).resolve())],
                                   cwd=destination, env=env, stdout=output,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        (destination / "process.json").write_text(json.dumps({"pid": process.pid}) + "\n")
        try:
            process.wait(timeout=seconds + 10)
        except subprocess.TimeoutExpired:
            forced = True
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
    result = {"exit_code": process.returncode, "elapsed_seconds": round(time.monotonic() - started, 2),
              "watchdog_required": forced, "frames": len(list((destination / "frames").glob("*.png"))),
              "gameplay_validated": False}
    (destination / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=ROOT / "work/game")
    parser.add_argument("--boot", type=Path, default=ROOT / "work/disc/SLUS_219.38")
    parser.add_argument("--kit", type=Path, default=ROOT / "work/compiler")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seconds", type=int, choices=range(5, 901), default=45)
    parser.add_argument("--visible", action="store_true")
    parser.add_argument("--buttons", default="")
    args = parser.parse_args()
    probe(args.game, args.boot, args.out, args.kit, args.seconds, args.visible, args.buttons)
