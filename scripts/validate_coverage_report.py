#!/usr/bin/env python3
"""Validate a Cobertura coverage report before it is uploaded.

A report that contains no owned source, no executable lines, or paths that are
not repository-relative is not evidence of anything. Catching that here means a
partial report is never presented as the successful canonical upload.
"""

from __future__ import annotations

import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path


def validate(report_path: Path) -> None:
    root = ElementTree.parse(report_path).getroot()
    filenames = [element.get("filename", "") for element in root.iter("class")]

    if not filenames:
        raise SystemExit("the coverage report contains no source files")

    owned = [name for name in filenames if name.startswith("src/")]
    if not owned:
        raise SystemExit(f"no owned src/ files in the report; first entries: {filenames[:5]}")

    foreign = [
        name
        for name in filenames
        if name.startswith(("/", "build/", "vcpkg_installed/", ".vcpkg/"))
    ]
    if foreign:
        raise SystemExit(f"dependency or build paths masquerading as source: {foreign[:5]}")

    generated = [name for name in filenames if "_autogen" in name or "/moc_" in name]
    if generated:
        raise SystemExit(f"generated Qt code leaked into the report: {generated[:5]}")

    lines = sum(len(element.findall(".//line")) for element in root.iter("class"))
    if lines == 0:
        raise SystemExit("the coverage report contains no executable lines")

    print(f"validated {len(owned)} owned files and {lines} executable lines")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <coverage.xml>", file=sys.stderr)
        return 2

    report_path = Path(sys.argv[1])
    if not report_path.is_file() or report_path.stat().st_size == 0:
        print(f"no coverage report at {report_path}", file=sys.stderr)
        return 1

    validate(report_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
