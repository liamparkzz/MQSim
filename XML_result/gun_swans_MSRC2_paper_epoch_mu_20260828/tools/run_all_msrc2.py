#!/usr/bin/env python3
"""Resume-safe MSRC_2 -> gun_swans batch runner.

Each CSV is converted with original timing (1x), simulated independently, XML
validated, and then the temporary MQSim trace is removed.  Results and state
remain usable if the batch is interrupted and restarted.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path


def now_iso() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def atomic_json(path: Path, value: dict) -> None:
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temp, path)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(8 * 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def run_with_heartbeat(command: list[str], cwd: Path, log_path: Path, label: str) -> tuple[int, float]:
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8", errors="replace") as log_file:
        process = subprocess.Popen(
            command,
            cwd=str(cwd),
            stdout=log_file,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
        )
        while True:
            try:
                process.wait(timeout=20)
                break
            except subprocess.TimeoutExpired:
                elapsed = time.monotonic() - started
                print(f"[HEARTBEAT] {label}: {elapsed:.0f}s elapsed", flush=True)
    return process.returncode, time.monotonic() - started


def make_workload(path: Path, trace_file_name: str) -> None:
    text = f'''<?xml version="1.0" encoding="us-ascii"?>
<MQSim_IO_Scenarios>
  <IO_Scenario>
    <IO_Flow_Parameter_Set_Trace_Based>
      <Priority_Class>HIGH</Priority_Class>
      <Device_Level_Data_Caching_Mode>TURNED_OFF</Device_Level_Data_Caching_Mode>
      <Channel_IDs>0,1,2,3,4,5,6,7</Channel_IDs>
      <Chip_IDs>0,1,2,3</Chip_IDs>
      <Die_IDs>0,1</Die_IDs>
      <Plane_IDs>0,1</Plane_IDs>
      <Initial_Occupancy_Percentage>70</Initial_Occupancy_Percentage>
      <File_Path>{trace_file_name}</File_Path>
      <Percentage_To_Be_Executed>100</Percentage_To_Be_Executed>
      <Relay_Count>1</Relay_Count>
      <Time_Unit>NANOSECOND</Time_Unit>
    </IO_Flow_Parameter_Set_Trace_Based>
  </IO_Scenario>
</MQSim_IO_Scenarios>
'''
    path.write_text(text, encoding="ascii")


def child_values(element: ET.Element | None) -> dict[str, str]:
    if element is None:
        return {}
    return {child.tag: (child.text or "") for child in list(element)}


def validate_result(path: Path) -> dict[str, str | int]:
    tree = ET.parse(path)
    root = tree.getroot()
    epoch_count = 0
    controller_values: dict[str, str] = {}
    eol_values: dict[str, str] = {}
    host_values: dict[str, str] = {}
    for element in root.iter():
        if element.tag == "SWANS_Epoch_Record":
            epoch_count += 1
        elif element.tag.endswith("RAIDController"):
            controller_values = child_values(element)
        elif element.tag.endswith("WearLeveling.EOLSummary"):
            eol_values = child_values(element)
        elif element.tag.endswith("Host.IO_Flow"):
            host_values = child_values(element)

    declared_epochs = int(controller_values.get("SWANS_Epoch_History_Count", "0") or 0)
    if declared_epochs != epoch_count:
        raise ValueError(f"epoch count mismatch: declared={declared_epochs}, parsed={epoch_count}")
    required_eol = {
        "EOL_Triggered",
        "First_EOL_SSD_ID",
        "EOL_Time",
        "EOL_Bad_Block_Count",
        "EOL_Effective_OP_Ratio",
        "Bad_Block_Count",
        "Current_Effective_OP_Ratio",
        "Max_Block_Erase_Count",
        "Total_Block_Erase_Count",
    }
    missing = sorted(required_eol - set(eol_values))
    if missing:
        raise ValueError("missing EOLSummary fields: " + ", ".join(missing))
    return {
        "request_count": int(float(host_values.get("Request_Count", "0") or 0)),
        "read_request_count": int(float(host_values.get("Read_Request_Count", "0") or 0)),
        "write_request_count": int(float(host_values.get("Write_Request_Count", "0") or 0)),
        "epoch_count": epoch_count,
        "migration_operations": int(float(controller_values.get("SWANS_Migration_Operations", "0") or 0)),
        "redirect_operations": int(float(controller_values.get("SWANS_Redirect_Operations", "0") or 0)),
        "eol_triggered": eol_values.get("EOL_Triggered", "false"),
        "first_eol_ssd_id": eol_values.get("First_EOL_SSD_ID", "-1"),
        "eol_time_ns": eol_values.get("EOL_Time", "0"),
    }


def converter_metrics(log_path: Path) -> dict[str, int]:
    text = log_path.read_text(encoding="utf-8", errors="replace")
    match = re.search(
        r"rows=(\d+), scanned=(\d+).*?source_requests=(\d+)",
        text,
        flags=re.DOTALL,
    )
    if not match:
        return {}
    return {
        "converted_rows": int(match.group(1)),
        "source_rows_scanned": int(match.group(2)),
        "source_requests": int(match.group(3)),
    }


def write_manifest_csv(path: Path, runs: dict[str, dict]) -> None:
    fields = [
        "trace_name", "status", "source_revision", "executable_sha256", "source_csv", "source_size_bytes", "source_sha256",
        "source_rows_scanned", "source_requests", "converted_rows", "time_acceleration",
        "address_mode", "request_count", "read_request_count", "write_request_count",
        "epoch_count", "migration_operations", "redirect_operations", "eol_triggered",
        "first_eol_ssd_id", "eol_time_ns", "convert_elapsed_seconds",
        "simulation_elapsed_seconds", "result_xml", "started_at", "completed_at", "error",
    ]
    temp = path.with_suffix(".tmp")
    with temp.open("w", encoding="utf-8-sig", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for trace_name in sorted(runs):
            row = {"trace_name": trace_name, **runs[trace_name]}
            writer.writerow(row)
    os.replace(temp, path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace-dir", type=Path, required=True)
    parser.add_argument("--only", action="append", default=[], help="Run only named CSV file(s)")
    parser.add_argument("--state-tag", default="", help="Use an isolated manifest suffix for parallel jobs")
    parser.add_argument("--source-revision", default="gun_swans_v1_3_gc_candidate_fallback")
    parser.add_argument("--executable", type=Path, help="Override the bundled MQSim executable")
    args = parser.parse_args()

    bundle = Path(__file__).resolve().parent.parent
    trace_dir = args.trace_dir.resolve()
    converter = bundle / "tools" / "msrc_to_raid_first_n.py"
    executable = (
        args.executable.resolve()
        if args.executable
        else bundle / "bin" / "MQSim_gun_swans_paper_epochlog_v1_3_gc_candidate_fallback.exe"
    )
    config = bundle / "inputs" / "ssdconfig_gun_swans_paper_epochlog.xml"
    workload_dir = bundle / "inputs" / "workloads"
    result_dir = bundle / "results" / "raw_xml"
    log_dir = bundle / "logs"
    work_dir = bundle / "work"
    manifest_dir = bundle / "manifest"
    state_tag = re.sub(r"[^A-Za-z0-9_-]", "_", args.state_tag.strip())
    state_suffix = f"_{state_tag}" if state_tag else ""
    state_path = manifest_dir / f"run_status{state_suffix}.json"
    manifest_path = manifest_dir / f"trace_manifest{state_suffix}.csv"

    for path in (workload_dir, result_dir, log_dir, work_dir, manifest_dir):
        path.mkdir(parents=True, exist_ok=True)
    for required in (converter, executable, config):
        if not required.is_file():
            raise FileNotFoundError(required)
    executable_sha256 = sha256_file(executable)

    if state_path.exists():
        state = json.loads(state_path.read_text(encoding="utf-8"))
    else:
        state = {"batch_started_at": now_iso(), "runs": {}}
    runs: dict[str, dict] = state.setdefault("runs", {})

    trace_paths = sorted(trace_dir.glob("*.csv"), key=lambda item: item.name.lower())
    if args.only:
        selected = set(args.only)
        trace_paths = [item for item in trace_paths if item.name in selected]
    print(f"[BATCH] {len(trace_paths)} trace(s), bundle={bundle}", flush=True)

    for index, source_csv in enumerate(trace_paths, start=1):
        trace_name = source_csv.name
        slug = source_csv.stem
        result_xml = result_dir / f"{slug}.xml"
        workload = workload_dir / f"workload_{slug}.xml"
        generated_xml = workload.with_name(workload.stem + "_scenario_1.xml")
        temp_trace = work_dir / f"{slug}.trace"
        convert_log = log_dir / f"{slug}_convert.log"
        simulator_log = log_dir / f"{slug}_simulator.log"

        previous = runs.get(trace_name, {})
        if previous.get("status") == "COMPLETED" and result_xml.is_file():
            try:
                validate_result(result_xml)
                print(f"[{index}/{len(trace_paths)}] SKIP completed {trace_name}", flush=True)
                continue
            except Exception:
                pass

        info = {
            **previous,
            "status": "RUNNING",
            "source_revision": args.source_revision,
            "executable_sha256": executable_sha256,
            "source_csv": str(source_csv),
            "source_size_bytes": source_csv.stat().st_size,
            "time_acceleration": 1,
            "address_mode": "GLOBAL_LBA",
            "started_at": now_iso(),
            "completed_at": "",
            "error": "",
        }
        runs[trace_name] = info
        atomic_json(state_path, state)
        write_manifest_csv(manifest_path, runs)
        print(f"[{index}/{len(trace_paths)}] START {trace_name} ({source_csv.stat().st_size:,} bytes)", flush=True)

        try:
            if not info.get("source_sha256"):
                print(f"[{index}/{len(trace_paths)}] SHA256 {trace_name}", flush=True)
                info["source_sha256"] = sha256_file(source_csv)
                atomic_json(state_path, state)

            if info.get("conversion_status") != "COMPLETED" or not temp_trace.is_file():
                if temp_trace.is_file():
                    temp_trace.unlink()
                command = [
                    sys.executable,
                    str(converter),
                    "--input", str(source_csv),
                    "--output", str(temp_trace),
                    "--address-mode", "GLOBAL_LBA",
                    "--ssd-count", "4",
                    "--stripe-lba", "512",
                    "--time-acceleration", "1",
                    "--max-source-requests", "0",
                ]
                code, elapsed = run_with_heartbeat(command, bundle, convert_log, f"convert {trace_name}")
                info["convert_elapsed_seconds"] = round(elapsed, 3)
                info.update(converter_metrics(convert_log))
                if code != 0 or not temp_trace.is_file():
                    raise RuntimeError(f"conversion failed with exit code {code}")
                info["conversion_status"] = "COMPLETED"
                atomic_json(state_path, state)
                write_manifest_csv(manifest_path, runs)

            make_workload(workload, temp_trace.name)
            if generated_xml.is_file():
                generated_xml.unlink()
            command = [
                str(executable),
                "-i", str(config),
                "-w", str(workload),
            ]
            code, elapsed = run_with_heartbeat(command, work_dir, simulator_log, f"simulate {trace_name}")
            info["simulation_elapsed_seconds"] = round(elapsed, 3)
            if code != 0:
                raise RuntimeError(f"MQSim failed with exit code {code}")
            if not generated_xml.is_file():
                raise FileNotFoundError(f"MQSim result not found: {generated_xml}")
            metrics = validate_result(generated_xml)
            if result_xml.is_file():
                result_xml.unlink()
            shutil.move(str(generated_xml), str(result_xml))
            info.update(metrics)
            info["result_xml"] = str(result_xml)
            info["result_xml_size_bytes"] = result_xml.stat().st_size
            info["status"] = "COMPLETED"
            info["completed_at"] = now_iso()
            info["conversion_status"] = "CLEANED"
            if temp_trace.is_file():
                temp_trace.unlink()
            print(
                f"[{index}/{len(trace_paths)}] DONE {trace_name}: "
                f"requests={metrics['request_count']:,}, epochs={metrics['epoch_count']:,}, "
                f"migrations={metrics['migration_operations']:,}, sim={elapsed:.1f}s",
                flush=True,
            )
        except Exception as error:
            info["status"] = "FAILED"
            info["completed_at"] = now_iso()
            info["error"] = f"{type(error).__name__}: {error}"
            print(f"[{index}/{len(trace_paths)}] FAILED {trace_name}: {info['error']}", flush=True)
        finally:
            atomic_json(state_path, state)
            write_manifest_csv(manifest_path, runs)

    completed = sum(1 for value in runs.values() if value.get("status") == "COMPLETED")
    failed = sum(1 for value in runs.values() if value.get("status") == "FAILED")
    state["batch_last_finished_at"] = now_iso()
    state["completed_count"] = completed
    state["failed_count"] = failed
    atomic_json(state_path, state)
    write_manifest_csv(manifest_path, runs)
    print(f"[BATCH] finished: completed={completed}, failed={failed}", flush=True)
    return 0 if failed == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
