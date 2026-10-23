"""Run real parameter and management service code against deterministic NVS/RTOS fakes."""
from pathlib import Path
import os
import json
import subprocess
import tempfile
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


def build(executable):
    flags = ["-std=c++11", "-DBOARD_AMOV_RID", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", "-Wno-missing-field-initializers", "-Wno-switch"]
    if os.environ.get("CN_TEST_SANITIZE") == "1":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    includes = [ROOT, ROOT / "tests/management_fakes", ROOT / "RemoteIDModule",
                ROOT / "protocol/amov_device_bridge", ROOT / "libraries/opendroneidcn/src"]
    flags += [flag for path in includes for flag in ("-I", str(path))]
    for path in (ROOT / "libraries/DroneCAN_generated/include", ROOT / "modules/libcanard"):
        flags += ["-isystem", str(path)]
    sources = ["tests/test_management.cpp", "RemoteIDModule/management_service.cpp",
               "RemoteIDModule/management_parameters.cpp", "RemoteIDModule/parameters.cpp",
               "RemoteIDModule/platform_activation.cpp", "RemoteIDModule/activation.cpp",
               "RemoteIDModule/monocypher.cpp", "RemoteIDModule/CNData.cpp"]
    subprocess.run(["g++", *flags, *(str(ROOT / source) for source in sources), "-o", str(executable)], check=True)


class ManagementTests(unittest.TestCase):
    def test_real_service_storage_failures(self):
        with tempfile.TemporaryDirectory(prefix="rid-management-") as directory:
            executable = Path(directory) / "management"
            build(executable)
            subprocess.run([str(executable)], cwd=ROOT, check=True)
            vector = json.loads((ROOT / "protocol/amov_device_bridge/status_digest_vector.json").read_text())
            fixture = Path(directory) / "digest-vector.bin"
            fixture.write_bytes(bytes.fromhex(vector["request_hex"] + vector["blake2b_256"]))
            subprocess.run([str(executable), "--digest-vector", str(fixture)], cwd=ROOT, check=True)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--metadata":
        with tempfile.TemporaryDirectory(prefix="rid-management-metadata-") as directory:
            executable = Path(directory) / "management"
            build(executable)
            result = subprocess.run([str(executable), "--dump-json"], cwd=ROOT, check=True, capture_output=True, text=True)
            Path(sys.argv[2]).write_text(result.stdout, encoding="utf-8")
    else:
        unittest.main()
