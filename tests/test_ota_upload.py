from pathlib import Path
import subprocess
import tempfile
import unittest


class OTAUploadTransactionTest(unittest.TestCase):
    def test_optimized_helper_does_not_embed_descriptor_magic(self):
        root = Path(__file__).resolve().parents[1]
        source = '''#include "RemoteIDModule/ota_upload.h"
bool probe(OTAUpload::Backend &backend, const uint8_t *data, size_t size) {
    OTAUpload upload;
    return upload.start(backend, true) && upload.write(backend, data, size) &&
           upload.finish(backend, size) && upload.complete(backend);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            cpp = directory / "probe.cpp"
            obj = directory / "probe.o"
            cpp.write_text(source)
            subprocess.run([
                "g++", "-std=c++11", "-O2", "-I", str(root), "-c", str(cpp),
                "-o", str(obj),
            ], check=True)
            compiled = obj.read_bytes()
            forward_magic = bytes.fromhex("432af13746e27519")
            self.assertNotIn(forward_magic, compiled)
            self.assertIn(forward_magic[::-1], compiled)

    def test_upload_failures_and_retries(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "test_ota_upload"
            subprocess.run([
                "g++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(root), str(root / "tests/test_ota_upload.cpp"),
                "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
