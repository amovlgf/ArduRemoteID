#!/usr/bin/env python3
"""Verify a legacy ArduRemoteID signed OTA file using a public key only.

This checks the existing uploader's package/signature/space contract. It does
not prove bootability, change a board, or enable bootloader rollback.
"""

import argparse
import base64
import binascii
import hashlib
import json
from pathlib import Path
import struct
import sys


DESCRIPTOR_MAGIC = b"\x43\x2a\xf1\x37\x46\xe2\x75\x19"
DESCRIPTOR_SIZE = 80
LEGACY_FLUSH_PADDING = 4097
DEFAULT_PARTITION_SIZE = 0x1F0000


class OTAValidationError(ValueError):
    pass


def read_public_key(path):
    text = Path(path).read_text(encoding="ascii").strip()
    prefix = "PUBLIC_KEYV1:"
    if not text.startswith(prefix):
        raise OTAValidationError("expected a PUBLIC_KEYV1 public key file")
    try:
        key = base64.b64decode(text[len(prefix):], validate=True)
    except (ValueError, binascii.Error) as exc:
        raise OTAValidationError("invalid public key encoding") from exc
    if len(key) != 32:
        raise OTAValidationError("public key must contain 32 bytes")
    return key


def check_image(data, public_key, board_id=13, partition_size=DEFAULT_PARTITION_SIZE):
    if partition_size <= LEGACY_FLUSH_PADDING:
        raise OTAValidationError("invalid OTA partition size")
    if len(public_key) != 32:
        raise OTAValidationError("public key must contain 32 bytes")
    if len(data) + LEGACY_FLUSH_PADDING > partition_size:
        raise OTAValidationError("OTA file plus 4097-byte legacy flush padding exceeds partition")
    if len(data) < 24 + DESCRIPTOR_SIZE or data[0] != 0xE9:
        raise OTAValidationError("missing or truncated ESP application image")
    descriptor_offset = len(data) - DESCRIPTOR_SIZE
    if data[descriptor_offset:descriptor_offset + 8] != DESCRIPTOR_MAGIC:
        raise OTAValidationError("missing complete 80-byte descriptor at end of file")
    # The old firmware uses the first magic match in flash, not the last one.
    if data.find(DESCRIPTOR_MAGIC) != descriptor_offset:
        raise OTAValidationError("earlier descriptor magic would confuse the legacy verifier")
    actual_board, image_size, signature = struct.unpack_from("<II64s", data, descriptor_offset + 8)
    if actual_board != board_id:
        raise OTAValidationError(f"board ID {actual_board} does not match required {board_id}")
    if image_size != descriptor_offset:
        raise OTAValidationError("descriptor image length does not match file")
    try:
        import monocypher
    except ImportError as exc:
        raise OTAValidationError("install pymonocypher==3.1.3.2 to verify signatures") from exc
    if not monocypher.signature_check(signature, public_key, data[:image_size]):
        raise OTAValidationError("signature does not match the supplied public key")
    return {
        "board_id": actual_board,
        "image_bytes": image_size,
        "ota_bytes": len(data),
        "legacy_padding_bytes": LEGACY_FLUSH_PADDING,
        "partition_bytes": partition_size,
        "remaining_bytes": partition_size - len(data) - LEGACY_FLUSH_PADDING,
        "sha256": hashlib.sha256(data).hexdigest(),
        "public_key_sha256": hashlib.sha256(public_key).hexdigest(),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("--public-key", required=True, type=Path)
    parser.add_argument("--board-id", type=lambda value: int(value, 0), default=13)
    parser.add_argument("--partition-size", type=lambda value: int(value, 0), default=DEFAULT_PARTITION_SIZE)
    parser.add_argument("--json", action="store_true", help="print verification details as JSON")
    args = parser.parse_args(argv)
    try:
        report = check_image(args.firmware.read_bytes(), read_public_key(args.public_key),
                             args.board_id, args.partition_size)
    except (OSError, UnicodeError, OTAValidationError) as exc:
        print(f"OTA check failed: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"OTA check passed: board {report['board_id']}, {report['ota_bytes']} bytes, "
              f"{report['remaining_bytes']} bytes spare including legacy flush padding")
        print(f"SHA256: {report['sha256']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
