"""Fix ODAS's JSON sinks on current glibc.

ODAS builds its JSON with sprintf(obj->buffer, "%s...", obj->buffer, ...):
source and destination overlap, which is undefined behaviour, and current
glibc keeps only the last fragment, so every frame prints as just "}".
Rewrite each such call to append at obj->buffer + strlen(obj->buffer).

    python3 patch_odas_sprintf.py <odas source dir>
"""
import pathlib
import re
import sys

PAT = re.compile(r'sprintf\(obj->buffer,"%s((?:[^"\\]|\\.)*)",(\s*)obj->buffer\s*([,)])')


def fix(m):
    tail = "," + m.group(2) if m.group(3) == "," else ")"
    return f'sprintf(obj->buffer+strlen(obj->buffer),"{m.group(1)}"{tail}'


total = 0
for f in pathlib.Path(sys.argv[1]).rglob("src/**/*.c"):
    src = f.read_text()
    new, n = PAT.subn(fix, src)
    if n:
        if "#include <string.h>" not in new:
            new = "#include <string.h>\n" + new
        f.write_text(new)
        total += n
        print(f"{f}: {n} calls")
print(f"patched {total} sprintf calls")
