#!/usr/bin/env python3
"""Closed-bench GPS/System input for PX4 -> DroneCAN -> China RID.

Requires pymavlink and pyserial. Close QGC before opening the same serial port.
The operator enables MAV_USEHILGPS in QGC and restores it after the test.
This program does not write parameters, arm, or command vehicle movement.
"""

import argparse
import json
import math
import os
import struct
import sys
import time

EPOCH_2019 = 1546300800
LAT = 310000000
LON = 1210000000
ALT_MM = 100000


def sample_fields(unix_us, valid=True):
    """Both messages use the same clock; coordinates are synthetic bench data."""
    seconds_2019 = unix_us // 1000000 - EPOCH_2019
    if not 1 <= seconds_2019 <= 0xFFFFFFFF:
        raise ValueError("Computer UTC must fit the nonzero RID timestamp range")
    gps = dict(time_usec=unix_us if valid else 0, fix_type=3 if valid else 0,
               lat=LAT if valid else 0, lon=LON if valid else 0,
               alt=ALT_MM if valid else 0, eph=100, epv=150,
               vel=0, vn=0, ve=0, vd=0, cog=0, satellites_visible=12 if valid else 0)
    system = dict(id_or_mac=[0] * 20, operator_location_type=2,
                  classification_type=0, operator_latitude=LAT if valid else 0,
                  operator_longitude=LON if valid else 0, area_count=1,
                  area_radius=0, area_ceiling=-1000.0, area_floor=-1000.0,
                  category_eu=0, class_eu=0,
                  operator_altitude_geo=100.0 if valid else -1000.0,
                  timestamp=seconds_2019 if valid else 0)
    return gps, system


def px4_int32(wire_float):
    # PX4 PARAM_VALUE packs the integer bits inside the float32 wire field.
    return struct.unpack('<i', struct.pack('<f', wire_float))[0]


def require_disarmed(heartbeat, mav):
    if heartbeat.base_mode & mav.MAV_MODE_FLAG_SAFETY_ARMED:
        raise RuntimeError("Vehicle is armed: stopping bench input")


