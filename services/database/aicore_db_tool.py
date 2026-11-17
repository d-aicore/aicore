"""AI > DATABASE's work: the template examples database (AICore's exemplos.py, unchanged).

    python aicore_db_tool.py rebuild <library> <catalogue folder or "">
    python aicore_db_tool.py add <library> <catalogue> <template> <verdict> <why> <audio or ""> <sections> <tracks>
    python aicore_db_tool.py summary <library>
    python aicore_db_tool.py companions <template>

One JSON line on stdout ({"ok": true, ...} or {"ok": false, "error"}). The catalogue is AICore's own templates
folder (Settings), never the Core's.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


def rebuild(library: Path, catalogue: str) -> dict:
    from aicore_studio import exemplos
    library.mkdir(parents=True, exist_ok=True)
    sources = [library]
    if catalogue and Path(catalogue).is_dir() and Path(catalogue).resolve() != library.resolve():
        sources.append(Path(catalogue))
    return exemplos.build(sources, library / "exemplos.sqlite", [library / "vereditos.json"])


def main(argv):
    from aicore_studio import exemplos
    cmd = argv[0] if argv else ""
    if cmd == "rebuild":
        out = rebuild(Path(argv[1]), argv[2])
        return {"templates": out["templates"], "exemplos": out["exemplos"]}
    if cmd == "add":
        library = Path(argv[1])
        out = exemplos.add_to_library(library, Path(argv[3]), argv[4], argv[5], Path(argv[6]) if argv[6] else None, argv[7], argv[8])
        built = rebuild(library, argv[2])
        return {"id": out["id"], "ficheiros": out["ficheiros"], "exemplos": built["exemplos"]}
    if cmd == "summary":
        db = Path(argv[1]) / "exemplos.sqlite"
        rows = exemplos.summary(db)["templates"] if db.exists() else []
        return {"rows": [{"id": r["id"], "estilo": r.get("estilo") or "", "veredito": r["veredito"],
                          "audio": Path(r["audio"]).name if r.get("audio") else "", "ficha": bool(r.get("ficha"))} for r in rows]}
    if cmd == "companions":
        wav, _ = exemplos.companions(Path(argv[1]))
        return {"audio": str(wav) if wav else ""}
    raise ValueError("usage: rebuild | add | summary | companions")


if __name__ == "__main__":
    try:
        result = main(sys.argv[1:])
        print(json.dumps({"ok": True, **result}, ensure_ascii=False))
        sys.exit(0)
    except Exception as e:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": str(e)[:600]}, ensure_ascii=False))
        sys.exit(2)
