#!/usr/bin/env python3
"""Deterministic, strict single-device trace conversion for original MQSim.

Python 3.10+, standard library. See README.md for source formats and policies.
"""
from __future__ import annotations

import argparse
import bz2
import contextlib
import csv
from dataclasses import dataclass, asdict
from decimal import Decimal, InvalidOperation
import gzip
import hashlib
import io
import json
import lzma
import os
from pathlib import Path
import re
import sqlite3
import struct
import sys
import tempfile

VERSION = "1.0.0"
CONVERTER_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
FORMATS = ("rocksdb", "msrc", "mobile", "cloudphysics-csv", "cloudphysics-vscsi")
ALIASES = {"msrc1": "msrc", "msrc2": "msrc",
           "rocksdb-overwritezipf": "rocksdb", "rocksdb-ycsba": "rocksdb"}
READ_COMMANDS = {0x08, 0x28, 0x88, 0xA8}  # SCSI READ(6/10/16/12)
WRITE_COMMANDS = {0x0A, 0x2A, 0x8A, 0xAA}  # SCSI WRITE(6/10/16/12)
MAX_TIME = (1 << 63) - 1
MAX_LBA = (1 << 64) - 1
MAX_SECTORS = (1 << 32) - 1


class ConversionError(ValueError):
    pass


@dataclass(frozen=True)
class Record:
    time_ns: int
    device: str
    lba: int
    sectors: int
    operation: int  # MQSim: 0 write, 1 read


@dataclass
class Counts:
    requests: int = 0
    reads: int = 0
    writes: int = 0
    read_bytes: int = 0
    write_bytes: int = 0

    def add(self, record):
        self.requests += 1
        if record.operation == 1:
            self.reads += 1
            self.read_bytes += record.sectors * 512
        else:
            self.writes += 1
            self.write_bytes += record.sectors * 512


def unsigned(value, field):
    text = str(value).strip()
    if not re.fullmatch(r"[0-9]+", text):
        raise ConversionError(f"{field}: expected a nonnegative decimal integer, got {text!r}")
    return int(text)


def time_to_ns(text, multiplier, policy="error", metadata=None):
    try:
        value = Decimal(str(text).strip())
        if not value.is_finite() or value < 0:
            raise ValueError()
        numerator, denominator = value.as_integer_ratio()
        ns, remainder = divmod(numerator * multiplier, denominator)
        if remainder:
            if policy == "error":
                raise ConversionError(f"Timestamp {text!r} has sub-nanosecond precision; choose --subnanosecond floor or nearest explicitly")
            if policy not in ("floor", "nearest"):
                raise ConversionError(f"Invalid subnanosecond policy: {policy}")
            if policy == "nearest" and (2 * remainder > denominator or (2 * remainder == denominator and ns % 2)):
                ns += 1  # Exact rational comparison; ties round to the even integer.
            if metadata is not None:
                metadata["subnanosecond_rows"] += 1
        if ns >= 10**40:
            raise ValueError()
        return ns
    except (InvalidOperation, ValueError, OverflowError) as error:
        if isinstance(error, ConversionError):
            raise
        raise ConversionError(f"Invalid timestamp: {text!r}") from error


def byte_sectors(text, field, positive=False):
    value = unsigned(text, field)
    if value % 512 or (positive and value == 0):
        raise ConversionError(f"{field}: {value} bytes is not a {'positive ' if positive else ''}multiple of 512")
    return value // 512


def rw(text, read, write):
    if text == read:
        return 1
    if text == write:
        return 0
    raise ConversionError(f"Unsupported operation {text!r}; expected {read!r} or {write!r}")


def scsi_operation(text):
    value = str(text).strip()
    if re.fullmatch(r"0[xX][0-9a-fA-F]+", value):
        command = int(value, 16)
    else:
        command = unsigned(value, "SCSI command")
    if command in READ_COMMANDS:
        return 1
    if command in WRITE_COMMANDS:
        return 0
    raise ConversionError(f"Unsupported SCSI command 0x{command:x}; only READ/WRITE(6/10/12/16) are supported")


