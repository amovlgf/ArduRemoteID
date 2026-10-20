#!/usr/bin/env python3
"""Independent decoder fixtures and deterministic DroneCAN input checks.

Run: python3 -m unittest discover -s scripts/tests -v
"""

import argparse
import base64
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
import cn_rid_decode as decoder
import cn_dronecan_test as simulator


# Independently transcribed Table 1-3 vector: 66 data bytes + 6 header bytes.
# ID 12345678901234567890, UIN ABCDEFGH, class small, takeoff position,
# lon 121 / lat 31 degrees, operator 80 m, aircraft 100 m, track 90 deg,
# speed 12.3 m/s, ground, WGS84, accuracy 10/4/3, Unix 1735689600123 ms.
GOLDEN = bytes.fromhex(
    "ff2042dfe5fe3132333435363738393031323334353637383930"
    "4142434445464748020080221f4880397a12700880221f4880397a12"
    "84037b00980801000a04037b7c291f940105")


def beacon_fixture(radiotap=False, fcs=False):
    mac = bytes.fromhex("020000000001")
    header = bytes.fromhex("80000000") + b"\xff" * 6 + mac + mac + b"\x00\x00"
    elements = b"\x00\x07RID lab" + bytes([221, len(GOLDEN) + 4]) + bytes.fromhex("02000001") + GOLDEN
    frame = header + bytes(12) + elements
    if fcs:
        frame += b"\x12\x34\x56\x78"
    if radiotap:
        frame = (bytes.fromhex("000009000200000010") if fcs else bytes.fromhex("0000080000000000")) + frame
    return frame


def pcap_fixture(frame, endian="<", nano=False, linktype=105):
    magic = struct.pack(endian + "I", 0xA1B23C4D if nano else 0xA1B2C3D4)
    header = magic + struct.pack(endian + "HHIIII", 2, 4, 0, 0, 65535, linktype)
    return header + struct.pack(endian + "IIII", 1700000000, 500000000 if nano else 500000,
                                len(frame), len(frame)) + frame


def ng_block(kind, body, endian="<"):
    size = 12 + len(body)
    return struct.pack(endian + "II", kind, size) + body + struct.pack(endian + "I", size)


def pcapng_fixture(frame, endian="<", linktype=105):
    section = ng_block(0x0A0D0D0A, struct.pack(endian + "IHHq", 0x1A2B3C4D, 1, 0, -1), endian)
    # Nanosecond timestamp option and explicit end-of-options marker.
    options = struct.pack(endian + "HH", 9, 1) + b"\x09\x00\x00\x00" + bytes(4)
    interface = ng_block(1, struct.pack(endian + "HHI", linktype, 0, 65535) + options, endian)
    timestamp = 1700000000500000000
    packet = struct.pack(endian + "IIIII", 0, timestamp >> 32, timestamp & 0xFFFFFFFF,
                         len(frame), len(frame)) + frame + bytes((-len(frame)) % 4)
    return section + interface + ng_block(6, packet, endian)


