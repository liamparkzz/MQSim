import contextlib
import csv
import gzip
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest

import mqsim_trace_converter as m


class ConverterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def source(self, text, name="input.csv"):
        path = self.root / name
        if isinstance(text, bytes):
            path.write_bytes(text)
        else:
            path.write_text(text, encoding="utf-8", newline="")
        return path

    def convert(self, text, fmt="rocksdb", **kwargs):
        path = self.source(text)
        output = self.root / "output.trace"
        report = m.convert(path, output, fmt, **kwargs)
        self.assertEqual(m.verify(output), report)
        return output.read_text(), report

    def test_rocksdb_mapping_precision(self):
        data = "0,128,8,1000000000.000000001\n1,8,16,1000000000.000000002\n"
        output, report = self.convert(data)
        self.assertEqual(output, "0 0 128 8 1\n1 0 8 16 0\n")
        self.assertEqual(report["selected"], dict(requests=2, reads=1, writes=1, read_bytes=4096, write_bytes=8192))

    def test_msrc1_msrc2_filetime(self):
        for fmt in ("msrc1", "msrc2"):
            with self.subTest(fmt=fmt):
                source = self.source("128166372002993263,usr,0,Read,1024,4096,30\n128166372002993266,usr,0,Write,512,512,9\n")
                output = self.root / (fmt + ".trace")
                m.convert(source, output, fmt)
                self.assertEqual(output.read_text(), "0 0 2 8 1\n300 0 1 1 0\n")

    def test_mobile_header_gzip(self):
        raw = b"proces,device,rw_flag,sector,size,timestamp\nx,8388608,W,128,8,0.100000001\ny,8388608,R,136,16,0.100000002\n"
        source = self.source(gzip.compress(raw, mtime=0), "phone.csv.gz")
        output = self.root / "phone.trace"
        report = m.convert(source, output, "mobile")
        self.assertEqual(output.read_text(), "0 0 128 8 0\n1 0 136 16 1\n")
        self.assertEqual(report["source"]["header_rows"], 1)

    def test_subnanosecond_explicit_policies(self):
        data = "0,0,8,5218128.1812390005\n1,8,8,5218128.1812390015\n0,16,8,5218128.1812390026\n"
        source = self.source(data)
        with self.assertRaisesRegex(m.ConversionError, "sub-nanosecond"):
            m.convert(source, self.root / "error.trace", "rocksdb")
        for policy, expected in (("floor", "0 0 0 8 1\n1 0 8 8 0\n2 0 16 8 1\n"),
                                 ("nearest", "0 0 0 8 1\n2 0 8 8 0\n3 0 16 8 1\n")):
            out = self.root / (policy + ".trace")
            report = m.convert(source, out, "rocksdb", subnanosecond=policy)
            self.assertEqual(out.read_text(), expected)
            self.assertEqual(report["source"]["subnanosecond_rows"], 3)
            self.assertEqual(report["options"]["subnanosecond"], policy)
            self.assertEqual(report, m.verify(out))
        with self.assertRaisesRegex(m.ConversionError, "subnanosecond must"):
            m.convert(source, self.root / "bad.trace", "rocksdb", subnanosecond="truncate")

    def test_rounding_exact_values_and_large_seconds(self):
        self.assertEqual(m.time_to_ns("1000000000.0000000016", 10**9, "nearest"), 1000000000000000002)
        self.assertEqual(m.time_to_ns("0.0000000009", 10**9, "floor"), 0)
        self.assertEqual(m.time_to_ns("0.0000000025", 10**9, "nearest"), 2)
        self.assertEqual(m.time_to_ns("0.0000000035", 10**9, "nearest"), 4)
        self.assertEqual(m.time_to_ns("0.000000001", 10**9), 1)

    def test_cloudphysics_csv_explicit_units(self):
        source = self.source("timestamp,lbn,len,cmd,ver\n1234567,100,4096,40,256\n1234568,200,512,0x2a,256\n")
        with self.assertRaisesRegex(m.ConversionError, "length-unit"):
            m.convert(source, self.root / "none.trace", "cloudphysics-csv")
        out = self.root / "bytes.trace"
        m.convert(source, out, "cloudphysics-csv", length_unit="bytes")
        self.assertEqual(out.read_text(), "0 0 100 8 1\n1000 0 200 1 0\n")
        out = self.root / "sectors.trace"
        m.convert(source, out, "cloudphysics-csv", length_unit="sectors")
        self.assertEqual(out.read_text(), "0 0 100 4096 1\n1000 0 200 512 0\n")

    def test_vscsi_both_versions(self):
        v1 = struct.pack("<IIIHHQQ", 1, 4096, 1, 40, 256, 24, 12345)
        v1 += struct.pack("<IIIHHQQ", 2, 512, 1, 42, 256, 40, 12347)
        v2 = struct.pack("<HHIIIQQQ", 40, 512, 1, 4096, 1, 24, 12345, 55)
        v2 += struct.pack("<HHIIIQQQ", 42, 512, 2, 512, 1, 40, 12347, 77)
        for n, data in enumerate((v1, v2), 1):
            source = self.source(data, f"v{n}.vscsi")
            output = self.root / f"v{n}.trace"
            report = m.convert(source, output, "cloudphysics-vscsi")
            self.assertEqual(output.read_text(), "0 0 24 8 1\n2000 0 40 1 0\n")
            self.assertEqual(report["source"]["vscsi_version"], n)

    def test_truncated_vscsi(self):
        data = struct.pack("<IIIHHQQ", 1, 4096, 1, 40, 256, 24, 12345) + b"x"
        with self.assertRaisesRegex(m.ConversionError, "Truncated"):
            self.convert(data, "cloudphysics-vscsi")
        self.assertFalse((self.root / "output.trace").exists())

    def test_scsi_unknown_command_rejected(self):
        data = struct.pack("<IIIHHQQ", 1, 4096, 1, 0x42, 256, 24, 12345)
        with self.assertRaisesRegex(m.ConversionError, "SCSI command"):
            self.convert(data, "cloudphysics-vscsi")

    def test_sort_time_stable_and_explicit(self):
        data = "1,64,8,2\n0,16,8,1\n1,24,8,1\n"
        source = self.source(data)
        with self.assertRaisesRegex(m.ConversionError, "decreasing"):
            m.convert(source, self.root / "reject.trace", "rocksdb")
        self.assertFalse((self.root / "reject.trace.manifest.json").exists())
        output = self.root / "sort.trace"
        report = m.convert(source, output, "rocksdb", sort_time=True)
        self.assertEqual(output.read_text(), "0 0 16 8 1\n0 0 24 8 0\n1000000000 0 64 8 0\n")
        self.assertEqual(report["transformations"]["timestamp_reversals_in_source"], 1)

    def test_sort_filetime_larger_than_sqlite_int64(self):
        data = "128166372002993266,usr,0,Read,1024,512,0\n128166372002993263,usr,0,Write,0,512,0\n"
        output, _ = self.convert(data, "msrc", sort_time=True)
        self.assertEqual(output, "0 0 0 1 0\n300 0 2 1 1\n")

    def test_mixed_device_requires_selection(self):
        data = "10,usr,0,Read,0,512,0\n20,usr,1,Write,0,1024,0\n"
        source = self.source(data)
        with self.assertRaisesRegex(m.ConversionError, "Multiple source devices"):
            m.convert(source, self.root / "mixed.trace", "msrc")
        output = self.root / "selected.trace"
        report = m.convert(source, output, "msrc", source_device="usr:1")
        self.assertEqual(output.read_text(), "0 0 0 2 0\n")
        self.assertEqual(report["excluded_by_device"]["requests"], 1)
        self.assertEqual(report["input"]["requests"], 2)

    def test_capacity_uses_request_end_and_never_remaps(self):
        self.assertEqual(m.parse_capacity("256GB"), 256000000000)
        self.assertEqual(m.parse_capacity("256GiB"), 274877906944)
        source = self.source("0,7,2,0\n")
        with self.assertRaisesRegex(m.ConversionError, "beyond capacity"):
            m.convert(source, self.root / "bad.trace", "rocksdb", capacity="4KiB")
        output = self.root / "valid.trace"
        m.convert(source, output, "rocksdb", capacity="8KiB")
        self.assertEqual(output.read_text(), "0 0 7 2 1\n")

    def test_deterministic_result_and_manifest(self):
        source = self.source("0,0,8,1\n1,8,8,2\n")
        a, b = self.root / "a.trace", self.root / "elsewhere/b.trace"
        first = m.convert(source, a, "rocksdb")
        second = m.convert(source, b, "rocksdb")
        self.assertEqual(a.read_bytes(), b.read_bytes())
        self.assertEqual(first, second)
        self.assertEqual(Path(str(a) + ".manifest.json").read_bytes(), Path(str(b) + ".manifest.json").read_bytes())

    def test_existing_output_untouched(self):
        source = self.source("0,0,8,1\n")
        output = self.source("keep me", "existing.trace")
        with self.assertRaisesRegex(m.ConversionError, "existing output"):
            m.convert(source, output, "rocksdb")
        self.assertEqual(output.read_text(), "keep me")

    def test_unsupported_or_corrupt_rows_fail_without_output(self):
        for n, data in enumerate(("2,0,8,0\n", "0,0,0,0\n", "0,-1,8,0\n", "0,0,8,NaN\n", "0,0,8,0.0000000001\n", "0,0,8\n", "")):
            with self.subTest(data=data):
                source = self.source(data, f"bad{n}.csv")
                output = self.root / f"bad{n}.trace"
                with self.assertRaises(m.ConversionError):
                    m.convert(source, output, "rocksdb")
                self.assertFalse(output.exists())
                self.assertFalse(Path(str(output) + ".manifest.json").exists())

    def test_unaligned_bytes_rejected(self):
        with self.assertRaisesRegex(m.ConversionError, "multiple of 512"):
            self.convert("10,usr,0,Read,1,4096,0\n", "msrc")

    def test_bad_input_does_not_publish_partial_success(self):
        with self.assertRaises(m.ConversionError):
            self.convert("0,0,8,0\n2,8,8,1\n")
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ["input.csv"])

    def test_gzip_corruption_fails(self):
        source = self.source(gzip.compress(b"0,0,8,0\n", mtime=0)[:-5], "broken.csv.gz")
        with self.assertRaises(EOFError):
            m.convert(source, self.root / "bad.trace", "rocksdb")
        self.assertFalse((self.root / "bad.trace").exists())

    def test_verify_detects_tampering(self):
        self.convert("0,0,8,0\n")
        output = self.root / "output.trace"
        output.write_bytes(b"0 0 0 8 0\n")
        with self.assertRaisesRegex(m.ConversionError, "manifest"):
            m.verify(output)

    def test_batch_relative_paths(self):
        self.source("0,0,8,0\n", "a.csv")
        self.source("10,x,0,Write,0,512,0\n", "b.csv")
        config = {"schema_version": 1, "jobs": [
            {"format": "rocksdb", "input": "a.csv", "output": "out/a.trace"},
            {"format": "msrc2", "input": "b.csv", "output": "out/b.trace"}]}
        path = self.source(json.dumps(config), "batch.json")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(m.run_batch(path), 0)
        self.assertEqual((self.root / "out/b.trace").read_text(), "0 0 0 1 0\n")

    def test_batch_rejects_collisions_before_any_job(self):
        self.source("0,0,8,0\n", "a.csv")
        config = {"schema_version": 1, "jobs": [
            {"format": "rocksdb", "input": "a.csv", "output": "out.trace"},
            {"format": "rocksdb", "input": "a.csv", "output": "out.trace"}]}
        with self.assertRaisesRegex(m.ConversionError, "share output"):
            m.run_batch(self.source(json.dumps(config), "batch.json"))
        self.assertFalse((self.root / "out.trace").exists())

    def test_derived_lcs_is_not_mistaken_for_original_io(self):
        source = self.source(b"placeholder", "cloud.lcs.zst")
        with self.assertRaisesRegex(m.ConversionError, "remapped addresses"):
            m.convert(source, self.root / "bad.trace", "cloudphysics-vscsi")


if __name__ == "__main__":
    unittest.main()
