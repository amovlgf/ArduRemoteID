"""Compile the firmware identity helpers and verify their stable wire format."""

import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ActivationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="rid-activation-tests-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.executable = Path(cls.directory.name) / "activation_test"
        subprocess.run([
            "g++", "-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(ROOT), str(ROOT / "tests/test_activation.cpp"),
            str(ROOT / "RemoteIDModule/activation.cpp"), str(ROOT / "RemoteIDModule/monocypher.cpp"),
            "-o", str(cls.executable),
        ], check=True)

    def test_identity_status_and_result_compatibility(self):
        subprocess.run([str(self.executable)], check=True)

    def test_identity_hash_matches_wire_format(self):
        for revision, registration, uin in ((1, "REG-12345678", "12345678"),
                                            (123456, "", ""),
                                            (0xFFFFFFFF, "A" * 56 + "12345678", "12345678")):
            actual = subprocess.check_output(
                [str(self.executable), str(revision), registration, uin], text=True).strip()
            identity = (b"RID-ID-V1\x00" + revision.to_bytes(4, "little") + bytes.fromhex("AABBCCDDEEFF")
                        + bytes([2]) + b"AMOV0123456789012345" + bytes([len(registration)])
                        + registration.encode("ascii").ljust(64, b"\x00") + bytes([len(uin)])
                        + uin.encode("ascii").ljust(8, b"\x00"))
            self.assertEqual(len(identity), 115)
            self.assertEqual(actual, hashlib.blake2b(identity, digest_size=32).hexdigest().upper())


if __name__ == "__main__":
    unittest.main()
