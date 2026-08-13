"""Stable hashing for experiment input files and directory trees."""

from __future__ import annotations

import hashlib
from pathlib import Path


def hash_path(path: Path) -> str:
    if not path.exists():
        return "missing"
    digest = hashlib.sha256()
    if path.is_file():
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    for child in sorted(
            item for item in path.rglob("*")
            if item.is_file() and
            "__pycache__" not in item.parts and
            item.suffix not in {".pyc", ".log"}):
        digest.update(str(child.relative_to(path)).encode())
        digest.update(hash_path(child).encode())
    return digest.hexdigest()
