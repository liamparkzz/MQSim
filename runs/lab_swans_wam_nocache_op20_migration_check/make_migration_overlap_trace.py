from pathlib import Path


source = Path("prxy0_500k_realtime.trace")
target = Path("migration_overlap_rw.trace")
epoch_ns = 40_000_000_000
zone_size_lba = 32_768
hot_zone_id = 40
burst_count = 200

kept = 0
with source.open("r", encoding="ascii") as src, target.open("w", encoding="ascii", newline="\n") as dst:
    for line in src:
        stripped = line.strip()
        if not stripped:
            continue
        timestamp = int(stripped.split()[0])
        if timestamp > epoch_ns:
            break
        dst.write(stripped + "\n")
        kept += 1

    zone_start_lba = hot_zone_id * zone_size_lba
    for index in range(burst_count):
        timestamp = epoch_ns + 1 + index
        lba = zone_start_lba + (index % 1024) * 8
        request_type = index % 2  # 0=write, 1=read in MQSim trace format
        dst.write(f"{timestamp} 0 {lba} 8 {request_type}\n")

print(f"kept={kept} injected={burst_count} output={target}")
