#!/usr/bin/env python3
# image_symbols.h from config/<image>.toml alone, without the game: the names and addresses the host binds to are all
# in the config, so CI can compile the port's own sources (not link them: the recompiled code needs default.xbe).
# Same layout and order as xbe_recomp's (tools/recomp/codegen_emit.cpp, names sorted as in its std::map).
#   symbols_header.py config/scda_v2.toml <out dir>
import pathlib
import sys
import tomllib

config, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
cfg = tomllib.loads(config.read_text())
lines = ["#pragma once", '#define IMAGE_XBE "%s"' % pathlib.PurePosixPath(cfg["xbe"]["path"]).name]
absent = 0
for name, va in sorted(cfg.get("symbols", {}).items()):
    a = va or 0xFFFF0000 + absent
    absent += not va
    lines += ["#define SYM_%s %08X" % (name, a), "#define HAS_%s %d" % (name, 1 if va else 0)]
for name, va in sorted(cfg.get("data", {}).items()):
    lines += ["#define SYM_%s %08X" % (name, va), "#define HAS_%s %d" % (name, 1 if va else 0)]
out.mkdir(parents=True, exist_ok=True)
(out / "image_symbols.h").write_text("\n".join(lines) + "\n")
