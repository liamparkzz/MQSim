import argparse
import csv
import heapq
import math
import os
from dataclasses import dataclass
from typing import Iterator, Optional, Sequence

SECTOR_SIZE = 512
WRITE_CODE = "0"
READ_CODE = "1"

DEFAULT_INPUT = "CAMRESSTGA01-lvm0.csv"
DEFAULT_OUTPUT = "../traces/CAMRESSTGA01-lvm0_128.trace"
ADDRESS_MODE_GLOBAL = "GLOBAL_LBA"
ADDRESS_MODE_DEVICE_LOCAL = "DEVICE_LOCAL"
DEFAULT_TIME_ACCELERATION = 16
FILETIME_TICKS_PER_SECOND = 10_000_000


@dataclass(frozen=True)
class MSRCRequest:
    timestamp_ticks: int
    source_disk: int
    start_lba: int
    lba_count: int
    type_code: str
    source_row: int


@dataclass(frozen=True)
class MappedSource:
    input_csv: str
    target_ssd: int


def validate_time_acceleration(time_acceleration: int) -> None:
    if time_acceleration <= 0:
        raise ValueError("time_acceleration must be greater than zero")


def validate_device_local_geometry(
    ssd_count: int,
    stripe_lba: int,
    ssd_lba_count: Optional[int],
) -> int:
    if ssd_count <= 0:
        raise ValueError("ssd_count must be greater than zero")
    if stripe_lba <= 0:
        raise ValueError("stripe_lba must be greater than zero")
    if ssd_lba_count is None or ssd_lba_count <= 0:
        raise ValueError("DEVICE_LOCAL mode requires ssd_lba_count greater than zero")

    usable_ssd_lba_count = (ssd_lba_count // stripe_lba) * stripe_lba
    if usable_ssd_lba_count == 0:
        raise ValueError("ssd_lba_count must contain at least one complete RAID stripe")
    return usable_ssd_lba_count


def iter_msrc_requests(input_csv: str) -> Iterator[MSRCRequest]:
    """Yield valid MSRC requests with their original absolute FILETIME timestamp."""
    previous_timestamp = None

    with open(input_csv, "r", newline="") as f_in:
        reader = csv.reader(f_in)
        for source_row, row in enumerate(reader, start=1):
            if len(row) < 7:
                continue

            # MSR columns: Timestamp, Hostname, DiskNumber, Type, Offset, Size, ResponseTime
            ts_str, _host, disk_str, typ, off_str, size_str, _resp = row[:7]

            try:
                timestamp_ticks = int(float(ts_str))
                source_disk = int(float(disk_str))
                offset_bytes = int(float(off_str))
                size_bytes = int(float(size_str))
            except ValueError:
                # Header or malformed line.
                continue

            if size_bytes <= 0:
                continue

            typ_lower = typ.strip().lower()
            if typ_lower == "write":
                type_code = WRITE_CODE
            elif typ_lower == "read":
                type_code = READ_CODE
            else:
                continue

            if previous_timestamp is not None and timestamp_ticks < previous_timestamp:
                raise RuntimeError(
                    f"non-monotonic timestamp in {input_csv} at row {source_row}: "
                    f"{timestamp_ticks} < {previous_timestamp}"
                )
            previous_timestamp = timestamp_ticks

            start_lba = offset_bytes // SECTOR_SIZE
            lba_count = math.ceil(size_bytes / SECTOR_SIZE)
            if lba_count <= 0:
                lba_count = 1

            yield MSRCRequest(
                timestamp_ticks,
                source_disk,
                start_lba,
                lba_count,
                type_code,
                source_row,
            )


def iter_device_local_segments(
    local_lba: int,
    lba_count: int,
    target_ssd: int,
    ssd_count: int,
    stripe_lba: int,
    usable_ssd_lba_count: Optional[int] = None,
):
    """Convert one SSD-local request into RAID-global, stripe-bounded segments."""
    remaining = lba_count
    current_local_lba = (
        local_lba % usable_ssd_lba_count
        if usable_ssd_lba_count is not None
        else local_lba
    )
    while remaining > 0:
        local_stripe = current_local_lba // stripe_lba
        in_stripe_offset = current_local_lba % stripe_lba
        segment_lba_count = min(remaining, stripe_lba - in_stripe_offset)
        if usable_ssd_lba_count is not None:
            segment_lba_count = min(
                segment_lba_count,
                usable_ssd_lba_count - current_local_lba,
            )
        global_stripe = local_stripe * ssd_count + target_ssd
        global_lba = global_stripe * stripe_lba + in_stripe_offset
        yield global_lba, segment_lba_count
        current_local_lba += segment_lba_count
        if (
            usable_ssd_lba_count is not None
            and current_local_lba == usable_ssd_lba_count
        ):
            current_local_lba = 0
        remaining -= segment_lba_count


def convert(
    input_csv: str,
    output_trace: str,
    address_mode: str = ADDRESS_MODE_GLOBAL,
    target_ssd: Optional[int] = None,
    ssd_count: int = 4,
    stripe_lba: int = 512,
    ssd_lba_count: Optional[int] = None,
    time_acceleration: int = DEFAULT_TIME_ACCELERATION,
    max_source_requests: int = 0,
) -> None:
    validate_time_acceleration(time_acceleration)
    if address_mode not in (ADDRESS_MODE_GLOBAL, ADDRESS_MODE_DEVICE_LOCAL):
        raise ValueError(f"unsupported address_mode: {address_mode}")
    if address_mode == ADDRESS_MODE_DEVICE_LOCAL:
        if target_ssd is None or target_ssd < 0 or target_ssd >= ssd_count:
            raise ValueError("DEVICE_LOCAL mode requires 0 <= target_ssd < ssd_count")

    usable_ssd_lba_count = None
    if address_mode == ADDRESS_MODE_DEVICE_LOCAL:
        usable_ssd_lba_count = validate_device_local_geometry(
            ssd_count,
            stripe_lba,
            ssd_lba_count,
        )

    t0 = None
    total_rows = 0
    written_rows = 0
    source_devices = set()
    split_rows = 0
    wrapped_rows = 0
    accepted_source_requests = 0

    os.makedirs(os.path.dirname(output_trace) or ".", exist_ok=True)

    with open(input_csv, "r", newline="") as f_in, open(output_trace, "w", newline="") as f_out:
        reader = csv.reader(f_in)
        for row in reader:
            total_rows += 1
            if len(row) < 7:
                continue

            # MSR columns: Timestamp, Hostname, DiskNumber, Type, Offset, Size, ResponseTime
            ts_str, _host, disk_str, typ, off_str, size_str, _resp = row[:7]

            try:
                ts = int(float(ts_str))
                disk = int(float(disk_str))
                offset_bytes = int(float(off_str))
                size_bytes = int(float(size_str))
            except ValueError:
                # header or malformed line
                continue

            if t0 is None:
                t0 = ts
            source_devices.add(disk)

            # FILETIME unit = 100ns ticks -> ns (normalized from first request).
            time_ns = (ts - t0) * 100 // time_acceleration
            if time_ns < 0:
                continue

            # bytes -> sectors(LBA)
            start_lba = offset_bytes // SECTOR_SIZE
            lba_count = math.ceil(size_bytes / SECTOR_SIZE)
            if lba_count <= 0:
                lba_count = 1

            typ_lower = typ.strip().lower()
            if typ_lower == "write":
                type_code = WRITE_CODE
            elif typ_lower == "read":
                type_code = READ_CODE
            else:
                continue

            if max_source_requests and accepted_source_requests >= max_source_requests:
                break
            accepted_source_requests += 1

            # MQSim ASCII trace format: time device lba size type.
            # MQSim currently consumes the LBA as an array-global address and
            # ignores the device column. DEVICE_LOCAL mode therefore encodes
            # the selected SSD into the global LBA before simulation.
            if address_mode == ADDRESS_MODE_DEVICE_LOCAL:
                if start_lba >= usable_ssd_lba_count or start_lba + lba_count > usable_ssd_lba_count:
                    wrapped_rows += 1
                segments = list(
                    iter_device_local_segments(
                        start_lba,
                        lba_count,
                        target_ssd,
                        ssd_count,
                        stripe_lba,
                        usable_ssd_lba_count,
                    )
                )
                if len(segments) > 1:
                    split_rows += 1
                for global_lba, segment_lba_count in segments:
                    f_out.write(f"{time_ns} 0 {global_lba} {segment_lba_count} {type_code}\n")
                    written_rows += 1
            else:
                f_out.write(f"{time_ns} {disk} {start_lba} {lba_count} {type_code}\n")
                written_rows += 1

    print(
        f"Done: {output_trace} "
        f"(rows={written_rows}, scanned={total_rows}, mode={address_mode}, "
        f"source_requests={accepted_source_requests}, "
        f"source_devices={sorted(source_devices)}, split_source_rows={split_rows}, "
        f"wrapped_source_rows={wrapped_rows}, usable_ssd_lba_count={usable_ssd_lba_count})"
    )


def parse_mapped_source(value: str) -> MappedSource:
    """Parse a repeated CLI source in TARGET_SSD=PATH form."""
    target_text, separator, input_csv = value.partition("=")
    if not separator or not target_text.strip() or not input_csv.strip():
        raise ValueError("mapped source must use TARGET_SSD=PATH format")
    try:
        target_ssd = int(target_text, 0)
    except ValueError as error:
        raise ValueError(f"invalid target SSD in mapped source: {target_text}") from error
    return MappedSource(input_csv.strip(), target_ssd)


def convert_concurrent(
    sources: Sequence[MappedSource],
    output_trace: str,
    ssd_count: int,
    stripe_lba: int,
    ssd_lba_count: int,
    time_acceleration: int = DEFAULT_TIME_ACCELERATION,
    window_start_seconds: float = 0,
    window_duration_seconds: Optional[float] = None,
    max_source_requests: int = 0,
) -> dict:
    """Merge volume traces by absolute time and encode each volume into its target SSD."""
    validate_time_acceleration(time_acceleration)
    usable_ssd_lba_count = validate_device_local_geometry(
        ssd_count,
        stripe_lba,
        ssd_lba_count,
    )
    if not sources:
        raise ValueError("at least one mapped source is required")
    if window_start_seconds < 0:
        raise ValueError("window_start_seconds must not be negative")
    if window_duration_seconds is not None and window_duration_seconds <= 0:
        raise ValueError("window_duration_seconds must be greater than zero")
    if max_source_requests < 0:
        raise ValueError("max_source_requests must not be negative")

    for source in sources:
        if source.target_ssd < 0 or source.target_ssd >= ssd_count:
            raise ValueError(
                f"target SSD {source.target_ssd} for {source.input_csv} is outside "
                f"0..{ssd_count - 1}"
            )

    iterators = [iter(iter_msrc_requests(source.input_csv)) for source in sources]
    heap = []
    insertion_order = 0
    for source_index, iterator in enumerate(iterators):
        try:
            request = next(iterator)
        except StopIteration:
            continue
        heapq.heappush(
            heap,
            (request.timestamp_ticks, insertion_order, source_index, request),
        )
        insertion_order += 1

    if not heap:
        raise ValueError("mapped sources do not contain any valid MSRC requests")

    first_global_timestamp = heap[0][0]
    window_start_ticks = first_global_timestamp + round(
        window_start_seconds * FILETIME_TICKS_PER_SECOND
    )
    window_end_ticks = None
    if window_duration_seconds is not None:
        window_end_ticks = window_start_ticks + round(
            window_duration_seconds * FILETIME_TICKS_PER_SECOND
        )

    os.makedirs(os.path.dirname(output_trace) or ".", exist_ok=True)

    accepted_source_requests = 0
    written_rows = 0
    split_source_rows = 0
    wrapped_source_rows = 0
    first_emitted_timestamp = None
    last_emitted_timestamp = None
    per_ssd = {
        ssd: {
            "source_requests": 0,
            "written_rows": 0,
            "write_sectors": 0,
            "read_sectors": 0,
        }
        for ssd in range(ssd_count)
    }

    with open(output_trace, "w", newline="") as f_out:
        while heap:
            timestamp_ticks, _order, source_index, request = heapq.heappop(heap)

            if window_end_ticks is not None and timestamp_ticks >= window_end_ticks:
                break
            if max_source_requests and accepted_source_requests >= max_source_requests:
                break

            if timestamp_ticks >= window_start_ticks:
                source = sources[source_index]
                target_ssd = source.target_ssd
                accepted_source_requests += 1
                per_ssd[target_ssd]["source_requests"] += 1

                if (
                    request.start_lba >= usable_ssd_lba_count
                    or request.start_lba + request.lba_count > usable_ssd_lba_count
                ):
                    wrapped_source_rows += 1

                segments = list(
                    iter_device_local_segments(
                        request.start_lba,
                        request.lba_count,
                        target_ssd,
                        ssd_count,
                        stripe_lba,
                        usable_ssd_lba_count,
                    )
                )
                if len(segments) > 1:
                    split_source_rows += 1

                time_ns = (
                    (timestamp_ticks - window_start_ticks) * 100 // time_acceleration
                )
                if first_emitted_timestamp is None:
                    first_emitted_timestamp = timestamp_ticks
                last_emitted_timestamp = timestamp_ticks

                sector_key = (
                    "write_sectors" if request.type_code == WRITE_CODE else "read_sectors"
                )
                per_ssd[target_ssd][sector_key] += request.lba_count

                for global_lba, segment_lba_count in segments:
                    f_out.write(
                        f"{time_ns} {target_ssd} {global_lba} "
                        f"{segment_lba_count} {request.type_code}\n"
                    )
                    written_rows += 1
                    per_ssd[target_ssd]["written_rows"] += 1

            try:
                next_request = next(iterators[source_index])
            except StopIteration:
                continue
            heapq.heappush(
                heap,
                (
                    next_request.timestamp_ticks,
                    insertion_order,
                    source_index,
                    next_request,
                ),
            )
            insertion_order += 1

    summary = {
        "output_trace": output_trace,
        "source_count": len(sources),
        "accepted_source_requests": accepted_source_requests,
        "written_rows": written_rows,
        "split_source_rows": split_source_rows,
        "wrapped_source_rows": wrapped_source_rows,
        "time_acceleration": time_acceleration,
        "window_start_seconds": window_start_seconds,
        "window_duration_seconds": window_duration_seconds,
        "first_global_timestamp": first_global_timestamp,
        "first_emitted_timestamp": first_emitted_timestamp,
        "last_emitted_timestamp": last_emitted_timestamp,
        "usable_ssd_lba_count": usable_ssd_lba_count,
        "per_ssd": per_ssd,
    }

    print(
        f"Done: {output_trace} "
        f"(source_requests={accepted_source_requests}, written_rows={written_rows}, "
        f"sources={len(sources)}, acceleration={time_acceleration}x, "
        f"split_source_rows={split_source_rows}, "
        f"wrapped_source_rows={wrapped_source_rows})"
    )
    for ssd in range(ssd_count):
        stats = per_ssd[ssd]
        print(
            f"  SSD{ssd}: source_requests={stats['source_requests']}, "
            f"written_rows={stats['written_rows']}, "
            f"write_sectors={stats['write_sectors']}, "
            f"read_sectors={stats['read_sectors']}"
        )
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert MSRC CSV trace to MQSim ASCII trace format")
    parser.add_argument("-i", "--input", default=DEFAULT_INPUT, help="Input MSRC CSV path")
    parser.add_argument("-o", "--output", default=DEFAULT_OUTPUT, help="Output MQSim trace path")
    parser.add_argument(
        "--source",
        action="append",
        default=[],
        metavar="TARGET_SSD=CSV",
        help=(
            "Map one MSRC volume CSV to an SSD in concurrent mode. Repeat this option "
            "for multiple volumes; requests are merged by their absolute timestamps."
        ),
    )
    parser.add_argument(
        "--address-mode",
        choices=(ADDRESS_MODE_GLOBAL, ADDRESS_MODE_DEVICE_LOCAL),
        default=ADDRESS_MODE_GLOBAL,
        help=(
            "GLOBAL_LBA keeps the source LBA unchanged. DEVICE_LOCAL encodes a file-level "
            "target SSD into a RAID-global LBA and splits requests at stripe boundaries."
        ),
    )
    parser.add_argument(
        "--target-ssd",
        type=int,
        help="Destination SSD for every row in DEVICE_LOCAL mode (zero based)",
    )
    parser.add_argument("--ssd-count", type=int, default=4, help="RAID SSD count for DEVICE_LOCAL mode")
    parser.add_argument("--stripe-lba", type=int, default=512, help="RAID stripe size in 512-byte sectors")
    parser.add_argument(
        "--ssd-lba-count",
        type=int,
        help=(
            "Logical sector count of one SSD in DEVICE_LOCAL mode. The converter rounds this down "
            "to a stripe multiple before wrapping source LBAs."
        ),
    )
    parser.add_argument(
        "--time-acceleration",
        type=int,
        default=DEFAULT_TIME_ACCELERATION,
        help=(
            "Replay-time acceleration factor. 1 preserves original timing; 16 retains "
            "the converter's previous behavior. Default: 16"
        ),
    )
    parser.add_argument(
        "--window-start-seconds",
        type=float,
        default=0,
        help="Concurrent mode: skip this many original-trace seconds from the earliest source",
    )
    parser.add_argument(
        "--window-duration-seconds",
        type=float,
        help="Concurrent mode: include this many original-trace seconds",
    )
    parser.add_argument(
        "--max-source-requests",
        type=int,
        default=0,
        help=(
            "Stop after this many valid source requests. Stripe-split output "
            "rows do not count toward this limit. Use 0 for no limit"
        ),
    )
    args = parser.parse_args()

    if args.source:
        if args.target_ssd is not None:
            parser.error("--target-ssd cannot be combined with repeated --source mappings")
        if args.ssd_lba_count is None:
            parser.error("concurrent --source mode requires --ssd-lba-count")
        try:
            sources = [parse_mapped_source(value) for value in args.source]
            convert_concurrent(
                sources,
                args.output,
                args.ssd_count,
                args.stripe_lba,
                args.ssd_lba_count,
                args.time_acceleration,
                args.window_start_seconds,
                args.window_duration_seconds,
                args.max_source_requests,
            )
        except (OSError, RuntimeError, ValueError) as error:
            parser.error(str(error))
        return

    if args.window_start_seconds or args.window_duration_seconds is not None:
        parser.error("window options require at least one --source mapping")
    convert(
        args.input,
        args.output,
        args.address_mode,
        args.target_ssd,
        args.ssd_count,
        args.stripe_lba,
        args.ssd_lba_count,
        args.time_acceleration,
        args.max_source_requests,
    )


if __name__ == "__main__":
    main()
