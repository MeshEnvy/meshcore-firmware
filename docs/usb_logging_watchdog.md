<!-- meshcore-hosted-doc-link:start -->
<p class="meshcore-hosted-doc-link"><a href="https://mikecarper.github.io/MeshCore/usb_logging_watchdog/">View this page on MeshCore Docs</a>.</p>
<!-- meshcore-hosted-doc-link:end -->

# USB logging watchdog

Current source includes a saved USB logging watchdog in Companion, Repeater,
Room Server, and Sensor. It repairs the radio's USB connection; it cannot
restart a Pi, restore SSH, or prove MQTT or disk delivery. Supported backends
are ESP32 native TinyUSB/HWCDC and nRF52 TinyUSB. An external USB-to-UART
adapter is not an observable native-USB backend.
Unsupported targets compile out recovery timers and statefile transactions;
their watchdog commands report unsupported rather than saving an unusable mode.
Management still reports the logging master with USB support false.

## Commands and defaults

```text
get usb.watchdog
get usb.watchdog.last
set usb.watchdog auto
set usb.watchdog on
set usb.watchdog off
```

Setters accept exactly one lowercase mode, save it before applying, and do not
reboot immediately. They do not enable `usb.logging` or `usb.debug`.

| Mode | Behavior |
| --- | --- |
| `auto` | Default for missing watchdog state, including upgrades. Requires 14 continuous healthy logging-client days before saving `on`, with the retry tier reset to one hour. A previously confirmed client can receive USB-only recovery before promotion, but Auto never reboots the MCU. |
| `on` | Explicitly arms physical USB recovery and eventual MCU reboot. A confirmed stats client also gets USB-only recovery if its stats polls stop; missing polls alone cannot authorize MCU reboot. |
| `off` | No watchdog probes, automatic USB recovery, qualification, or MCU reset. Packet logging remains independent. |

Existing valid saved On/Off choices win. Auto qualification is RAM-only and
restarts on reboot, USB disconnect, observed stall, expired client lease,
logging master Off, or an explicit mode change. A node that has never confirmed
a logging client does not cycle USB in Auto merely because no cable is attached.

## Confirming a logging client

The existing `meshcoretomqtt` five-minute `stats-core`, `stats-radio`, and
`stats-packets` polls renew a 15-minute lease. Only complete, exact stats
commands received on the actual USB logging endpoint count; BLE, network,
LoRa, management reports, and ordinary framed Companion traffic do not.
No new heartbeat command is required. A cable, open TTY, DTR, or kernel USB
ACK alone cannot qualify Auto.

