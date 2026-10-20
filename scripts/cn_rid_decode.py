#!/usr/bin/env python3
"""Independent GB 46750-2025 Table 1-3 decoder for lab verification.

No third-party packages are required. PCAP/PCAPNG input requires an explicit
experimental/verified Wi-Fi vendor identifier. The Wi-Fi
envelope is a project test convention, not a claim about a CAAC allocation.
"""

import argparse
import json
import struct
import sys
from pathlib import Path


class DecodeError(ValueError):
    """Malformed or unsupported packet."""


# Table 2 order, Table 3 byte lengths; kept independent of firmware headers.
FIELDS = (
    ("uas_id", 20), ("uin", 8), ("operation_category", 1),
    ("aircraft_class", 1), ("operator_position_type", 1),
    ("operator_position", 8), ("operator_altitude_m", 2),
    ("aircraft_position", 8), ("track_angle_deg", 2),
    ("ground_speed_m_s", 2), ("relative_altitude_m", 2),
    ("vertical_speed_m_s", 1), ("geodetic_altitude_m", 2),
    ("barometric_altitude_m", 2), ("operation_status", 1),
    ("coordinate_system", 1), ("horizontal_accuracy", 1),
    ("vertical_accuracy", 1), ("speed_accuracy", 1),
    ("timestamp_ms", 6), ("timestamp_accuracy", 1),
)
OPTIONAL = {3, 11, 12, 14}
ENUM_MAX = {3: 3, 4: 4, 5: 1, 15: 5, 16: 1, 17: 12, 18: 6, 19: 4, 21: 8}


