"""Run the real activation service for AMOV and an unaffected standard board."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ActivationServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="rid-activation-service-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.programs = {}
        for board in ("AMOV_RID", "ESP32C3_DEV"):
            executable = Path(cls.directory.name) / board
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("CN_TEST_SANITIZE") == "1" else []
            subprocess.run([
                "g++", *flags, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-DBOARD_" + board, "-I", str(ROOT), "-I", str(ROOT / "tests/activation_service_fakes"),
                *(str(ROOT / source) for source in (
                    "tests/test_activation_service.cpp", "RemoteIDModule/activation_service.cpp",
                    "RemoteIDModule/platform_activation.cpp", "RemoteIDModule/activation.cpp",
                    "RemoteIDModule/monocypher.cpp")), "-o", str(executable)], check=True)
            cls.programs[board] = executable

    def run_case(self, scenario, board="AMOV_RID"):
        subprocess.run([str(self.programs[board]), scenario], check=True)

    def test_non_amov_never_opens_activation_storage(self):
        self.run_case("missing", "ESP32C3_DEV")

    def test_missing_identity_cannot_broadcast_or_activate(self):
        self.run_case("missing")

    def test_factory_identity_and_platform_state_survive_restart(self):
        self.run_case("provisioned")

    def test_first_provision_checks_lock_and_publishes_without_mutating_main_parameters(self):
        self.run_case("first_provision")

    def test_provision_failures_reopen_nvs_and_reconcile_durable_identity(self):
        self.run_case("provision_errors")

    def test_storage_open_failure_blocks_broadcast(self):
        self.run_case("open_error")

    def test_storage_read_failure_blocks_broadcast(self):
        self.run_case("read_error")

    def test_mutex_creation_failure_blocks_broadcast_without_opening_nvs(self):
        self.run_case("mutex_error")


if __name__ == "__main__":
    unittest.main()
