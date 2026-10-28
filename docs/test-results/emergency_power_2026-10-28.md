<!-- meshcore-hosted-doc-link:start -->
<p class="meshcore-hosted-doc-link"><a href="https://mikecarper.github.io/MeshCore/test-results/emergency_power_2026-10-28/">View this page on MeshCore Docs</a>.</p>
<!-- meshcore-hosted-doc-link:end -->

# Emergency battery schedule, 28 October 2026

Native checks and **nine packaged firmware builds pass**, using
[`f4f945324`](https://github.com/mikecarper/MeshCore/commit/f4f9453248d351bd91be269ca0a58a6edb51882e),
plus the ESP32 USB recovery fix in
[`be6d636c7`](https://github.com/mikecarper/MeshCore/commit/be6d636c7).
SolarXiao 33S, RAK3401 and Heltec V4.3 passed physical sleep, override, automatic
wake and reboot-persistence trials. Current savings and operation with a real
battery at 5% were not measured.

## Behavior

Supported infrastructure images enter automatic emergency mode at **5% or
less**, and recover at **7% or more**, using a voltage-derived battery estimate
sampled every 30 seconds. Invalid readings neither activate the mode nor clear
an existing low-battery state. Existing hard low-voltage shutdown still applies.

Normal work runs during the first **15 minutes of each UTC hour**. Outside that
window, LoRa sleeps and normal forwarding, sensor and display work pauses.
With an implausible clock timestamp, the same cycle follows monotonic uptime
from boot. A plausible timestamp still needs synchronization to UTC.
Brief maintenance wakes, at intervals of **10 seconds or less**, service commands,
clock and watchdogs. Active updates and unfinished critical work defer sleep.

```text
get power.emergency
set power.emergency on
set power.awake 30
set power.awake 0
set power.emergency auto
```

`on` persists manual emergency mode; `auto` restores the default automatic
policy. Entry allows five seconds for the reply. `power.awake` grants a runtime
override of 0–10080 minutes; zero cancels it and reboot clears it. LoRa commands
require an awake window. Local USB can wait for the next maintenance wake.
Status separates the desired `window` from actual radio ownership.

nRF52 retains state through timed RTOS delay and idle sleep; ESP32 uses
timer-woken light sleep. Neither path uses a rebooting deep-sleep cycle.
ESP32 keeps an attached native USB console usable with short CPU idle waits
instead of peripheral-stopping light sleep; detached operation uses timed light sleep.
Wireless services restore their previous live subset and preserve manual
wireless-off settings. RAK3401 places its amplifier in standby and restores it
before radio wake, without cutting the shared supply rail.

Forwarding defaults move from 10/20% to **15/25%**. The exact old pair migrates
from a v1 record; other pairs, enabled controls and voltage endpoints survive.
A v2 record preserves an explicitly selected 10/20 pair. Companion and profiles
that omit battery-forwarding controls retain their existing feature scope.

## Checks

**85 native tests across eleven suites pass**: policy/CLI 7, runtime 13, platform
backends 3, OTA work 1, battery forwarding 20, build-role policy 8, wireless 4,
logging/sleep 3, sleep deadlines 3, CPU clock 9, and bench harness 14. The OTA test compiles
three configurations and executes the actual guards for all three roles.
Coverage includes rollover, failed persistence, reply holds, active transfers,
bounded serving holds, wireless restoration failures and prior Tracker ownership.

| Packaged firmware | Capability checks | Available / required RAM, bytes |
| --- | ---: | ---: |
| SolarXiao 33S Repeater Full | 11/11 | 151,348 / 82,784 |
| SolarXiao 33S Repeater Reduced | 9/9 | 152,528 / 80,736 |
| RAK3401 Repeater Full | 14/14 | 80,124 / 75,920 |
| RAK3401 Repeater Reduced | 12/12 | 82,648 / 73,872 |
| Heltec V4 Full MQTT Repeater | 16/16 | 240,112 / 238,732 |
| Heltec V4 Full MQTT Repeater, USB fix | 16/16 | 240,112 / 238,732 |
| SolarXiao 33S Room Server Full | 7/7 | 157,108 / 67,328 |
| SolarXiao 33S Room Server Reduced | 5/5 | 158,264 / 65,280 |
| Heltec V4 Sensor Full | 9/9 | 271,872 / 150,592 |

Memory admission and packaged update-path checks pass. These are linked capacity
estimates, not measurements of peak runtime heap. PlatformIO ran sequentially.

The [safe machine-readable report](emergency_power_2026-10-28.safe.json) records
ELF, package and manifest hashes. Package hashes were independently compared
with every corresponding memory-manifest entry.

## Physical trials

Each board entered manual radio sleep, woke for a real one-minute override,
returned to sleep when it expired, and retained manual mode after reboot.
Two accelerated hourly boundaries then produced queued adverts on an
independent RAW receiver before any further USB write to the target.

| Target | Independent receiver | Passive capture intervals |
| --- | --- | --- |
| SolarXiao 33S | RAK3401 | 31.586 / 31.587 s |
| RAK3401 | SolarXiao 33S | 31.586 / 31.583 s |
| Heltec V4.3 OLED | RAK3401 | 21.730 / 21.677 s |

The clock was set near the hour boundary; these intervals are evidence of
autonomous RF resumption, not a measurement of the CPU's exact wake latency.
The V4's initial build lost its USB console during off-window light sleep.
The corrected build kept USB responsive while RF slept and passed the whole
trial. WiFi/NTP was temporarily stopped so it could not reset the test clock.

All identities, existing settings and present clocks were checked after
cleanup. SolarXiao retains the tested Full Repeater in automatic mode with no
awake hold. RAK3401 has its original unified Repeater; V4 has its original Full
Companion and verified original NVS/filesystem. The Pi collector is active
with its original configuration and serial ownership.

## Limits

Trials used manual entry and USB power. They do not qualify real 5% battery
entry, current draw, or a continuous 45-minute sleep interval. V4 exercised
the USB-held CPU idle path; detached light-sleep current remains unmeasured.
Solar LTC4162 telemetry was unavailable, and the self ADC is not carrier state
of charge. Room and Sensor hooks have native/build coverage, not physical
trials. Sustained RF delivery and maximum runtime load need separate evidence.