class DecoderTests(unittest.TestCase):
    def test_standard_golden_vector(self):
        result = decoder.decode_packet(GOLDEN)
        fields = result["fields"]
        self.assertEqual(result["packet_length"], 72)
        self.assertEqual(result["content_length"], 66)
        self.assertEqual(fields["uas_id"], "12345678901234567890")
        self.assertEqual(fields["uin"], "ABCDEFGH")
        self.assertEqual(fields["aircraft_class"], 2)
        self.assertEqual(fields["operator_position"], {"longitude_deg": 121, "latitude_deg": 31})
        self.assertEqual(fields["operator_altitude_m"], 80)
        self.assertEqual(fields["geodetic_altitude_m"], 100)
        self.assertEqual(fields["track_angle_deg"], 90)
        self.assertEqual(fields["ground_speed_m_s"], 12.3)
        self.assertEqual(fields["timestamp_ms"], 1735689600123)
        self.assertEqual(result["warnings"], [])

    def test_unknown_values_remain_unknown_and_empty_registration_is_valid(self):
        packet = bytearray(GOLDEN)
        packet[26:34] = bytes(8)
        packet[36:44] = b"\xff" * 8
        packet[44:46] = bytes(2)
        packet[46:54] = b"\xff" * 8
        packet[54:58] = b"\xff" * 4
        packet[58:60] = bytes(2)
        packet[65:71] = bytes(6)
        fields = decoder.decode_packet(packet)["fields"]
        self.assertEqual(fields["uin"], "")
        self.assertIsNone(fields["operator_position"]["longitude_deg"])
        self.assertIsNone(fields["aircraft_position"]["latitude_deg"])
        for name in ("operator_altitude_m", "track_angle_deg", "ground_speed_m_s",
                     "geodetic_altitude_m", "timestamp_ms"):
            self.assertIsNone(fields[name])

    def test_zero_coordinate_is_distinct_from_unknown(self):
        packet = bytearray(GOLDEN)
        packet[46:54] = bytes(8)
        self.assertEqual(decoder.decode_packet(packet)["fields"]["aircraft_position"],
                         {"longitude_deg": 0, "latitude_deg": 0})

    def test_all_optional_fields_and_negative_vertical_speed(self):
        # Insert category=1, relative altitude=20m, vertical speed=-2.5m/s,
        # pressure altitude=95m at their Table 3 positions.
        packet = (bytes.fromhex("ff2048fffffe") + GOLDEN[6:34] + b"\x01" +
                  GOLDEN[34:58] + bytes.fromhex("784685") + GOLDEN[58:60] +
                  bytes.fromhex("8e08") + GOLDEN[60:])
        fields = decoder.decode_packet(packet)["fields"]
        self.assertEqual(fields["operation_category"], 1)
        self.assertEqual(fields["relative_altitude_m"], 20)
        self.assertEqual(fields["vertical_speed_m_s"], -2.5)
        self.assertEqual(fields["barometric_altitude_m"], 95)
        unknown = bytearray(packet)
        unknown[61] = 255
        self.assertIsNone(decoder.decode_packet(unknown)["fields"]["vertical_speed_m_s"])

    def test_structural_errors(self):
        bad_flags = bytearray(GOLDEN)
        bad_flags[3] &= ~0x40
        extension = bytearray(GOLDEN)
        extension[5] |= 1
        bad_angle = bytearray(GOLDEN)
        bad_angle[54:56] = bytes.fromhex("100e")
        bad_position = bytearray(GOLDEN)
        bad_position[46:50] = (1810000000).to_bytes(4, "little")
        for packet in (b"", GOLDEN[:-1], GOLDEN + b"\x00", bad_flags,
                       extension, bad_angle, bad_position, b"\x00" + GOLDEN[1:]):
            with self.subTest(packet=bytes(packet).hex()):
                with self.assertRaises(decoder.DecodeError):
                    decoder.decode_packet(packet)

    def test_reserved_enumerations_are_reported(self):
        packet = bytearray(GOLDEN)
        packet[34] = 15
        self.assertTrue(decoder.decode_packet(packet)["warnings"])
        packet[34] = 16
        with self.assertRaises(decoder.DecodeError):
            decoder.decode_packet(packet)

    def test_vendor_ie_matching_respects_boundaries_and_identifier(self):
        wifi_id = bytes.fromhex("02000001")
        vendor = bytes([221, len(GOLDEN) + 4]) + wifi_id + GOLDEN
        decoy = bytes([0, len(GOLDEN)]) + GOLDEN  # Header-looking SSID data.
        other = bytes([221, len(GOLDEN) + 4]) + bytes.fromhex("fa0bbc0d") + GOLDEN
        self.assertEqual(decoder.extract_vendor_payloads(decoy + other + vendor, wifi_id), [GOLDEN])
        self.assertEqual(decoder.extract_vendor_payloads(decoy + other, wifi_id), [])
        with self.assertRaises(decoder.DecodeError):
            decoder.extract_vendor_payloads(vendor[:-1], wifi_id)

    def test_hex_cli(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(decoder.main(["--hex", GOLDEN.hex()]), 0)
        self.assertEqual(json.loads(output.getvalue())["fields"]["timestamp_ms"], 1735689600123)

    def test_pcap_and_pcapng_capture(self):
        wifi_id = bytes.fromhex("02000001")
        with tempfile.TemporaryDirectory() as directory:
            for linktype in (105, 127):
                for endian in ("<", ">"):
                    frame = beacon_fixture(radiotap=linktype == 127, fcs=linktype == 127)
                    captures = (pcap_fixture(frame, endian, False, linktype),
                                pcap_fixture(frame, endian, True, linktype),
                                pcapng_fixture(frame, endian, linktype))
                    for capture in captures:
                        with self.subTest(linktype=linktype, endian=endian, magic=capture[:4].hex()):
                            path = Path(directory) / "capture"
                            path.write_bytes(capture)
                            records = list(decoder.decode_capture(path, wifi_id))
                            self.assertEqual(len(records), 1)
                            self.assertEqual(records[0]["source_mac"], "02:00:00:00:00:01")
                            self.assertEqual(records[0]["capture_time"], 1700000000.5)
                            self.assertEqual(records[0]["rid"]["raw_hex"], GOLDEN.hex())

    def test_capture_truncation_bad_boundaries_and_unsupported_linktype(self):
        pcap = pcap_fixture(beacon_fixture())
        pcapng = pcapng_fixture(beacon_fixture())
        bad_trailer = bytearray(pcapng)
        bad_trailer[-1] = 1
        invalid_captures = (b"", b"not a pcap", pcap[:10], pcap[:-1], pcapng[:15], pcapng[:-1],
                            bad_trailer, pcap_fixture(beacon_fixture(), linktype=1),
                            pcapng_fixture(beacon_fixture(), linktype=1))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture"
            for capture in invalid_captures:
                with self.subTest(length=len(capture), magic=bytes(capture[:4]).hex()):
                    path.write_bytes(capture)
                    with self.assertRaises(decoder.DecodeError):
                        list(decoder.capture_packets(path))

    def test_pcapng_sections_reset_interface_and_byte_order(self):
        capture = pcapng_fixture(beacon_fixture(), "<") + pcapng_fixture(beacon_fixture(True), ">", 127)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture"
            path.write_bytes(capture)
            records = list(decoder.decode_capture(path, bytes.fromhex("02000001")))
            self.assertEqual(len(records), 2)
            self.assertEqual([record["capture_packet"] for record in records], [1, 2])
            self.assertTrue(all(record["rid"]["raw_hex"] == GOLDEN.hex() for record in records))


class SimulatorTests(unittest.TestCase):
    def test_utc_hour_rollover(self):
        before_ms = 1735693199500  # 2025-01-01 00:59:59.500 UTC
        before, before_system = simulator.sample_fields(before_ms)
        after, after_system = simulator.sample_fields(before_ms + 1000)
        self.assertEqual(before["timestamp"], 3599.5)
        self.assertEqual(after["timestamp"], 0.5)
        self.assertEqual(after_system["timestamp"] - before_system["timestamp"], 1)
        for location, system, expected in ((before, before_system, before_ms),
                                           (after, after_system, before_ms + 1000)):
            utc_second = system["timestamp"] + simulator.EPOCH_2019
            reconstructed = utc_second // 3600 * 3600000 + round(location["timestamp"] * 1000)
            self.assertEqual(reconstructed, expected)

    def test_fault_scenarios(self):
        stamp = 1735689600123
        location, system = simulator.sample_fields(stamp, "no-time", True)
        self.assertEqual(location["timestamp"], 65535)
        self.assertEqual(system["timestamp"], 0)
        location, system = simulator.sample_fields(stamp, "no-position", True)
        self.assertEqual(location["latitude"], 0)
        self.assertEqual(system["operator_altitude_geo"], -1000)
        self.assertEqual(simulator.sample_fields(stamp, "emergency", True)[0]["status"], 3)
        self.assertEqual(simulator.enabled_messages("drop-location", True), (False, True))
        self.assertEqual(simulator.enabled_messages("drop-system", True), (True, False))
        self.assertEqual(simulator.enabled_messages("drop-system", False), (True, True))

    def test_configuration_validates_lengths_ranges_and_preserves_leading_zeroes(self):
        self.assertEqual(simulator.parameter_assignment("CN_UIN="), ("CN_UIN", ""))
        self.assertEqual(simulator.parameter_assignment("CN_UIN=00000001"), ("CN_UIN", "00000001"))
        self.assertEqual(simulator.parameter_assignment("CN_WIFI_ID=02000001"), ("CN_WIFI_ID", "02000001"))
        self.assertEqual(simulator.parameter_assignment("CN_CAN_NODE=42"), ("CN_CAN_NODE", 42))
        for argument in ("CN_UIN=1234", "CN_UAS_ID=short", "CN_CLASS=255", "CN_CAN_NODE=0",
                         "CN_WIFI_ID=fa0bbc0d", "CN_WIFI_ID=0 000001", "RID_PROFILE=2",
                         "LOCK_LEVEL=0", "WEBSERVER_EN=1", "PUBLIC_KEY4=anything",
                         "CN_UAS_ID=1234567890123456789 ", "CN_UIN=1234 678"):
            with self.subTest(argument=argument):
                with self.assertRaises(argparse.ArgumentTypeError):
                    simulator.parameter_assignment(argument)

    def test_dry_run_needs_no_hardware(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(simulator.main(["--dry-run", "--start-unix-ms", "1735689600123"]), 0)
        report = json.loads(output.getvalue())
        self.assertEqual(report["node_id"], 42)
        self.assertEqual(report["set"], [])

    def test_complete_ota_tutorial_read_command_dry_run(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(simulator.main([
                "can0", "--node-id", "42", "--target-node", "125", "--no-stream",
                "--get", "LOCK_LEVEL", "--get", "PUBLIC_KEY4", "--get", "WEBSERVER_EN",
                "--get", "WIFI_SSID", "--get", "WIFI_CHANNEL", "--dry-run"]), 0)
        report = json.loads(output.getvalue())
        self.assertEqual(report["get"], ["LOCK_LEVEL", "PUBLIC_KEY4", "WEBSERVER_EN", "WIFI_SSID", "WIFI_CHANNEL"])
        self.assertEqual(report["set"], [])
        self.assertFalse(report["stream"])
        self.assertIsNone(report["install_public_key"])
        for slot in range(1, 6):
            args = simulator.argument_parser().parse_args(["--get", "PUBLIC_KEY{}".format(slot)])
            self.assertEqual(args.get, ["PUBLIC_KEY{}".format(slot)])

    def test_public_key_file_validation(self):
        expected = "PUBLIC_KEYV1:" + base64.b64encode(bytes(range(32))).decode("ascii")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test_public_key.dat"
            path.write_text(expected + "\n", encoding="ascii")
            self.assertEqual(simulator.read_public_key(path), expected)
            for content in ("PRIVATE_KEYV1:" + "secret", "PUBLIC_KEYV1:not-base64",
                            "PUBLIC_KEYV1:" + base64.b64encode(bytes(31)).decode("ascii"),
                            expected + "\n" * 100):
                path.write_text(content, encoding="ascii")
                with self.assertRaises(ValueError):
                    simulator.read_public_key(path)

    def test_private_key_rejected_before_reading_key_material(self):
        class TrackingStream(io.BytesIO):
            sizes = []

            def read(self, size=-1):
                self.sizes.append(size)
                return super().read(size)

        stream = TrackingStream(b"PRIVATE_KEYV1:" + b"unread secret material")
        with mock.patch.object(Path, "open", return_value=stream):
            with self.assertRaisesRegex(ValueError, "private keys are not accepted"):
                simulator.read_public_key("synthetic_private_key.dat")
        self.assertEqual(stream.sizes, [len(b"PUBLIC_KEYV1:")])

    def test_public_key_installation_preconditions_and_readback(self):
        key = "PUBLIC_KEYV1:" + base64.b64encode(bytes(range(32))).decode("ascii")

        class FakeParameters:
            def __init__(self, lock_level=0, existing="", bad_readback=False):
                self.lock_level = lock_level
                self.existing = existing
                self.bad_readback = bad_readback
                self.operations = []
                self.written = False

            def __call__(self, dronecan, node, target, name, value, timeout):
                self.operations.append((name, value))
                if name == "LOCK_LEVEL":
                    return self.lock_level
                if value is not None:
                    self.existing = value
                    self.written = True
                    return value
                return "corrupted" if self.written and self.bad_readback else self.existing

        for level in (-1, 1, 2):
            fake = FakeParameters(lock_level=level)
            with mock.patch.object(simulator, "parameter_request", side_effect=fake):
                with self.assertRaisesRegex(RuntimeError, "LOCK_LEVEL=0"):
                    simulator.install_public_key(None, None, 125, key, 4, 3)
            self.assertEqual(fake.operations, [("LOCK_LEVEL", None)])

        occupied = FakeParameters(existing="different existing public key")
        with mock.patch.object(simulator, "parameter_request", side_effect=occupied):
            with self.assertRaisesRegex(RuntimeError, "refusing to overwrite"):
                simulator.install_public_key(None, None, 125, key, 4, 3)
        self.assertFalse(occupied.written)

        installed = FakeParameters(existing=key)
        with mock.patch.object(simulator, "parameter_request", side_effect=installed):
            with contextlib.redirect_stdout(io.StringIO()):
                simulator.install_public_key(None, None, 125, key, 4, 3)
        self.assertFalse(installed.written)

        empty = FakeParameters()
        with mock.patch.object(simulator, "parameter_request", side_effect=empty):
            with contextlib.redirect_stdout(io.StringIO()):
                simulator.install_public_key(None, None, 125, key, 4, 3)
        self.assertEqual(empty.operations, [("LOCK_LEVEL", None), ("PUBLIC_KEY4", None),
                                           ("PUBLIC_KEY4", key), ("PUBLIC_KEY4", None)])

        bad = FakeParameters(bad_readback=True)
        with mock.patch.object(simulator, "parameter_request", side_effect=bad):
            with self.assertRaisesRegex(RuntimeError, "post-write readback"):
                simulator.install_public_key(None, None, 125, key, 4, 3)

    def test_public_key_installation_dry_run(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test_public_key.dat"
            path.write_text("PUBLIC_KEYV1:" + base64.b64encode(bytes(32)).decode("ascii"), encoding="ascii")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(simulator.main(["--dry-run", "--no-stream", "--target-node", "125",
                                                 "--install-public-key", str(path)]), 0)
            report = json.loads(output.getvalue())
            self.assertFalse(report["stream"])
            self.assertEqual(report["key_slot"], 4)
            self.assertEqual(report["install_public_key"], str(path))

    @unittest.skipUnless(importlib.util.find_spec("dronecan"), "optional python dronecan not installed")
    def test_existing_dsdl_accepts_simulated_messages(self):
        import dronecan
        for scenario in simulator.SCENARIOS:
            fields = simulator.sample_fields(1735689600123, scenario, True)
            location, system = simulator.build_messages(dronecan, *fields)
            self.assertEqual(location.latitude, fields[0]["latitude"])
            self.assertEqual(system.timestamp, fields[1]["timestamp"])
            # Serialization exercises DSDL bounds without opening a CAN interface.
            self.assertTrue(location._pack())
            self.assertTrue(system._pack())

    @unittest.skipUnless(importlib.util.find_spec("dronecan"), "optional python dronecan not installed")
    def test_parameter_readback_empty_registration_and_rejected_write(self):
        import dronecan

        class FakeNode:
            requested = None
            reject_write = False

            def request(self, request, target, callback, timeout):
                self.requested = request
                response = dronecan.uavcan.protocol.param.GetSet.Response(name=request.name)
                if self.reject_write:
                    response.value.integer_value = 0
                else:
                    response.value.string_value = b""
                callback(SimpleNamespace(response=response))

        node = FakeNode()
        with contextlib.redirect_stdout(io.StringIO()):
            simulator.parameter_request(dronecan, node, 125, "CN_UIN", "", 3)
        self.assertEqual(dronecan.get_active_union_field(node.requested.value), "string_value")
        node.reject_write = True
        with self.assertRaisesRegex(RuntimeError, "not applied"):
            simulator.parameter_request(dronecan, node, 125, "RID_PROFILE", 1, 3)


if __name__ == "__main__":
    unittest.main()