def parse_capacity(text):
    if text is None:
        return None
    match = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?)(B|KB|MB|GB|TB|KiB|MiB|GiB|TiB)", str(text))
    if not match:
        raise ConversionError("Capacity requires an explicit unit, for example 256GB or 256GiB")
    scale = {"B": 1, "KB": 10**3, "MB": 10**6, "GB": 10**9, "TB": 10**12,
             "KiB": 2**10, "MiB": 2**20, "GiB": 2**30, "TiB": 2**40}[match[2]]
    numerator, denominator = Decimal(match[1]).as_integer_ratio()
    size, remainder = divmod(numerator * scale, denominator)
    if remainder or size <= 0 or size % 512:
        raise ConversionError("Capacity must be a positive whole number of 512-byte sectors")
    return size


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


@contextlib.contextmanager
def open_binary(path):
    suffix = path.suffix.lower()
    opener = {".gz": gzip.open, ".bz2": bz2.open, ".xz": lzma.open}.get(suffix)
    if opener:
        with opener(path, "rb") as stream:
            yield stream
    elif suffix == ".zst":
        try:
            from compression import zstd
        except ImportError:
            try:
                import zstandard
            except ImportError as error:
                raise ConversionError(".zst requires Python 3.14+ or: python -m pip install zstandard") from error
            with path.open("rb") as raw, zstandard.ZstdDecompressor().stream_reader(raw) as stream:
                yield stream
        else:
            with zstd.open(path, "rb") as stream:
                yield stream
    else:
        with path.open("rb") as stream:
            yield stream


def csv_records(path, fmt, length_unit, metadata, subnanosecond):
    expected = {
        "rocksdb": ["io_type", "offset_secs", "length_secs", "ts"],
        "msrc": ["timestamp", "hostname", "disknumber", "type", "offset", "size", "responsetime"],
        "mobile": ["process", "device", "rw_flag", "sector", "size", "timestamp"],
        "cloudphysics-csv": ["timestamp", "lbn", "len", "cmd", "ver"],
    }[fmt]
    with open_binary(path) as binary:
        with io.TextIOWrapper(binary, encoding="utf-8-sig", newline="") as text:
            reader = csv.reader(text, strict=True)
            first = True
            indices = list(range(len(expected)))
            for row in reader:
                line = reader.line_num
                if not row:
                    metadata["blank_rows"] += 1
                    continue
                try:
                    if len(row) != len(expected):
                        raise ConversionError(f"Expected {len(expected)} CSV columns, found {len(row)}")
                    row = [cell.strip() for cell in row]
                    names = [cell.lower() for cell in row]
                    if names[0] == "proces":
                        names[0] = "process"  # spelling in published Mobile traces
                    if first:
                        first = False
                        if sorted(names) == sorted(expected):
                            indices = [names.index(name) for name in expected]
                            metadata["header_rows"] = 1
                            continue
                        if fmt in ("mobile", "cloudphysics-csv"):
                            raise ConversionError(f"Required CSV header: {','.join(expected)}")
                    row = [row[index] for index in indices]
                    if fmt == "rocksdb":
                        op, lba, size, ts = row
                        record = Record(time_to_ns(ts, 10**9, subnanosecond, metadata), "0", unsigned(lba, "offset_secs"),
                                        unsigned(size, "length_secs"), rw(op, "0", "1"))
                    elif fmt == "msrc":
                        ts, host, disk, op, offset, size, response = row
                        if not host:
                            raise ConversionError("Empty MSRC hostname")
                        disk = unsigned(disk, "DiskNumber")
                        unsigned(response, "ResponseTime")
                        record = Record(unsigned(ts, "FILETIME") * 100, f"{host}:{disk}",
                                        byte_sectors(offset, "Offset"), byte_sectors(size, "Size", True),
                                        rw(op, "Read", "Write"))
                    elif fmt == "mobile":
                        process, device, op, sector, size, ts = row
                        record = Record(time_to_ns(ts, 10**9, subnanosecond, metadata), str(unsigned(device, "device")),
                                        unsigned(sector, "sector"), unsigned(size, "size"), rw(op, "R", "W"))
                    else:
                        ts, lbn, length, command, ver = row
                        unsigned(ver, "ver")
                        size = byte_sectors(length, "len", True) if length_unit == "bytes" else unsigned(length, "len")
                        record = Record(time_to_ns(ts, 1000, subnanosecond, metadata), "0", unsigned(lbn, "lbn"), size, scsi_operation(command))
                    yield line, record
                except (ValueError, OverflowError) as error:
                    raise ConversionError(f"{path.name}: row {line}: {error}") from error


