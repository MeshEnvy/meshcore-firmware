# Opt-in RAK4631 bootloader migration over LoRa

This is a temporary application bridge, not a bootloader `.mota` format change.
It embeds a replacement bootloader in the application's internal flash, receives
that application through the existing signed LoRa delta path, then invokes Nordic
MBR `COPY_BL` after a separate authenticated admin command. No external storage
or reserved RAM arena is used. Ordinary builds do not enable migration.

The qualified transition is MeshEnvy `0.9.2-ev1` to the normal RAK4631 OTAFIX
`2.4.12` image, retaining the application, SoftDevice, filesystem and settings.
See the [hardware qualification](rak4631-bootloader-migration-validation.md) for
exact revisions, hashes and limitations. This is not a general fleet recovery
mechanism: the old bootloader's first-stage application-delta apply is destructive,
and power-loss recovery has not been qualified. Arrange local recovery access.

## Build the bridge

Requirements: the repository's normal PlatformIO toolchain, Python `intelhex`,
the exact installed application image (including EndF) for delta generation, and
the exact installed bootloader image. A version string alone is not enough:
different builds can have different bytes. The qualified legacy source was
MeshEnvy/Adafruit_nRF52_Bootloader_OTAFIX commit
`fe618a03f8fb39bc70a86572caecfc9bde2f7062`; a rebuild is acceptable only if its
entire padded bootloader region matches the installed device.
The lab legacy region SHA-256 was
`64bfe69d56eefc2d0fe51770902aff70ea0cc06cad261d3d219e4a3c0efac8dc`.

Obtain the normal `wiscore_rak4631_auto` HEX from
[OTAFIX 2.4.12](https://github.com/mikecarper/Adafruit_nRF52_Bootloader_OTAFIX/releases/tag/v0.11.0-OTAFIX2.4.12)
and verify its SHA-256 against the release manifest before building. Do not use
a recovery image or another board profile. Both inputs accept Intel HEX (only
`0xF4000..0xFE000` is extracted, with erased bytes padded to FF) or an exactly
40,960-byte raw bootloader region. SoftDevice/UICR data in a HEX is never embedded.

From this repository's root, using absolute paths to the two inputs:

```sh
python -m pip install intelhex
MC_MIGRATION_LEGACY_BOOTLOADER=/absolute/path/legacy.hex \
MC_MIGRATION_CANDIDATE_BOOTLOADER=/absolute/path/candidate.hex \
  pio run -e RAK_4631_repeater
```

Both variables are mandatory together. Other nRF52 environments are refused. The hook
generates aligned `const` data under the ignored `.pio/build/` directory, embeds
the full candidate plus both SHA-256 digests, and enables the guarded commands.
Do not set these variables in normal release jobs or publish bridge images as
ordinary fleet updates. Unset both variables for subsequent normal builds.

## Deliver and activate

1. Generate a signed format-2, codec-2 in-place application delta from the exact
   deployed application to the bridge's EndF-stamped `firmware.hex`, using the
   existing `.mota` tooling. Retain the normal `RAK_4631_repeater` target identity.
   Verify it with the old loader's host decoder before sending it remotely; not
   every codec accepted by a newer bootloader is accepted by `0.9.2-ev1`.
2. Deliver and install the bridge using the existing
   [LoRa OTA workflow](ota_user_guide.md). Confirm the bridge application hash
   after reboot. No USB or BLE transfer is required for this step.
3. Through the target's existing authenticated admin CLI, run:

   ```text
   ota bootloader status
   ota bootloader audit
   ```

   Save the audit's application/settings/filesystem/SoftDevice hashes. Finish or
   cancel any other OTA session, and disable automatic fetching/installing for
   the maintenance window if configured. Record the old policy for restoration.
4. Run `ota bootloader install <image-hash>`, copying the uppercase 16-digit
   confirmation from status. It is the candidate SHA-256 prefix, not its CRC or
   the downloaded HEX-file hash. The qualified image uses `EC860FB46697AC4B`.
   A wrong confirmation is refused. The reply means **armed**, not completed:
   migration waits at least 1.5 seconds for queued and in-flight TX to finish.
   It cancels if another OTA operation starts or TX does not drain within 30 s.
5. After reboot, query `get bootloader.ver`, `ota fw` and `ota bootloader audit`.
   The older application reports stable 2.4.12 as `BL_0x02040CFF`. Compare all four
   audit hashes with the pre-copy values. If a reply is lost, query status/version
   before deciding what to do; do not assume an acknowledgment proves completion.

Before copying, the bridge requires the expected nRF52840 geometry, S140 6.1.1,
bootloader at `0xF4000`, MBR parameter page at `0xFE000`, a source wholly inside
the verified application body below `0xED000`, and exact full candidate/current
bootloader SHA-256 matches. It refuses locked read/write regions instead of
unlocking them. The watchdog must be stopped or allow at least five seconds.
The MBR parameter page is intentionally used; settings at `0xFF000` are not erased.
An already-installed candidate is refused rather than copied again.

`status`'s `last` value is diagnostic: 1 application lookup, 2 SoftDevice stop,
4 chip, 5 SoftDevice layout, 6 boot address, 7 parameter address, 8 source,
9 protection, 10 candidate hash, 11 already installed, 12 legacy hash,
13 watchdog, 14 unexpected MBR return, 15 TX timeout, 16 concurrent OTA.
The reset-persisted error marker uses GPREGRET; it is not a durable update log.

The bridge remains the running application after migration. Install the final
normal application separately through the new bootloader's supported OTA path,
then restore maintenance policies and remove any temporary admin/signer grants.
Do not change the SoftDevice or filesystem layout as part of this procedure.

## Host checks

With a host C++ compiler available (`CXX` may override `g++`):

```sh
python tools/mota/test_bootloader_migration.py
python tools/mota/test_bootloader_migration_runtime.py
```
