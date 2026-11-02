#!/usr/bin/env python3
"""The strict-warnings check: compiles the files holding our healer code with -Wall -Wextra and fails on any
warning inside our own files (SimulationCraft's own warnings are not ours to fix, so they are only counted).
  wowhub/ci/strict_warnings.py <cmake build folder>     (configured with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)"""
import json, re, shlex, subprocess, sys
from pathlib import Path

OURS = ["sc_paladin_holy_heal.cpp", "sc_shaman_resto_heal.inc", "sc_evoker_preservation_heal.inc",
        "sc_monk_mistweaver_heal.inc"]
UNITS = ["paladin/sc_paladin_holy_heal.cpp", "class_modules/sc_shaman.cpp", "class_modules/sc_evoker.cpp",
         "monk/sc_monk.cpp"]
db = json.load(open(Path(sys.argv[1]) / "compile_commands.json"))
mine, theirs = [], 0
for unit in UNITS:
    entry = next(e for e in db if e["file"].endswith(unit))
    cmd = shlex.split(entry["command"]) if "command" in entry else entry["arguments"]
    cmd = [c for i, c in enumerate(cmd) if c != "-o" and (i == 0 or cmd[i - 1] != "-o")]
    cmd += ["-fsyntax-only", "-Wall", "-Wextra"]
    err = subprocess.run(cmd, cwd=entry["directory"], capture_output=True, text=True).stderr
    for line in err.splitlines():
        m = re.match(r"(.+?):(\d+):\d+: warning: (.*)", line)
        if not m:
            continue
        if any(m.group(1).endswith(f) for f in OURS):
            mine.append(f"{Path(m.group(1)).name}:{m.group(2)}: {m.group(3)}")
        else:
            theirs += 1
for w in sorted(set(mine)):
    print("FAIL ", w)
print(f"{len(set(mine))} warnings in our healer files, {theirs} in SimulationCraft's own code (not ours, not counted).")
sys.exit(1 if mine else 0)
