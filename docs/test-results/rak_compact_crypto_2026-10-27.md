<!-- meshcore-hosted-doc-link:start -->
<p class="meshcore-hosted-doc-link"><a href="https://mikecarper.github.io/MeshCore/test-results/rak_compact_crypto_2026-10-27/">View this page on MeshCore Docs</a>.</p>
<!-- meshcore-hosted-doc-link:end -->

# RAK repeater flash and crypto timing, 27 October 2026

Selected: **compact scalar arithmetic with the existing fast field arithmetic**.
The GPS parser and CLI reply-copy changes are also enabled. No commands, GPS
providers, keys, packet formats or configured capacities are removed.

Both Reduced RAK repeater builds save **11,580 bytes (11.31 KiB)**. The much
slower compact-field experiment saves only another 4,384 bytes (4.28 KiB) and
is **not enabled by this change**.

## Physical RAK3401 measurements

The Pi's GPIO SWD debugger ran each implementation on the same RAK3401. Values
below are medians from 18 measurements per operation, using two public test
seeds from RFC 8032 and one discarded warmup per seed/operation.

| Operation | Before, ms | Selected scalar-only, ms | Rejected scalar + field, ms |
| --- | ---: | ---: | ---: |
| Generate keypair | 47.513 | 47.145 | 297.863 |
| Sign empty message | 48.738 | 48.753 | 299.472 |
| Sign 96-byte message | 50.887 | 50.902 | 301.621 |
| Sign 256-byte message | 52.991 | 53.006 | 303.725 |
| Calculate shared secret | 38.147 | 37.951 | 274.195 |

The selected implementation adds **0.015 ms** to signing in this measurement.
The unchanged field routines also show small timing differences between
layouts; these measurements do not promise a 15 us difference in every build.
The rejected field implementation adds about 251 ms to signing and 236 ms to
key exchange.

Code and constants ran from **SRAM**, at 64 MHz, with interrupts masked. The
Cortex-M4 DWT cycle counter measured each operation. The 20-cycle measurement
overhead is included in every result. These are isolated algorithm timings,
not flash-resident firmware, Bluetooth, LoRa airtime or end-to-end CLI timings.

Every measured public key, signature and shared secret matched independent
host cryptography. Signature checks covered empty, 96-byte and 256-byte
messages for both seeds. Shared secrets were checked through an independent
Edwards-to-Montgomery conversion and X25519 implementation.

The existing watchdog survives software reset. The harness feeds its enabled
channels between operations, outside the timed region. Initial attempts that
failed reset/watchdog setup gates produced no accepted measurements. The final
three runs completed and reset into the existing application. Application
flash, bootloader and stored data were not updated; the Pi's gateway and host
CLI services were restored and reported active.

## When this work runs

- Signing happens when creating the repeater's own adverts and signed fleet or
  host-service messages. Advert frequency follows the saved advert intervals.
- Fresh shared-secret calculations happen for login attempts and anonymous
  region, owner and clock requests addressed to the node, including outgoing
  region queries. Boot also reconstructs secrets for stored ACL entries.
- Established ACL sessions copy cached secrets. Ordinary relaying does not
  sign every forwarded packet. Receive-side signature verification retains
  its existing CC310 implementation and existing software fallback.

Already queued software packets can wait during crypto, but the radio ISR only
records an event. The main loop reads the radio FIFO and services radio2
switching later. Extending a synchronous operation by a quarter second can
therefore lose new RF packets; software queues do not make that lossless.
The selected policy avoids that added pause. **Live RF packet-loss testing was
not performed for this optimization.**

## Firmware size and build checks

Sizes are ELF `.text + .data`, not the larger UF2 container size. Baselines
already include the earlier reduced-build OTA target-name catalog saving.

| Build | Before, bytes | Selected, bytes | Saving, bytes |
| --- | ---: | ---: | ---: |
| RAK3401 Reduced repeater | 575,644 | 564,064 | 11,580 |
| RAK4631 Reduced repeater | 626,884 | 615,304 | 11,580 |

Full and Reduced production packaging passed for both RAKs with the Cascade
profile and required LoRa OTA. Checks included capability markers, conservative
RAM budgets, compiled Bluetooth DFU handoff and application-only UF2 address
ranges. This is build validation, not physical OTA qualification. Full RAK4631
retains a tight conservative RAM budget: 83,180 bytes available against an
83,136-byte estimate. The existing 8 KiB loop stack is retained.

Focused scalar/field arithmetic, RFC/random crypto parity, sanitizer, GNSS
parser, CLI argument and fleet schedule tests passed. Checks of both selected
linked ARM firmware images also reproduced the expected public keys,
signatures and shared secrets. Their emulated signing frame grows from 3,312
to 3,512 bytes, excluding caller and interrupt frames; keypair and key-exchange
frames are unchanged. This fits within the retained loop stack but is not a
measurement of whole-application stack usage.

## Reproduction and evidence

The [RAM harness](https://github.com/mikecarper/MeshCore/blob/keymindCascade/tools/hil/nrf52_crypto_benchmark.c) and
[linker script](https://github.com/mikecarper/MeshCore/blob/keymindCascade/tools/hil/nrf52_crypto_benchmark.ld) are for debugger use
on an nRF52840 only. They are not production application images. Build with
GCC 14.2.1, `-Os -flto -mcpu=cortex-m4 -mthumb -mfloat-abi=hard
-mfpu=fpv4-sp-d16 -ffunction-sections -fdata-sections -fno-strict-aliasing`,
`ED25519_COMPACT_BASE=1`, `ED25519_COMPACT_SHA512=1` and `ED25519_NO_SEED=1`.
Link `fe.c`, `ge.c`, `keypair.c`, `sc.c`, `sha512.c`, `sign.c` and
`key_exchange.c` from `lib/ed25519`, with `-nostartfiles`, the supplied linker
script, `--gc-sections`, libc and libgcc.

Use scalar/field flags `0/0` for baseline, `1/0` for selected, and `1/1` for the
rejected experiment. `BENCH_COMPACT` is only a report label (0, 2 and 1,
respectively). The recorded scalar-only run used scalar flag 2, which selects
the same nonzero preprocessor branch as production value 1.

After verifying the physical chip identity, reset/halt, load the ELF into RAM,
resume at `bench_start`, and read `report` only after its completion marker is
`0xC0DEF17E`. The watchdog must pause while halted. Reset back into the normal
application afterward. No flash programming is needed for this benchmark.

[Machine-readable results](rak_compact_crypto_2026-10-27.safe.json) include raw
cycle counts, image and harness hashes, build checks, and the rejected candidate
for comparison.
