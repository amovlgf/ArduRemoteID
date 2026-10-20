"""Compile the real hardware-independent CN adapter and codec on the host."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest


class CNDataAdapterTest(unittest.TestCase):
    def test_adapter_and_codec(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="cn-data-test-") as directory:
            codec_object = Path(directory) / "opendroneidcn.o"
            executable = Path(directory) / "test_cn_data"
            flags = ["-Wall", "-Wextra", "-Werror", "-pedantic"]
            if os.environ.get("CN_TEST_SANITIZE") == "1":
                flags += ["-fsanitize=address,undefined,float-cast-overflow", "-fno-omit-frame-pointer"]
            codec_include = root / "libraries/opendroneidcn/src"
            subprocess.run([
                "cc", "-std=c99", *flags, "-I", str(codec_include), "-c",
                str(codec_include / "opendroneidcn.c"), "-o", str(codec_object),
            ], check=True)
            includes = [root / "RemoteIDModule", codec_include]
            include_flags = [flag for path in includes for flag in ["-I", str(path)]]
            # Unchanged generated/libcanard C headers use flexible arrays.
            for dependency in [root / "libraries/DroneCAN_generated/include", root / "modules/libcanard"]:
                include_flags += ["-isystem", str(dependency)]
            subprocess.run([
                "g++", "-std=c++11", *flags, *include_flags,
                str(root / "tests/test_cn_data.cpp"), str(root / "RemoteIDModule/CNData.cpp"),
                str(codec_object), "-lm", "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
