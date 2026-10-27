"""Select USB control-line states before opening a radio's serial port."""
from __future__ import annotations

import os
import re


def _device_path(value) -> str:
    text = str(value)
    # Windows callers may use either COM7 or the equivalent \\.\COM7.
    match = re.fullmatch(r"(?:\\\\\.\\)?COM(\d+)", text, re.I)
    if match:
        return "com" + str(int(match[1]))
    return os.path.realpath(text).casefold()


def serial_line_states(port, *, ports=None) -> tuple[bool, bool]:
    """Return (DTR, RTS), resolving aliases against the selected USB device.

    ESP32 Serial/JTAG 303A:1001 and UART bridges must leave DTR released
    across firmware reboots. Other Espressif CDC, Seeed and Adafruit/nRF52
    CDC devices need DTR asserted. Unknown ports use the released state.
    """
    if ports is None:
        try:
            from serial.tools import list_ports
            ports = list_ports.comports()
        except (ImportError, OSError):
            ports = ()
    selected = _device_path(port)
    for info in ports:
        device = getattr(info, "device", None)
        if device is None or _device_path(device) != selected:
            continue
        vid, pid = getattr(info, "vid", None), getattr(info, "pid", None)
        return vid in (0x303A, 0x2886, 0x239A) and (vid, pid) != (0x303A, 0x1001), False
    return False, False


def configure_serial_session(connection, port=None, *, ports=None) -> tuple[bool, bool]:
    """Configure a closed pySerial-compatible object without toggling a device."""
    if getattr(connection, "is_open", False):
        raise ValueError("USB control lines must be configured before opening the port")
    if port is not None:
        connection.port = port
    dtr, rts = serial_line_states(connection.port, ports=ports)
    connection.rts = rts
    connection.dtr = dtr
    return dtr, rts


def open_serial_session(connection, port=None, *, ports=None) -> None:
    """Open configured lines without a POSIX USB-UART bridge reset pulse."""
    configure_serial_session(connection, port, ports=ports)
    # POSIX pySerial normally applies DTR before RTS. Releasing DTR while
    # the driver still asserts RTS can pulse ESP32 EN. Defer DTR until RTS
    # has been released, matching tools/hil/serial_session.py.
    defer_dtr = os.name == "posix" and not connection.dtr
    previous_dsrdtr = getattr(connection, "dsrdtr", False)
    try:
        if defer_dtr:
            connection.dsrdtr = True
        connection.open()
        if defer_dtr:
            connection.rts = False
            connection.dtr = False
            connection.dsrdtr = previous_dsrdtr
    except BaseException:
        try:
            connection.close()
        finally:
            if defer_dtr:
                connection.dsrdtr = previous_dsrdtr
        raise