Interactive text commands on a shared USB console pause packet logs for five
minutes after the last command. The three routine stats polls above keep
their replies and renew the lease without starting or extending this pause.
Dedicated logging ports and explicit Companion stream mode keep logging.
The pause does not disable measurements, LoRa telemetry, direct WiFi MQTT,
or the saved logging master. USB packet collectors miss log copies during
the pause. See [live USB logging](cli_commands.md#control-live-usb-logging).

`logger=1` requires the USB logging master, an observed host and reader, a fresh
stats lease, and no physical TX stall. Any program polling those commands can
qualify: this is not application identification or proof that packets were
parsed, published to MQTT, or written to disk. A one-time manual stats query
can also establish the lease; use watchdog Off when later USB-only reconnection
would be unwanted.

On nRF52 Full Companion, only the dedicated logging CDC (`*-if02`) establishes
the lease. That endpoint accepts bounded stats-poll markers without replying
with statistics or exposing arbitrary CLI/configuration commands. Queries and
configuration still use primary CDC (`*-if00`), whose stats queries do not
prove a logging reader. Existing single-port bridges needing identity/CLI
replies still require separate control-port support.

The default nRF52 framework logger follows the same runtime USB logging and
debug settings. Bluefruit flash-event diagnostics use the selected bounded
logging stream, including dedicated CDC on Full Companion, so saving contacts
does not insert ASCII into the primary Companion protocol. A disconnected or
full logging endpoint drops diagnostic output without falling back to the
control port. Explicit framework UART and RTT logger selections keep their
existing destinations.

On ESP32 USB-Serial/JTAG builds, ESP-IDF diagnostics also follow both
`usb.logging` and `usb.debug`. Driver tasks submit bounded text; the USB owner
emits it through the same session and writer guards as functional output.
Logging-off, host changes and congestion discard pending diagnostics instead
of falling back to the raw SDK console. Fragmented diagnostics from concurrent
SDK tasks remain best effort. UART and native TinyUSB routes are unchanged.

## Recovery timing and safety

For a physical host/reader outage, or pending TX with no real completion,
software USB recovery is requested after five minutes. Re-enumeration follows
after another 60 seconds if health is not restored. These are one staged
attempt per outage, not a repeated five-minute reboot loop. Busy backends retry
at most once every five seconds without consuming an attempted stage.

After a confirmed client's stats lease expires, USB-only repair is due five
minutes later: normally 20 minutes after its last stats poll. This applies in
Auto and, once a client is confirmed during that boot, On. On with no stats
client observed remains compatible with non-stats log readers. A gap in stats
polls does not by itself reboot a healthy USB transport.

MCU reboot requires On, a physical USB fault lasting the entire saved tier,
and the USB recovery grace periods. Tiers grow after reset authorization:
1, 2, 4, 8, 16, 32, 64, 128, then 168 hours (one week). An older app-only
stats outage cannot shorten a later physical fault's interval. Ten minutes
of healthy monitored operation resets a raised tier to one hour.

Native TX-completion events are progress evidence. On the pinned ESP32 HWCDC
backend, an exclusive check of both the empty software queue and acknowledged
hardware FIFO also completes a pending write: the SDK does not emit another
TX event for the final FIFO acknowledgement. An empty software queue alone
does not establish completion. Queueing, purging,
reinitializing, or accepting the optional bounded minute probe is not proof
of recovery. The watchdog never waits for a host. USB connection reports,
especially HWCDC's host/reader flags, are SDK approximations and do not reliably
identify an open TTY process.
HWCDC therefore uses physical host presence with a 100 ms continuous-loss
check rather than its unreliable open-reader heuristic. A brief status dip
does not revoke a valid stats lease. A real USB bus reset invalidates the lease
immediately; sustained host loss and actual TX stalls remain observable.

Recovery waits for active Binary USB clients, mOTA/update owners, unsaved
contacts, radio tests/calibration/temporary-radio operations, functional CLI
work, and selected/in-flight radio transmissions to clear. Stale ASCII output
from a disconnected or stalled host does not indefinitely prevent recovery.
Before MCU reset, the next tier must commit and pass readback; the firmware
then rechecks transport health and ownership. Re-enumeration preserves the
configured USB descriptors, but can disconnect host applications and lose
best-effort packet records.

## Persistence and status

The independent, versioned, CRC-protected `/usb_wdg` statefile does not
change existing role preference offsets. Unreadable/corrupt state or a volatile
fallback filesystem disables automatic recovery/promotion. Failed commits
remain fail-closed; an explicit setter can retry or repair this statefile on
durable storage, applying only after complete verification. It cannot make
volatile storage durable. A verified backup repairs a conclusively missing
primary after an interrupted rename; unreadable live state is not replaced
automatically.

Current writes use the 33-byte UW2 format, including the latest watchdog event.
Valid 20-byte UW1 files still load with their saved mode, tier, and counters;
they upgrade on the next legitimate event/configuration save, not merely on
boot. Each attempted software recovery or re-enumeration saves its event and
counters once. Probes, idle ticks, ownership/backend deferrals, and status
reads do not write flash. A failed save leaves the live event marked unsaved
and automatic actions fail closed until explicit repair.

### Last-event review

```text
get usb.watchdog.last
```

This returns the latest event only, not an unbounded history. New events replace
older ones; exporting periodic management reports can retain observations
off-node. Mode changes preserve the record. Erasing the filesystem or restoring
a backup can remove or roll back it.

The event includes an advisory Unix epoch from the node's RTC, uptime seconds
at that recorded boot, a saturating event sequence, action, reason bitmask, and
durability status. Uptime extends the millisecond clock across rollover.
RTC time is not verified UTC: default clocks can start at a plausible fixed
date, and a later time correction does not rewrite old events. Epoch zero
is allowed and can mean no usable timestamp source. Use a synchronized node
clock for meaningful calendar times; sequence and recorded uptime remain useful when
clock time is wrong.

Reason bits are `1` host absent, `2` reader absent, `4` physical TX stalled,
and `8` previously confirmed stats client inactive. Physical failures can set
multiple bits; a stats-only expiry uses bit 8 and cannot authorize an MCU reset.
Actions are `soft-recovery`, `reenumerate`, `reboot-requested`, and
`reboot-cancelled`. Software actions mean the backend reported an attempt, not
that USB was successfully repaired. A reboot request is persisted with the
next backoff tier before authorization; health/ownership are rechecked after
the commit. A subsequent veto records cancellation immediately in RAM. If OTA
or another owner makes storage unsafe, its durable save waits for a safe
application tick; power loss before that save can leave the earlier reboot
intent on disk. Neither the intent record
nor the existing reboot counter proves a completed physical reset. Power loss
or a hardware main-loop watchdog can occur without any opportunity to save
an exact trigger time; this record covers the staged USB logging watchdog.

`get usb.watchdog` includes these compact fields:

| Field | Meaning |
| --- | --- |
| `usb`, `log`, `host`, `reader`, `logger` | Backend supported; logging master; observed physical USB host/reader; confirmed healthy stats client. |
| `stall`, `stage`, `step` | Physical TX stall; stage 0 idle, 1 software wait, 2 re-enumeration wait, 3 reset pending; saved tier index 0-8. |
| `retry`, `idle`, `auto` | Saved MCU interval, current monitored fault age, and continuous Auto qualification age, in seconds. Retry is not an action countdown. |
| `defer`, `fs` | Ownership/backend deferral and verified durable persistence readiness. |
| `rec`, `boot` | Attempted USB recoveries and durable reset authorizations. Attempts save the event and recovery count together; a failed save can leave the live recovery count unsaved. The reset count can include a later safety/health veto; it is not a count of completed physical reboots. |

The [MGR2 management report](management_reports.md) exports the USB status and
the same latest-event record as a frozen, authenticated public snapshot. Updated
Python and browser decoders retain released MGR1 support; older decoder copies
must be updated to read MGR2. Events appear on the normal management schedule, not
as immediate alerts. Reading status does not renew the logging-client lease.

Concurrent readers receive one coherent status publication, including its tier,
counters, and latest event. Reads never wait for a writer. If three bounded
attempts cannot obtain a snapshot, status conservatively reports deferred
recovery and no verified persistence, ages, counters, or event. It preserves
published hardware/logging/mode intent and protects a possibly active reader.
This temporary fallback cannot authorize a reset. The separate awake hint keeps
an already durable watchdog policy serviced while publication is busy.

On nRF52, the CPU can wait for interrupts between watchdog checks in both
`on` and `auto` modes. Its clock and wake interrupts continue running, so this
does not restart recovery deadlines or disable the watchdog. Active USB work
and unfinished transport detach/reattach still prevent that idle wait. Other
platforms retain their existing sleep guard, including protection against
sleep modes that restart the application clock.

## Verification

The two qualification stages below precede the subsequent upstream USB/GPS
and repeater trace merge. Their firmware byte margins describe those earlier
images, not the current merged source.

Initial watchdog qualification on 2026-10-03, before the latest-event extension,
passed 109 USB regression tests, 143 native test cases, 11 management protocol
tests, and the browser decoder checks.
Tests execute the production policy, runtime, marker parser, role ownership
guards, and native USB boundaries with simulated clock, filesystem, and FIFO
faults. The 14-day qualification uses an advanced test clock, not a hardware
soak. Bounded console output, unread hosts, session races, and exact stats
markers are covered.

Those initial compiled checks passed Station G2 Full Companion, Repeater, and Room Server;
XIAO nRF52 Full Companion and Repeater (full and reduced OTA qualifications);
and ordinary RAK 3x72 STM32 USB Companion. The STM32 image retains all CLI
features and its 32 KiB filesystem, but has only 368 bytes of app flash margin;
its unsupported USB watchdog machinery is compiled out.

Latest-event verification on the same date passed 116 USB regression tests,
15 management tests with fatal AddressSanitizer/UndefinedBehaviorSanitizer
checks, and the browser decoder tests. Coverage includes timestamp callbacks,
clock corrections and rollover, legacy-state migration, commit failures,
reboot-intent cancellation, deferred cancellation saves, and authenticated
cross-language decoding of the development report formats. The extension was
called MGR3 during those pre-release checks, then assigned to MGR2 because the
earlier USB-only MGR2 layout had never been released. Current decoders support
released MGR1 and the finalized MGR2 layout; this reassignment was not a migration
of deployed MGR2 reports. The full native suite passed 1,713 cases
during implementation; the final packet validator also passed a fresh 116-case
routing suite and 5,855,168 previous/current validator comparisons under fatal
sanitizers.

The extension compiled in standard Station G2 Full Companion and Repeater,
XIAO nRF52 Repeater, and RAK 3x72 USB Companion. The normal `build.sh` STM32
recipe at version `1.17.1.9` retains all CLI features and the 32 KiB filesystem,
with 372 bytes of app flash margin; longer version strings or future changes
can still exceed that tight budget. No features or filesystem space were cut
to fit this extension. This is not an all-board release qualification or a
physical USB recovery test. No hardware was flashed, and verification artifacts
stayed outside `out`.

The subsequent USB/GPS/repeater-trace merge passed all 1,718 native cases,
133 USB regression methods, 25 management/event/watchdog methods plus the
browser decoder, and 10 GPS/trace/persistence methods. Companion preference
tests exercise six platform/layout variants under fatal sanitizers. The
compact final preference writer also matches the previous full image for
256 tail-value sets per variant and preserves committed files through every
partial-tail write/readback boundary and rename failure.

Compile checks for that merge passed standard XIAO S3 Full Companion, Station
G2 Repeater and Room Server, native-USB XIAO S3 USB Companion with all five
session/progress hooks linked, and XIAO nRF52 Repeater. The final compact writer
passed both the bare STM32 CI recipe and the normal `1.17.1.9` recipe; the
latter uses 229,020 of 229,376 app flash bytes (356 bytes of margin), retaining
all features and the 32 KiB filesystem. The bare recipe is tighter still.
These checks do not qualify every board or physical USB recovery. No device
was flashed and existing `out` artifacts were not changed.

Finalizing the event-capable layout as MGR2 passed 1,719 native cases, 16
management tests with real C++/Python/browser cryptography, the standalone
browser decoder checks, and the bare STM32 Companion compile. Coverage retains
MGR1, rejects retired formats and old prototype ciphertext, and verifies that
invalid management sends release their packet without queueing it.
