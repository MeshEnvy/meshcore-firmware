# lotato

> The firmware-only [Potato Mesh](https://github.com/l5yth/potato-mesh) MeshCore ingestor. No sidecar needed.

## Introduction

Lotato is a pure MeshCore ingestor for Potato Mesh. It does not require an RPi sidecar, or a Python environment, or anything else. It runs directly on the firmware and has no external hardware dependencies.

## Requirements

* A MeshCore router node with WiFi capability. (Heltec V3 and V4 are very inexpensive popular WiFi-capable devices).
* A MeshCore companion node to remote admin into the Lotato repeater and configure it.

## Getting Started

**Flash Lotato**
Use https://meshforge.org/MeshEnvy/MeshCore-lotato to flash the latest version of Lotato to your chosen device. 

**Find a home for your Lotato repeater**
This device will be your Potato Mesh ingestor. It will likely sit on your desk, plugged into the wall via USB, and patiently ingest MeshCore traffic day and night.

**Configure Repeater Settings**
For one-time repeater setup, you need a USB connection to your computer. Use https://config.meshcore.io/ to configure your repeater for the first time.  Choose a name, radio presets, and most importantly, **choose an admin password**. All future Lotato config is done via remote admin.

Test remote admin access now to be sure. 

**Essential Setup**
Log in via remote access and access the CLI tool. Lotato implements some extra CLI commands to help you get set up:

* `lotato wifi <ssid> <pwd>` to connect to your WiFi network
* `lotato endpoint <url>` to set the Potato Mesh ingestor URL (example: https://monitor.meshenvy.org)
* `lotato token <val>` to set the Potato Mesh API key (see Potato Mesh docs)

After that, you should begin to see MeshCore nodes appearing on your Potato Mesh network!

## Full Command Reference

| Command | Description |
|---|---|
| `lotato` | Same as `lotato status` |
| `lotato status` | Show WiFi, IP, node count, ingest queue, HTTP status, endpoint, token, and Lotato debug (on/off) |
| `lotato pause` | Pause ingest (stops POSTing to Potato Mesh) |
| `lotato resume` | Resume ingest |
| `lotato contacts` | Show node store stats (count, max, repost interval, file path) |
| `lotato flush` | Mark all known nodes for immediate re-post on next ingest sweep |
| `lotato scan [pg]` | Scan for nearby WiFi networks (async — run twice to see results) |
| `lotato wifi` | Show current WiFi connection status |
| `lotato wifi scan [pg]` | Scan for nearby WiFi networks |
| `lotato wifi <n> [pwd]` | Connect to a network by scan result index |
| `lotato wifi <ssid> [pwd]` | Connect to a network by SSID |
| `lotato endpoint <url>` | Set the Potato Mesh ingest endpoint URL |
| `lotato token <val>` | Set the API bearer token |
| `lotato debug on` / `off` | Enable or disable Lotato `Serial` debug logging (stored in NVS; default off until set) |
| `lotato debug` | Toggle debug logging (same NVS flag) |
| `lotato help` | Show command help |

## Changelog (Lotato)

Lotato releases use annotated git tags of the form `lotato-v<lotato>-repeater-v<meshcore>`, for example `lotato-v0.1.0-repeater-v1.14.1`, where the `repeater-v…` suffix is the upstream [MeshCore](https://github.com/ripplebiz/MeshCore) repeater release that revision was based on.

### Unreleased (`lotato` branch, not yet tagged)

- Rename MeshForge-facing naming and unify **Lotato** branding in CLI, configuration, and source (follow-up to the Potato Mesh ingestor naming used in earlier tags).
- **Debug logging:** removed the compile-time `LOTATO_DEBUG` switch; Lotato debug instrumentation is always in the build and is controlled only at runtime. Use `lotato debug on`, `lotato debug off`, or bare `lotato debug` to toggle; the setting persists in NVS (`lotato` / `dbg`). Fresh devices default to debug **off** until you turn it on. `lotato` / `lotato status` include a `Debug: on|off` line.

### [0.1.2] — 2026-04-11 (`lotato-v0.1.2-repeater-v1.14.1`)

- Documentation refresh for Lotato usage and setup.

### [0.1.1] — 2026-04-11 (`lotato-v0.1.1-repeater-v1.14.1`)

- MeshForge / flasher-oriented project configuration updates.
- **Lotato** branding (project and user-facing naming).

### [0.1.0] — 2026-04-09 (`lotato-v0.1.0-repeater-v1.14.1`)

First tagged Lotato release, based on MeshCore **repeater v1.14.1**.

- Initial Potato Mesh / MeshEnvy ingestor firmware path (WiFi repeater ingest to a remote HTTP endpoint).
- Batch posting fixes for the ingest pipeline.
- Fix MeshCore platform reporting for this build.
- ESP32 CLI improvements: chunked serial replies to reduce blocking, larger reply buffer, WiFi failover with rotation across known networks, and related serial output handling.
- Asynchronous handling for certain CLI command responses.
- HTTPS / TLS certificate handling fixes for outbound ingest.

-------

## About MeshCore

MeshCore is a lightweight, portable C++ library that enables multi-hop packet routing for embedded projects using LoRa and other packet radios. It is designed for developers who want to create resilient, decentralized communication networks that work without the internet.

## 🔍 What is MeshCore?

MeshCore now supports a range of LoRa devices, allowing for easy flashing without the need to compile firmware manually. Users can flash a pre-built binary using tools like Adafruit ESPTool and interact with the network through a serial console.
MeshCore provides the ability to create wireless mesh networks, similar to Meshtastic and Reticulum but with a focus on lightweight multi-hop packet routing for embedded projects. Unlike Meshtastic, which is tailored for casual LoRa communication, or Reticulum, which offers advanced networking, MeshCore balances simplicity with scalability, making it ideal for custom embedded solutions., where devices (nodes) can communicate over long distances by relaying messages through intermediate nodes. This is especially useful in off-grid, emergency, or tactical situations where traditional communication infrastructure is unavailable.

## ⚡ Key Features

* Multi-Hop Packet Routing
  * Devices can forward messages across multiple nodes, extending range beyond a single radio's reach.
  * Supports up to a configurable number of hops to balance network efficiency and prevent excessive traffic.
  * Nodes use fixed roles where "Companion" nodes are not repeating messages at all to prevent adverse routing paths from being used.
* Supports LoRa Radios – Works with Heltec, RAK Wireless, and other LoRa-based hardware.
* Decentralized & Resilient – No central server or internet required; the network is self-healing.
* Low Power Consumption – Ideal for battery-powered or solar-powered devices.
* Simple to Deploy – Pre-built example applications make it easy to get started.

## 🎯 What Can You Use MeshCore For?

* Off-Grid Communication: Stay connected even in remote areas.
* Emergency Response & Disaster Recovery: Set up instant networks where infrastructure is down.
* Outdoor Activities: Hiking, camping, and adventure racing communication.
* Tactical & Security Applications: Military, law enforcement, and private security use cases.
* IoT & Sensor Networks: Collect data from remote sensors and relay it back to a central location.

## 🚀 How to Get Started

- Watch the [MeshCore Intro Video](https://www.youtube.com/watch?v=t1qne8uJBAc) by Andy Kirby.
- Read through our [Frequently Asked Questions](./docs/faq.md) section.
- Flash the MeshCore firmware on a supported device.
- Connect with a supported client.

For developers;

- Install [PlatformIO](https://docs.platformio.org) in [Visual Studio Code](https://code.visualstudio.com).
- Clone and open the MeshCore repository in Visual Studio Code.
- See the example applications you can modify and run:
  - [Companion Radio](./examples/companion_radio) - For use with an external chat app, over BLE, USB or WiFi.
  - [KISS Modem](./examples/kiss_modem) - Serial KISS protocol bridge for host applications. ([protocol docs](./docs/kiss_modem_protocol.md))
  - [Simple Repeater](./examples/simple_repeater) - Extends network coverage by relaying messages.
  - [Simple Room Server](./examples/simple_room_server) - A simple BBS server for shared Posts.
  - [Simple Secure Chat](./examples/simple_secure_chat) - Secure terminal based text communication between devices.
  - [Simple Sensor](./examples/simple_sensor) - Remote sensor node with telemetry and alerting.

The Simple Secure Chat example can be interacted with through the Serial Monitor in Visual Studio Code, or with a Serial USB Terminal on Android.

## ⚡️ MeshCore Flasher

We have prebuilt firmware ready to flash on supported devices.

- Launch https://flasher.meshcore.co.uk
- Select a supported device
- Flash one of the firmware types:
  - Companion, Repeater or Room Server
- Once flashing is complete, you can connect with one of the MeshCore clients below.

## 📱 MeshCore Clients

**Companion Firmware**

The companion firmware can be connected to via BLE, USB or WiFi depending on the firmware type you flashed.

- Web: https://app.meshcore.nz
- Android: https://play.google.com/store/apps/details?id=com.liamcottle.meshcore.android
- iOS: https://apps.apple.com/us/app/meshcore/id6742354151?platform=iphone
- NodeJS: https://github.com/liamcottle/meshcore.js
- Python: https://github.com/fdlamotte/meshcore-cli

**Repeater and Room Server Firmware**

The repeater and room server firmwares can be setup via USB in the web config tool.

- https://config.meshcore.dev

They can also be managed via LoRa in the mobile app by using the Remote Management feature.

## 🛠 Hardware Compatibility

MeshCore is designed for devices listed in the [MeshCore Flasher](https://flasher.meshcore.co.uk)

## 📜 License

MeshCore is open-source software released under the MIT License. You are free to use, modify, and distribute it for personal and commercial projects.

## Contributing

Please submit PR's using 'dev' as the base branch!
For minor changes just submit your PR and we'll try to review it, but for anything more 'impactful' please open an Issue first and start a discussion. Is better to sound out what it is you want to achieve first, and try to come to a consensus on what the best approach is, especially when it impacts the structure or architecture of this codebase.

Here are some general principals you should try to adhere to:
* Keep it simple. Please, don't think like a high-level lang programmer. Think embedded, and keep code concise, without any unnecessary layers.
* No dynamic memory allocation, except during setup/begin functions.
* Use the same brace and indenting style that's in the core source modules. (A .clang-format is prob going to be added soon, but please do NOT retroactively re-format existing code. This just creates unnecessary diffs that make finding problems harder)

Help us prioritize! Please react with thumbs-up to issues/PRs you care about most. We look at reaction counts when planning work.

## Road-Map / To-Do

There are a number of fairly major features in the pipeline, with no particular time-frames attached yet. In very rough chronological order:
- [X] Companion radio: UI redesign
- [X] Repeater + Room Server: add ACL's (like Sensor Node has)
- [X] Standardise Bridge mode for repeaters
- [ ] Repeater/Bridge: Standardise the Transport Codes for zoning/filtering
- [X] Core + Repeater: enhanced zero-hop neighbour discovery
- [ ] Core: round-trip manual path support
- [ ] Companion + Apps: support for multiple sub-meshes (and 'off-grid' client repeat mode)
- [ ] Core + Apps: support for LZW message compression
- [ ] Core: dynamic CR (Coding Rate) for weak vs strong hops
- [ ] Core: new framework for hosting multiple virtual nodes on one physical device
- [ ] V2 protocol spec: discussion and consensus around V2 packet protocol, including path hashes, new encryption specs, etc

## 📞 Get Support

- Report bugs and request features on the [GitHub Issues](https://github.com/ripplebiz/MeshCore/issues) page.
- Find additional guides and components on [my site](https://buymeacoffee.com/ripplebiz).
- Join [MeshCore Discord](https://discord.gg/BMwCtwHj5V) to chat with the developers and get help from the community.
