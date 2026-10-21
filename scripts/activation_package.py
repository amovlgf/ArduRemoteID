#!/usr/bin/env python3
"""Public TEST activation package generator, never a production signing service.

Requires pymonocypher==3.1.3.2 (EdDSA with BLAKE2b, NOT SHA-512 Ed25519).
The private seed below is intentionally public. Only TEST firmware trusts it.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct

TEST_SEED = bytes(range(32))
TEST_KEY_ID = 65535
TEST_PUBLIC_KEY = bytes.fromhex("f65333fa6303b6a23defd7de2af8aa461cb047ccbf12d4edd29ef3b1eba6706b")


def _ascii(text, minimum, maximum):
    raw = text.encode("ascii")
    if not minimum <= len(raw) <= maximum or any(b < 33 or b > 126 for b in raw):
        raise ValueError("identity must have the specified length and printable ASCII without spaces")
    return raw


def build_package(uid, product, registration="", uin="", aircraft_class=0,
                  revision=1, board_id=13, token=None):
    import monocypher
    raw_uid = bytes.fromhex(uid)
    if len(uid) != 12 or len(raw_uid) != 6 or not any(raw_uid):
        raise ValueError("UID must be exactly twelve hex digits, nonzero")
    product_raw = _ascii(product, 20, 20)
    registration_raw = _ascii(registration, 0, 64)
    uin_raw = _ascii(uin, 0, 8)
    if len(uin_raw) not in (0, 8):
        raise ValueError("UIN must be empty or exactly eight characters")
    if registration_raw and (len(registration_raw) < 8 or
                             uin_raw != registration_raw[-8:]):
        raise ValueError("known full registration requires its explicit matching eight-character UIN")
    if not 0 <= aircraft_class <= 4 or not 1 <= revision <= 0xffffffff or not 0 <= board_id <= 65535:
        raise ValueError("class, revision or board out of range")
    token = os.urandom(16) if token is None else token
    if len(token) != 16 or not any(token):
        raise ValueError("token must be sixteen nonzero-as-a-whole bytes")
    signed = (b"AMOVACT1" + bytes((1, 1)) +
              struct.pack("<HHHI", TEST_KEY_ID, board_id, 1, revision) +
              raw_uid + bytes((aircraft_class, len(registration_raw), len(uin_raw))) +
              bytes(3) + token + product_raw +
              registration_raw.ljust(64, b"\0") + uin_raw.ljust(8, b"\0"))
    assert len(signed) == 140
    assert monocypher.compute_signing_public_key(TEST_SEED) == TEST_PUBLIC_KEY
    return signed + monocypher.signature_sign(TEST_SEED, signed)


def identity_hash(package):
    """Uppercase BLAKE2b-256 of canonical, stable 115-byte identity snapshot."""
    if len(package) != 204:
        raise ValueError("expected 204-byte package")
    canonical = (b"RID-ID-V1\0" + package[16:20] + package[20:27] +
                 package[48:68] + package[27:28] + package[68:132] +
                 package[28:29] + package[132:140])
    assert len(canonical) == 115
    return hashlib.blake2b(canonical, digest_size=32).hexdigest().upper()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    sign = commands.add_parser("sign-test", help="create a publicly forgeable TEST-only package")
    sign.add_argument("--uid", required=True)
    sign.add_argument("--product", required=True)
    sign.add_argument("--registration", default="")
    sign.add_argument("--uin", default="")
    sign.add_argument("--class", dest="aircraft_class", type=int, default=0)
    sign.add_argument("--revision", type=int, required=True)
    sign.add_argument("--board-id", type=int, default=13)
    sign.add_argument("--output", type=Path, required=True)
    commands.add_parser("test-public-key", help="print only the TEST public key")
    args = parser.parse_args()
    if args.command == "test-public-key":
        print(TEST_PUBLIC_KEY.hex().upper())
        return
    try:
        package = build_package(args.uid, args.product, args.registration, args.uin,
                                args.aircraft_class, args.revision, args.board_id)
    except (ValueError, UnicodeError) as error:
        parser.error(str(error))
    # Refuse to overwrite an existing package accidentally.
    with args.output.open("xb") as stream:
        stream.write(package)
    print("TEST ONLY - not trusted by production firmware")
    print("identity_hash=" + identity_hash(package))
    print("written=" + str(args.output))


if __name__ == "__main__":
    main()
