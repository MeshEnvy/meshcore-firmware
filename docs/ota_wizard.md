<!-- meshcore-hosted-doc-link:start -->
<p class="meshcore-hosted-doc-link"><a href="https://mikecarper.github.io/MeshCore/ota_wizard/">View this page on MeshCore Docs</a>.</p>
<!-- meshcore-hosted-doc-link:end -->

# Guided LoRa updates

Run one Python command to update a directly reachable Repeater or Room Server.
The wizard identifies the board, chooses compatible firmware, prepares the
transfer, installs it, checks the result, and restores the original radio,
RX power-saving, and OTA settings. On supported nRF52 boards it also checks
for a newer compatible signed bootloader and includes that update in the plan.

## Connect a source radio

Use either connection:

| Computer connection | Source radio | Destination |
| --- | --- | --- |
| Raspberry Pi → USB | ESP32 Repeater with OTA and `ota.peer` support | Repeater or Room Server |
| Computer or Pi → USB | Full Companion with USB OTA source and direct admin support | Repeater or Room Server |

A **Station G2 Repeater connected to a Pi** can manage and serve an update to
an **Ikoka Stick Repeater** through that single USB connection. It does not
need a second local radio or the phone app. Install a source firmware build
containing `ota.peer` first; older Repeater firmware cannot manage the remote
node through USB. Check with `ota.peer status` in its USB console.

The Full Companion source also needs a current build containing the wizard's
direct admin guard. An older Full Companion may serve OTA files but cannot
provide this guarded admin connection; the wizard will ask for a source
firmware upgrade instead of proceeding with that connection.

The source and destination must already use the same main-channel settings
and hear each other directly. The destination needs working LoRa OTA support
and its admin password. A stock nRF52 bootloader may need a matching local
installation before it can accept remote updates.

The wizard sends discovery, admin commands, and OTA traffic directly with no
relay hops. An older destination may put its first login reply in a flood
envelope; the wizard accepts it only if it arrives with zero hops, establishes
a direct return route, then requires a fresh direct identity check.

Close other apps using the USB port before starting.

## Start

From the MeshCore checkout:

```bash
python3 tools/lora_ota/ota_wizard.py
```

On Windows, use `py` in place of `python3`. The first run installs USB and
verification dependencies in a private Python environment. On Raspberry Pi OS,
install `python3-venv` if Python reports that it is missing.

The wizard selects the USB radio and destination automatically when there is
only one of each. Otherwise, choose from the displayed list. Enter the
destination admin password once, choose firmware, then review the plan and
press Enter to start. Password entry is hidden and passwords are never saved.

Firmware choices are:

- **Published GitHub firmware:** the default, matched to the exact board and
  role. A running version that is already current is left in place.
- **Custom firmware file:** an application DFU ZIP, BIN, HEX, full mOTA, or a
  prepared update bundle.
- **Build custom Full firmware from source:** offered on a computer. This
  needs the firmware build tools and Bash; it is not offered by default on
  a Pi. The generated version is newer than the detected installed version.

The displayed plan includes package sizes, firmware version, and any
bootloader or trusted update signer being installed. After a reboot, the
wizard checks the running firmware hash and version, or the bootloader's
identity and version, before reporting success.

## Differential or full updates

The wizard looks for the **exact running firmware** in its cache and published
GitHub assets. It uses a differential package when it has a compatible builder,
can match that exact base, and the resulting package is smaller. A file with
the same version label but a different body hash is not a usable base.

On a computer, the differential builder can be prepared automatically with a
compatible Rust toolchain. On a Pi without a builder, the default is a full
update made by Python without compiling. This requires destination storage
that supports full updates, such as the Ikoka Stick's QSPI flash.

An internal-flash nRF52 destination that requires differential installation
cannot use that full fallback. Prepare its bundle on a computer instead:

1. On the Pi, inspect the connected radios:

   ```bash
   python3 tools/lora_ota/ota_wizard.py --check
   ```

   Copy the JSON file shown after **Preparation request:** to the computer.
   It contains public board information and the exact running firmware hash,
   without the admin password.

2. On the computer, prepare the update using the request, new firmware, and
   the exact firmware currently running on the destination:

   ```bash
   python3 tools/lora_ota/ota_wizard.py --prepare-only \
     --request DEVICE.json --firmware NEW_FIRMWARE.zip \
     --base RUNNING_FIRMWARE.zip --output update.ota.zip
   ```

   No radio is opened. If the compatible differential builder and all input
   files are already present, this also works without network access. Add
   `--motatool /path/to/motatool` to use an existing builder.

3. Copy `update.ota.zip` back to the Pi and run:

   ```bash
   python3 tools/lora_ota/ota_wizard.py --firmware update.ota.zip
   ```

   The live destination is checked again before installation. A changed base
   hash or a different board stops the differential update.

For an offline full bundle on a destination that supports it, add `--full`
to the preparation command. This path needs neither the differential builder
nor USB dependencies.

## Useful options

| Option | Purpose |
| --- | --- |
| `--check` | Authenticate and inspect OTA support without downloading or installing firmware |
| `--port /dev/ttyUSB0` | Choose a USB port; Windows example: `--port COM5` |
| `--mode repeater` or `--mode companion` | Select the source type if automatic detection is unsuitable |
| `--target PUBLIC_KEY` | Choose a destination by its full 64-character key; it must still answer direct discovery |
| `--firmware FILE` | Use a local firmware image or prepared bundle |
| `--base FILE` | Supply the exact running firmware for differential preparation |
| `--full` | Choose a full application transfer when the destination supports it |
| `--build` | Build custom Full firmware from this checkout |
| `--version 1.18.0.3-custom` | Set the custom build version; it must be newer than the running version |
| `--no-bootloader` | Update application firmware only |
| `--cache DIRECTORY` | Choose where packages, public requests, and recovery records are saved |
| `--yes` | Accept the displayed plan; required choices and credentials must still be supplied |

For unattended use, provide `MESHCORE_ADMIN_PASSWORD` in the process environment
and pass the port, source type, and target key. The wizard does not write that
password into its preferences, recovery records, or custom build environment.

## If the connection is interrupted

Keep the same source radio connected and run the wizard again. It checks the
saved source and destination public keys before restoring settings from an
unfinished update. To restore settings and exit:

```bash
python3 tools/lora_ota/ota_wizard.py --recover --target PUBLIC_KEY
```

Keep the recovery record until the wizard reports that restoration is
complete. A lost install reply is followed by a running-image check; the
wizard does not blindly repeat an install command. Temporary radio windows
use the existing main-channel settings, so an interrupted process does not
leave the destination on a separate update frequency.

The cache is normally `~/.cache/meshcore/ota` on Linux,
`~/Library/Caches/MeshCore/ota` on macOS, or `%LOCALAPPDATA%\MeshCore\ota` on
Windows. It contains firmware, public preparation records, selected board
keys, and restoration settings. Trusted signer keys added for an update remain
installed on the destination.

For the individual commands and a manual board-by-board example, see
[Station G2 to Ikoka Stick](ota_station_g2_to_ikoka_stick.md). For routed
updates and explicit multi-node setup, use the
[scripted LoRa OTA guide](lora_ota_automation.md).
