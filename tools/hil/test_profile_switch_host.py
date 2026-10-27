"""Regression tests for the serial HIL result reader."""
import unittest
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch
from types import SimpleNamespace
from profile_switch import read_response, exchange_ready, configure_session, exchange, open_session

if os.name == "posix":
    import serial.serialposix

    class PosixModemSerial(serial.serialposix.Serial):
        """Actual pySerial on a PTY, with only modem ioctls simulated.

        The CP2102 kernel-open state observed in HIL asserts both lines. The
        board's reset circuit is driven when DTR is cleared before RTS.
        """
        def __init__(self, *args, **kwargs):
            self.modem_lines = {"dtr": True, "rts": True}
            self.modem_edges = []
            self.flow_at_reconfigure = []
            super().__init__(*args, **kwargs)

        def _reconfigure_port(self, force_update=False):
            self.flow_at_reconfigure.append(self.dsrdtr)
            super()._reconfigure_port(force_update)

        def _set_modem_line(self, name, value):
            self.modem_lines[name] = value
            self.modem_edges.append((name, value, self.modem_lines.copy()))

        def _update_dtr_state(self):
            self._set_modem_line("dtr", self._dtr_state)

        def _update_rts_state(self):
            self._set_modem_line("rts", self._rts_state)

        def has_reset_transition(self):
            return any(not lines["dtr"] and lines["rts"]
                       for _, _, lines in self.modem_edges)


class Port:
    port = "test"

    def __init__(self, parts):
        self.parts = iter(parts)
        self.writes = []

    def write(self, data):
        self.writes.append(data)

    def readline(self):
        return next(self.parts)


class SessionPort:
    def __init__(self, device):
        self.port = device
        self.is_open = False
        self.dtr = True
        self.rts = True
        self.dsrdtr = False
        self.opened_control_lines = None
        self.close_count = 0

    def open(self):
        self.opened_control_lines = (self.dtr, self.rts)
        self.is_open = True

    def close(self):
        self.close_count += 1
        self.is_open = False


class FailingSessionPort(SessionPort):
    def __init__(self, phase):
        super().__init__("test")
        self.failure_phase = phase

    def __setattr__(self, name, value):
        if getattr(self, "is_open", False):
            phase = getattr(self, "failure_phase", None)
            if name == phase or (phase == "restore" and name == "dsrdtr" and not value):
                raise RuntimeError(phase + " failed")
        object.__setattr__(self, name, value)

    def open(self):
        if self.failure_phase == "open":
            raise RuntimeError("open failed")
        super().open()


class ResultReaderTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "posix", "Requires the POSIX pySerial backend")
    def test_actual_posix_open_avoids_reset_and_preserves_native_dtr(self):
        master, slave = os.openpty()
        try:
            device = os.ttyname(slave)
            for vid, pid, expected in ((0x10C4, 0xEA60, False), (0x1A86, 0x7523, False),
                                       (0x303A, 0x1001, False), (0x303A, 0x4001, True),
                                       (0x2886, 0x8045, True), (0x239A, 0x8029, True)):
                with self.subTest(vid=vid, pid=pid):
                    port = PosixModemSerial()
                    port.port = device
                    try:
                        with patch("profile_switch.list_ports.comports", return_value=[
                                SimpleNamespace(device=device, vid=vid, pid=pid)]):
                            open_session(port)
                        self.assertFalse(port.has_reset_transition(), port.modem_edges)
                        self.assertEqual(port.modem_lines, {"dtr": expected, "rts": False})
                        self.assertFalse(port.dsrdtr)
                        if expected:
                            self.assertNotIn(True, port.flow_at_reconfigure,
                                             "Native USB entered the bridge handoff")
                    finally:
                        port.close()
        finally:
            os.close(master)
            os.close(slave)

    @unittest.skipUnless(os.name == "posix", "Requires the POSIX pySerial backend")
    def test_actual_posix_old_open_order_is_a_reset_negative_control(self):
        master, slave = os.openpty()
        port = PosixModemSerial()
        port.port = os.ttyname(slave)
        try:
            with patch("profile_switch.list_ports.comports", return_value=[]):
                configure_session(port)
            port.open()  # Former caller sequence, using actual pySerial open.
            self.assertTrue(port.has_reset_transition(), port.modem_edges)
        finally:
            port.close()
            os.close(master)
            os.close(slave)

    def test_other_backend_does_not_use_dsr_handshaking(self):
        class OtherBackendPort(SessionPort):
            def __setattr__(self, name, value):
                if name == "dsrdtr" and value:
                    raise AssertionError("Other backend entered DSR handshaking")
                super().__setattr__(name, value)

        port = OtherBackendPort("test")
        with patch("profile_switch.os.name", "nt"), patch(
                "profile_switch.list_ports.comports", return_value=[]):
            open_session(port)
        self.assertTrue(port.is_open)
        self.assertEqual(port.opened_control_lines, (False, False))
        self.assertFalse(port.dsrdtr)

    def test_failed_open_or_line_release_closes_and_restores_flow(self):
        for phase in ("open", "rts", "dtr", "restore"):
            with self.subTest(phase=phase):
                port = FailingSessionPort(phase)
                with patch("profile_switch.os.name", "posix"), patch(
                        "profile_switch.list_ports.comports", return_value=[]):
                    with self.assertRaisesRegex(RuntimeError, phase + " failed"):
                        open_session(port)
                self.assertFalse(port.is_open)
                self.assertFalse(port.dsrdtr)
                self.assertEqual(port.close_count, 1)

    def test_timing_runs_get_correlated_cached_replies(self):
        port = Port([])
        with patch("profile_switch._run_sequences", iter([99])), patch(
                "profile_switch.read_response", side_effect=[TimeoutError("late"), {"result": 99}]):
            result = exchange(port, "run 1 7 16 1 768 8", "result", 5)
        self.assertEqual(port.writes, [b"run 1 7 16 1 768 8 99\n", b"result result 99\n"])
        self.assertTrue(result["transport_recovered"])

    def test_native_usb_and_bridge_have_distinct_control_lines(self):
        for vid, pid, expected in ((0x303A, 0x1001, False), (0x303A, 0x4001, True),
                                   (0x2886, 0x8045, True), (0x239A, 0x8029, True),
                                   (0x1A86, 0x7523, False), (0x10C4, 0xEA60, False), (None, None, False)):
            port = SessionPort("test")
            with patch("profile_switch.list_ports.comports", return_value=[
                    SimpleNamespace(device="TEST", vid=vid, pid=pid),
                    SimpleNamespace(device="different", vid=0x303A)]):
                configure_session(port)
            self.assertFalse(port.is_open)
            port.open()
            self.assertEqual(port.opened_control_lines, (expected, False))
            self.assertEqual(port.dtr, expected)
            self.assertFalse(port.rts)

    def test_by_id_symlinks_select_the_actual_native_or_cp2102_device(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            native = root / "ttyACM0"
            bridge = root / "ttyUSB0"
            native.touch()
            bridge.touch()
            by_id = root / "serial" / "by-id"
            by_id.mkdir(parents=True)
            native_id = by_id / "usb-Espressif_USB_JTAG-if00"
            bridge_id = by_id / "usb-Silicon_Labs_CP2102-if00-port0"
            native_id.symlink_to(native)
            bridge_id.symlink_to(bridge)
            inventory = [SimpleNamespace(device=str(native), vid=0x303A, pid=0x1001),
                         SimpleNamespace(device=str(bridge), vid=0x10C4)]
            for device, expected in ((native_id, False), (bridge_id, False)):
                with self.subTest(device=device.name):
                    port = SessionPort(str(device))
                    with patch("profile_switch.list_ports.comports", return_value=inventory):
                        configure_session(port)
                    self.assertFalse(port.is_open)
                    port.open()
                    self.assertEqual(port.opened_control_lines, (expected, False))

    def test_timeout_replays_cached_result_without_repeating_tx(self):
        port = Port([])
        with patch("profile_switch.read_response", side_effect=[
                TimeoutError("late"), {"sent": 12, "rc": 0}]) as reader:
            result = exchange(port, "tx 7 0 12 16 32 -9 8 1", "sent", 6, 12, cached=True)
        self.assertTrue(result["transport_recovered"])
        self.assertEqual(port.writes, [b"tx 7 0 12 16 32 -9 8 1\n", b"result sent 12\n"])
        self.assertEqual(reader.call_args_list[-1].args, (port, "sent", 3, 12))
        with patch("profile_switch.read_response", side_effect=[
                TimeoutError("late"), RuntimeError("result unavailable")]):
            with self.assertRaisesRegex(RuntimeError, "result unavailable"):
                exchange(port, "tx", "sent", 6, 13, cached=True)

    def test_partial_reads_and_idle_timeout_do_not_split_json(self):
        port = Port([b"startup\n", b'{"result":true,"directions":', b"", b"[]", b"}\n"])
        self.assertEqual(read_response(port, "result", 1), {"result": True, "directions": []})

    def test_ignores_other_complete_messages_but_never_hides_errors(self):
        port = Port([b'{"ready":true}\n', b'{"received":7}\n'])
        self.assertEqual(read_response(port, "received", 1), {"received": 7})
        port = Port([b'{"received":7}\n', b'{"received":8}\n'])
        self.assertEqual(read_response(port, "received", 1, expected=8),
                         {"received": 8, "stale_replies": [{"received": 7}]})
        with self.assertRaisesRegex(RuntimeError, "bad hop"):
            read_response(Port([b'{"error":"bad hop"}\n']), "result", 1)

    def test_only_initial_guard_rejections_are_retried_and_counted(self):
        with patch("profile_switch.exchange", side_effect=[RuntimeError("primary rejected"), {"result": True}]), patch("profile_switch.time.sleep") as sleep:
            self.assertEqual(exchange_ready(None, "run", "result", 1)["setup_deferrals"], 1)
            sleep.assert_called_once_with(0.1)
        with patch("profile_switch.exchange", side_effect=RuntimeError("receiver hop failed")), patch("profile_switch.time.sleep") as sleep:
            with self.assertRaisesRegex(RuntimeError, "receiver hop failed"):
                exchange_ready(None, "listen", "listening", 1)
            sleep.assert_not_called()
        with patch("profile_switch.exchange", side_effect=RuntimeError("primary rejected")) as command, patch("profile_switch.time.sleep"):
            with self.assertRaises(RuntimeError):
                exchange_ready(None, "run", "result", 1)
            self.assertEqual(command.call_count, 31)


if __name__ == "__main__":
    unittest.main()
