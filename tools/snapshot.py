#!/usr/bin/env python3
"""Save source and ignored working documents in a private local checkpoint."""
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[1]
ROOT_FILES = ("xmake.lua", "LICENSE", "LICENSING.md", "README.md", "TODO.md", ".gitignore", "AGENTS.md")
SOURCE_DIRS = ("app", "include", "src", "examples", "modules", "tests", "tools", "docs", "legal")
SUFFIXES = {".h", ".hpp", ".c", ".cpp", ".cc", ".cxx", ".java", ".lua", ".md", ".sh", ".py"}


def main():
    selected = [ROOT / name for name in ROOT_FILES if (ROOT / name).is_file()]
    for directory in SOURCE_DIRS:
        for path in (ROOT / directory).rglob("*"):
            relative = path.relative_to(ROOT)
            if any(part.startswith(".") or part == "__pycache__" for part in relative.parts):
                continue
            if path.is_file() and path.suffix in SUFFIXES:
                selected.append(path)

    records = []
    contents = []
    for path in sorted(selected):
        if path.is_symlink() or not path.resolve().is_relative_to(ROOT):
            raise SystemExit(f"Refusing source link outside the snapshot contract: {path}")
        name = path.relative_to(ROOT).as_posix()
        data = path.read_bytes()
        records.append({"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        contents.append((name, data, path.stat().st_mode & 0o777))

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    destination = ROOT / ".local" / "snapshots"
    destination.mkdir(parents=True, exist_ok=True)
    output = destination / f"circuit-source-{stamp}.tar.gz"
    manifest = json.dumps({"format": 1, "created_utc": stamp, "files": records}, indent=2).encode() + b"\n"
    with output.open("xb") as stream, tarfile.open(fileobj=stream, mode="w:gz") as archive:
        for name, data, mode in [*contents, ("SNAPSHOT-MANIFEST.json", manifest, 0o644)]:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = mode
            archive.addfile(info, io.BytesIO(data))
    print(f"Saved {len(records)} source files: {output}")


if __name__ == "__main__":
    main()
