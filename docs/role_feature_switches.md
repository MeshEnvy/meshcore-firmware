<!-- meshcore-hosted-doc-link:start -->
<p class="meshcore-hosted-doc-link"><a href="https://mikecarper.github.io/MeshCore/role_feature_switches/">View this page on MeshCore Docs</a>.</p>
<!-- meshcore-hosted-doc-link:end -->

# Feature controls by role

Use these settings with the exact board's image from the
[current release](releases/1.18.0.1.md). Former
logging, power-saving, FEM-gain, and rotated-display variants are now runtime
choices where the hardware supports them. A setting cannot add missing radio
hardware, MQTT code, storage, or an OTA partition. Check the download's
`.capabilities.json` and the [firmware picker](firmware_picker.md).
For exact old device/variant names, search the
[1.17.1.5 variant map](old-releases/1.17.1.5-variant-map.tsv): it maps all 1,361
previous release entries to 1,325 covered choices or 36 excluded entries.
Many old choices share one current image; the command tables below explain
how to select their former behavior. Excluded entries have no download.

**Search terms** are alternative wording for finding these instructions, not
CLI aliases. Use the commands in the role-specific tables and examples.

## Open the USB web console

**Search terms:** USB terminal, serial console, ASCII terminal, browser terminal, web serial.

