"""Exercise the standard firmware's factory identity and platform state path."""

import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PlatformActivationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="rid-platform-tests-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.output = Path(cls.directory.name)
        cls.program = cls.output / "test"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("CN_TEST_SANITIZE") == "1" else []
        subprocess.run(["g++", *flags, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(ROOT),
                        str(ROOT / "tests/test_platform_activation.cpp"),
                        str(ROOT / "RemoteIDModule/platform_activation.cpp"),
                        str(ROOT / "RemoteIDModule/activation.cpp"),
                        str(ROOT / "RemoteIDModule/monocypher.cpp"), "-o", str(cls.program)], check=True)

    def run_case(self, name):
        subprocess.run([str(self.program), "--case", name], check=True)

    def test_first_provision_survives_restart_and_cannot_be_overwritten(self):
        self.run_case("first_provision")

    def test_first_provision_rejects_invalid_fields(self):
        self.run_case("provision_invalid")

    def test_first_provision_requires_confirmed_empty_storage(self):
        self.run_case("provision_history")

    def test_first_provision_reconciles_ambiguous_storage_failures(self):
        self.run_case("provision_failures")

    def test_reject_invalid_factory_records(self):
        self.run_case("factory_validation")

    def test_factory_identity_never_activates(self):
        self.run_case("factory_initialization")

    def test_retired_authorizations_are_untouched_and_ignored(self):
        self.run_case("retired_authorizations")

    def test_platform_activation_deactivation_and_restart(self):
        self.run_case("platform_sync")

    def test_existing_imported_identity_and_state_are_preserved(self):
        self.run_case("persisted_imported")

    def test_invalid_factory_identity_blocks_activation(self):
        self.run_case("invalid_factory")

    def test_storage_failure_and_dual_slot_recovery(self):
        self.run_case("storage_recovery")

    def test_state_corruption_and_conflicts_fail_closed(self):
        self.run_case("state_conflicts")

    def test_factory_tool_matches_firmware(self):
        spec = importlib.util.spec_from_file_location("factory_identity", ROOT / "scripts/factory_identity.py")
        factory = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(factory)
        for revision, registration, uin in ((1, "", ""), (7, "REG-12345678", "12345678"),
                                            (0xFFFFFFFF, "A" * 56 + "12345678", "12345678")):
            blob = self.output / "factory.bin"
            blob.write_bytes(factory.build_identity("AABBCCDDEEFF", "AMOV0123456789012345", 2,
                                                     revision, registration, uin))
            actual = subprocess.check_output([str(self.program), str(blob)], text=True).strip()
            identity = (b"RID-ID-V1\x00" + revision.to_bytes(4, "little") + bytes.fromhex("AABBCCDDEEFF")
                        + bytes([2]) + b"AMOV0123456789012345" + bytes([len(registration)])
                        + registration.encode("ascii").ljust(64, b"\x00") + bytes([len(uin)])
                        + uin.encode("ascii").ljust(8, b"\x00"))
            self.assertEqual(actual, hashlib.blake2b(identity, digest_size=32).hexdigest().upper())


if __name__ == "__main__":
    unittest.main()
