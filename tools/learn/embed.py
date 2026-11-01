#!/usr/bin/env python3
# OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
"""Embed models/opensection.osm into engine/src/BuiltinModel.inc"""
import pathlib, sys
root = pathlib.Path(__file__).resolve().parents[2]
src = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "models/opensection.osm"
text = src.read_text()
assert text.startswith("OSM1") and ")OSM\"" not in text
(root / "engine/src/BuiltinModel.inc").write_text('R"OSM(' + text + ')OSM"\n')
print("eingebettet:", src)
