"""Adopt a successful translation while retaining unaffected C++ cache batches.

Additional generated entry wrappers use the same fixed ABI. Their declarations
can live beside the dispatch table; adding them need not change the shared header
or shift every existing unity batch. Any ABI/header change takes the normal path.
"""
from pathlib import Path
import os
import re
import shutil
import tempfile

DECLARATION = re.compile(
    r"^void (\w+)\(uint8_t\* rdram, R5900Context\* ctx, PS2Runtime \*runtime\);\n", re.M)


def retain_shared_declarations(previous, generated, extra):
    previous, generated, extra = map(Path, (previous, generated, extra))
    header = "ps2_recompiled_functions.h"
    if not (previous / header).is_file():
        return 0
    before, after = (previous / header).read_text(), (generated / header).read_text()
    old = {m.group(1): m.group() for m in DECLARATION.finditer(before)}
    new = {m.group(1): m.group() for m in DECLARATION.finditer(after)}
    if not old or not old.keys() <= new.keys() or DECLARATION.sub("", before) != DECLARATION.sub("", after):
        return 0
    added = sorted(new.keys() - old.keys())
    old_files = {p.name for p in previous.glob("*.cpp")}
    additional = [p for p in generated.glob("*.cpp") if p.name not in old_files]
    if {p.stem for p in additional} != set(added):
        return 0
    extra.mkdir(exist_ok=True)
    for path in additional:
        shutil.move(str(path), extra / path.name)
    (generated / header).write_text(before)
    table = generated / "register_functions.cpp"
    table.write_text('#include "ps2_runtime.h"\n' + ''.join(new[name] for name in added) + table.read_text())
    return len(added)


def adopt(previous, generated, extra, generated_extra=None):
    previous, generated, extra = map(Path, (previous, generated, extra))
    with tempfile.TemporaryDirectory(prefix=".translation-backup-", dir=previous.parent) as temporary:
        backup = Path(temporary)
        if previous.exists():
            os.replace(previous, backup / "output")
        if extra.exists():
            os.replace(extra, backup / "extra")
        try:
            os.replace(generated, previous)
            if generated_extra is not None and Path(generated_extra).exists():
                os.replace(generated_extra, extra)
        except BaseException:
            # Roll back both directories if the second rename fails. The caller
            # keeps its translation stamp until the complete transaction succeeds.
            if previous.exists():
                os.replace(previous, generated)
            if extra.exists() and generated_extra is not None:
                os.replace(extra, generated_extra)
            if (backup / "output").exists(): os.replace(backup / "output", previous)
            if (backup / "extra").exists(): os.replace(backup / "extra", extra)
            raise
