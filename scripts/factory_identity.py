#!/usr/bin/env python3
"""Prepare and verify per-device factory identities. Never writes hardware."""
import argparse
import csv
import io
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import zlib

NVS_SIZE = 0x5000


def build_identity(uid, sn, aircraft_class, revision=1, registration="", uin=""):
    if not re.fullmatch(r"[0-9a-fA-F]{12}", uid):
        raise ValueError("UID must be twelve hexadecimal characters")
    raw_uid = bytes.fromhex(uid)
    if not any(raw_uid):
        raise ValueError("UID must be a nonzero six-byte factory UID")
    fields = [value.encode("ascii") for value in (sn, registration, uin)]
    if any(any(byte < 33 or byte > 126 for byte in value) for value in fields):
        raise ValueError("Identity strings must contain printable ASCII without spaces")
    sn_bytes, reg_bytes, uin_bytes = fields
    if len(sn_bytes) != 20 or not 0 <= aircraft_class <= 4 or not 1 <= revision <= 0xFFFFFFFF:
        raise ValueError("SN must be exactly 20 bytes; class is 0..4 and revision is nonzero uint32")
    if len(reg_bytes) > 64 or len(uin_bytes) not in (0, 8):
        raise ValueError("Registration is at most 64 bytes; UIN is empty or 8 bytes")
    if reg_bytes and (len(reg_bytes) < 8 or uin_bytes != reg_bytes[-8:]):
        raise ValueError("Known registration requires its exact last eight bytes as UIN")
    record = bytearray(120)
    record[:8] = b"AMOVFID1"
    record[8:12] = bytes((1, aircraft_class, len(reg_bytes), len(uin_bytes)))
    struct.pack_into("<I", record, 12, revision)
    record[16:22] = raw_uid
    record[22:42] = sn_bytes
    record[42:42 + len(reg_bytes)] = reg_bytes
    record[106:106 + len(uin_bytes)] = uin_bytes
    struct.pack_into("<I", record, 116, zlib.crc32(record[:116]))
    return bytes(record)


def decode_identity(record, expected_uid=None):
    """Validate a canonical FACTORY-source record, including padding and CRC."""
    if len(record) != 120 or record[:8] != b"AMOVFID1" or record[8] != 1:
        raise ValueError("Not a 120-byte AMOVFID1 version 1 factory identity")
    if record[114:116] != b"\0\0":
        raise ValueError("Factory identity source and reserved byte must be zero")
    if struct.unpack_from("<I", record, 116)[0] != zlib.crc32(record[:116]):
        raise ValueError("Factory identity CRC mismatch")
    fields = {
        "uid": record[16:22].hex().upper(),
        "sn": record[22:42].decode("ascii"),
        "aircraft_class": record[9],
        "revision": struct.unpack_from("<I", record, 12)[0],
        "registration": record[42:42 + record[10]].decode("ascii"),
        "uin": record[106:106 + record[11]].decode("ascii"),
    }
    if build_identity(**fields) != record:
        raise ValueError("Factory identity has invalid lengths, padding or fields")
    if expected_uid is not None and fields["uid"] != expected_uid.upper():
        raise ValueError("Factory identity belongs to a different UID")
    return fields


def nvs_csv(blob_path):
    output = io.StringIO(newline="")
    writer = csv.writer(output)
    writer.writerow(("key", "type", "encoding", "value"))
    writer.writerow(("rid_platform", "namespace", "", ""))
    writer.writerow(("factory", "file", "binary", str(blob_path.resolve())))
    return output.getvalue().encode("utf-8")


def prepare_artifacts(record, output, csv_output=None, nvs_output=None, generator=None):
    """Build every byte before publishing any destination file."""
    decode_identity(record)
    artifacts = [(output, record)]
    if csv_output:
        artifacts.append((csv_output, nvs_csv(output)))
    if nvs_output:
        if not generator or not generator.is_file():
            raise ValueError("--nvs-bin requires --nvs-generator pointing to ESP-IDF nvs_partition_gen.py")
        with tempfile.TemporaryDirectory(prefix="rid-factory-nvs-") as directory:
            stage = pathlib.Path(directory)
            blob, table, image = stage / "factory.bin", stage / "nvs.csv", stage / "nvs.bin"
            blob.write_bytes(record)
            table.write_bytes(nvs_csv(blob))
            result = subprocess.run([sys.executable, str(generator.resolve()), "generate", str(table),
                                     str(image), hex(NVS_SIZE), "--version", "2"],
                                    capture_output=True, text=True)
            if result.returncode:
                raise ValueError("NVS generator failed: " + (result.stderr or result.stdout).strip())
            data = image.read_bytes()
            if len(data) != NVS_SIZE:
                raise ValueError("NVS generator did not produce the required 0x5000-byte partition")
            artifacts.append((nvs_output, data))
    return artifacts