Open the [MeshCore USB web console](https://flasher.meshcore.io/console) in
Chrome or Edge, connect a data-capable USB cable, close other applications
using that port, and select the device. Use **115200 baud** when prompted.
Full Companion, Repeater, Room Server, and Sensor start with an **ASCII USB
terminal**. Run `board`, then `ver` on any role.

If a Companion app has already switched USB to its binary protocol, send:

```text
+++MESHCORE-TERM-START
```

To hand the port back to a Companion app or USB MOTA host, send
`+++MESHCORE-TERM-STOP`, close the console, and connect the app/tool. Dedicated
USB/BLE/WiFi Companion images can start in binary mode; use the start token
there too. KISS firmware uses its modem protocol and is outside this release.

The linked console runs in your computer's browser over **USB**. It does not
need node WiFi or `set webui on`. **WebConfig** is a separate settings website
served by supported ESP32 images. Both infrastructure and WiFi Companion
WebConfig include a browser command terminal (`set wifi.cli on`), enabled by
default on the LAN. The Companion browser CLI uses the same terminal commands
as USB, including contact import, chat, recipient selection, and streaming
replies. Full ESP32 Companion also exposes this terminal at TCP port **5002**.
See the [terminal command guide](terminal_chat_cli.md#companion-wifi-browser-terminal).

## Which old variant setting should I use?

**Search terms:** old firmware variants, restore features, turn features on or off, enable MQTT, disable MQTT.

These controls apply to every board with the corresponding compiled feature;
check the hardware requirements below.

| Former choice | Current image / runtime control |
| --- | --- |
| USB logging or Station G2/G3 logging variant | Same role's canonical image; `set usb.logging on` / `off` |
| USB logging, WiFi MQTT, or both | MQTT-capable image with USB logging; `set logging.output usb`, `wifi`, `both`, or `off` |
| Companion WiFi-MQTT variant | `set mqtt.enabled on` / `off`; configure broker slots through the CLI or WebConfig |
| `_ps` power-saving variant | `set powersaving on` / `off`; check `get powersaving` on every role |
| `_femoff` / FEM-gain variant | `set radio.fem.rxgain on` / `off`; separate TX switch where controllable |
| RX boost variant | `set radio.rxgain on` / `off` on a supported radio |
| iKOKA rotated-display variant | Full Companion: `set display.rotation 180`; `0` restores the board default |
| Separate USB, BLE, WiFi, Ethernet, or Terminal Chat Companion | Exact board's Full Companion where listed; supported transports are included, with SenseCAP selection below |
| RS232 repeater variant consolidated into the ordinary repeater | `set bridge.enabled on` / `off` on a build containing the RS232 bridge |
| ESP-NOW bridge or primary ESP-NOW radio | Select the matching hardware/role image first; bridge and primary-radio channel commands differ |
| LoRa OTA / external-storage variant | Still choose the exact receiver/storage image and matching bootloader; this is not a software on/off switch |

Companion, Repeater, Room Server, and Sensor use the same command names for
shared settings. Use `get <setting>` to check and `set <setting> <value>` to
change them. A command requires its feature to be compiled into the build.
`set usb.logging on|off` changes only USB logging. On MQTT-capable builds,
`set mqtt.enabled on|off` changes only MQTT and keeps broker settings;
`set logging.output off|usb|wifi|both` selects both outputs together.
`set usb.debug on|off` separately saves verbose USB diagnostics. Debug output
requires both USB switches on; RAW/RX/TX packet records need only
`usb.logging`. Leave `usb.debug off` for USB-to-MQTT packet capture.

Current source also includes a saved [USB logging watchdog](usb_logging_watchdog.md):
`get usb.watchdog` and `set usb.watchdog off|on|auto`. Missing state defaults to
Auto, qualifying from 14 continuous healthy USB stats-polling days, not a cable
or open TTY alone. Confirmed client loss can trigger USB-only repair; MCU resets
require On and an entire physical-fault tier, growing from one hour to one week.
This repairs the node's USB connection, not the host Pi or MQTT service.

## Full Companion commands

| Setting | Enable / select | Disable / restore | Read back |
| --- | --- | --- | --- |
| Device power saving | `set powersaving on` | `set powersaving off` | `get powersaving` |
| LoRa RX power saving | `set radio.rxps on` | `set radio.rxps off` | `get radio.rxps` |
| Radio RX boost | `set radio.rxgain on` | `set radio.rxgain off` | `get radio.rxgain` |
| External FEM RX gain | `set radio.fem.rxgain on` | `set radio.fem.rxgain off` | `get radio.fem.rxgain` |
| External FEM TX gain | `set radio.fem.txgain on` | `set radio.fem.txgain off` | `get radio.fem.txgain` |
| ESP32 USB logging | `set powersaving off`, then `set usb.logging on` | `set usb.logging off` | `get powersaving`, `get usb.logging` |
| nRF52 second USB logging port | `set usb.logging on reboot` | `set usb.logging off reboot` | `get usb.logging` |
| Verbose USB diagnostics | `set usb.debug on` while USB logging is on | `set usb.debug off` | `get usb.debug` reports saved intent |
| USB logging watchdog (current source) | `set usb.watchdog auto` or `on` | `set usb.watchdog off` | `get usb.watchdog`; native USB and durable storage required |
| MQTT master | `set mqtt.enabled on` | `set mqtt.enabled off` | `get mqtt.enabled`, `get mqtt.running`, `get mqtt.status` |
| ESP32 persistent WebConfig | `set webui on` | `set webui off` | `get webui` |
| ESP32 WebConfig browser console | `set wifi.cli on` | `set wifi.cli off` | `get wifi.cli` |
| ESP32 temporary setup AP | `start webconfig ap` | `stop webconfig` | `get webui` |
| Display rotation | `set display.rotation 90`, `180`, or `270` | `set display.rotation 0` | `get display.rotation` |
| Temporary MOTA radio window | `tempradio 910.525,250,5,5,120` | `normalradio` | `tempradio` |

Saved settings apply immediately unless noted. Rotation and gain controls
report unsupported hardware instead of creating that feature. Fresh Full
Companion preferences enable device power saving and leave USB logging off;
existing saved preferences win after an update.
USB debug defaults off, including on upgrade from preferences without that
field. Its saved intent survives switching the USB master off. Verbose messages
still require compiled debug support; changing this preference adds no missing
code and does not require a reboot.

**ESP32:** logging and binary Companion traffic share one USB port. Turn logging
off before handing USB to an app/MOTA host. **nRF52:** the optional second CDC
port is for logs; use the primary port for Companion/MOTA. Adding/removing the
second port requires a reboot; the optional suffix shown above requests it.
Full Companion includes complete ASCII RAW packet records and decoded RX/TX
summaries, independent of verbose debug. The nRF52 log port is output-only;
CLI/identity queries belong to interface `00`. A USB-to-MQTT program that expects
both logs and CLI replies on one port cannot use interface `02` alone.

### MQTT controls shared by Companion and infrastructure

**Search terms:** Companion MQTT settings, MQTT on, MQTT off, broker configuration.

Configure a broker through the MQTT tab or with the same commands on either
role. For example:

```text
set mqtt.iata SEA
set mqtt1.preset custom
set mqtt1.server broker.example.com
set mqtt1.port 1883
set mqtt.enabled on
get mqtt.enabled
get mqtt.running
get mqtt.status
```

`set mqtt.enabled off` disconnects brokers while preserving their settings.
`set mqtt.enabled on` allows configured brokers to reconnect. An enabled switch
is separate from a running connection: check `get mqtt.running` and
`get mqtt.status`. The MQTT tab's **Enable MQTT** checkbox controls this same
saved switch. Slot credentials and presets are available through `get/set
mqtt1.*`, `mqtt2.*`, and the remaining supported slots; secrets stay masked in
the browser. WiFi Companion accepts these commands through its USB terminal
and browser CLI; Full Companion also accepts TCP port 5002.

`set mqtt1.preset none` disables just that slot. Publication controls such as
`set mqtt.status off` do not disconnect brokers. USB logging is independent.
Images without MQTT code do not show MQTT cards or accept MQTT controls.

### Companion WiFi, Bluetooth, GPS, and board exceptions

Configure ESP32 WiFi with:

```text
set wifi.ssid MyNetwork
set wifi.pwd my-password
get wifi.status
```

`set wifi.powersave min` enables WiFi modem sleep; `none` disables it. `max`
is accepted only where the BLE/WiFi coexistence policy allows it. Read back
with `get wifi.powersave`. This is separate from device power saving and RXPS.
The assigned WiFi button/display switch controls WiFi services on supported
boards. `stop webconfig` closes only the portal; it is not a WiFi master switch.
On Bluetooth-capable ESP32 and nRF52 Companions, `get bluetooth` reports the
running state and `set bluetooth on|off` saves the service preference.
Plain `off` requires another active management connection;
`set bluetooth off force` explicitly permits disconnecting the only client.
See [Bluetooth controls](cli_commands.md#turn-bluetooth-on-or-off-companion)
for reply draining and boot-transport restrictions. Ethernet controls remain
specific to the board's recipe; there is no universal Ethernet-off command.

**T1000-E Companion:** USB data takes priority over Bluetooth. Once a computer
enumerates USB, the firmware stops Bluetooth advertising and disconnects any
Bluetooth client, even if no serial application is open. Bluetooth resumes
500 ms after USB disconnects, unless it was switched off manually. Battery
power and a charge-only wall supply leave Bluetooth available. The saved
Bluetooth setting, pairing PIN and bonds are preserved. `get bluetooth`
reports the USB pause; `set bluetooth on` saves the choice for after unplugging.

**SenseCAP Indicator Full only:** `set companion.transport wifi` followed by
`reboot` selects WiFi; `set companion.transport ble` followed by `reboot`
selects Bluetooth. Check `get companion.transport`. USB remains available.
Primary ESP-NOW Indicator images keep their mesh radio in either selection.

**GPS-equipped builds:** use `get gps`, `set gps on`, and `set gps off` on
Companion and infrastructure. The Companion app's `gps=1` / `gps=0` custom
setting controls the same GPS. See [GPS tracking](gps_tracking.md) for
location-sharing settings.

## Repeater, Room Server, and Sensor commands

These roles use CommonCLI. Feature-dependent commands require the relevant
hardware/build; MQTT is present in observer builds, and WebConfig is absent
from some portable builds. The USB browser console still works without it.

| Setting | Enable | Disable | Read back |
| --- | --- | --- | --- |
| Live USB logging | ESP32 1.17.1.5: `set powersaving off`, then `set usb.logging on`; other platforms: `set usb.logging on` | `set usb.logging off` | `get powersaving`, `get usb.logging` |
| Verbose USB diagnostics | `set usb.debug on` while USB logging is on | `set usb.debug off` | `get usb.debug` reports saved intent |
| Capture RX log to node storage | `log start` | `log stop` | `log` prints the capture locally |
| RS232 / ESP-NOW bridge master | `set bridge.enabled on` | `set bridge.enabled off` | `get bridge.enabled`, `get bridge.running`, `get bridge.type` |
| Independent RS232 in combined Full MQTT images | `set rs232.enabled on` | `set rs232.enabled off` | `get rs232.enabled`, `get rs232.running` |
| Independent ESP-NOW in combined Full images | `set espnow.enabled on` | `set espnow.enabled off` | `get espnow.enabled`, `get espnow.running` |
| MQTT periodic status publication | `set mqtt.status on` | `set mqtt.status off` | `get mqtt.status` shows connection status |
| MQTT packet publication | `set mqtt.packets on` | `set mqtt.packets off` | `get mqtt.packets` |
| SNMP on supported MQTT infrastructure | `set snmp on`, then `reboot` | `set snmp off`, then `reboot` | `get snmp` |
| MQTT raw packet publication | `set mqtt.raw on` | `set mqtt.raw off` | `get mqtt.raw` |
| MQTT receive capture | `set mqtt.rx on` | `set mqtt.rx off` | `get mqtt.rx` |
| MQTT transmit capture | `set mqtt.tx on` (or `advert`) | `set mqtt.tx off` | `get mqtt.tx` |
| MQTT master | `set mqtt.enabled on` | `set mqtt.enabled off` | `get mqtt.enabled`, `get mqtt.running`, `get mqtt.status` |
| ESP32 persistent WebConfig | `set webui on` | `set webui off` | `get webui` |
| WebConfig browser command terminal | `set wifi.cli on` | `set wifi.cli off` | `get wifi.cli` |
| LoRa RX power saving | `set radio.rxps on` | `set radio.rxps off` | `get radio.rxps` |
| Radio RX boost | `set radio.rxgain on` | `set radio.rxgain off` | `get radio.rxgain` |
| Controllable FEM RX / TX gain | `set radio.fem.rxgain on` / `set radio.fem.txgain on` | `set radio.fem.rxgain off` / `set radio.fem.txgain off` | Corresponding `get radio.fem.rxgain` / `get radio.fem.txgain` |
| GPS, when compiled | `set gps on` | `set gps off` | `get gps` |

Use `set usb.logging on|off` on every role. Adding the optional `reboot` suffix
requests a reboot only when changing the USB interfaces requires it, as on
nRF52 Full Companion. `log start/stop` records to storage independently of live
USB logging. Use
`log erase` to delete that capture.

Fresh ESP32 Full infrastructure installs default to USB logging off. Upgrades
retain the saved choice. With device power saving enabled, an idle node can
sleep after the setup AP closes and all radio services stop; live USB logging,
an attached native USB host, and active MQTT/ESP-NOW still hold it awake.

For quiet packet capture in current firmware, use `set usb.debug off` and
`set usb.logging on`. Verbose diagnostics default off for fresh and upgraded
preferences without the new field. The debug choice is saved separately;
turning logging off, or selecting `logging.output wifi`, does not erase it.
Debug output is effective only when USB logging is on and the image includes
the relevant debug code. RAW/RX/TX records remain enabled without debug.

For **ESP32 1.17.1.5 USB logging**, run these as separate commands in the
role's text terminal (or remote admin CLI on infrastructure):

```text
set powersaving off
set usb.logging on
get powersaving
get usb.logging
```

Check for power saving `off` and USB logging `on`. This avoids the released
ESP32 USB sleep bug, including the G3 report. Both settings are saved; turning
logging off later does not automatically restore power saving. LoRa RXPS is
independent. nRF52 logging does not need this ESP32 workaround.

With the [G3 sleep correction](old-releases/1.17.1.5.md#g3-usb-disappearance-with-power-saving-enabled),
enabled live USB logging keeps ESP32 USB serviced and blocks light sleep,
including when a host closes the port or disconnects. CPU idle/yield remains
available. `set usb.logging off` removes that blocker; an attached native USB
host still prevents sleep. On Full infrastructure, `set logging.output usb`
and `both` enable the same USB blocker; `wifi` and `off` remove it. File capture
with `log start` is independent. The original 1.17.1.5 binaries require the
`set powersaving off` workaround described in the release note.

### Shared MQTT and logging output

**Search terms:** MQTT settings, MQTT on, MQTT off, logging output, USB and WiFi logging.

On any Companion or infrastructure build with both MQTT and USB logging compiled:

| Command | USB logs | MQTT bridge |
| --- | --- | --- |
| `set logging.output off` | Off | Off |
| `set powersaving off`, then `set logging.output usb` | On | Off |
| `set logging.output wifi` | Off | On |
| `set powersaving off`, then `set logging.output both` | On | On |

`get logging.output` reports the selection. Fresh unified Full infrastructure
preferences select `both`; Full Companion starts with USB logging off. Saved
settings override these defaults. To toggle only MQTT while keeping
USB logging unchanged, use `set mqtt.enabled off` / `on`. Neither setting
turns LoRa repeating off. Repeater forwarding uses `set repeat off` / `on`
and `get repeat` separately.
Repeaters can opt in to routed trace diagnostics with `set repeat.trace on`
while `repeat` stays off; the saved exception defaults off and does not relay
other payload types. See [repeat-off traces](cli_commands.md#allow-routed-traces-while-repeating-is-disabled-repeater-only).
The selector changes USB master output, not saved `usb.debug` intent. Direct
WiFi MQTT is independent of that USB verbosity preference.

The `set powersaving off` step above is the **1.17.1.5 ESP32 USB workaround**.
**WiFi/MQTT-only logging does not need it while the Repeater/Room Server MQTT
bridge is running:** that sleep guard already exists in the released firmware.
Use `get mqtt.running` to check that MQTT is running; an enabled
preference alone is not the running state. `set logging.output wifi` keeps
USB logging off and uses that MQTT guard.

For a custom broker on an MQTT-capable Companion, Repeater, or Room Server:

```text
set wifi.ssid MyNetwork
set wifi.pwd my-password
set mqtt.iata SEA
set mqtt1.preset custom
set mqtt1.server broker.example.com
set mqtt1.port 1883
set mqtt.enabled on
get mqtt.status
```

Set `mqtt1.username` / `mqtt1.password` if required by that broker. Configure only
as many slots as the board supports. `set mqtt1.preset none` disables slot 1;
other configured slots remain enabled, within the build's supported slot count.
Disabled and partially configured slots remain saved across reboot. For a custom
server URL, `set mqtt1.port 0` clears the port override so the URI supplies it.
`set mqtt.status off` disables status
messages, not MQTT itself. See the [MQTT reference](https://github.com/mikecarper/MeshCore/blob/keymindCascade/MQTT_IMPLEMENTATION.md)
for presets, TLS, credentials, and slot limits.

### Infrastructure power saving and bridges

Use `get powersaving`, `set powersaving on`, and `set powersaving off` on every
role. The preference is saved. Hardware and active USB/network services decide
when sleeping is possible; the command names do not change between roles.
Device power saving is separate from WiFi modem sleep and LoRa RXPS.

On RS232-capable repeater images, stop the bridge before changing its serial
port or baud rate, then restart it:

```text
set bridge.enabled off
set bridge.baud 115200
set bridge.enabled on
```

**RAK4631:** `set bridge.uart 2` selects UART2 while the bridge is stopped.
Canonical GPS-enabled builds reserve UART1 for GPS; UART1 requires a dedicated
GPS-free image. **ESP-NOW bridge:** use `set bridge.channel <1..13>` and
`set bridge.format wrapped` / `raw` where supported. **Primary ESP-NOW mesh:**
use `set espnow.channel <1..13>` and reboot; this is a different radio setting.

For infrastructure WebConfig on the LAN, use `start webconfig` /
`stop webconfig`. To force a setup AP on an observer, first run
`set mqtt.enabled off`, then `start webconfig ap`. When finished, run
`stop webconfig` and restore `set mqtt.enabled on` if you did not reboot.

## Updating and sending MOTA

**Search terms:** mOTA, LoRa OTA, update over LoRa, wireless firmware transfer.

All released Full Companions can serve MOTA to other nodes. Close the console
and run `motatool serve --serial /dev/ttyACM0 --dir ./motas -v` on the USB host.
See [Full Companion instructions](full_companion_features.md) for WiFi/BLE
source commands and the bounded temporary radio setup.

| Role / hardware | Start self-update | Stop / requirement |
| --- | --- | --- |
| ESP32 Repeater, Room Server, Sensor with WiFi updater | `start ota` or `start ota ap`; WiFi starts as needed and WebConfig stops automatically; open the returned URL (normally port 80, `/update`) | `stop ota` |
| ESP32 Full Companion with two application slots | `start ota` or `start ota ap`; returned URL uses port 8080, `/update` | `stop ota`; single-slot Full builds use USB |
| nRF52 infrastructure with Bluetooth DFU | `start ota` enters the Bluetooth update flow | Matching application DFU ZIP and board bootloader required |
| Qualified LoRa OTA receiver | Follow [LoRa OTA directions](ota_easy.md) | Exact destination package, storage profile, and overlapping temporary radio windows |

For nRF52 OTAFIX installations use the exact board/storage build from
[OTAFIX 2.4.12](https://github.com/mikecarper/Adafruit_nRF52_Bootloader_OTAFIX/releases/tag/v0.11.0-OTAFIX2.4.12).
For **1.18.0.1**, follow the [update guide](ota_user_guide.md) and
[bootloader instructions](ota_nrf52_bootloader_update.md). Its retained-RAM
handoff is required by internal-flash hybrid receiver images. Full Companions
are sources; their own updates use supported USB, Bluetooth DFU or WiFi methods.

The [complete infrastructure CLI](cli_commands.md), [role/build matrix](cli_build_matrix.md),
and [Full Companion feature guide](full_companion_features.md) provide details.
