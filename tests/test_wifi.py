"""Fault-inject the real AP driver without ESP32 hardware or wall-clock waits."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WiFiTests(unittest.TestCase):
    def test_ap_recovery_and_dhcp_readiness(self):
        flags = ["-std=c++11", "-DBOARD_AMOV_RID", "-Wall", "-Wextra", "-Werror"]
        if os.environ.get("CN_TEST_SANITIZE") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        includes = [ROOT, ROOT / "tests/wifi_fakes", ROOT / "RemoteIDModule",
                    ROOT / "modules/opendroneid-core-c/libopendroneid",
                    ROOT / "libraries/opendroneidcn/src"]
        flags += [flag for path in includes for flag in ("-I", str(path))]
        for path in (ROOT / "libraries/DroneCAN_generated/include", ROOT / "modules/libcanard"):
            flags += ["-isystem", str(path)]
        sources = ["tests/test_wifi.cpp", "RemoteIDModule/WiFi_TX.cpp", "RemoteIDModule/transmitter.cpp"]
        with tempfile.TemporaryDirectory(prefix="rid-wifi-") as directory:
            executable = Path(directory) / "wifi"
            subprocess.run(["g++", *flags, *(str(ROOT / source) for source in sources),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    unittest.main()