def vscsi_records(path, metadata):
    with open_binary(path) as stream:
        prefix = stream.read(16)
        if len(prefix) != 16:
            raise ConversionError("VSCSI input is empty or has a truncated record")
        versions = []
        if struct.unpack_from("<H", prefix, 14)[0] >> 8 == 1:
            versions.append(1)
        if struct.unpack_from("<H", prefix, 2)[0] >> 8 == 2:
            versions.append(2)
        if len(versions) != 1:
            raise ConversionError("Cannot unambiguously detect VSCSI1/VSCSI2; LCS and oracleGeneral are not raw VSCSI")
        version = versions[0]
        metadata["vscsi_version"] = version
        parser = struct.Struct("<IIIHHQQ" if version == 1 else "<HHIIIQQQ")
        index = 0
        raw = prefix + stream.read(parser.size - 16)
        while raw:
            index += 1
            try:
                if len(raw) != parser.size:
                    raise ConversionError("Truncated VSCSI record")
                fields = parser.unpack(raw)
                if version == 1:
                    _, length, _, command, ver, lbn, timestamp = fields
                else:
                    command, ver, _, length, _, lbn, timestamp, _ = fields
                if ver >> 8 != version:
                    raise ConversionError("Mixed or invalid VSCSI version")
                yield index, Record(timestamp * 1000, "0", lbn, byte_sectors(length, "len", True), scsi_operation(command))
            except ValueError as error:
                raise ConversionError(f"{path.name}: record {index}: {error}") from error
            raw = stream.read(parser.size)


def validate_record(record, capacity):
    if record.time_ns < 0 or record.time_ns >= 10**40:
        raise ConversionError("Timestamp outside supported range")
    if not 0 <= record.lba <= MAX_LBA or not 1 <= record.sectors <= MAX_SECTORS:
        raise ConversionError("LBA or request size outside MQSim integer range")
    if record.lba + record.sectors - 1 > MAX_LBA:
        raise ConversionError("Request end LBA overflows uint64")
    if capacity is not None and (record.lba + record.sectors) * 512 > capacity:
        raise ConversionError(f"Request ends at byte {(record.lba + record.sectors) * 512}, beyond capacity {capacity}; address folding is disabled")


def inspect_output(path):
    """Independent second pass over the published output syntax and counters."""
    counts = Counts()
    digest = hashlib.sha256()
    previous = None
    first = None
    minimum = None
    maximum = 0
    with Path(path).open("rb") as stream:
        for number, raw in enumerate(stream, 1):
            digest.update(raw)
            if not re.fullmatch(rb"[0-9]+ 0 [0-9]+ [0-9]+ [01]\n", raw):
                raise ConversionError(f"Output row {number}: invalid MQSim syntax (requires LF and five columns)")
            ts, device, lba, size, op = map(int, raw.split())
            record = Record(ts, str(device), lba, size, op)
            validate_record(record, None)
            if ts > MAX_TIME or (previous is not None and ts < previous):
                raise ConversionError(f"Output row {number}: timestamp overflow or decreasing timestamp")
            if first is None:
                first = ts
            previous = ts
            minimum = lba if minimum is None else min(minimum, lba)
            maximum = max(maximum, lba + size)
            counts.add(record)
    if not counts.requests:
        raise ConversionError("No I/O requests in output")
    return {**asdict(counts), "sha256": digest.hexdigest(), "first_time_ns": first,
            "last_time_ns": previous, "duration_ns": previous - first,
            "min_lba": minimum, "max_lba_exclusive": maximum, "required_capacity_bytes": maximum * 512}


