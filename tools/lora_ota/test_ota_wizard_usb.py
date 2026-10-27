"""USB enumeration policy and pre-open line ordering without hardware access."""
import sys
from types import ModuleType, SimpleNamespace
import unittest
from unittest import mock

import ota_wizard as wizard
import ota_wizard_usb as usb
from ota_wizard_repeater import RepeaterTransport
from test_ota_wizard_repeater import FakeSerial


def metadata(device="COM7", vid=0x303A, pid=0x1001):
    return SimpleNamespace(device=device, vid=vid, pid=pid)


class SerialFixture:
    def __init__(self, **kwargs):
        self.is_open = False
        self.events = []
        self.port = None
        self._rts = self._dtr = True
        self._dsrdtr = False
        self.open_error = None

    @property
    def dtr(self):
        return self._dtr

    @dtr.setter
    def dtr(self, value):
        self.events.append(("dtr", value, self.is_open))
        self._dtr = value

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, value):
        self.events.append(("rts", value, self.is_open))
        self._rts = value

    @property
    def dsrdtr(self):
        return self._dsrdtr

    @dsrdtr.setter
    def dsrdtr(self, value):
        self.events.append(("dsrdtr", value, self.is_open))
        self._dsrdtr = value

    def open(self):
        self.events.append(("open", self.dtr, self.rts, self.dsrdtr, self.port))
        self.is_open = True
        if self.open_error:
            raise self.open_error

    def close(self):
        self.events.append(("close",))
        self.is_open = False

    def write(self, data):
        self.events.append(("write", data))
        return len(data)

    def read(self, length):
        return b"  -> v1.18.0.1\r\n"


