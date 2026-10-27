<!-- meshcore-hosted-doc-link:start -->
<p class="meshcore-hosted-doc-link"><a href="https://mikecarper.github.io/MeshCore/test-results/esp32_psram_storage_2026-10-27/">View this page on MeshCore Docs</a>.</p>
<!-- meshcore-hosted-doc-link:end -->

# ESP32 PSRAM offload, 2026-10-27

V4, G2 Full and T-Deck Full each free **36,792 bytes of internal RAM** by moving
their scope tables and advert history to PSRAM. MQTT buffers can save another
**2,050 bytes per initialized client**. Features, capacities and saved formats
remain unchanged. No board was flashed during these checks.

The [measured evidence](esp32_psram_storage_2026-10-27.safe.json) binds source
hashes, matched builds, capabilities and the two existing packaging failures.
The paired measurements isolate these PSRAM changes on baseline `74c03cb30`.
Concurrent Companion PIN and startup-screen changes were integrated afterward;
a further G2 Full build at `6a5a04b1b` passes with the same RAM result and
15/15 capability checks. It fits its OTA slots at 2,125,736 application bytes.

## Enabled by default

ESP32-S3 repeater builds with `BOARD_HAS_PSRAM` move complete scope tables and
advert history to PSRAM. Existing explicit RTC/IRAM advert backends retain
priority. This covers these configured board families and their repeater aliases:

| Board families | PSRAM interface |
| --- | --- |
| Station G2/G3, T-Deck, T-ETH Elite, T-Beam 1W | OPI |
| V4 R8, RC32, E213, E290, T190 | OPI |
| W12, Wio Tracker L2, XIAO ESP32-S3/WIO, ThinkNode M7/M9 | OPI |
| Original SenseCAP Indicator | OPI |
| Standard V4, T3-S3 SX1262/SX1276, EoRa-S3 | QSPI |
| T-Beam S3 Supreme, Waveshare S3 LR1121, Indicator N16R2 | QSPI |

The IDF 4 WebSocket wrapper also prefers PSRAM on all PSRAM-enabled ESP32
builds that initialize MQTT clients, including companions, room servers and
classic T-Beam. It retains both 1,025-byte padded buffers per initialized
client, including plain MQTT clients. IDF 5 remains a passthrough.

Classic T-Beam keeps its established table placement. Its qualified Full
MQTT profiles already store all 17,920 advert-history bytes in separate
word-accessible IRAM; only 3,162 scope-table bytes remain. This extension
retains that backend and its cache-workaround flags. Boards without compiled
PSRAM support retain existing allocation paths.

## Startup and fallback

QSPI PSRAM initializes in `initArduino()`, after global constructors. The
tables now allocate in `MyMesh::begin`, before routing and persistence start,
on both QSPI and OPI boards. Constructors retain only pointers. Getters never
allocate or move storage, and return complete array references so `sizeof`,
saved bytes, rollback and const access remain correct.

Internal fallback must preserve the 16 KiB DMA reserve and hold each complete
table; otherwise startup reports failure and stops. The RAM check resolves
the actual board MCU and retains both full fallback allowances. Packet pools,
task stacks, ISR state and DMA buffers retain their existing placement.
`MESH_REPEATER_PSRAM_TABLES=0` restores inline table storage for custom builds.

## Matched Full builds

| Target | Internal heap before | Internal heap after | Gain | Capability checks |
| --- | ---: | ---: | ---: | ---: |
| V4 | 203,456 | 240,248 | 36,792 | 16/16 |
| G2 | 200,328 | 237,120 | 36,792 | 15/15 |
| T-Deck | 223,960 | 260,752 | 36,792 | 14/14 |

All three pass compilation, RAM admission, dual-slot OTA fit and artifact
checks with identical capability lists and no declared reductions. The RAM
model still reserves 36,828 bytes for complete internal fallback, so its
conservative admission margins remain similar. These are linked capacity
measurements, not running free-heap or maximum broker-load measurements.

Two packaging failures were reproduced before and after these changes:

- G2 standard exceeds its old 1,310,720-byte slot: linked program size falls
  from 1,375,509 to 1,374,093 bytes but still fails. G2 Full passes separately.
- Classic T-Beam Full MQTT compiles and passes RAM admission, but fails its
  existing dual-slot OTA fit check. This image was not qualified or flashed.

Builds used `build.sh build-firmware TARGET --full-exact` with
`--firmware-version v1.18.0.2-psram-test --profile cascade
--radio-preset usa-cascade-fixed --require-ota`; G2 legacy testing substituted
`--standard`. PlatformIO processes ran one at a time.

## Software coverage

- All 65 RAM-policy tests pass, including real-MCU selection and unchanged
  fallback, reserve and contiguous-block thresholds.
- Scope storage, advert storage and existing capacity/RTC/IRAM tests pass
  (23 methods), including constructor-no-allocation, QSPI startup, full
  persisted-byte comparisons, rollback, lifetime and allocation failures.
- WebSocket tests pass three methods and 32 scenarios over seven platform
  configurations, with padding, cleanup, reserve and IDF 4/5 coverage.

Relevant native storage/transport tests run under ASan/UBSan. Physical boot,
RF timing, persistence and sustained MQTT load still need hardware validation.
