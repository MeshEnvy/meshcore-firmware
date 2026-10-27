<!-- meshcore-hosted-doc-link:start -->
<p class="meshcore-hosted-doc-link"><a href="https://mikecarper.github.io/MeshCore/test-results/g3_psram_storage_2026-10-27/">View this page on MeshCore Docs</a>.</p>
<!-- meshcore-hosted-doc-link:end -->

# G3 repeater PSRAM storage, 2026-10-27

The G3 Full repeater frees **36,776 bytes of static internal RAM** by moving
its channel/scope tables and advert-limit history to PSRAM. The MQTT SDK
WebSocket buffers also prefer PSRAM: **2,050 bytes per initialized client**,
or 10,250 bytes with five initialized clients. Capacities, saved formats and
radio queues remain unchanged.

These are linked measurements and verified allocation sizes. No G3 was
flashed; running heap, RF timing and sustained broker load were not measured.
The [machine-readable evidence](g3_psram_storage_2026-10-27.safe.json) records
the matched builds, hashes and checks.

## Matched builds

Baseline: `88bfaebb5`. Final: the production changes accompanying this report.
Both use the same G3 Full recipe, version, Cascade profile and USA preset.

| Measurement | Baseline | Final |
| --- | ---: | ---: |
| Internal BSS bytes | 142,088 | 105,312 |
| Internal data bytes | 23,980 | 23,980 |
| Linked internal heap capacity | 199,976 | 236,752 |
| Modeled internal requirement | 197,808 | 234,636 |
| Conservative admission margin | 2,168 | 2,116 |
| Packaged application bytes | 2,156,504 | 2,155,752 |
| OTA application-slot bytes | 6,553,600 | 6,553,600 |

The model still reserves complete internal fallback for both tables: 18,892
bytes for scope storage and 17,936 for advert history, including overhead.
That explains why the conservative admission margin does not rise with the
normal PSRAM-backed free heap. The 16 KiB DMA reserve and existing startup
requirements are unchanged. All 16 capability checks pass, with no declared
feature reductions; WiFi/LoRa OTA, MQTT, logging, WebConfig, bridges and host
KISS remain compiled in.

## Allocation behavior

- G3 repeater defaults select PSRAM for all 255 scope entries, 32 direct-scope
  names, 255 scope requirements and 128 advert-source histories.
- Allocation happens once during construction. Array references preserve
  addresses, complete-array `sizeof`, file formats and rollback behavior.
- The pinned ESP32-S3 SDK initializes PSRAM heaps before global constructors;
  Arduino's later `psramFound()` flag is not used as a constructor-time gate.
- Complete internal fallback is allowed only while preserving the existing
  DMA reserve. Otherwise startup reports the allocation failure and stops.
  It never silently reduces filter or advert-protection capacity.
- `MESH_REPEATER_PSRAM_TABLES=0` restores inline storage. Other boards retain
  their existing default storage, including RTC/IRAM paths.
- The IDF 4 SDK initializes both WS and WSS transports for each MQTT client,
  even plain MQTT. Both buffers retain the existing 1,025-byte overflow fix.
  A failed padded allocation destroys that transport safely instead of
  returning its original undersized buffer. IDF 5 remains a passthrough.

## Verification

- Scope storage: three ASan/UBSan tests cover full capacities, identical 18,885
  persisted bytes, rollback on short writes/rename failures, lifetime, guarded
  fallback, fatal allocation failure and platform/SDK gates.
- Advert history: four ASan/UBSan tests compare all 128 entries against inline
  storage, including quota, duplicate, badlist and rollover behavior. Existing
  RTC (eight), IRAM (seven) and native limiter (33 per backend) tests pass.
- WebSocket wrapper: three ASan/UBSan tests exercise 17 scenarios across G3,
  other PSRAM boards, internal-only boards and IDF 4/5, including the extra
  byte, cleanup, parent lifetime and fallback boundaries.
- RAM policy: all 63 tests pass, including six new fallback regressions.
  Existing policy capacity, rule layout, moderation and CLI tests pass.
- Canonical G3 Full package: compile, internal-RAM admission, dual-OTA fit and
  capability checks pass. PlatformIO ran one process at a time.

Reproduce the final build with:

```sh
bash build.sh build-firmware Station_G3_ESP32_repeater --full-exact --firmware-version v1.18.0.2-psram-test --profile cascade --radio-preset usa-cascade-fixed --require-ota
```

Physical acceptance remains a G3 boot, persistence, radio traffic and MQTT
reconnect/load run with internal/DMA heap monitoring. The maximum five-broker
TLS/WSS/outbox workload remains unqualified by these software checks.
