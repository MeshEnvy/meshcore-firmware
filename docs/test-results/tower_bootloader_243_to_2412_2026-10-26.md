<!-- meshcore-hosted-doc-link:start -->
<p class="meshcore-hosted-doc-link"><a href="https://mikecarper.github.io/MeshCore/test-results/tower_bootloader_243_to_2412_2026-10-26/">View this page on MeshCore Docs</a>.</p>
<!-- meshcore-hosted-doc-link:end -->

# MeshTower SD: OTAFIX 2.4.3 to 2.4.12

**PASS, 2026-10-26.** The MercerWoodMesh Pi's Heltec MeshTower V2 SD
downloaded the official signed 2.4.12 bootloader over LoRa and installed it
from microSD while running the exact older firmware from the reported node.
No intermediate application or bootloader upgrade was needed.

## Fixture and result

| Check | Result |
| --- | --- |
| Starting application | `4d5ccbdd`, v1.17.1.4, body hash `F5C095D649FFBE43` |
| Starting application lengths | 474,964-byte body; 475,020-byte image |
| Starting bootloader | OTAFIX 2.4.3, CRC `D48D45A9` |
| Bootloader identity | Board `239A0071`, target `1150F50E`, `TOWER_V2_OTA`, ABI 3, caps `09` |
| LoRa download | MID `57CBECA2`, 40/40 blocks, ready at 159 seconds |
| Staged image hash | `C9479891C5D0446D` |
| Installed bootloader | OTAFIX 2.4.12, CRC `59326292` |
| Apply result | `blup:C8`, no download, `staged:none` |
| Application after bootloader update | Original `F5C095D649FFBE43` body unchanged |

The source was a Heltec V4.3 Full Companion running `5aac9570`. Both radios
used temporary 909.5 MHz, BW500, SF7, CR5 at 0 dBm. Package bytes crossed
LoRa; target commands, including the single explicit install command, used
USB. Remote admin control over LoRa was not exercised in this run.

The test used
`update-heltec_mesh_tower_v2_sdcard_bootloader-v0.11.0-OTAFIX2.4.12.mota`
from the [official 2.4.12 bundle](https://github.com/mikecarper/Adafruit_nRF52_Bootloader_OTAFIX/releases/download/v0.11.0-OTAFIX2.4.12/OTAFIX-2.4.12-bootloader-mota.zip).
Bundle SHA256:
`a0c6f223684972d527c115c19e2d38cbc2d271084df7caf2a26a60b35bdc7f87`.
Package SHA256:
`8d9dd04bd63f1fc40cf582de7ef4ae29cf6d6decbdcff516af8444314d933895`.
The official Ed25519 signature was verified independently before transfer.
The seeder executable SHA256 was
`203e70aeb72330f9ce32c1d8c7f84502cf437e9f7272643bb3c8a24d56c1ef45`.

## Restoration and limits

The Tower's original v1.18.0.1 application `807343de` was restored through
application-only USB serial DFU. Its body hash returned to `E5324C388E987A8E`;
the installed bootloader remained 2.4.12, CRC `59326292`.
Name, public identity, saved radio, TX power, repeat, RXPS, radio2, radio2.cross,
temporary-radio state and the original three trusted signer entries matched
the baseline. The temporary official signer entry was removed afterward.
No settings erase was performed.

The source's radio, TX power and radio2 settings were restored, its folder
disconnected, serving count zero and temporary radio inactive. Both serial
ports were released. ModemManager was restored and the unrelated soak broker
remained active.

The controlled local rollback to 2.4.3 used a bootloader-only UF2. Linux
reported a final `sync` EIO after detach; the returned application independently
confirmed the exact 2.4.3 CRC and unchanged application hash. During subsequent
application restoration, one 1200-baud touch and one serial open returned USB
protocol error 71. The same device was reidentified before continuing; no
uncertain install command was replayed. The signed update's install reply was
also lost at reboot; version, CRC, unchanged application hash and `C8` resolved
the outcome. These are not clean USB transport results or power-cut tests.

Private evidence archive SHA256:
`97dc0b6851a7c478beb7f871a2e613b4186509af8e5019a8014ba235cd6946dd`.
