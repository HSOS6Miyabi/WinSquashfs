# SPDX-License-Identifier: GPL-3.0-only
"""Compare verified reads through the mounted filesystem on the same runner."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import tempfile
import time

MIB = 1024 * 1024
SIZE = 64 * MIB
SEED = 746591

def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    source = root / "source"
    source.mkdir(exist_ok=True)
    rng = random.Random(SEED)
    with (source / "payload.bin").open("wb") as f:
        for _ in range(64):
            block = rng.randbytes(MIB // 2)
            f.write(block)
            f.write(block)
    return source

def measure(path, reference, threads, random_access, repeat=3):
    block = 65536 if random_access else MIB
    rng = random.Random(SEED)
    offsets = ([rng.randrange(0, len(reference) // block) * block for _ in range(2048)]
               if random_access else list(range(0, len(reference), block)))
    def read_chunk(group):
        samples = []
        with path.open("rb", buffering=0) as f:
            for offset in group:
                t = time.perf_counter_ns()
                f.seek(offset)
                data = f.read(block)
                samples.append((time.perf_counter_ns() - t) / 1e6)
                if data != reference[offset:offset + block]:
                    raise ValueError(f"Incorrect data at offset {offset}")
        return samples
    rounds = []
    for _ in range(repeat):
        start = time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=threads) as pool:
            samples = [t for group in pool.map(read_chunk, [offsets[i::threads] for i in range(threads)]) for t in group]
        elapsed = time.perf_counter() - start
        rounds.append({"mib_s": len(offsets) * block / MIB / elapsed,
                       "p50_ms": statistics.median(samples),
                       "p95_ms": sorted(samples)[int(0.95 * (len(samples) - 1))]})
    return {"threads": threads, "pattern": "random" if random_access else "sequential",
            "block_bytes": block, "reads": len(offsets), "rounds": rounds,
            "median_mib_s": statistics.median(r["mib_s"] for r in rounds)}

def mounted(cli, root, label, output):
    reference = (root / "source" / "payload.bin").read_bytes()
    image = root / "comparison.squashfs"
    if label == "legacy":
        subprocess.run([str(cli), "pack", str(root / "source"), str(image), "zstd"], check=True, timeout=120)
    if not image.is_file():
        raise FileNotFoundError(image)
    drive = next((f"{c}:" for c in "ZYXWVUTSRQPONMLKJIHGFED" if not Path(f"{c}:/").exists()), None)
    if not drive:
        raise RuntimeError("No free drive letter")
    path = Path(drive + "/payload.bin")
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen([str(cli), "mount", str(image), drive], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 45
            while not path.is_file():
                if process.poll() is not None or time.monotonic() >= deadline:
                    log.seek(0)
                    raise RuntimeError("Mount failed: " + log.read().decode("utf-8", errors="replace"))
                time.sleep(0.1)
            # Warm both mounted files before collecting comparable cached workloads.
            if path.read_bytes() != reference:
                raise ValueError("Initial mounted data differs")
            results = [measure(path, reference, n, random_access)
                       for n in (1, 8) for random_access in (False, True)]
        finally:
            try:
                subprocess.run([str(cli), "eject", drive], check=True, timeout=30)
                process.wait(timeout=30)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
    report = {"version": label, "image_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
              "payload_sha256": hashlib.sha256(reference).hexdigest(), "payload_bytes": len(reference),
              "cpu_count": os.cpu_count(), "cache_condition": "warm; same image, same runner",
              "results": results}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["prepare", "measure", "self-test", "compare"])
    parser.add_argument("--root", type=Path)
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--label", choices=["legacy", "native"])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.mode == "self-test":
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "payload.bin"
            reference = random.Random(SEED).randbytes(2 * MIB)
            path.write_bytes(reference)
            for threads in (1, 8):
                for pattern in (False, True):
                    result = measure(path, reference, threads, pattern, 1)
                    assert result["median_mib_s"] > 0
            path.write_bytes(bytes(len(reference)))
            try:
                measure(path, reference, 1, False, 1)
            except ValueError:
                pass
            else:
                raise AssertionError("Corrupt data accepted")
        print("PASS benchmark I/O and corruption detection")
    elif args.mode == "prepare":
        prepare(args.root)
    elif args.mode == "measure":
        mounted(args.cli.resolve(), args.root.resolve(), args.label, args.output.resolve())
    else:
        old = json.loads((args.root / "legacy.json").read_text(encoding="utf-8"))
        new = json.loads((args.root / "native.json").read_text(encoding="utf-8"))
        for key in ("image_sha256", "payload_sha256", "payload_bytes"):
            if old[key] != new[key]:
                raise ValueError(f"Benchmark inputs differ: {key}")
        rows = []
        for a, b in zip(old["results"], new["results"], strict=True):
            for key in ("threads", "pattern", "block_bytes", "reads"):
                if a[key] != b[key]:
                    raise ValueError(f"Benchmark workloads differ: {key}")
            rows.append({"threads": a["threads"], "pattern": a["pattern"],
                         "legacy_mib_s": a["median_mib_s"], "native_mib_s": b["median_mib_s"],
                         "speedup": b["median_mib_s"] / a["median_mib_s"]})
        args.output.write_text(json.dumps(rows, indent=2), encoding="utf-8")
        print(json.dumps(rows, indent=2))

if __name__ == "__main__":
    main()
