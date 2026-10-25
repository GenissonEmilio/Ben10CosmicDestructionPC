"""Preserve every dispatch slot while replacing a huge assignment constructor.

Adjacent slots pointing to the same function become ranges initialized by one
loop. This reduces compiler memory use without dropping fallback entry points.
"""
from pathlib import Path
import re

MARKER = "// Cosmic dispatch ranges v1\n"
ASSIGNMENT = re.compile(r"        g_ps2RecompiledFunctionTable\[(\d+)\] = ([A-Za-z_]\w*); // 0x[0-9a-f]+\n")


def compact(path):
    path = Path(path)
    text = path.read_text()
    if text.startswith(MARKER):
        return
    prefix, separator, body = text.partition("namespace {\nstruct GeneratedFunctionTableInitializer {\n    GeneratedFunctionTableInitializer() {\n")
    suffix = "    }\n};\nstatic const GeneratedFunctionTableInitializer g_generatedFunctionTableInitializer;\n}\n"
    if not separator or not body.endswith(suffix):
        raise ValueError("Formato desconhecido da tabela de funções; conteúdo preservado")
    body = body[:-len(suffix)]
    matches = list(ASSIGNMENT.finditer(body))
    if "".join(m.group() for m in matches) != body:
        raise ValueError("Instrução desconhecida na tabela; conteúdo preservado")
    count = re.search(r"g_ps2RecompiledFunctionTable\[(\d+)u\]", prefix)
    if count is None:
        raise ValueError("Tamanho da tabela ausente")
    limit = int(count[1])
    ranges = []
    seen = set()
    for match in matches:
        slot, function = int(match[1]), match[2]
        if slot in seen or not 0 <= slot < limit:
            raise ValueError("Índice duplicado ou fora da tabela")
        seen.add(slot)
        if ranges and ranges[-1][0] + ranges[-1][1] == slot and ranges[-1][2] == function:
            start, length, _ = ranges[-1]
            ranges[-1] = (start, length + 1, function)
        else:
            ranges.append((slot, 1, function))
    lines = [MARKER, prefix, "namespace {\nstruct CosmicRange { uint32_t start, count; PS2Runtime::RecompiledFunction fn; };\n",
             "const CosmicRange cosmicRanges[] = {\n"]
    lines.extend(f"    {{{start}u, {length}u, {function}}},\n" for start, length, function in ranges)
    lines.append("};\nstruct GeneratedFunctionTableInitializer {\n    GeneratedFunctionTableInitializer() {\n"
                 "        for (const auto &range : cosmicRanges)\n"
                 "            for (uint32_t i = 0; i < range.count; ++i)\n"
                 "                g_ps2RecompiledFunctionTable[range.start + i] = range.fn;\n" + suffix)
    path.write_text("".join(lines))
    print(f"Dispatch: {len(matches)} slots preservados em {len(ranges)} intervalos", flush=True)


if __name__ == "__main__":
    import sys
    compact(sys.argv[1])
