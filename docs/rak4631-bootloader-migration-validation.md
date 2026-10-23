# RAK4631 LoRa bootloader migration qualification

Date: 2026-10-23

## Result

One real RAK4631 successfully migrated from MeshEnvy `0.9.2-ev1` to the
published stable OTAFIX `2.4.12` without external storage. The temporary
MeshEnvy application remained running, and its identity and all four
preservation hashes were unchanged across the bootloader replacement.
Post-reboot encrypted LoRa admin queries independently confirmed the new
bootloader, unchanged application hash and original node name.

## Exact hardware and images

- Receiver: MercerWoodMesh Pi RAK4631, USB serial `9AB3B64C641BA927`,
  named `HIL-RAK4631`. Its fitted W25Q16 NOR was not used for staging or copying.
- LoRa image source: MeshTower SD, serial `9352162A72082314`.
- LoRa admin controller: MeshPocket, serial `93DA83ADF7B2092B`.
- The separately connected RAK Tag was not modified.
- Source bootloader: MeshEnvy commit
  `fe618a03f8fb39bc70a86572caecfc9bde2f7062`, built as `0.9.2-ev1`.
- Source application: MeshEnvy `envyos/main` commit
  `ea5368d6c46dbe5b551cdd33a9848286fb1dca98`, `RAK_4631_repeater`, v1.18.0.
  Its body hash was `56DAF6302F2BCCA9`.
- Replacement: public release `v0.11.0-OTAFIX2.4.12`, exact
  `wiscore_rak4631_auto_bootloader-v0.11.0-OTAFIX2.4.12_s140_6.1.1.hex`.
  Download SHA-256 matched the published release manifest:
  `48bc983c66ff5f0fd764f3e5f64576a8ddfb3b260ba0d562f3c8f4072ba99c1d`.
- Embedded bootloader region: 40,960 bytes at destination `0xF4000`.
  SHA-256: `ec860fb46697ac4b458f4b961dec69b428380be1ce31a226cff60298f3281545`.
  BLMF CRC: `FF374B1B`; stable packed version: `0x02040CFF`.

## What ran on hardware

1. Local USB/BLE provisioning established the original MeshEnvy baseline.
   This setup is separate from the migration test.
2. A signed format-2, codec-2 delta delivered the bridge application over
   LoRa at 916.1 MHz, 500 kHz, SF7, CR5. The package was 85,895 bytes,
   with 84 logical 1 KiB blocks. Download completed in about 361 seconds.
3. The receiver verified the trusted signature and applied the application
   delta using the unchanged old bootloader. Apply/reboot took about one
   minute. The bridge image was 556,232 bytes, body hash `EA74E7DCFE48102A`.
4. A temporary, explicitly granted MeshPocket admin entry enabled encrypted
   LoRa commands without changing the saved admin password. An incorrect
   image confirmation was refused before any copy was armed.
5. The correct `ota bootloader install EC860FB46697AC4B` command and its
   acknowledgment both traversed LoRa. The bridge checked the exact old
   bootloader and candidate hashes, memory layout and protection registers,
   then invoked Nordic MBR `COPY_BL` from its persistent internal-flash resource.
6. After reboot, the application reported `BL_0x02040CFF` and bootloader
   capabilities ABI 3 / codecs `0x5`. This packed string is how the older
   MeshEnvy version reader displays the stable 2.4.12 release.
7. Independent post-migration LoRa queries confirmed that version, the
   original application hash and `HIL-RAK4631` name.

USB carried monitoring, local provisioning and temporary test-access setup;
it did not carry the migration image or final bootloader-install command.
No BLE transfer was used for the migration itself.

## Preservation check

These are the first 8 bytes of SHA-256 over each entire region, read before
and after the MBR copy, before temporary test-access cleanup.

| Region | Before | After |
| --- | --- | --- |
| Application including EndF | `34507BC036002C24` | `34507BC036002C24` |
| Bootloader settings, 0xFF000 page | `C92D04B8D714B445` | `C92D04B8D714B445` |
| Filesystem, 0xED000..0xF4000 | `ED038A486519D646` | `ED038A486519D646` |
| MBR and SoftDevice, 0..0x26000 | `0972167BB84FD824` | `0972167BB84FD824` |

The MBR parameter page at `0xFE000` is intentionally used by the copy and is
not part of the unchanged-settings claim.

## Additional checks and limits

- Four host migration tests passed, including exact candidate validation,
  source geometry and all eight ACL slots. The real old bootloader decoder
  reconstructed the bridge byte-for-byte in its host simulator. Its existing
  coherent/stale-readback regression suite also passed.
- The final bridge disassembly retained all MBR command fields and the
  command pointer at SVC 0x18. A bridge-local `noipa`/memory-clobber wrapper
  avoids relying on the older framework's SVC optimization assumptions.
- Radio read replies occasionally needed a retry. The real bootloader-copy
  activation was submitted once, not blindly repeated after a timeout.
- The first host harness expected a 12-second application reboot and a
  human-readable bootloader version. Those assertions were too narrow:
  actual old-loader apply took about one minute, and the new version is
  exposed as `BL_0x02040CFF`. Independent measurements confirmed success;
  the harness now waits longer and accepts the exact packed stable version.
- This is one successful hardware qualification, not a power-failure or
  fleet reliability claim. The original MeshEnvy application-delta stage is
  destructive and not power-fail resumable. A failure there may require
  local recovery. Each deployed application still needs its exact delta base.
- The bridge is opt-in and RAK4631-only. It does not unlock protected regions
  or change the SoftDevice. Ordinary builds remain unchanged.
- The board is intentionally left running the MeshEnvy bridge application
  under OTAFIX 2.4.12; installing the user's final normal MeshCore application
  is a separate next step.
- Cleanup removed the temporary MeshPocket admin grant and lab signing key.
  The original target ACL entry, public key, name and normal radio remained.
  MeshPocket and MeshTower normal-radio settings were restored. The existing
  MeshPocket contact's routing/settings were restored; its firmware-managed
  contact modification timestamp advanced as expected.
