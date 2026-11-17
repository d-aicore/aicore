"""EDIT > MIDI's reader and writer: a Guitar Pro file -> one .mid per track (AICore's guitar_pro.py, unchanged).

    python aicore_gp_tool.py summary <file.gp5>
    python aicore_gp_tool.py export <file.gp5> <folder> <0,2,3>

One JSON line on stdout ({"ok": true, ...} or {"ok": false, "error": "..."}); exit 0 / 2. The native page shows it.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    try:
        from aicore_studio import guitar_pro
        if len(argv) >= 2 and argv[0] == "summary":
            song = guitar_pro.load(Path(argv[1]))
            rows = guitar_pro.summary(song)
            out = {"ok": True, "title": song["title"], "tempo": song["tempo"],
                   "bars": rows[0]["bars"] if rows else 0, "outFolder": str(Path(argv[1]).parent / f"{guitar_pro.safe_name(song['title'])} MIDI"),
                   "rows": [{"name": r["name"], "kind": r["kind"], "strings": r["strings"] or 0, "notes": r["notes"]} for r in rows]}
        elif len(argv) >= 4 and argv[0] == "export":
            song = guitar_pro.load(Path(argv[1]))
            chosen = [int(x) for x in argv[3].split(",") if x.strip()]
            files = guitar_pro.export(song, argv[2], chosen)
            out = {"ok": True, "files": [str(f) for f in files]}
        else:
            raise ValueError("usage: summary <file> | export <file> <folder> <i,j,...>")
    except Exception as e:  # a damaged or unknown file: the page says so
        print(json.dumps({"ok": False, "error": str(e)[:600]}, ensure_ascii=False))
        return 2
    print(json.dumps(out, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
