#!/usr/bin/env python3
"""Execute synthetic translated MIPS branches split from their delay slots."""
from pathlib import Path
import argparse
import os
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def fixture(path, branch):
    # Hand-authored ELF32/MIPS only. No retail instructions or assets.
    strings = b'\0caller\0delay\0resume\0callee\0'
    names = b'\0.text\0.symtab\0.strtab\0.shstrtab\0'
    image = bytearray(0x1300)
    ident = b'\x7fELF' + bytes((1, 1, 1)) + bytes(9)
    struct.pack_into('<16sHHIIIIIHHHHHH', image, 0, ident, 2, 8, 1, 0x100000,
                     52, 0x1200, 0x20924001, 52, 32, 1, 40, 5, 4)
    struct.pack_into('<8I', image, 52, 1, 0x1000, 0x100000, 0x100000, 0x2c, 0x2c, 5, 0x1000)
    words = [branch, 0x2404004d, 0x03e00008, 0, 0, 0, 0, 0,
             0xac042000, 0x03e00008, 0]
    struct.pack_into('<11I', image, 0x1000, *words)
    image[0x1040:0x1040 + len(strings)] = strings
    symbols = [(0, 0, 0, 0, 0, 0), (1, 0x100000, 4, 0x12, 0, 1),
               (8, 0x100004, 4, 0x12, 0, 1), (14, 0x100008, 8, 0x12, 0, 1),
               (21, 0x100020, 12, 0x12, 0, 1)]
    for index, symbol in enumerate(symbols): struct.pack_into('<IIIBBH', image, 0x1080 + 16*index, *symbol)
    image[0x10e0:0x10e0 + len(names)] = names
    sections = [(0,)*10, (1, 1, 6, 0x100000, 0x1000, 0x2c, 0, 0, 4, 0),
                (7, 2, 0, 0, 0x1080, 80, 3, 1, 4, 16),
                (15, 3, 0, 0, 0x1040, len(strings), 0, 0, 1, 0),
                (23, 3, 0, 0, 0x10e0, len(names), 0, 0, 1, 0)]
    for index, section in enumerate(sections): struct.pack_into('<10I', image, 0x1200 + index*40, *section)
    path.write_bytes(image)


def check(kit, translator):
    kit, translator = Path(kit).resolve(), Path(translator).resolve()
    env = dict(os.environ, LD_LIBRARY_PATH=str(kit / 'lib'))
    with tempfile.TemporaryDirectory(prefix='translation-regression-') as temp:
        directory = Path(temp)
        # JAL, BEQ, BEQL, and an annulled BNEL cover both taken and likely-not-taken slots.
        for kind, branch, expected, memory in [('jal', 0x0c040008, 77, 77), ('beq', 0x10000007, 77, 77),
                                              ('beql', 0x50000007, 77, 77), ('bnel', 0x54000007, 123, 0)]:
            case = directory / kind; case.mkdir()
            elf, output = case / 'synthetic.elf', case / 'output'
            fixture(elf, branch)
            config = case / 'config.toml'
            config.write_text(f'[general]\ninput="{elf}"\noutput="{output}/"\n'
                              'single_file_output=false\noutput_worker_threads=1\nlow_memory_mode=true\n')
            run = subprocess.run([str(translator), str(config)], env=env, capture_output=True, text=True)
            if run.returncode: raise RuntimeError(run.stdout + run.stderr)
            driver = case / 'check.cpp'
            driver.write_text('''#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <cstring>
int main() {
 PS2Runtime runtime;
 if (!runtime.memory().initialize()) return 1;
 auto *ram = runtime.memory().getRDRAM();
 R5900Context ctx{};
 SET_GPR_U32(&ctx, 4, 123); SET_GPR_U32(&ctx, 29, 0x3000);
 ctx.pc = 0x100000;
 uint32_t written = 0;
 for (unsigned step = 0; step < 16 && ctx.pc; ++step) {
  auto fn = runtime.lookupFunction(ctx.pc);
  if (!fn) return 2;
  fn(ram, &ctx, &runtime);
  std::memcpy(&written, ram + 0x2000, 4);
  if (written) break;
 }
 return getRegU32(&ctx, 4) == EXPECT_ARG && written == EXPECT_MEMORY ? 0 : 3;
}
''')
            command = ['g++', '-std=c++20', '-O1', '-msse4.1', '-fno-strict-aliasing',
                       '-I' + str(ROOT / 'PS2Recomp/ps2xRuntime/include'),
                       '-I' + str(ROOT / 'PS2Recomp/ps2xIOP/include'),
                       '-I' + str(ROOT / 'PS2Recomp/ps2xRuntime/src/lib/Kernel'),
                       '-I' + str(kit / 'source/raylib'), '-I' + str(output),
                       '-DEXPECT_ARG=' + str(expected), '-DEXPECT_MEMORY=' + str(memory),
                       str(driver), *map(str, output.glob('*.cpp')),
                       str(ROOT / 'PS2Recomp/ps2xRuntime/src/lib/gow2_desktop.cpp'), '-Wl,--start-group',
                       *[str(kit / 'lib' / name) for name in ('libps2_runtime.a', 'libps2_iop.a', 'liblibmpeg2.a', 'libraylib.a')],
                       '-Wl,--end-group', '-lGL', '-lX11', '-pthread', '-ldl', '-lm',
                       '-static-libstdc++', '-static-libgcc', '-o', str(case / 'check')]
            compiled = subprocess.run(command, env=env, capture_output=True, text=True)
            if compiled.returncode: raise RuntimeError(compiled.stdout + compiled.stderr)
            subprocess.run([str(case / 'check')], env=env, check=True, timeout=10)
            print('Split delay slot:', kind, 'passed', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kit', type=Path, default=ROOT / 'artifacts/compiler')
    parser.add_argument('--translator', type=Path)
    args = parser.parse_args()
    check(args.kit, args.translator or args.kit / 'bin/ps2_recomp')
