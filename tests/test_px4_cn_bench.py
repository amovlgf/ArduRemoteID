"""Offline wire-format and interlock tests; no serial port or vehicle needed."""
import importlib.util
import io
from pathlib import Path
import struct
import time
from types import SimpleNamespace
import unittest

try:
    from pymavlink.dialects.v20 import common as mav
except ImportError:
    mav = None

spec = importlib.util.spec_from_file_location('bench', Path(__file__).parents[1] / 'scripts/px4_cn_bench.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


@unittest.skipIf(mav is None, 'Optional bench tool requires pymavlink')
class BenchTests(unittest.TestCase):
    def test_wire_messages_units_and_shared_time(self):
        unix_us = (bench.EPOCH_2019 + 243074697) * 1000000 + 123000
        gps, system = bench.sample_fields(unix_us)
        output = io.BytesIO()
        sender = mav.MAVLink(output, srcSystem=1, srcComponent=191)
        sender.hil_gps_send(**gps)
        sender.open_drone_id_system_send(target_system=1, target_component=1, **system)
        frames = mav.MAVLink(None).parse_buffer(output.getvalue())
        self.assertEqual([m.get_type() for m in frames], ['HIL_GPS', 'OPEN_DRONE_ID_SYSTEM'])
        self.assertTrue(all(m.get_srcSystem() == 1 and m.get_srcComponent() == 191 for m in frames))
        self.assertEqual(frames[0].lat * 1e-7, 31)
        self.assertEqual(frames[0].lon * 1e-7, 121)
        self.assertEqual(frames[0].alt * 1e-3, frames[1].operator_altitude_geo)
        self.assertEqual(frames[0].fix_type, 3)
        self.assertEqual(frames[0].time_usec // 1000000 - bench.EPOCH_2019, frames[1].timestamp)
        self.assertEqual(frames[1].operator_location_type, 2)

    def test_end_samples_are_unknown(self):
        gps, system = bench.sample_fields((bench.EPOCH_2019 + 100) * 1000000, valid=False)
        self.assertEqual((gps['fix_type'], gps['time_usec'], gps['satellites_visible']), (0, 0, 0))
        self.assertEqual((system['timestamp'], system['operator_latitude'], system['operator_longitude']), (0, 0, 0))
        self.assertEqual(system['operator_altitude_geo'], -1000)

    def test_px4_integer_byte_encoding(self):
        for value in (0, 1, -1):
            wire = struct.unpack('<f', struct.pack('<i', value))[0]
            self.assertEqual(bench.px4_int32(wire), value)

    def test_armed_vehicle_is_rejected(self):
        bench.require_disarmed(SimpleNamespace(base_mode=0), mav)
        with self.assertRaisesRegex(RuntimeError, 'armed'):
            bench.require_disarmed(SimpleNamespace(base_mode=mav.MAV_MODE_FLAG_SAFETY_ARMED), mav)

    def test_bad_clock_is_rejected(self):
        with self.assertRaises(ValueError):
            bench.sample_fields(0)

    def test_input_parameter_must_be_enabled(self):
        for value in (0, 1):
            with self.subTest(value=value):
                requests = []
                reply = SimpleNamespace(
                    param_id='MAV_USEHILGPS', param_type=mav.MAV_PARAM_TYPE_INT32,
                    param_value=struct.unpack('<f', struct.pack('<i', value))[0],
                    get_type=lambda: 'PARAM_VALUE', get_srcSystem=lambda: 1,
                    get_srcComponent=lambda: 1)
                connection = SimpleNamespace(
                    mav=SimpleNamespace(param_request_read_send=lambda *args: requests.append(args)),
                    recv_match=lambda **kwargs: reply)
                link = bench.Link(connection, mav)
                link.system, link.component = 1, 1
                link.last_heartbeat = time.monotonic()
                if value == 0:
                    with self.assertRaisesRegex(RuntimeError, 'enable it in QGC'):
                        link.check_input_parameter()
                else:
                    link.check_input_parameter()
                self.assertEqual(requests, [(1, 1, b'MAV_USEHILGPS', -1)])

    def test_lost_heartbeat_stops_input(self):
        connection = SimpleNamespace(recv_match=lambda **kwargs: None)
        link = bench.Link(connection, mav)
        link.last_heartbeat = time.monotonic() - 6
        with self.assertRaisesRegex(RuntimeError, 'heartbeat lost'):
            link.receive()

    def test_arming_during_stream_stops_input(self):
        heartbeat = SimpleNamespace(
            base_mode=mav.MAV_MODE_FLAG_SAFETY_ARMED,
            get_type=lambda: 'HEARTBEAT', get_srcSystem=lambda: 1,
            get_srcComponent=lambda: 1)
        link = bench.Link(SimpleNamespace(recv_match=lambda **kwargs: heartbeat), mav)
        link.system, link.component = 1, 1
        with self.assertRaisesRegex(RuntimeError, 'armed'):
            link.receive()
        self.assertTrue(link.armed)


if __name__ == '__main__':
    unittest.main()
