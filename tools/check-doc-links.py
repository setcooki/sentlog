#!/usr/bin/env python3
"""Fails if a Markdown file links to a repo path that doesn't exist."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)[^)]*\)")
FENCE = re.compile(r"^```.*?^```", re.S | re.M)


def main() -> int:
    tracked = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "*.md"],
        cwd=ROOT, capture_output=True, text=True, check=True,
    ).stdout.split()
    broken = []
    for name in tracked:
        path = ROOT / name
        text = FENCE.sub("", path.read_text(encoding="utf-8"))
        for target in LINK.findall(text):
            if re.match(r"[a-z]+:", target) or target.startswith("#"):
                continue
            if not (path.parent / target.split("#")[0]).exists():
                broken.append(f"{name}: {target}")
    for line in broken:
        print(line)
    print(f"[check-doc-links] {len(tracked)} files, {len(broken)} broken links")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
