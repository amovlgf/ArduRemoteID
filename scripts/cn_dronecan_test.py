#!/usr/bin/env python3
"""Send simulated OpenDroneID Location/System messages to a lab CAN bus.

Uses existing DroneCAN DSDL at 1 Mbit/s. It does not claim that the input
protocol carries every GB 46750 field. China identity/classification remain
module parameters. Writes require explicit --set or --install-public-key options.
Example (bench hardware only):
  python3 scripts/cn_dronecan_test.py can0 --node-id 42 --duration 30
  python3 scripts/cn_dronecan_test.py --dry-run --start-unix-ms 1789347599500
"""

import argparse
import base64
import binascii
import json
import math
from pathlib import Path
import sys
import time

EPOCH_2019 = 1546300800
PARAMETERS = {"RID_PROFILE": (0, 1), "CN_CAN_NODE": (1, 127),
              "CN_CLASS": (0, 4), "CN_UAS_ID": "identity",
              "CN_UIN": "registration", "CN_WIFI_ID": "wifi"}
READABLE_PARAMETERS = tuple(PARAMETERS) + ("LOCK_LEVEL", "WEBSERVER_EN", "WIFI_SSID", "WIFI_CHANNEL") + tuple(
    "PUBLIC_KEY{}".format(slot) for slot in range(1, 6))
SCENARIOS = ("nominal", "drop-location", "drop-system", "no-position", "no-time", "emergency")


def parameter_assignment(text):
    """Validate new-profile parameters before contacting a node."""
    if "=" not in text:
        raise argparse.ArgumentTypeError("parameter assignment must be NAME=VALUE")
    name, value = text.split("=", 1)
    if name not in PARAMETERS:
        raise argparse.ArgumentTypeError("supported parameters: " + ", ".join(PARAMETERS))
    spec = PARAMETERS[name]
    if isinstance(spec, tuple):
        try:
            value = int(value)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(name + " must be an integer") from exc
        if not spec[0] <= value <= spec[1]:
            raise argparse.ArgumentTypeError("{} must be {}..{}".format(name, *spec))
    else:
        if any(ord(char) < 33 or ord(char) > 126 for char in value):
            raise argparse.ArgumentTypeError(name + " must use ASCII 33..126 without spaces")
        if spec == "identity" and len(value) != 20:
            raise argparse.ArgumentTypeError("CN_UAS_ID must be exactly 20 ASCII characters without spaces")
        if spec == "registration" and len(value) not in (0, 8):
            raise argparse.ArgumentTypeError("CN_UIN must be empty or eight ASCII characters without spaces")
        if spec == "wifi":
            if len(value) != 8 or any(char not in "0123456789abcdefABCDEF" for char in value):
                raise argparse.ArgumentTypeError("CN_WIFI_ID must be exactly eight hex characters")
            if value.lower() == "fa0bbc0d":
                raise argparse.ArgumentTypeError("the ASTM vendor identifier cannot identify this CN envelope")
    return name, value


def sample_fields(unix_ms, scenario="nominal", fault_active=False):
    """Produce deterministic, inspectable native DroneCAN field dictionaries."""
    unix_ms = int(unix_ms)
    system_timestamp = unix_ms // 1000 - EPOCH_2019
    if not 1 <= system_timestamp <= 0xFFFFFFFF:
        raise ValueError("simulation time must fit nonzero System seconds since 2019")
    location = {
        "status": 1, "direction": 9000, "speed_horizontal": 1234,
        "speed_vertical": 0, "latitude": 310000000, "longitude": 1210000000,
        "altitude_barometric": 95.0, "altitude_geodetic": 100.0,
        "height_reference": 0, "height": 20.0,
        "horizontal_accuracy": 10, "vertical_accuracy": 4,
        "barometer_accuracy": 0, "speed_accuracy": 3,
        "timestamp": (unix_ms % 3600000) / 1000.0, "timestamp_accuracy": 1,
    }
    system = {
        "operator_location_type": 0, "classification_type": 0,
        "operator_latitude": 310000000, "operator_longitude": 1210000000,
        "area_count": 1, "area_radius": 0,
        "area_ceiling": -1000.0, "area_floor": -1000.0,
        "category_eu": 0, "class_eu": 0,
        "operator_altitude_geo": 80.0, "timestamp": system_timestamp,
    }
    if fault_active:
        if scenario == "no-position":
            location.update(latitude=0, longitude=0, altitude_geodetic=-1000.0,
                            horizontal_accuracy=0, vertical_accuracy=0)
            system.update(operator_latitude=0, operator_longitude=0, operator_altitude_geo=-1000.0)
        elif scenario == "no-time":
            location.update(timestamp=65535.0, timestamp_accuracy=0)
            system["timestamp"] = 0
        elif scenario == "emergency":
            location["status"] = 3
    return location, system


