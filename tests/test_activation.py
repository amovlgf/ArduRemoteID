"""Compile the real firmware activation code and cross-check the signing tool."""

import hashlib
import importlib.util
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
        cls.build = Path(cls.directory.name)
        cls.executable = cls.build / "activation_test"
        subprocess.run([
            "g++", "-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(ROOT), str(ROOT / "tests/test_activation.cpp"),
            str(ROOT / "RemoteIDModule/activation.cpp"), str(ROOT / "RemoteIDModule/monocypher.cpp"),
            "-o", str(cls.executable),
        ], check=True)

    def test_firmware_transactions(self):
        subprocess.run([str(self.executable)], check=True)

    def test_python_signer_cpp_verifier(self):
        spec = importlib.util.spec_from_file_location("activation_package", ROOT / "scripts/activation_package.py")
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        for revision, registration, uin in ((1, "REG-12345678", "12345678"),
                                            (123456, "", ""),
                                            (0xFFFFFFFF, "A" * 56 + "12345678", "12345678")):
            package = tool.build_package(uid="AABBCCDDEEFF", product="AMOV0123456789012345",
                                         registration=registration, uin=uin, aircraft_class=2,
                                         revision=revision, token=bytes(range(16)))
            self.assertEqual(len(package), 204)
            path = self.build / (str(revision) + ".act")
            path.write_bytes(package)
            actual = subprocess.check_output([str(self.executable), str(path)], text=True).strip()
            identity = (b"RID-ID-V1\x00" + revision.to_bytes(4, "little") + bytes.fromhex("AABBCCDDEEFF")
                        + bytes([2]) + b"AMOV0123456789012345" + bytes([len(registration)])
                        + registration.encode("ascii").ljust(64, b"\x00") + bytes([len(uin)])
                        + uin.encode("ascii").ljust(8, b"\x00"))
            self.assertEqual(len(identity), 115)
            self.assertEqual(actual, hashlib.blake2b(identity, digest_size=32).hexdigest().upper())


if __name__ == "__main__":
    unittest.main()