class Link:
    def __init__(self, connection, mav):
        self.connection = connection
        self.mav = mav
        self.system = None
        self.component = None
        self.last_heartbeat = 0.0
        self.armed = False

    def discover(self):
        self.connection.mav.ping_send(time.time_ns() // 1000, 0, 0, 0)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            msg = self.connection.recv_match(type='HEARTBEAT', blocking=True, timeout=0.5)
            if msg is None or msg.autopilot != self.mav.MAV_AUTOPILOT_PX4:
                continue
            if msg.get_srcComponent() != self.mav.MAV_COMP_ID_AUTOPILOT1:
                continue
            require_disarmed(msg, self.mav)
            self.system, self.component = msg.get_srcSystem(), msg.get_srcComponent()
            self.last_heartbeat = time.monotonic()
            # PX4 accepts non-HIL-mode HIL_GPS only from its own system ID.
            self.connection.mav.srcSystem = self.system
            print('PX4 system={}, component={} (disarmed)'.format(self.system, self.component))
            return
        raise RuntimeError('No PX4 heartbeat: check port and close QGC')

    def receive(self, timeout=0):
        msg = self.connection.recv_match(blocking=timeout > 0, timeout=timeout)
        if msg is not None and (msg.get_srcSystem(), msg.get_srcComponent()) == (self.system, self.component):
            if msg.get_type() == 'HEARTBEAT':
                self.last_heartbeat = time.monotonic()
                self.armed = bool(msg.base_mode & self.mav.MAV_MODE_FLAG_SAFETY_ARMED)
                require_disarmed(msg, self.mav)
        if time.monotonic() - self.last_heartbeat > 5:
            raise RuntimeError('PX4 heartbeat lost: stopping bench input')
        return msg

    def check_input_parameter(self):
        name = b'MAV_USEHILGPS'
        for _ in range(3):
            self.connection.mav.param_request_read_send(self.system, self.component, name, -1)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                msg = self.receive(0.1)
                if msg is None or msg.get_type() != 'PARAM_VALUE':
                    continue
                if (msg.get_srcSystem(), msg.get_srcComponent()) != (self.system, self.component):
                    continue
                param_id = msg.param_id
                if isinstance(param_id, bytes):
                    param_id = param_id.decode('ascii')
                if param_id.rstrip('\0') != name.decode('ascii'):
                    continue
                if msg.param_type != self.mav.MAV_PARAM_TYPE_INT32:
                    raise RuntimeError('Unexpected MAV_USEHILGPS parameter type')
                value = px4_int32(msg.param_value)
                if value != 1:
                    raise RuntimeError('MAV_USEHILGPS={}; enable it in QGC for this bench test first'.format(value))
                print('MAV_USEHILGPS=1 verified; program makes no parameter changes')
                return
        raise RuntimeError('MAV_USEHILGPS could not be read; no simulated data sent')

    def send_system(self, fields):
        self.connection.mav.open_drone_id_system_send(
            target_system=self.system, target_component=self.component, **fields)

    def clear_samples(self):
        if self.armed or time.monotonic() - self.last_heartbeat > 5:
            return
        gps, system = sample_fields(time.time_ns() // 1000, valid=False)
        for _ in range(3):
            self.receive()
            self.connection.mav.hil_gps_send(**gps)
            self.send_system(system)
            time.sleep(0.2)
        print('Sent invalid GPS/System samples to end the simulation')


def stream(link, duration):
    start = time.monotonic()
    next_gps = next_system = next_report = start
    gps_count = system_count = 0
    try:
        while time.monotonic() - start < duration:
            # Drain a bounded batch so telemetry cannot starve the sender.
            for _ in range(100):
                if link.receive() is None:
                    break
            now = time.monotonic()
            gps, system = sample_fields(time.time_ns() // 1000)
            if now >= next_gps:
                link.connection.mav.hil_gps_send(**gps)
                gps_count += 1
                next_gps = now + 0.2
            if now >= next_system:
                link.send_system(system)
                system_count += 1
                next_system = now + 0.5
            if now >= next_report:
                print('BENCH SIMULATION: {:.0f}s / {:.0f}s, GPS={}, System={}, lat=31 lon=121 alt=100m'.format(
                    now - start, duration, gps_count, system_count), flush=True)
                next_report = now + 5
            time.sleep(0.01)
    finally:
        if gps_count or system_count:
            try:
                link.clear_samples()
            except Exception as exc:
                print('Could not invalidate samples: {}'.format(exc), file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', help='Flight controller serial port, e.g. COM7')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--duration', type=float, default=120, help='Seconds to send (1..600; default 120)')
    parser.add_argument('--list-ports', action='store_true')
    parser.add_argument('--dry-run', action='store_true', help='Print samples without opening a port')
    args = parser.parse_args(argv)
    if not math.isfinite(args.duration) or not 1 <= args.duration <= 600:
        parser.error('--duration must be between 1 and 600 seconds')
    if args.dry_run:
        gps, system = sample_fields(time.time_ns() // 1000)
        print(json.dumps({'simulation': True, 'gps_5hz': gps, 'system_2hz': system}, indent=2))
        return 0
    if args.list_ports:
        from serial.tools import list_ports
        for port in list_ports.comports():
            print('{}: {}'.format(port.device, port.description))
        return 0
    if not args.port:
        parser.error('--port is required; use --list-ports to list connected serial ports')

    os.environ['MAVLINK20'] = '1'
    from pymavlink import mavutil
    mavutil.set_dialect('common')
    connection = None
    try:
        connection = mavutil.mavlink_connection(args.port, baud=args.baud,
                                               source_system=255, source_component=191,
                                               dialect='common', autoreconnect=False)
        link = Link(connection, mavutil.mavlink)
        link.discover()
        link.check_input_parameter()
        print('CLOSED BENCH ONLY: synthetic GPS and ground station at 31N, 121E, 100m.')
        print('Watch the RID page on your phone; Ctrl+C stops. Keep QGC closed.')
        stream(link, args.duration)
        return 0
    except KeyboardInterrupt:
        print('Stopped by operator')
        return 0
    except Exception as exc:
        print('ERROR: {}'.format(exc), file=sys.stderr)
        return 1
    finally:
        if connection is not None:
            connection.close()
        print('After testing: reopen QGC, restore the original MAV_USEHILGPS value, then power-cycle the whole board.')


if __name__ == '__main__':
    sys.exit(main())