def enabled_messages(scenario, fault_active):
    return (not (fault_active and scenario == "drop-location"),
            not (fault_active and scenario == "drop-system"))


def build_messages(dronecan, location_fields, system_fields):
    return (dronecan.dronecan.remoteid.Location(**location_fields),
            dronecan.dronecan.remoteid.System(**system_fields))


def parameter_request(dronecan, node, target, name, value, timeout):
    request = dronecan.uavcan.protocol.param.GetSet.Request(name=name.encode("ascii"))
    if value is not None:
        kind = "integer_value" if isinstance(value, int) else "string_value"
        setattr(request.value, kind, value if isinstance(value, int) else value.encode("ascii"))
    result = []
    node.request(request, target, result.append, timeout=timeout)
    deadline = time.monotonic() + timeout + 0.5
    while not result and time.monotonic() < deadline:
        node.spin(timeout=0.05)
    if not result or result[0] is None:
        raise RuntimeError("{} request timed out for node {}".format(name, target))
    response = result[0].response
    if bytes(response.name).decode("ascii") != name:
        raise RuntimeError("{} is not available on node {}".format(name, target))
    kind = dronecan.get_active_union_field(response.value)
    if kind == "empty":
        raise RuntimeError(name + " returned no value")
    actual = getattr(response.value, kind)
    if kind == "string_value":
        actual = bytes(actual).decode("ascii")
    if value is not None and actual != value:
        raise RuntimeError("{} was not applied: requested {!r}, returned {!r}; check LOCK_LEVEL".format(
            name, value, actual))
    print(json.dumps({"parameter": name, "value": actual, "target_node": target}))
    return actual