def verify(output):
    output = Path(output)
    manifest = json.loads(Path(str(output) + ".manifest.json").read_text(encoding="utf-8"))
    measured = inspect_output(output)
    if manifest.get("schema_version") != 1 or measured != manifest.get("output"):
        raise ConversionError("Output does not match its manifest")
    if {k: measured[k] for k in asdict(Counts())} != manifest["selected"]:
        raise ConversionError("Selected input counts differ from output counts")
    capacity = manifest["options"]["capacity_bytes"]
    if capacity is not None and measured["required_capacity_bytes"] > capacity:
        raise ConversionError("Output exceeds declared capacity")
    return manifest


def convert(input_path, output_path, format, *, capacity=None, source_device=None,
            sort_time=False, length_unit=None, subnanosecond="error", progress=None):
    source, output = Path(input_path).resolve(), Path(output_path).resolve()
    fmt = ALIASES.get(format, format)
    if fmt not in FORMATS:
        raise ConversionError(f"Unsupported format: {format}")
    if not source.is_file():
        raise ConversionError(f"Input file not found: {source}")
    if any(part in source.name.lower() for part in (".lcs", "oraclegeneral")):
        raise ConversionError("LCS/oracleGeneral may have remapped addresses, split requests, or lost R/W/time precision. Supply original VSCSI or a documented CSV.")
    if fmt == "cloudphysics-csv" and length_unit not in ("bytes", "sectors"):
        raise ConversionError("CloudPhysics CSV requires --length-unit bytes or sectors; the dataset descriptions disagree")
    if fmt != "cloudphysics-csv" and length_unit is not None:
        raise ConversionError("--length-unit applies only to cloudphysics-csv")
    if not isinstance(sort_time, bool):
        raise ConversionError("sort_time must be true or false")
    if subnanosecond not in ("error", "floor", "nearest"):
        raise ConversionError("subnanosecond must be error, floor, or nearest")
    if source_device is not None and not isinstance(source_device, str):
        raise ConversionError("source_device must be a string")
    capacity_bytes = parse_capacity(capacity)
    manifest_path = Path(str(output) + ".manifest.json")
    if source in (output, manifest_path) or output.exists() or manifest_path.exists():
        raise ConversionError("Input/output collision or existing output; choose a new output filename")
    output.parent.mkdir(parents=True, exist_ok=True)
    signature = source.stat()
    source_hash = sha256(source)
    metadata = {"blank_rows": 0, "header_rows": 0, "subnanosecond_rows": 0}
    total, selected, excluded = Counts(), Counts(), Counts()
    devices = {}
    previous = None
    reversals = 0
    chosen_device = source_device
    published = []
    try:
        with tempfile.TemporaryDirectory(prefix=".mqsim-convert-", dir=output.parent) as scratch:
            scratch = Path(scratch).resolve()
            if scratch.parent != output.parent:
                raise ConversionError("Temporary directory is outside output directory")
            trace_temp = scratch / "output.trace"
            records = vscsi_records(source, metadata) if fmt == "cloudphysics-vscsi" else csv_records(source, fmt, length_unit, metadata, subnanosecond)

            def selected_records():
                nonlocal chosen_device, previous, reversals
                for position, record in records:
                    validate_record(record, None)
                    total.add(record)
                    devices[record.device] = devices.get(record.device, 0) + 1
                    if chosen_device is None:
                        chosen_device = record.device
                    if source_device is None and record.device != chosen_device:
                        raise ConversionError(f"Multiple source devices ({chosen_device!r}, {record.device!r}); use --source-device and convert each device separately")
                    if record.device != chosen_device:
                        excluded.add(record)
                        continue
                    try:
                        validate_record(record, capacity_bytes)
                    except ConversionError as error:
                        raise ConversionError(f"{source.name}: request/row {position}: {error}") from error
                    if previous is not None and record.time_ns < previous:
                        reversals += 1
                        if not sort_time:
                            raise ConversionError(f"{source.name}: request/row {position}: decreasing timestamp; explicit --sort-time performs a stable disk sort")
                    previous = record.time_ns
                    selected.add(record)
                    if progress and total.requests % 100000 == 0:
                        progress(total.requests)
                    yield record

            def write_rows(rows):
                first = None
                with trace_temp.open("w", encoding="ascii", newline="\n", buffering=1024 * 1024) as out:
                    for record in rows:
                        if first is None:
                            first = record.time_ns
                        ts = record.time_ns - first
                        if ts < 0 or ts > MAX_TIME:
                            raise ConversionError("Relative timestamp exceeds MQSim int64 range")
                        out.write(f"{ts} 0 {record.lba} {record.sectors} {record.operation}\n")
                return first

            if sort_time:
                with contextlib.closing(sqlite3.connect(scratch / "sort.sqlite")) as db:
                    db.execute("PRAGMA temp_store=FILE")
                    db.execute("PRAGMA cache_size=-32768")
                    db.execute("CREATE TABLE requests (seq INTEGER PRIMARY KEY, ts TEXT, lba TEXT, size INTEGER, op INTEGER)")
                    db.executemany("INSERT INTO requests VALUES (?,?,?,?,?)",
                                   ((i, str(r.time_ns).zfill(40), str(r.lba), r.sectors, r.operation)
                                    for i, r in enumerate(selected_records())))
                    db.commit()
                    cursor = db.execute("SELECT ts,lba,size,op FROM requests ORDER BY ts,seq")
                    origin = write_rows(Record(int(t), chosen_device, int(l), s, o) for t, l, s, o in cursor)
            else:
                origin = write_rows(selected_records())
            if selected.requests == 0:
                raise ConversionError(f"No requests selected (source device {source_device!r})")
            after = source.stat()
            if (signature.st_size, signature.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise ConversionError("Source changed during conversion")
            measured = inspect_output(trace_temp)
            if {k: measured[k] for k in asdict(selected)} != asdict(selected):
                raise ConversionError("Input/output request or byte counts differ")
            result = {
                "schema_version": 1, "converter_version": VERSION,
                "converter_sha256": CONVERTER_SHA256,
                "format": fmt,
                "source": {"name": source.name, "bytes": signature.st_size, "sha256": source_hash,
                           "devices": devices, **metadata},
                "options": {"capacity_bytes": capacity_bytes, "source_device_filter": source_device,
                            "sort_time": sort_time, "cloudphysics_length_unit": length_unit,
                            "subnanosecond": subnanosecond},
                "transformations": {"time_unit": "nanosecond", "sector_bytes": 512,
                                    "time_origin_ns": origin, "selected_source_device": chosen_device,
                                    "output_device": 0, "write_code": 0, "read_code": 1,
                                    "timestamp_reversals_in_source": reversals,
                                    "address_remapping": "none", "replay_count": 1},
                "input": asdict(total), "selected": asdict(selected), "excluded_by_device": asdict(excluded),
                "output": measured,
                "validation": {"independent_output_pass": "passed", "counts_and_bytes": "passed",
                               "capacity": "checked" if capacity_bytes is not None else "not_checked"},
            }
            manifest_temp = scratch / "manifest.json"
            manifest_temp.write_text(json.dumps(result, ensure_ascii=True, indent=2, sort_keys=True) + "\n", encoding="ascii", newline="\n")
            # Exclusive creation prevents accidental replacement, including a concurrent writer.
            for target, temp in ((manifest_path, manifest_temp), (output, trace_temp)):
                with target.open("xb") as out, temp.open("rb") as inp:
                    published.append(target)
                    for block in iter(lambda: inp.read(1024 * 1024), b""):
                        out.write(block)
            return result
    except BaseException:
        for path in published:
            path.unlink(missing_ok=True)
        raise


def run_batch(config_path):
    config_path = Path(config_path).resolve()
    config = json.loads(config_path.read_text(encoding="utf-8-sig"))
    if set(config) != {"schema_version", "jobs"} or config["schema_version"] != 1 or not isinstance(config["jobs"], list) or not config["jobs"]:
        raise ConversionError("Batch config requires schema_version: 1 and a nonempty jobs array")
    allowed = {"input", "output", "format", "capacity", "source_device", "sort_time", "length_unit", "subnanosecond"}
    jobs, destinations = [], set()
    for number, job in enumerate(config["jobs"], 1):
        if not isinstance(job, dict) or set(job) - allowed or not {"input", "output", "format"} <= set(job):
            raise ConversionError(f"Invalid batch job {number}")
        args = dict(job)
        args["input_path"] = (config_path.parent / args.pop("input")).resolve()
        args["output_path"] = (config_path.parent / args.pop("output")).resolve()
        paths = {args["output_path"], Path(str(args["output_path"]) + ".manifest.json")}
        if paths & destinations:
            raise ConversionError("Batch jobs share output filenames")
        destinations |= paths
        jobs.append(args)
    if any(job["input_path"] in destinations for job in jobs):
        raise ConversionError("A batch output would overwrite another job's input")
    failed = False
    for number, job in enumerate(jobs, 1):
        try:
            result = convert(**job)
            print(json.dumps({"job": number, "status": "ok", "output": str(job["output_path"]), **result["output"]}), flush=True)
        except (OSError, ValueError, csv.Error, EOFError, sqlite3.Error) as error:
            failed = True
            print(json.dumps({"job": number, "status": "error", "error": str(error)}), flush=True)
    return int(failed)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", action="version", version=VERSION)
    sub = parser.add_subparsers(dest="command", required=True)
    single = sub.add_parser("convert", help="Convert one source device from one file")
    single.add_argument("--format", required=True, choices=FORMATS + tuple(ALIASES))
    single.add_argument("--input", required=True, dest="input_path")
    single.add_argument("--output", required=True, dest="output_path")
    single.add_argument("--capacity", help="Optional logical capacity, e.g. 256GB or 256GiB (includes unit)")
    single.add_argument("--source-device", help="MSRC: hostname:disk; Mobile: device number")
    single.add_argument("--sort-time", action="store_true", help="Explicit stable external sort; ties keep source order")
    single.add_argument("--length-unit", choices=("bytes", "sectors"), help="Required only for CloudPhysics CSV")
    single.add_argument("--subnanosecond", choices=("error", "floor", "nearest"), default="error",
                        help="Explicit sub-ns policy; nearest uses ties-to-even. Default: error")
    check = sub.add_parser("verify", help="Re-read output and verify its SHA-256 and manifest counts")
    check.add_argument("output")
    batch = sub.add_parser("batch", help="Run a shared JSON batch configuration")
    batch.add_argument("config")
    args = vars(parser.parse_args(argv))
    command = args.pop("command")
    try:
        if command == "batch":
            return run_batch(args["config"])
        result = verify(args["output"]) if command == "verify" else convert(**args)
        print(json.dumps({"status": "ok", **result["output"]}, ensure_ascii=True), flush=True)
        return 0
    except (OSError, ValueError, csv.Error, EOFError, sqlite3.Error) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
