#!/usr/bin/env python3
"""
Extract the plain-C parts of a Windows .idl into a usable header.

vkd3d-shader needs only the enums, structs and cpp_quote'd defines from
d3dcommon.idl -- never the COM interfaces. Those parts are already valid C, so
generating them does not need widl (which is not shipped with the Wine runtime
package). Interfaces, imports and attribute blocks are dropped.

usage: idl2h.py <in.idl> <out.h> <include guard>
"""
import re
import sys


def main():
    src, dst, guard = sys.argv[1], sys.argv[2], sys.argv[3]
    text = open(src).read()

    out = []

    # typedef enum ... { ... } NAME, *PNAME;   and the struct equivalent.
    for match in re.finditer(
            r'^typedef\s+(enum|struct)\b[^{;]*\{.*?\}[^;]*;',
            text, re.MULTILINE | re.DOTALL):
        block = match.group(0)
        # Drop IDL attribute blocks like [v1_enum] that may precede members.
        block = re.sub(r'\[[^\]\n]*\]\s*', '', block)
        out.append(block)

    # cpp_quote("...") passes its argument through verbatim.
    for match in re.finditer(r'cpp_quote\(\s*"(.*?)"\s*\)', text, re.DOTALL):
        line = match.group(1).replace('\\"', '"').replace('\\\\', '\\')
        # Only keep #define lines; the rest reference COM types.
        if line.lstrip().startswith('#define'):
            out.append(line)

    with open(dst, 'w') as f:
        f.write(f"/* Generated from {src} by idl2h.py -- enums, structs and defines only. */\n")
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write("\n\n".join(out))
        f.write(f"\n\n#endif /* {guard} */\n")

    print(f"wrote {dst}: {len(out)} blocks")


if __name__ == "__main__":
    main()
