#!/usr/bin/env python3
"""Build locally generated guest code with the portable Linux compiler kit.

The kit contains open-source tools and runtime libraries, never the game ELF.
Game code and object files stay in the ignored work directory.
"""
from __future__ import annotations
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import threading
from compact_register import compact

ROOT = Path(__file__).resolve().parents[1]


def fingerprint(paths, flags):
    digest = hashlib.sha256(repr(flags).encode())
    for path in sorted(paths):
        digest.update(str(path).encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


def build(kit, stage, optimize="2", jobs=1):
    kit, stage = Path(kit).resolve(), Path(stage).resolve()
    generated, tc = stage / "output", kit / "toolchain"
    compact(generated / "register_functions.cpp")
    source = ROOT
    includes = [generated, source / "PS2Recomp/ps2xRuntime/include",
                source / "PS2Recomp/ps2xIOP/include", kit / "source/raylib",
                source / "PS2Recomp/ps2xRuntime/src/lib/Kernel",
                source / "PS2Recomp/ps2xIOP/src",
                source / "PS2Recomp/ps2xRuntime/src/lib/vu"]
    # Decoder headers are not part of the generated-code ABI fingerprint.
    decoder_headers = source / "PS2Recomp/ps2xRuntime/third_party/libmpeg2"
    flags = [str(tc / "bin/g++"), "--sysroot=" + str(tc / "sysroot"),
             "-B" + str(tc / "lib/gcc/x86_64-linux-gnu/13") + "/",
             "-std=c++20", "-O" + optimize, "-DNDEBUG", "-msse4.1",
             "-fno-strict-aliasing", "-nostdinc++", "-isystem", str(tc / "include/c++/13"),
             "-isystem", str(tc / "include/x86_64-linux-gnu/c++/13"),
             "-isystem", str(tc / "include/c++/13/backward"),
             *["-I" + str(p) for p in includes],
             "-DPS2_RUNTIME_LOGS=1", "-DPS2X_ENABLE_IOP_RPC_TRACE=0"]
    env = dict(os.environ, LD_LIBRARY_PATH=str(tc / "lib"),
               GCC_EXEC_PREFIX=str(tc / "lib/gcc") + "/",
               PATH=str(tc / "bin") + os.pathsep + os.environ.get("PATH", ""))
    objects = stage / ("objects-O" + optimize)
    objects.mkdir(exist_ok=True)
    headers = list(generated.glob("*.h"))
    for include in includes[1:]:
        headers.extend(include.rglob("*.h"))
        headers.extend(include.rglob("*.hpp"))
    header_key = fingerprint(headers, flags)
    files = sorted(generated.glob("*.cpp"))
    if not files:
        raise ValueError("O recompilador não gerou arquivos C++.")
    batches = [files[i:i + 32] for i in range(0, len(files), 32)]
    batches.append([ROOT / "PS2Recomp/ps2xRuntime/src/main.cpp"])
    # These small runtime units replace the corresponding archive members. In
    # particular, the base kit must not install God of War's IOP address hooks.
    batches.extend([[ROOT / name] for name in (
        "PS2Recomp/ps2xIOP/src/native/iop_native_overrides.cpp",
        "PS2Recomp/ps2xRuntime/src/lib/gow2_desktop.cpp",
        "PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/CD.cpp",
        "PS2Recomp/ps2xIOP/src/emulator/iop_emulator.cpp",
        "PS2Recomp/ps2xRuntime/src/lib/Kernel/Syscalls/RPC.cpp",
        "PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp",
        "PS2Recomp/ps2xIOP/src/emulator/services/iop_rpc.cpp",
        "PS2Recomp/ps2xRuntime/src/lib/ps2_pad.cpp",
        "PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp",
        "PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/IPU.cpp",
    )])
    batches.extend([[path] for path in sorted((ROOT / "overrides").glob("*.cpp"))])
    batches.extend([[path] for path in sorted((stage / "generated-extra").glob("*.cpp"))])
    # Compile only our main: God of War's libbase.a contains title-specific patches.
    stopped = threading.Event()
    def compile_batch(item):
        index, batch = item
        if stopped.is_set():
            raise RuntimeError("Compilação interrompida após erro anterior")
        unit = objects / f"batch-{index:04}.cpp"
        unit.write_text("\n".join('#include "' + str(p) + '"' for p in batch) + "\n")
        obj, stamp = unit.with_suffix(".o"), unit.with_suffix(".key")
        key = fingerprint(batch, [header_key])
        batch_flags = flags
        if any(path.name == "MPEG.cpp" for path in batch):
            batch_flags = [*flags, "-I" + str(decoder_headers)]
            key = fingerprint([*batch, *decoder_headers.glob("*.h")], [header_key])
        if not (obj.is_file() and stamp.is_file() and stamp.read_text() == key):
            try:
                subprocess.run([*batch_flags, "-c", str(unit), "-o", str(obj)], env=env, check=True)
                stamp.write_text(key)
            except BaseException:
                stopped.set()
                obj.unlink(missing_ok=True)
                raise
        return obj
    result = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for index, obj in enumerate(pool.map(compile_batch, enumerate(batches)), 1):
            result.append(obj)
            print(f"Compilando: {index}/{len(batches)}", flush=True)
    libs = [kit / "lib" / name for name in
            ("libps2_runtime.a", "libps2_iop.a", "liblibmpeg2.a", "libraylib.a", "libX11.so.6")]
    libs.append(tc / "sysroot/usr/lib/x86_64-linux-gnu/libGL.so")
    executable = stage / "game"
    subprocess.run([*flags, *map(str, result), "-Wl,--start-group", *map(str, libs),
                    "-Wl,--end-group", "-static-libstdc++", "-static-libgcc",
                    "-Wl,-rpath-link," + str(kit / "lib"),
                    "-Wl,-rpath-link," + str(tc / "sysroot/usr/lib/x86_64-linux-gnu"),
                    "-pthread", "-ldl", "-lm", "-o", str(executable)], env=env, check=True)
    (stage / "build.json").write_text(json.dumps({
        "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "optimization": optimize, "gameplay_validated": False,
        "runtime_key": fingerprint(libs, flags),
    }, indent=2) + "\n")
    print(executable, flush=True)
    return executable


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kit", type=Path, default=ROOT / "work/compiler")
    parser.add_argument("--stage", type=Path, default=ROOT / "work")
    parser.add_argument("--optimize", choices=("0", "1", "2", "3"), default="2")
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    build(args.kit, args.stage, args.optimize, args.jobs)
