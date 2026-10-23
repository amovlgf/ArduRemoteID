"""Factory production artifacts: invalid input, no replacement and batch rollback."""
import csv
import importlib.util
import io
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "scripts/factory_identity.py"
SPEC = importlib.util.spec_from_file_location("factory_identity", TOOL)
factory = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(factory)
UID = "AABBCCDDEEFF"
SN = "AMOV0123456789012345"


class FactoryIdentityTests(unittest.TestCase):
    def cli(self, *args, success=True):
        result = subprocess.run([sys.executable, str(TOOL), *map(str, args)],
                                capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def test_identity_rejects_ambiguous_or_invalid_fields(self):
        for override in ({"uid": "AA BBCCDDEEF"}, {"uid": "000000000000"},
                         {"sn": "AMOV012345678901234 "}, {"sn": "中" * 20},
                         {"revision": 0}, {"revision": 2**32}, {"aircraft_class": 5},
                         {"registration": "REG-12345678", "uin": "87654321"},
                         {"registration": "ABC1234", "uin": "ABC12345"},
                         {"uin": "1234567"}, {"registration": "X" * 65, "uin": "X" * 8}):
            fields = dict(uid=UID, sn=SN, aircraft_class=2)
            fields.update(override)
            with self.subTest(override=override), self.assertRaises(ValueError):
                factory.build_identity(**fields)

    def test_verify_rejects_damage_and_noncanonical_records(self):
        record = factory.build_identity(UID, SN, 2)
        self.assertEqual(factory.decode_identity(record, UID)["sn"], SN)
        with self.assertRaises(ValueError):
            factory.decode_identity(record, "112233445566")
        for offset, value in ((8, 2), (9, 5), (10, 65), (11, 7),
                              (42, ord("X")), (106, ord("X")), (114, 1), (115, 1)):
            damaged = bytearray(record)
            damaged[offset] = value
            struct.pack_into("<I", damaged, 116, zlib.crc32(damaged[:116]))
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                factory.decode_identity(damaged)
        with self.assertRaisesRegex(ValueError, "CRC"):
            factory.decode_identity(record[:-1] + bytes([record[-1] ^ 1]))
        with self.assertRaises(ValueError):
            factory.decode_identity(record + b"\0")

    def test_single_device_cli_and_csv_follow_actual_output_path(self):
        with tempfile.TemporaryDirectory(prefix="rid factory,") as directory:
            output = Path(directory)
            blob, table = output / "device.bin", output / "device.csv"
            self.cli("--uid", UID, "--sn", SN, "--class", "2", "--output", blob, "--nvs-csv", table)
            self.cli("--verify", blob, "--uid", UID)
            self.cli("--verify", blob, "--uid", "112233445566", success=False)
            rows = list(csv.DictReader(io.StringIO(table.read_text())))
            self.assertEqual(rows[0]["key"], "rid_platform")
            self.assertEqual(rows[1]["value"], str(blob.resolve()))
            self.assertEqual(blob.stat().st_size, 120)

    def test_existing_outputs_and_aliases_never_get_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            blob, table = base / "identity.bin", base / "identity.csv"
            table.write_bytes(b"existing")
            with self.assertRaises(ValueError):
                factory.publish_artifacts([(blob, b"new"), (table, b"replacement")])
            self.assertEqual(table.read_bytes(), b"existing")
            self.assertFalse(blob.exists())
            with self.assertRaisesRegex(ValueError, "distinct"):
                factory.publish_artifacts([(blob, b"a"), (base / "./identity.bin", b"b")])
            table.unlink()
            table.symlink_to(base / "absent")
            with self.assertRaises(ValueError):
                factory.publish_artifacts([(table, b"replacement")])
            self.assertTrue(table.is_symlink())

    def test_publication_failure_rolls_back_only_own_files(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            blob, table = base / "identity.bin", base / "identity.csv"
            original = os.link

            def collision(source, destination):
                if destination == table:
                    table.write_bytes(b"another process")
                return original(source, destination)

            with mock.patch.object(factory.os, "link", side_effect=collision):
                with self.assertRaises(FileExistsError):
                    factory.publish_artifacts([(blob, b"one"), (table, b"two")])
            self.assertFalse(blob.exists())
            self.assertEqual(table.read_bytes(), b"another process")
            self.assertEqual(set(base.iterdir()), {table})

    def test_batch_validates_all_devices_and_refuses_duplicate_uid_or_sn(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            table, output = base / "batch.csv", base / "output"
            output.mkdir()
            header = "uid,sn,class,revision,registration,uin\n"
            first = f"{UID},{SN},2,1,REG-12345678,12345678\n"
            for second in (f"{UID.lower()},AMOV0123456789012346,2,1,,\n",
                           f"112233445566,{SN},2,1,,\n",
                           "112233445566,short,2,1,,\n"):
                table.write_text(header + first + second)
                self.cli("--batch", table, "--output-dir", output, success=False)
                self.assertEqual(list(output.iterdir()), [])
            table.write_text(header + first + "112233445566,AMOV0123456789012346,2,1,,\n")
            self.cli("--batch", table, "--output-dir", output)
            self.assertEqual(len(list(output.iterdir())), 4)
            self.assertEqual(factory.decode_identity((output / f"{UID}.factory.bin").read_bytes())["uin"],
                             "12345678")
            snapshots = {path: path.read_bytes() for path in output.iterdir()}
            self.cli("--batch", table, "--output-dir", output, success=False)
            self.assertEqual(snapshots, {path: path.read_bytes() for path in output.iterdir()})

    def test_missing_output_directory_and_generator_failure_leave_no_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            blob, table, nvs = base / "identity.bin", base / "identity.csv", base / "nvs.bin"
            self.cli("--uid", UID, "--sn", SN, "--class", 2, "--output", blob,
                     "--nvs-csv", base / "absent" / "nvs.csv", success=False)
            self.assertFalse(blob.exists())
            generator = base / "generator.py"
            generator.write_text("raise SystemExit('simulated generator failure')\n")
            result = self.cli("--uid", UID, "--sn", SN, "--class", 2, "--output", blob,
                              "--nvs-csv", table, "--nvs-bin", nvs, "--nvs-generator", generator, success=False)
            self.assertIn("simulated generator failure", result.stderr)
            self.assertEqual(list(base.iterdir()), [generator])

    @unittest.skipUnless(os.environ.get("RID_NVS_GENERATOR"), "Set RID_NVS_GENERATOR for ESP-IDF integration")
    def test_official_nvs_generator_builds_partition_with_exact_factory_blob(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            blob, table, nvs = base / "identity.bin", base / "identity.csv", base / "nvs.bin"
            self.cli("--uid", UID, "--sn", SN, "--class", 2, "--registration", "REG-12345678",
                     "--uin", "12345678", "--output", blob, "--nvs-csv", table, "--nvs-bin", nvs,
                     "--nvs-generator", os.environ["RID_NVS_GENERATOR"])
            data = nvs.read_bytes()
            self.assertEqual(len(data), 0x5000)
            self.assertIn(b"rid_platform\0", data)
            self.assertIn(b"factory\0", data)
            self.assertIn(blob.read_bytes(), data)
            self.assertEqual(data[-4096:], b"\xff" * 4096)


if __name__ == "__main__":
    unittest.main()