class UsbPolicyTests(unittest.TestCase):
    def test_vid_pid_matrix_dtr_with_rts_always_released(self):
        cases = ((0x303A, 0x1001, False), (0x303A, 0x4001, True),
                 (0x2886, 0x0044, True), (0x239A, 0x8029, True),
                 (0x10C4, 0xEA60, False), (0x1A86, 0x7523, False),
                 (0x0403, 0x6001, False), (None, None, False))
        for vid, pid, dtr in cases:
            with self.subTest(vid=vid, pid=pid):
                ports = [metadata(vid=vid, pid=pid)]
                self.assertEqual(usb.serial_line_states("COM7", ports=ports), (dtr, False))
        self.assertEqual(usb.serial_line_states("COM8", ports=[metadata(vid=0x239A)]),
                         (False, False))

    def test_windows_com_aliases_and_posix_by_id_resolve_same_device(self):
        self.assertEqual(usb.serial_line_states(r"\\.\COM7", ports=[metadata(vid=0x239A)]),
                         (True, False))
        self.assertEqual(usb.serial_line_states("com7", ports=[metadata(vid=0x2886)]),
                         (True, False))
        def resolved(value):
            return "/dev/ttyACM0" if value == "/dev/serial/by-id/radio" else value
        with mock.patch.object(usb.os.path, "realpath", side_effect=resolved):
            self.assertEqual(usb.serial_line_states("/dev/serial/by-id/radio",
                ports=[metadata("/dev/ttyACM0", vid=0x2886)]), (True, False))

    def test_lazy_enumeration_and_missing_metadata_default_to_released(self):
        serial, tools = ModuleType("serial"), ModuleType("serial.tools")
        listing = mock.Mock(return_value=[metadata(vid=0x239A)])
        tools.list_ports = SimpleNamespace(comports=listing)
        serial.tools = tools
        with mock.patch.dict(sys.modules, {"serial": serial, "serial.tools": tools}):
            self.assertEqual(usb.serial_line_states("COM7"), (True, False))
            listing.assert_called_once_with()
            listing.side_effect = OSError("USB device disappeared")
            self.assertEqual(usb.serial_line_states("COM7"), (False, False))

    def test_configuration_happens_before_open_and_rejects_open_object(self):
        port = SerialFixture()
        self.assertEqual(usb.configure_serial_session(port, "COM7", ports=[metadata()]),
                         (False, False))
        self.assertEqual(port.events, [("rts", False, False), ("dtr", False, False)])
        self.assertEqual(port.port, "COM7")
        port.is_open = True
        before = list(port.events)
        with self.assertRaisesRegex(ValueError, "before opening"):
            usb.configure_serial_session(port, "COM8", ports=[])
        self.assertEqual(port.events, before)
        self.assertEqual(port.port, "COM7")

    def test_posix_release_defers_dtr_until_after_rts_without_handshaking_left_on(self):
        port = SerialFixture()
        with mock.patch.object(usb, "os", SimpleNamespace(name="posix")), \
                mock.patch.object(usb, "serial_line_states", return_value=(False, False)):
            usb.open_serial_session(port, "/dev/ttyACM0")
        self.assertEqual(port.events, [
            ("rts", False, False), ("dtr", False, False), ("dsrdtr", True, False),
            ("open", False, False, True, "/dev/ttyACM0"),
            ("rts", False, True), ("dtr", False, True), ("dsrdtr", False, True)])
        self.assertFalse(port.dsrdtr)

    def test_native_cdc_and_windows_open_without_posix_bridge_sequence(self):
        for platform, dtr in (("posix", True), ("nt", True), ("nt", False)):
            port = SerialFixture()
            with self.subTest(platform=platform, dtr=dtr), \
                    mock.patch.object(usb, "os", SimpleNamespace(name=platform)), \
                    mock.patch.object(usb, "serial_line_states", return_value=(dtr, False)):
                usb.open_serial_session(port, "COM7")
            self.assertEqual(port.events, [("rts", False, False), ("dtr", dtr, False),
                                          ("open", dtr, False, False, "COM7")])

    def test_open_failure_closes_device_and_restores_handshaking(self):
        port = SerialFixture()
        port.open_error = OSError("open failed")
        with mock.patch.object(usb, "os", SimpleNamespace(name="posix")), \
                mock.patch.object(usb, "serial_line_states", return_value=(False, False)), \
                self.assertRaisesRegex(OSError, "open failed"):
            usb.open_serial_session(port, "/dev/ttyUSB0")
        self.assertFalse(port.is_open)
        self.assertFalse(port.dsrdtr)
        self.assertIn(("close",), port.events)

    def test_detect_mode_obeys_shared_policy_before_first_probe(self):
        serial = ModuleType("serial")
        connection = SerialFixture()
        serial.Serial = mock.Mock(return_value=connection)
        with mock.patch.dict(sys.modules, {"serial": serial}), \
                mock.patch.object(usb, "serial_line_states", return_value=(False, False)), \
                mock.patch.object(wizard.time, "sleep"), \
                mock.patch.object(wizard.time, "monotonic", side_effect=[0, 0, 4]):
            self.assertEqual(wizard.detect_mode("COM7"), "repeater")
        opened = next(event for event in connection.events if event[0] == "open")
        self.assertEqual(opened[1:3], (False, False))
        self.assertFalse(connection.is_open)


class RepeaterUsbPolicyTests(unittest.IsolatedAsyncioTestCase):
    async def test_repeater_opens_with_shared_policy_for_both_device_types(self):
        for dtr in (False, True):
            connection = FakeSerial()
            observed = []
            def open_port():
                observed.append((connection.dtr, connection.rts, connection.port))
                connection.is_open = True
            connection.open = open_port
            link = RepeaterTransport("COM7", open_settle=0,
                                     serial_factory=lambda **kwargs: connection)
            with self.subTest(dtr=dtr), \
                    mock.patch.object(usb, "serial_line_states", return_value=(dtr, False)):
                try:
                    await link.open()
                    self.assertEqual(observed, [(dtr, False, "COM7")])
                finally:
                    await link.close()


if __name__ == "__main__":
    unittest.main()
