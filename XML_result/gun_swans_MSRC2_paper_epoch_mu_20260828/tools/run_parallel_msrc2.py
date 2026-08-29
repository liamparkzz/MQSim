#!/usr/bin/env python3
"""Run remaining independent MSRC traces in balanced parallel jobs, then merge state."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def load_json(path: Path) -> dict:
    if not path.exists():
        return {"runs": {}}
    return json.loads(path.read_text(encoding="utf-8"))


def save_json(path: Path, value: dict) -> None:
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temp, path)


def tail(path: Path, lines: int = 2) -> str:
    if not path.exists():
        return ""
    content = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return " | ".join(content[-lines:])


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(8 * 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        raise ValueError("jobs must be positive")

    bundle = Path(__file__).resolve().parent.parent
    runner = bundle / "tools" / "run_all_msrc2.py"
    manifest_dir = bundle / "manifest"
    log_dir = bundle / "logs"
    base_state_path = manifest_dir / "run_status.json"
    base_state = load_json(base_state_path)
    base_runs = base_state.setdefault("runs", {})
    v1_executable = bundle / "bin" / "MQSim_gun_swans_paper_epochlog_v1.exe"
    v1_sha256 = sha256_file(v1_executable) if v1_executable.exists() else ""

    # Recover completed work from earlier isolated jobs before calculating what
    # remains.  Those results used v1; the GC/WL fix was not reachable because
    # their maximum erase count stayed below the static-WL threshold.
    for state_file in sorted(manifest_dir.glob("run_status_job*.json")):
        job_state = load_json(state_file)
        for trace_name, info in job_state.get("runs", {}).items():
            result = bundle / "results" / "raw_xml" / f"{Path(trace_name).stem}.xml"
            if info.get("status") == "COMPLETED" and result.is_file():
                info.setdefault("source_revision", "gun_swans_v1_epochlog")
                info.setdefault("executable_sha256", v1_sha256)
                base_runs[trace_name] = info
    for trace_name, info in base_runs.items():
        if info.get("status") == "COMPLETED":
            info.setdefault("source_revision", "gun_swans_v1_epochlog")
            info.setdefault("executable_sha256", v1_sha256)
    save_json(base_state_path, base_state)

    all_traces = sorted(args.trace_dir.resolve().glob("*.csv"), key=lambda p: p.name.lower())
    remaining = [
        path for path in all_traces
        if base_runs.get(path.name, {}).get("status") != "COMPLETED"
        or not (bundle / "results" / "raw_xml" / f"{path.stem}.xml").is_file()
    ]
    if not remaining:
        print("[PARALLEL] nothing remaining", flush=True)
        return subprocess.call([sys.executable, str(runner), "--trace-dir", str(args.trace_dir.resolve())])

    job_count = min(args.jobs, len(remaining))
    bins: list[list[Path]] = [[] for _ in range(job_count)]
    sizes = [0 for _ in range(job_count)]
    assigned: set[str] = set()
    for index in range(job_count):
        prior = load_json(manifest_dir / f"run_status_job{index}.json").get("runs", {})
        for trace in remaining:
            if trace.name in prior and prior[trace.name].get("status") != "COMPLETED":
                bins[index].append(trace)
                sizes[index] += trace.stat().st_size
                assigned.add(trace.name)
    for trace in sorted((item for item in remaining if item.name not in assigned), key=lambda p: p.stat().st_size, reverse=True):
        target = min(range(job_count), key=lambda index: sizes[index])
        bins[target].append(trace)
        sizes[target] += trace.stat().st_size

    processes = []
    log_handles = []
    started = time.monotonic()
    for index, traces in enumerate(bins):
        tag = f"job{index}"
        command = [
            sys.executable, str(runner), "--trace-dir", str(args.trace_dir.resolve()),
            "--state-tag", tag,
        ]
        for trace in sorted(traces, key=lambda p: p.name.lower()):
            command.extend(["--only", trace.name])
        job_log = log_dir / f"parallel_{tag}.log"
        handle = job_log.open("w", encoding="utf-8", errors="replace")
        process = subprocess.Popen(command, stdout=handle, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        processes.append((tag, process, job_log, traces))
        log_handles.append(handle)
        print(
            f"[PARALLEL] {tag}: {len(traces)} trace(s), {sizes[index]:,} bytes -> "
            + ", ".join(trace.name for trace in traces),
            flush=True,
        )

    while any(process.poll() is None for _tag, process, _log, _traces in processes):
        time.sleep(20)
        elapsed = time.monotonic() - started
        parts = []
        for tag, process, job_log, _traces in processes:
            state = load_json(manifest_dir / f"run_status_{tag}.json")
            runs = state.get("runs", {})
            completed = sum(1 for value in runs.values() if value.get("status") == "COMPLETED")
            failed = sum(1 for value in runs.values() if value.get("status") == "FAILED")
            running = [name for name, value in runs.items() if value.get("status") == "RUNNING"]
            label = running[0] if running else ("done" if process.poll() is not None else "starting")
            parts.append(f"{tag}:{completed} done/{failed} failed/{label}")
        print(f"[PARALLEL HEARTBEAT] {elapsed:.0f}s | " + " | ".join(parts), flush=True)

    for handle in log_handles:
        handle.close()
    for tag, process, job_log, _traces in processes:
        print(f"[PARALLEL] {tag} exit={process.returncode}: {tail(job_log)}", flush=True)

    merged = load_json(base_state_path)
    merged_runs = merged.setdefault("runs", {})
    for tag, _process, _job_log, _traces in processes:
        job_state = load_json(manifest_dir / f"run_status_{tag}.json")
        for trace_name, info in job_state.get("runs", {}).items():
            if info.get("status") in {"COMPLETED", "FAILED"}:
                info.setdefault("source_revision", "gun_swans_v1_epochlog")
                info.setdefault("executable_sha256", v1_sha256)
                merged_runs[trace_name] = info
    save_json(base_state_path, merged)
    print("[PARALLEL] states merged; running final validation/resume pass", flush=True)
    return subprocess.call([sys.executable, str(runner), "--trace-dir", str(args.trace_dir.resolve())])


if __name__ == "__main__":
    raise SystemExit(main())
