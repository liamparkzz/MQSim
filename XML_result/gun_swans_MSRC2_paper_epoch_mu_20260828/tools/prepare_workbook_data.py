#!/usr/bin/env python3
"""Extract gun_swans XML files into flat UTF-8 CSV tables for Excel authoring."""

from __future__ import annotations

import argparse
import csv
import json
import math
import xml.etree.ElementTree as ET
from pathlib import Path


def values(element: ET.Element) -> dict[str, str]:
    return {child.tag: (child.text or "") for child in list(element)}


def as_float(value: str | None) -> float:
    try:
        return float(value or 0)
    except ValueError:
        return 0.0


def as_int(value: str | None) -> int:
    return int(as_float(value))


def write_rows(path: Path, fields: list[str], rows: list[dict]) -> None:
    with path.open("w", encoding="utf-8-sig", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", type=Path, required=True)
    args = parser.parse_args()

    bundle = args.bundle.resolve()
    xml_dir = bundle / "results" / "raw_xml"
    data_dir = bundle / "workbook_data"
    data_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = bundle / "manifest" / "run_status.json"
    state = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest_runs: dict[str, dict] = state.get("runs", {})
    by_stem = {Path(name).stem: info for name, info in manifest_runs.items()}

    summary_fields = [
        "Trace_Name", "Status", "Source_CSV", "Source_Size_Bytes", "Source_SHA256",
        "Request_Count", "Read_Request_Count", "Write_Request_Count", "IOPS",
        "Bandwidth_Bytes_s", "Device_Response_Time_us", "End_to_End_Request_Delay_us",
        "Submitted_Requests", "Completed_Requests", "Average_Request_Completion_Latency_us",
        "SSD_Count", "Stripe_Unit_LBA", "SWANS_Zone_Size_LBA", "SWANS_Epoch_Evaluations",
        "SWANS_Normal_Epochs", "SWANS_Redirect_Epochs", "SWANS_Migration_Epochs",
        "SWANS_Redirect_Operations", "SWANS_Migration_Operations", "SWANS_Migration_History_Count",
        "SWANS_WAM_Host_Write_Sectors", "SWANS_WAM_Migration_Write_Sectors",
        "SWANS_WAM_Cumulative_Write_Sectors", "Logical_RAID_Write_Amplification",
        "Approx_Flash_Write_Amplification", "Parsed_Epoch_Count", "Parsed_Average_Mu_pp",
        "Parsed_Max_Mu_pp", "Parsed_Max_Mu_Time_ns", "Parsed_Epochs_Mu_GE_5",
        "Parsed_Epochs_Mu_GE_15", "EOL_Triggered", "First_EOL_SSD_ID", "EOL_Time_ns",
        "EOL_Bad_Block_Count", "EOL_Effective_OP_Ratio", "Bad_Block_Count",
        "Current_Effective_OP_Ratio", "Max_Block_Erase_Count", "Total_Block_Erase_Count",
        "Simulation_Elapsed_Seconds", "Result_XML", "Result_XML_Size_Bytes",
        "Source_Revision", "Executable_SHA256",
    ]
    epoch_fields = [
        "Trace_Name", "Sequence", "Time_ns", "Mu_pp", "State", "Hot_SSD", "Cold_SSD",
        "Redirect_Valid", "Migration_Candidate_Count", "Total_Host_Write_Sectors",
        "Total_Migration_Write_Sectors", "Total_Cumulative_Write_Sectors",
        "SSD_0_Cumulative_Write_Share_pct", "SSD_1_Cumulative_Write_Share_pct",
        "SSD_2_Cumulative_Write_Share_pct", "SSD_3_Cumulative_Write_Share_pct",
    ]
    per_ssd_fields = [
        "Trace_Name", "SSD_ID", "Submitted_SubRequests", "Completed_SubRequests",
        "Submitted_Read_Sectors", "Submitted_Write_Sectors", "Attributed_Host_Write_Sectors",
        "WAM_Host_Write_Sectors", "WAM_Migration_Write_Sectors", "WAM_Cumulative_Write_Sectors",
        "Average_SubRequest_Latency_us", "Bad_Block_Count", "Pending_Retirement_Count",
        "OP_Consumed_Block_Count", "OP_Spare_Block_Limit", "Current_Effective_OP_Ratio",
        "EOL_Triggered", "EOL_Time_ns", "EOL_Bad_Block_Count", "EOL_Effective_OP_Ratio",
        "Min_Block_Erase_Count", "Max_Block_Erase_Count", "Total_Block_Erase_Count",
        "Avg_Block_Erase_Count", "StdDev_Block_Erase_Count", "Flash_Page_Program_Count",
        "Flash_Page_Erase_Count", "Logical_Host_Write_Bytes_Attributed",
        "Approx_Flash_Programmed_Bytes", "Approx_Flash_Write_Amplification",
    ]
    migration_fields = [
        "Trace_Name", "Sequence", "Start_Time_ns", "Hot_SSD", "Cold_SSD", "Hot_Zone",
        "Cold_Zone", "First_Stream_ID", "Stream_Count", "First_Source_Disk",
        "First_Destination_Disk", "First_Source_LBA", "First_Destination_LBA",
        "Copy_Blocks", "Copy_Sectors",
    ]

    summary_rows: list[dict] = []
    per_ssd_rows: list[dict] = []
    migration_rows: list[dict] = []
    epoch_path = data_dir / "epoch_mu.csv"
    with epoch_path.open("w", encoding="utf-8-sig", newline="") as epoch_output:
        epoch_writer = csv.DictWriter(epoch_output, fieldnames=epoch_fields, extrasaction="ignore")
        epoch_writer.writeheader()

        xml_paths = sorted(xml_dir.glob("*.xml"), key=lambda item: item.name.lower())
        for index, xml_path in enumerate(xml_paths, start=1):
            trace_name = xml_path.stem
            manifest = by_stem.get(trace_name, {})
            print(f"[{index}/{len(xml_paths)}] parse {xml_path.name}", flush=True)
            host: dict[str, str] = {}
            controller: dict[str, str] = {}
            eol: dict[str, str] = {}
            raid_wa: dict[str, str] = {}
            controller_ssds: dict[int, dict[str, str]] = {}
            wear_ssds: dict[int, dict[str, str]] = {}
            epoch_count = 0
            mu_sum = 0.0
            max_mu = -math.inf
            max_mu_time = 0
            mu_ge_5 = 0
            mu_ge_15 = 0

            for event, element in ET.iterparse(xml_path, events=("end",)):
                tag = element.tag
                if tag == "SWANS_Epoch_Record":
                    row = values(element)
                    mu = as_float(row.get("Mu_pp"))
                    time_ns = as_int(row.get("Time_ns"))
                    epoch_count += 1
                    mu_sum += mu
                    if mu > max_mu:
                        max_mu = mu
                        max_mu_time = time_ns
                    if mu >= 5:
                        mu_ge_5 += 1
                    if mu >= 15:
                        mu_ge_15 += 1
                    epoch_writer.writerow({"Trace_Name": trace_name, **row})
                    element.clear()
                elif tag.startswith("SWANS_Migration_Record_"):
                    row = values(element)
                    row["Start_Time_ns"] = row.get("Start_Time", "")
                    migration_rows.append({"Trace_Name": trace_name, **row})
                    element.clear()
                elif tag.endswith("RAIDController.SSD"):
                    row = values(element)
                    controller_ssds[as_int(row.get("SSD_ID"))] = row
                    element.clear()
                elif tag.endswith("WearLeveling.SSD"):
                    row = values(element)
                    wear_ssds[as_int(row.get("SSD_ID"))] = row
                    element.clear()
                elif tag.endswith("WearLeveling.EOLSummary"):
                    eol = values(element)
                    element.clear()
                elif tag.endswith("WearLeveling.RAIDWriteAmplification"):
                    raid_wa = values(element)
                    element.clear()
                elif tag.endswith("Host.IO_Flow"):
                    host = values(element)
                    element.clear()
                elif tag.endswith("RAIDController"):
                    controller = values(element)
                    element.clear()

            declared_epochs = as_int(controller.get("SWANS_Epoch_History_Count"))
            if declared_epochs != epoch_count:
                raise ValueError(
                    f"{trace_name}: declared epochs {declared_epochs} != parsed {epoch_count}"
                )

            summary_rows.append({
                "Trace_Name": trace_name,
                "Status": manifest.get("status", ""),
                "Source_CSV": manifest.get("source_csv", ""),
                "Source_Size_Bytes": manifest.get("source_size_bytes", 0),
                "Source_SHA256": manifest.get("source_sha256", ""),
                "Request_Count": host.get("Request_Count", 0),
                "Read_Request_Count": host.get("Read_Request_Count", 0),
                "Write_Request_Count": host.get("Write_Request_Count", 0),
                "IOPS": host.get("IOPS", 0),
                "Bandwidth_Bytes_s": host.get("Bandwidth", 0),
                "Device_Response_Time_us": host.get("Device_Response_Time", 0),
                "End_to_End_Request_Delay_us": host.get("End_to_End_Request_Delay", 0),
                "Submitted_Requests": controller.get("Submitted_Requests", 0),
                "Completed_Requests": controller.get("Completed_Requests", 0),
                "Average_Request_Completion_Latency_us": controller.get("Average_Request_Completion_Latency_us", 0),
                "SSD_Count": controller.get("SSD_Count", 0),
                "Stripe_Unit_LBA": controller.get("Stripe_Unit_LBA", 0),
                "SWANS_Zone_Size_LBA": controller.get("SWANS_Zone_Size_LBA", 0),
                "SWANS_Epoch_Evaluations": controller.get("SWANS_Epoch_Evaluations", 0),
                "SWANS_Normal_Epochs": controller.get("SWANS_Normal_Epochs", 0),
                "SWANS_Redirect_Epochs": controller.get("SWANS_Redirect_Epochs", 0),
                "SWANS_Migration_Epochs": controller.get("SWANS_Migration_Epochs", 0),
                "SWANS_Redirect_Operations": controller.get("SWANS_Redirect_Operations", 0),
                "SWANS_Migration_Operations": controller.get("SWANS_Migration_Operations", 0),
                "SWANS_Migration_History_Count": controller.get("SWANS_Migration_History_Count", 0),
                "SWANS_WAM_Host_Write_Sectors": controller.get("SWANS_WAM_Host_Write_Sectors", 0),
                "SWANS_WAM_Migration_Write_Sectors": controller.get("SWANS_WAM_Migration_Write_Sectors", 0),
                "SWANS_WAM_Cumulative_Write_Sectors": controller.get("SWANS_WAM_Cumulative_Write_Sectors", 0),
                "Logical_RAID_Write_Amplification": raid_wa.get("Logical_RAID_Write_Amplification", 0),
                "Approx_Flash_Write_Amplification": raid_wa.get("Approx_Flash_Write_Amplification", 0),
                "Parsed_Epoch_Count": epoch_count,
                "Parsed_Average_Mu_pp": 0 if epoch_count == 0 else mu_sum / epoch_count,
                "Parsed_Max_Mu_pp": 0 if epoch_count == 0 else max_mu,
                "Parsed_Max_Mu_Time_ns": max_mu_time,
                "Parsed_Epochs_Mu_GE_5": mu_ge_5,
                "Parsed_Epochs_Mu_GE_15": mu_ge_15,
                "EOL_Triggered": eol.get("EOL_Triggered", ""),
                "First_EOL_SSD_ID": eol.get("First_EOL_SSD_ID", ""),
                "EOL_Time_ns": eol.get("EOL_Time", ""),
                "EOL_Bad_Block_Count": eol.get("EOL_Bad_Block_Count", ""),
                "EOL_Effective_OP_Ratio": eol.get("EOL_Effective_OP_Ratio", ""),
                "Bad_Block_Count": eol.get("Bad_Block_Count", ""),
                "Current_Effective_OP_Ratio": eol.get("Current_Effective_OP_Ratio", ""),
                "Max_Block_Erase_Count": eol.get("Max_Block_Erase_Count", ""),
                "Total_Block_Erase_Count": eol.get("Total_Block_Erase_Count", ""),
                "Simulation_Elapsed_Seconds": manifest.get("simulation_elapsed_seconds", 0),
                "Result_XML": str(xml_path),
                "Result_XML_Size_Bytes": xml_path.stat().st_size,
                "Source_Revision": manifest.get("source_revision", "gun_swans_v1_epochlog"),
                "Executable_SHA256": manifest.get("executable_sha256", ""),
            })

            for ssd_id in sorted(set(controller_ssds) | set(wear_ssds)):
                combined = {
                    "Trace_Name": trace_name,
                    "SSD_ID": ssd_id,
                    **controller_ssds.get(ssd_id, {}),
                    **wear_ssds.get(ssd_id, {}),
                }
                combined["EOL_Time_ns"] = combined.get("EOL_Time", "")
                per_ssd_rows.append(combined)

    write_rows(data_dir / "summary.csv", summary_fields, summary_rows)
    write_rows(data_dir / "per_ssd.csv", per_ssd_fields, per_ssd_rows)
    write_rows(data_dir / "migrations.csv", migration_fields, migration_rows)
    print(
        f"done: summaries={len(summary_rows)}, epochs={sum(as_int(r['Parsed_Epoch_Count']) for r in summary_rows)}, "
        f"per_ssd={len(per_ssd_rows)}, migrations={len(migration_rows)}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
