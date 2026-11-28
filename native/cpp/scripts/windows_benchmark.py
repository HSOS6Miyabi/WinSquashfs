#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import csv
import io
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile

app = Path(sys.argv[1]).resolve()
exe = Path(sys.argv[2]).resolve()
report = Path(sys.argv[3]).resolve()
results = []
with tempfile.TemporaryDirectory(prefix="archivefs-bench-") as tmp:
    root = Path(tmp)
    # Mixed compressible and incompressible blocks, deterministic reference.
    rng = random.Random(82973)
    payload = root / "payload.bin"
    with payload.open("wb") as f:
        for i in range(32):
            block = rng.randbytes(1 << 16) if i % 2 == 0 else rng.randbytes(128)
            f.write(block * ((1 << 20) // len(block)))
    for fmt in ("SquashFS", "DwarFS", "EROFS"):
        image = root / (fmt.lower() + ".img")
        subprocess.run([str(app / "archivefs.exe"), "create", fmt, "4", str(image), str(payload)], check=True, timeout=240)
        p = subprocess.run([str(exe), str(image), str(app), str(payload)], check=True, capture_output=True, text=True, timeout=240)
        print(fmt + "\n" + p.stdout, flush=True)
        rows = list(csv.DictReader(io.StringIO(p.stdout)))
        assert len(rows) == 6, rows
        for row in rows:
            assert float(row["mib_per_second"]) > 0
            results.append({"format": fmt, **row})
report.parent.mkdir(parents=True, exist_ok=True)
report.write_text(json.dumps(results, indent=2), encoding="utf-8")