def publish_artifacts(artifacts):
    """Publish without replacement; roll back files from this call on failure.

    A host crash cannot make several filesystem paths an atomic transaction.
    Use the successful process exit as the release gate for a factory batch.
    """
    destinations = [path.absolute() for path, _ in artifacts]
    if len({path.resolve() for path in destinations}) != len(destinations):
        raise ValueError("Artifact output paths must be distinct")
    for path in destinations:
        if path.exists() or path.is_symlink():
            raise ValueError("Refusing to overwrite existing factory artifact: " + str(path))
        if not path.parent.is_dir():
            raise ValueError("Output directory does not exist: " + str(path.parent))
    staged, published = [], []
    try:
        for path, (_, data) in zip(destinations, artifacts):
            # Stage alongside each destination so the no-replace hard link
            # also works when output directories are on different filesystems.
            with tempfile.NamedTemporaryFile(prefix=".rid-factory-", dir=path.parent, delete=False) as stream:
                temporary = pathlib.Path(stream.name)
                staged.append(temporary)
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.link(temporary, path)
            published.append(path)
    except BaseException:
        for path in reversed(published):
            path.unlink()
        raise
    finally:
        for path in staged:
            path.unlink()


def batch_records(path):
    """Validate an entire production list before creating any output."""
    records, uids, serials = [], set(), set()
    with path.open(newline="", encoding="utf-8") as stream:
        rows = csv.DictReader(stream)
        required = {"uid", "sn", "class"}
        optional = {"revision", "registration", "uin"}
        if (not rows.fieldnames or len(set(rows.fieldnames)) != len(rows.fieldnames)
                or not required.issubset(rows.fieldnames)
                or set(rows.fieldnames) - required - optional):
            raise ValueError("Batch CSV columns: uid,sn,class[,revision,registration,uin]")
        for row in rows:
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"Malformed batch CSV row {rows.line_num}")
            record = build_identity(row["uid"], row["sn"], int(row["class"]),
                                    int(row.get("revision") or "1"),
                                    row.get("registration", ""), row.get("uin", ""))
            identity = decode_identity(record)
            uid, sn = identity["uid"], identity["sn"]
            if uid in uids or sn in serials:
                raise ValueError(f"Duplicate UID or aircraft SN in batch row {rows.line_num}")
            uids.add(uid)
            serials.add(sn)
            records.append((uid, record))
    if not records:
        raise ValueError("Batch CSV contains no devices")
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uid", help="Actual device RID_UID; also the expected UID with --verify")
    parser.add_argument("--sn", help="Assigned 20-byte aircraft SN")
    parser.add_argument("--class", dest="aircraft_class", type=int)
    parser.add_argument("--revision", type=int, default=1)
    parser.add_argument("--registration", default="")
    parser.add_argument("--uin", default="")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--nvs-csv", type=pathlib.Path, help="NVS generator input for blank factory devices only")
    parser.add_argument("--nvs-bin", type=pathlib.Path, help="Whole 0x5000-byte NVS for blank factory devices only")
    parser.add_argument("--nvs-generator", type=pathlib.Path, help="Path to ESP-IDF nvs_partition_gen.py")
    parser.add_argument("--batch", type=pathlib.Path, help="CSV list with unique uid,sn,class and optional identity fields")
    parser.add_argument("--output-dir", type=pathlib.Path, help="Existing batch output directory; generator also adds NVS binaries")
    parser.add_argument("--verify", type=pathlib.Path, help="Validate a factory blob without generating artifacts")
    args = parser.parse_args()
    try:
        if args.verify:
            if any((args.batch, args.output, args.output_dir, args.sn, args.aircraft_class is not None,
                    args.nvs_csv, args.nvs_bin, args.nvs_generator, args.registration, args.uin,
                    args.revision != 1)):
                raise ValueError("--verify accepts only an optional --uid")
            fields = decode_identity(args.verify.read_bytes(), args.uid)
            print(f"Verified factory identity: UID={fields['uid']} SN={fields['sn']} revision={fields['revision']}")
            return
        if args.batch:
            if not args.output_dir or any((args.uid, args.sn, args.aircraft_class is not None, args.output,
                                           args.nvs_csv, args.nvs_bin, args.registration, args.uin,
                                           args.revision != 1)):
                raise ValueError("--batch requires --output-dir and cannot be mixed with single-device options")
            artifacts = []
            records = batch_records(args.batch)
            for uid, record in records:
                base = args.output_dir / uid
                artifacts.extend(prepare_artifacts(record, base.with_suffix(".factory.bin"),
                                                    base.with_suffix(".nvs.csv"),
                                                    base.with_suffix(".nvs.bin") if args.nvs_generator else None,
                                                    args.nvs_generator))
        else:
            if args.output_dir or not args.uid or not args.sn or args.aircraft_class is None or not args.output:
                raise ValueError("Single-device generation requires --uid, --sn, --class and --output")
            if args.nvs_generator and not args.nvs_bin:
                raise ValueError("--nvs-generator requires --nvs-bin in single-device mode")
            record = build_identity(args.uid, args.sn, args.aircraft_class, args.revision,
                                    args.registration, args.uin)
            records = [(args.uid.upper(), record)]
            artifacts = prepare_artifacts(record, args.output, args.nvs_csv, args.nvs_bin, args.nvs_generator)
        publish_artifacts(artifacts)
    except (ValueError, UnicodeError, OSError) as error:
        parser.error(str(error))
    print(f"Prepared {len(records)} factory identities in {len(artifacts)} artifacts.")
    print("No device was written. Whole NVS images are only for blank factory devices; they replace all existing settings.")


if __name__ == "__main__":
    main()