def parse_wifi_id(value):
    """Parse the explicitly selected four-byte vendor identifier."""
    if len(value) != 8:
        raise argparse.ArgumentTypeError("Wi-Fi ID must be exactly 8 hex characters")
    try:
        result = bytes.fromhex(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("Wi-Fi ID must contain only hex characters") from exc
    if len(result) != 4:
        raise argparse.ArgumentTypeError("Wi-Fi ID must be four bytes")
    return result


def _position(raw):
    result = {}
    for key, offset, limit in (("longitude_deg", 0, 180), ("latitude_deg", 4, 90)):
        word = raw[offset:offset + 4]
        value = None if word == b"\xff" * 4 else struct.unpack("<i", word)[0] / 1e7
        if value is not None and not -limit <= value <= limit:
            raise DecodeError("{} outside geographical range".format(key))
        result[key] = value
    return result


def decode_packet(packet):
    """Decode one complete raw RID payload; invalid measurements become null.

    Unknown extension fields are rejected because their lengths are not known.
    No bytes are located by searching for a header inside an arbitrary buffer.
    """
    if len(packet) < 6 or packet[0] != 0xFF:
        raise DecodeError("expected a complete payload starting with 0xFF")
    if packet[1] & 0xE0 != 0x20:
        raise DecodeError("unsupported major version")
    if not 1 <= packet[2] <= 200:
        raise DecodeError("data length must be 1..200")
    flags = []
    pos = 3
    while True:
        if pos >= len(packet):
            raise DecodeError("unterminated flag chain")
        flags.append(packet[pos])
        pos += 1
        if len(flags) > 3:
            raise DecodeError("unknown extended fields require an authoritative definition")
        if not flags[-1] & 1:
            break
    if len(packet) != pos + packet[2]:
        raise DecodeError("declared content length does not match payload length")

    warnings = []
    if packet[1] & 0x1F:
        warnings.append("nonzero sub-version interpreted using the V1.0 field table")
    values = {}
    present = []
    for field_id, (name, length) in enumerate(FIELDS, 1):
        flag_index, bit_index = divmod(field_id - 1, 7)
        enabled = flag_index < len(flags) and flags[flag_index] & (0x80 >> bit_index)
        if not enabled:
            if field_id not in OPTIONAL:
                raise DecodeError("missing mandatory field {:03d}".format(field_id))
            continue
        if pos + length > len(packet):
            raise DecodeError("truncated field {:03d}".format(field_id))
        raw = packet[pos:pos + length]
        pos += length
        present.append(field_id)
        number = int.from_bytes(raw, "little")
        if field_id in (1, 2):
            try:
                value = raw.strip(b"\x00").decode("ascii")
            except UnicodeDecodeError as exc:
                raise DecodeError("identity field must be ASCII") from exc
            if any(ord(char) < 32 or ord(char) > 126 for char in value):
                raise DecodeError("identity field has internal NULL or control characters")
            if field_id == 1 and not value:
                raise DecodeError("empty product identity")
            if field_id == 2 and value and len(value) != 8:
                raise DecodeError("registration mark must be empty or eight characters")
        elif field_id in (6, 8):
            value = _position(raw)
        elif field_id in (7, 11, 13, 14):
            value = None if number == 0 else number * 0.5 - (9000 if field_id == 11 else 1000)
        elif field_id in (9, 10):
            value = None if number == 65535 else number / 10.0
            if field_id == 9 and number != 65535 and number > 3599:
                raise DecodeError("track angle outside 0..359.9 degrees")
        elif field_id == 12:
            value = None if number == 255 else (number & 127) * (-0.5 if number & 128 else 0.5)
        elif field_id == 20:
            value = number or None
        else:
            if number > 15:
                raise DecodeError("field {:03d} exceeds its four-bit value range".format(field_id))
            if number > ENUM_MAX[field_id]:
                warnings.append("field {:03d} uses reserved value {}".format(field_id, number))
            value = number
        values[name] = value
    if pos != len(packet):
        raise DecodeError("unclaimed bytes after known fields")
    return {"version": "V1.{}".format(packet[1] & 31),
            "content_length": packet[2], "packet_length": len(packet),
            "flags_hex": bytes(flags).hex(), "present_fields": present,
            "fields": values, "warnings": warnings, "raw_hex": packet.hex()}


def extract_vendor_payloads(information_elements, wifi_id):
    """Walk complete 802.11 IEs and return only matching vendor IE contents."""
    if len(wifi_id) != 4:
        raise ValueError("Wi-Fi identifier must have four bytes")
    offset = 0
    payloads = []
    while offset < len(information_elements):
        if offset + 2 > len(information_elements):
            raise DecodeError("truncated information-element header")
        element_id, length = information_elements[offset:offset + 2]
        offset += 2
        if offset + length > len(information_elements):
            raise DecodeError("truncated information-element body")
        body = information_elements[offset:offset + length]
        offset += length
        if element_id == 221 and body[:4] == wifi_id:
            payloads.append(body[4:])
    return payloads


MAX_CAPTURE_BLOCK = 16 * 1024 * 1024
LINKTYPES = (105, 127)  # IEEE802_11, IEEE802_11_RADIOTAP
PCAP_MAGIC = {b"\xd4\xc3\xb2\xa1": ("<", 1000000),
              b"\xa1\xb2\xc3\xd4": (">", 1000000),
              b"\x4d\x3c\xb2\xa1": ("<", 1000000000),
              b"\xa1\xb2\x3c\x4d": (">", 1000000000)}


def _read_exact(stream, size, context):
    if not 0 <= size <= MAX_CAPTURE_BLOCK:
        raise DecodeError(context + ": invalid or excessively large length")
    value = stream.read(size)
    if len(value) != size:
        raise DecodeError(context + ": truncated capture")
    return value


def _check_linktype(linktype):
    if linktype not in LINKTYPES:
        raise DecodeError("unsupported capture linktype {}; use IEEE802_11 (105) or Radiotap (127)".format(linktype))


def _pcap_packets(stream, magic):
    endian, resolution = PCAP_MAGIC[magic]
    header = _read_exact(stream, 20, "PCAP header")
    major, minor, _, _, snaplen, network = struct.unpack(endian + "HHIIII", header)
    if (major, minor) != (2, 4):
        raise DecodeError("unsupported PCAP version; expected 2.4")
    linktype = network & 0xFFFF
    _check_linktype(linktype)
    # PCAP additional-linktype FCS information: bit 26 present, top nibble
    # is the FCS length in 16-bit words. Radiotap flags can also supply it.
    fcs_length = 2 * (network >> 28) if network & (1 << 26) else 0
    index = 0
    while True:
        first = stream.read(1)
        if not first:
            return
        record = first + _read_exact(stream, 15, "PCAP packet header")
        seconds, fraction, captured, original = struct.unpack(endian + "IIII", record)
        if fraction >= resolution or captured > original or (snaplen and captured > snaplen):
            raise DecodeError("invalid PCAP packet header values")
        if captured < original:
            raise DecodeError("PCAP packet truncated by capture snap length")
        packet = _read_exact(stream, captured, "PCAP packet data")
        index += 1
        yield index, seconds + fraction / resolution, linktype, fcs_length, packet


def _ng_options(data, endian):
    offset = 0
    while offset < len(data):
        if offset + 4 > len(data):
            raise DecodeError("truncated PCAPNG option header")
        code, length = struct.unpack_from(endian + "HH", data, offset)
        offset += 4
        if code == 0:
            if length != 0:
                raise DecodeError("invalid PCAPNG end-of-options marker")
            return
        padded = (length + 3) & ~3
        if offset + padded > len(data):
            raise DecodeError("truncated PCAPNG option value")
        yield code, data[offset:offset + length]
        offset += padded


def _pcapng_packets(stream, first_magic):
    endian = None
    interfaces = []
    index = 0
    prefix = first_magic
    while prefix:
        if len(prefix) != 4:
            raise DecodeError("truncated PCAPNG block header")
        length_bytes = _read_exact(stream, 4, "PCAPNG block length")
        section = prefix == b"\x0a\x0d\x0d\x0a"
        if section:
            bom = _read_exact(stream, 4, "PCAPNG byte-order marker")
            if bom == b"\x4d\x3c\x2b\x1a":
                endian = "<"
            elif bom == b"\x1a\x2b\x3c\x4d":
                endian = ">"
            else:
                raise DecodeError("invalid PCAPNG byte-order marker")
            already_read = bom
        else:
            if endian is None:
                raise DecodeError("PCAPNG needs a section header before other blocks")
            already_read = b""
        total = struct.unpack(endian + "I", length_bytes)[0]
        if total < (28 if section else 12) or total % 4:
            raise DecodeError("invalid PCAPNG block size")
        remaining = _read_exact(stream, total - 8 - len(already_read), "PCAPNG block")
        if struct.unpack(endian + "I", remaining[-4:])[0] != total:
            raise DecodeError("PCAPNG block lengths disagree")
        body = already_read + remaining[:-4]
        block_type = struct.unpack(endian + "I", prefix)[0]
        if section:
            if struct.unpack_from(endian + "HH", body, 4) != (1, 0):
                raise DecodeError("unsupported PCAPNG version; expected 1.0")
            interfaces = []
            list(_ng_options(body[16:], endian))
        elif block_type == 1:  # Interface Description Block
            if len(body) < 8:
                raise DecodeError("truncated PCAPNG interface description")
            linktype, _, snaplen = struct.unpack_from(endian + "HHI", body)
            _check_linktype(linktype)
            resolution, time_offset, fcs_length = 1000000, 0, 0
            for code, value in _ng_options(body[8:], endian):
                if code == 9:  # if_tsresol
                    if len(value) != 1:
                        raise DecodeError("invalid PCAPNG timestamp resolution")
                    resolution = (2 if value[0] & 0x80 else 10) ** (value[0] & 0x7F)
                elif code == 14:  # if_tsoffset, signed seconds
                    if len(value) != 8:
                        raise DecodeError("invalid PCAPNG timestamp offset")
                    time_offset = struct.unpack(endian + "q", value)[0]
                elif code == 13:  # if_fcslen, bits
                    if len(value) != 1 or value[0] % 8:
                        raise DecodeError("unsupported PCAPNG FCS length")
                    fcs_length = value[0] // 8
            interfaces.append((linktype, snaplen, resolution, time_offset, fcs_length))
        elif block_type == 6:  # Enhanced Packet Block
            if len(body) < 20:
                raise DecodeError("truncated PCAPNG enhanced packet header")
            interface, hi, lo, captured, original = struct.unpack_from(endian + "IIIII", body)
            if interface >= len(interfaces):
                raise DecodeError("PCAPNG packet references an unknown interface")
            linktype, snaplen, resolution, time_offset, fcs_length = interfaces[interface]
            padded = (captured + 3) & ~3
            if captured > original or (snaplen and captured > snaplen) or 20 + padded > len(body):
                raise DecodeError("invalid PCAPNG captured packet length")
            if captured < original:
                raise DecodeError("PCAPNG packet truncated by capture snap length")
            list(_ng_options(body[20 + padded:], endian))
            index += 1
            timestamp = ((hi << 32) | lo) / resolution + time_offset
            yield index, timestamp, linktype, fcs_length, body[20:20 + captured]
        elif block_type in (2, 3):
            raise DecodeError("unsupported PCAPNG packet block; convert to Enhanced Packet Blocks")
        # Metadata and unknown non-packet blocks are skipped after size checks.
        prefix = stream.read(4)


def capture_packets(path):
    """Read common PCAP and PCAPNG captures without external dependencies."""
    with Path(path).open("rb") as stream:
        magic = _read_exact(stream, 4, "capture magic")
        if magic in PCAP_MAGIC:
            yield from _pcap_packets(stream, magic)
        elif magic == b"\x0a\x0d\x0d\x0a":
            yield from _pcapng_packets(stream, magic)
        else:
            raise DecodeError("unsupported capture format; expected PCAP or PCAPNG")


def beacon_information_elements(packet, linktype, fcs_length=0):
    """Extract a beacon's IE area, accounting for Radiotap and declared FCS."""
    if linktype == 127:
        if len(packet) < 8 or packet[0] != 0:
            raise DecodeError("invalid Radiotap header")
        header_length = struct.unpack_from("<H", packet, 2)[0]
        if not 8 <= header_length <= len(packet):
            raise DecodeError("truncated Radiotap header")
        first_present = struct.unpack_from("<I", packet, 4)[0]
        present, offset = first_present, 8
        while present & (1 << 31):
            if offset + 4 > header_length:
                raise DecodeError("truncated Radiotap presence bitmap")
            present = struct.unpack_from("<I", packet, offset)[0]
            offset += 4
        if first_present & 1:  # TSFT precedes Flags and is aligned to 8 bytes.
            offset = ((offset + 7) & ~7) + 8
            if offset > header_length:
                raise DecodeError("truncated Radiotap TSFT field")
        if first_present & 2:
            if offset >= header_length:
                raise DecodeError("truncated Radiotap Flags field")
            if packet[offset] & 0x10:
                fcs_length = 4
        packet = packet[header_length:]
    if fcs_length:
        if len(packet) < fcs_length:
            raise DecodeError("truncated 802.11 FCS")
        packet = packet[:-fcs_length]
    if len(packet) < 2:
        raise DecodeError("truncated 802.11 frame control")
    frame_control = struct.unpack_from("<H", packet)[0]
    if frame_control & 0xFC != 0x80:  # Type=management, subtype=beacon
        return None
    if frame_control & 3:
        raise DecodeError("unsupported 802.11 protocol version")
    header_length = 24 + (4 if frame_control & 0x8000 else 0)
    if len(packet) < header_length + 12:
        raise DecodeError("truncated 802.11 beacon header")
    source_mac = ":".join("{:02x}".format(value) for value in packet[10:16])
    return source_mac, packet[header_length + 12:]


def decode_capture(path, wifi_id):
    """Yield matching vendor IE records, never searching arbitrary frame bytes."""
    for index, timestamp, linktype, fcs_length, frame in capture_packets(path):
        metadata = {"capture_packet": index, "capture_time": timestamp}
        try:
            beacon = beacon_information_elements(frame, linktype, fcs_length)
            if beacon is None:
                continue
            metadata["source_mac"], elements = beacon
            for payload in extract_vendor_payloads(elements, wifi_id):
                try:
                    yield dict(metadata, rid=decode_packet(payload))
                except DecodeError as exc:
                    yield dict(metadata, error=str(exc), raw_hex=payload.hex())
        except DecodeError as exc:
            yield dict(metadata, error="802.11: " + str(exc))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--hex", help="one complete raw CN payload as hexadecimal")
    inputs.add_argument("--bin", type=Path, help="one complete raw CN payload file")
    inputs.add_argument("--pcap", type=Path, help="802.11 PCAP/PCAPNG capture")
    parser.add_argument("--wifi-id", type=parse_wifi_id,
                        help="explicit 4-byte vendor ID; 02000001 is a lab convention only")
    args = parser.parse_args(argv)
    if args.pcap and args.wifi_id is None:
        parser.error("--pcap requires --wifi-id; there is no assumed official identifier")
    try:
        if args.pcap:
            count = 0
            failed = False
            for record in decode_capture(args.pcap, args.wifi_id):
                print(json.dumps(record, ensure_ascii=False, sort_keys=True))
                count += 1
                failed |= "error" in record
            if not count:
                print("No matching vendor IE payloads found", file=sys.stderr)
                return 1
            return int(failed)
        packet = args.bin.read_bytes() if args.bin else bytes.fromhex(args.hex)
        print(json.dumps(decode_packet(packet), indent=2, ensure_ascii=False, sort_keys=True))
        return 0
    except (ValueError, OSError, RuntimeError) as exc:
        print("Decode failed: {}".format(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
