#!/usr/bin/env python3
# OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
"""Embed models/opensection.osm into engine/src/BuiltinModel.inc"""
import pathlib, sys
root = pathlib.Path(__file__).resolve().parents[2]
src = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "models/opensection.osm"
text = src.read_text()
assert text.startswith("OSM1") and ")OSM\"" not in text
# MSVC limits one string literal to 16 KB: write pieces (split at line ends), joined at run time
pieces, cur = [], ""
for line in text.splitlines(keepends=True):
    if len(cur) + len(line) > 12000:
        pieces.append(cur); cur = ""
    cur += line
pieces.append(cur)
(root / "engine/src/BuiltinModel.inc").write_text("".join('R"OSM(' + p + ')OSM",\n' for p in pieces))
print("eingebettet:", src)
