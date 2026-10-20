import base64
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

import monocypher


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_ota", ROOT / "scripts/check_ota.py")
check_ota = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check_ota)


class CheckOTATest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Public, deterministic test-only seed. Never used for firmware releases.
        cls.test_seed = bytes(range(32))
        cls.public_key = monocypher.compute_signing_public_key(cls.test_seed)
        cls.image = b"\xe9" + bytes(range(1, 256))
        cls.signature = monocypher.signature_sign(cls.test_seed, cls.image)
        cls.signed = cls.image + check_ota.DESCRIPTOR_MAGIC + struct.pack(
            "<II64s", 13, len(cls.image), cls.signature)

    def test_valid_signature_and_space(self):
        # Match the old CheckFirmware memmem() rule, not just a tail lookup.
        self.assertEqual(self.signed.find(check_ota.DESCRIPTOR_MAGIC), len(self.image))
        report = check_ota.check_image(self.signed, self.public_key)
        self.assertEqual(report["board_id"], 13)
        self.assertEqual(report["image_bytes"], len(self.image))
        self.assertEqual(report["ota_bytes"], len(self.signed))
        exact_size = len(self.signed) + check_ota.LEGACY_FLUSH_PADDING
        self.assertEqual(check_ota.check_image(self.signed, self.public_key,
                                             partition_size=exact_size)["remaining_bytes"], 0)
        with self.assertRaisesRegex(check_ota.OTAValidationError, "exceeds"):
            check_ota.check_image(self.signed, self.public_key, partition_size=exact_size - 1)

    def test_tampered_payload_signature_and_wrong_key(self):
        for offset in (20, len(self.signed) - 1):
            data = bytearray(self.signed)
            data[offset] ^= 1
            with self.assertRaisesRegex(check_ota.OTAValidationError, "signature"):
                check_ota.check_image(data, self.public_key)
        other_key = monocypher.compute_signing_public_key(bytes(reversed(range(32))))
        with self.assertRaisesRegex(check_ota.OTAValidationError, "signature"):
            check_ota.check_image(self.signed, other_key)

    def test_wrong_board(self):
        with self.assertRaisesRegex(check_ota.OTAValidationError, "board ID"):
            check_ota.check_image(self.signed, self.public_key, board_id=3)

    def test_truncation_and_bad_lengths(self):
        for data in (b"", self.signed[:15], self.signed[:-1], self.image, self.signed + b"\xff"):
            with self.subTest(length=len(data)):
                with self.assertRaises(check_ota.OTAValidationError):
                    check_ota.check_image(data, self.public_key)
        data = bytearray(self.signed)
        struct.pack_into("<I", data, len(self.image) + 12, len(self.image) + 1)
        with self.assertRaisesRegex(check_ota.OTAValidationError, "length"):
            check_ota.check_image(data, self.public_key)

    def test_earlier_magic_and_bad_esp_header(self):
        data = bytearray(self.signed)
        data[32:40] = check_ota.DESCRIPTOR_MAGIC
        with self.assertRaisesRegex(check_ota.OTAValidationError, "earlier descriptor"):
            check_ota.check_image(data, self.public_key)
        data[0] = 0
        with self.assertRaisesRegex(check_ota.OTAValidationError, "ESP"):
            check_ota.check_image(data, self.public_key)

    def test_public_key_file_and_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            key_file = directory / "public.dat"
            key_file.write_text("PUBLIC_KEYV1:" + base64.b64encode(self.public_key).decode() + "\n")
            self.assertEqual(check_ota.read_public_key(key_file), self.public_key)
            image_file = directory / "firmware.bin"
            image_file.write_bytes(self.signed)
            self.assertEqual(check_ota.main([str(image_file), "--public-key", str(key_file)]), 0)
            key_file.write_text("INVALID:AAAA")
            with self.assertRaises(check_ota.OTAValidationError):
                check_ota.read_public_key(key_file)
            key_file.write_text("PUBLIC_KEYV1:AAAA")
            with self.assertRaises(check_ota.OTAValidationError):
                check_ota.read_public_key(key_file)


if __name__ == "__main__":
    unittest.main()