def read_public_key(path):
    """Read a public-key file, rejecting private-key headers before key data."""
    prefix = b"PUBLIC_KEYV1:"
    with Path(path).open("rb") as stream:
        if stream.read(len(prefix)) != prefix:
            raise ValueError("public-key installation requires PUBLIC_KEYV1:; private keys are not accepted")
        encoded = stream.read(65).strip()
        if stream.read(1):
            raise ValueError("public-key file is unexpectedly long")
    if len(encoded) != 44:
        raise ValueError("public key must be base64 encoding of exactly 32 bytes")
    try:
        key = base64.b64decode(encoded, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise ValueError("public key has invalid base64 encoding") from exc
    if len(key) != 32 or base64.b64encode(key) != encoded:
        raise ValueError("public key must use canonical base64 for exactly 32 bytes")
    return prefix.decode("ascii") + encoded.decode("ascii")


def install_public_key(dronecan, node, target, key, slot, timeout):
    """Install an explicitly supplied key into an empty, unlocked trust slot."""
    if not 1 <= slot <= 5:
        raise ValueError("public key slot must be 1..5")
    lock_level = parameter_request(dronecan, node, target, "LOCK_LEVEL", None, timeout)
    if lock_level != 0:
        raise RuntimeError("public-key installation requires LOCK_LEVEL=0; no parameters changed")
    name = "PUBLIC_KEY{}".format(slot)
    existing = parameter_request(dronecan, node, target, name, None, timeout)
    if existing == key:
        print(json.dumps({"public_key_slot": slot, "target_node": target, "result": "already installed"}))
        return
    if existing != "":
        raise RuntimeError("{} already contains a different key; refusing to overwrite it".format(name))
    parameter_request(dronecan, node, target, name, key, timeout)
    # An independent read checks the stored value after the set response.
    actual = parameter_request(dronecan, node, target, name, None, timeout)
    if actual != key:
        raise RuntimeError("{} failed post-write readback verification".format(name))
    print(json.dumps({"public_key_slot": slot, "target_node": target, "result": "installed and verified"}))


def argument_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("uri", nargs="?", help="DroneCAN interface, e.g. can0 or serial adapter URI")
    parser.add_argument("--node-id", type=int, default=42, help="simulated flight controller node (default 42)")
    parser.add_argument("--target-node", type=int, help="RID node for explicit parameter operations, e.g. 125")
    parser.add_argument("--bitrate", type=int, default=1000000, choices=[1000000])
    parser.add_argument("--rate", type=float, default=2.0, help="Location and System pairs per second")
    parser.add_argument("--duration", type=float, default=30.0, help="stream duration in seconds")
    parser.add_argument("--scenario", choices=SCENARIOS, default="nominal")
    parser.add_argument("--fault-after", type=float, default=5.0, help="seconds of normal input before fault")
    parser.add_argument("--start-unix-ms", type=int, help="UTC starting timestamp; default is current host time")
    parser.add_argument("--get", action="append", choices=READABLE_PARAMETERS, default=[], metavar="NAME")
    parser.add_argument("--set", action="append", type=parameter_assignment, default=[], metavar="NAME=VALUE")
    parser.add_argument("--install-public-key", type=Path, metavar="FILE",
                        help="explicitly install a PUBLIC_KEYV1 file into an empty slot; requires LOCK_LEVEL=0")
    parser.add_argument("--key-slot", type=int, choices=range(1, 6), default=4,
                        help="trust slot for --install-public-key (default 4); existing different keys are preserved")
    parser.add_argument("--no-stream", action="store_true", help="perform only requested parameter operations")
    parser.add_argument("--timeout", type=float, default=3.0, help="parameter response timeout")
    parser.add_argument("--dry-run", action="store_true", help="print intended operations and sample data; no CAN access")
    return parser


def main(argv=None):
    parser = argument_parser()
    args = parser.parse_args(argv)
    if not 1 <= args.node_id <= 127:
        parser.error("--node-id must be 1..127")
    if args.target_node is not None and not 1 <= args.target_node <= 127:
        parser.error("--target-node must be 1..127")
    if args.target_node == args.node_id:
        parser.error("source and target node IDs must differ")
    if (args.set or args.get or args.install_public_key) and args.target_node is None:
        parser.error("parameter operations require --target-node")
    for name, value in (("rate", args.rate), ("duration", args.duration), ("timeout", args.timeout)):
        if not math.isfinite(value) or value <= 0:
            parser.error("--{} must be finite and positive".format(name))
    if not math.isfinite(args.fault_after) or args.fault_after < 0:
        parser.error("--fault-after must be finite and nonnegative")
    if not args.uri and not args.dry_run:
        parser.error("provide a CAN interface URI or use --dry-run")
    start_ms = args.start_unix_ms if args.start_unix_ms is not None else time.time_ns() // 1000000
    try:
        public_key = read_public_key(args.install_public_key) if args.install_public_key else None
        initial = sample_fields(start_ms)
        if args.dry_run:
            fault_ms = start_ms + int(args.fault_after * 1000)
            print(json.dumps({"node_id": args.node_id, "target_node": args.target_node,
                              "set": args.set, "get": args.get, "stream": not args.no_stream,
                              "install_public_key": str(args.install_public_key) if public_key else None,
                              "key_slot": args.key_slot if public_key else None,
                              "start_unix_ms": start_ms, "initial_location": initial[0],
                              "initial_system": initial[1], "fault_after_s": args.fault_after,
                              "scenario": args.scenario,
                              "fault_fields": sample_fields(fault_ms, args.scenario, True),
                              "fault_send_location_system": enabled_messages(args.scenario, True)}, indent=2))
            return 0
        try:
            import dronecan
        except ImportError as exc:
            raise RuntimeError("install input support: python3 -m pip install dronecan") from exc
        node = dronecan.make_node(args.uri, node_id=args.node_id, bitrate=args.bitrate)
        try:
            if public_key:
                install_public_key(dronecan, node, args.target_node, public_key, args.key_slot, args.timeout)
            for name, value in args.set:
                parameter_request(dronecan, node, args.target_node, name, value, args.timeout)
            for name in args.get:
                parameter_request(dronecan, node, args.target_node, name, None, args.timeout)
            if args.no_stream:
                return 0
            print("Streaming simulated inputs from node {} at {} pairs/s for {} s".format(
                args.node_id, args.rate, args.duration), file=sys.stderr)
            started = time.monotonic()
            # Clock starts when the stream starts, after potentially slow configuration.
            if args.start_unix_ms is None:
                start_ms = time.time_ns() // 1000000
            next_send = started
            fault_reported = False
            while time.monotonic() - started < args.duration:
                now = time.monotonic()
                if now >= next_send:
                    elapsed = now - started
                    fault_active = elapsed >= args.fault_after
                    if fault_active and not fault_reported and args.scenario != "nominal":
                        print("Activating scenario: " + args.scenario, file=sys.stderr)
                        fault_reported = True
                    fields = sample_fields(start_ms + int(elapsed * 1000), args.scenario, fault_active)
                    location, system = build_messages(dronecan, *fields)
                    send_location, send_system = enabled_messages(args.scenario, fault_active)
                    # System supplies the hour before the matching Location arrives.
                    if send_system:
                        node.broadcast(system)
                    if send_location:
                        node.broadcast(location)
                    next_send = now + 1.0 / args.rate
                node.spin(timeout=min(0.05, max(0.0, next_send - time.monotonic())))
        finally:
            node.close()
        return 0
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError, RuntimeError) as exc:
        print("Simulation failed: {}".format(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
