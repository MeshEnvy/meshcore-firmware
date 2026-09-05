# Changelog

EnvyOS overlay notes for this MeshEnvy fork. Upstream MeshCore has no changelog file. Stock companion / repeater / room-server notes live on [meshcore-dev releases](https://github.com/meshcore-dev/MeshCore/releases).

- **Current overlay inventory:** [README](README.md#envyos-overlay) (drop a row when the PR merges or a core feature replaces it).
- **Fleet binaries:** [EnvyOS distro releases](https://github.com/MeshEnvy/envyos/releases). This repo does not publish EnvyOS GitHub Releases.

`0.1.0`–`0.1.3` used **distro-coupled** stamps (firmware version matched the EnvyOS tag). `1.17.1-ev1` is the first **`upstream-evN`** pin.

`./envyos publish` copies the matching pin section (and `[Unreleased]` if non-empty) into the distro release notes.

Add user-visible overlay work under **`## [Unreleased]`** in the same change set. Fold into the open `evN` heading until that pin ships. Then open a fresh Unreleased.

## [Unreleased]

### Added

- **CLI `try`.** `try [reboot] <secs> <set-args>` applies a pref trial, persists to `/try.json`, auto-reverts on RTC timeout unless `set` commits. Up to 4 slots. `get try` lists pending trials.

### Fixed

- **T096 slim:** hold GPS off and TFT backlight off in `T096Board::begin()`. Slim never compiled the GPS driver, so `GPS_EN` (active-low) stayed high-Z after reset.
- **EndF `ota self` on nRF52 (EC-022).** `find_self_firmware` hashed the flash body via `Utils::sha256` → CC310 `CRYS_HASH`, which DMA-reads SRAM only. Trailer was on the image; verify always failed (`target:00000000`). Hash with software SHA-256 instead.

## [1.17.1-ev1] - unpublished bench pin

First `upstream-evN` pin. Rebase onto MeshCore `companion-v1.17.1`. Overlay from `0.1.3` carried forward.

### Changed

- **MeshCore companion-v1.17.1** (EC-001). Stamp is `1.17.1-ev1`, not `0.1.4`.

### Added

- **Heltec T096 slim repeater.**
- **Heltec T096 USB seeder** (`Heltec_t096_seeder` / `heltec-t096-seeder`). Slim + folder relay. Distinct `target_id` from slim. Also registered `Heltec_t096_repeater_slim` in `OtaTargets.h` (was missing).

### Fixed

- **nRF `get bootloader.ver`.** Recognize EnvyOS `EnvyBoot ` INFO_UF2 marker (Adafruit `UF2 Bootloader ` still works). Empty version on v0.1.3 EnvyBoot nodes was this miss.
- **JSON prefs `ota.signers`.** EnvyOS sets `-DCONFIG_MAX_TOKEN_LEN=512` (4×32 B allowlist is 256 hex). `rd_len` widened to `uint16_t` so that override works above 255. [meshcore-dev#3322](https://github.com/meshcore-dev/MeshCore/pull/3322).
- **Stale `-DFIRMWARE_VERSION=v0.1.0` in `platformio.ini`.** Distro-coupled leftover. `./envyos build` injects the pin stamp; bare `pio run` uses the example header (`v1.17.1`).

## [0.1.3] - 2026-08-27

Hotfix. Field USB flashes 2026-08-23 (Razorback, Poito). GitHub Draft 2026-08-27.

### Fixed

- **Remote admin CLI stack overflow.** Defer CLI out of the RX handler (advert lockup on name-change then Send Advert). [meshcore-dev#3196](https://github.com/meshcore-dev/MeshCore/pull/3196).

## [0.1.2] - 2026-08-04

Firmware stamp `0.1.2` (overlay same line as `0.1.1`). Distro bundle added SenseCAP P1-Pro slim to shipped targets.

### Added

- **SenseCAP P1-Pro slim repeater** in the release target set.

## [0.1.1] - 2026-08-03

### Added

- **Slim RAK4631 repeater** (no OLED / external sensors / BLE). [vk496#4](https://github.com/vk496/MeshCore/pull/4).
- **Role-aware OTA staging ceiling.** [vk496#3](https://github.com/vk496/MeshCore/pull/3).
- **`ota ls` start-at-N.** [vk496#2](https://github.com/vk496/MeshCore/pull/2).
- **`hop.ignore`** bench drop for hop-retry tests.
- **`BLE_DFU_DISABLED`** opt-out for slim builds.

### Fixed

- **Zero-hop echo cancel** on next-hop retry.
- **`log tail on`** enables logging if it was off.

## [0.1.0] - 2026-07-26

First fleet pin. Distro tag `v0.1.0` (GitHub 2026-08-03). `hop.retry` default 0 for field units.

### Added

- **LoRa OTA** (`.mota` fetch/install, device `ota` CLI, self-serve). Contribution target [vk496/MeshCore](https://github.com/vk496/MeshCore) `feature/ota-lora`.
- **SD superseeder.** [vk496#5](https://github.com/vk496/MeshCore/pull/5).
- **Next-hop retry** (echo-primary, `hop.retry`). [meshcore-dev#2980](https://github.com/meshcore-dev/MeshCore/pull/2980) (draft).
- **Serial log tail** (`log tail on`). [meshcore-dev#2991](https://github.com/meshcore-dev/MeshCore/pull/2991).
- **Companion LittleFS fsck on boot.** [meshcore-dev#3012](https://github.com/meshcore-dev/MeshCore/pull/3012) (draft).

### Upstreamed (still in overlay until meshcore-dev has OTA)

- **`ota ls` session tag.** [vk496#1](https://github.com/vk496/MeshCore/pull/1) (merged).
