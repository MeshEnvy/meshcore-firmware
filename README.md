## PotatoCore

**PotatoCore** is a MeshCore fork aimed at [Potato Mesh](https://github.com/l5yth/potato-mesh): a federated dashboard for LoRa mesh nodes. On **ESP32 companion radios with Wi‑Fi**, firmware can **POST discovered nodes** to your Potato Mesh instance over HTTP, so you do not need a separate laptop or Raspberry Pi running an ingest script just to feed the map.

- **What it does:** While the radio is in Wi‑Fi station mode and ingest is configured, the device sends node snapshots to Potato Mesh’s `POST /api/nodes` endpoint (same contract as the Python ingestor). Discovery traffic on LoRa still works as in upstream MeshCore; **Wi‑Fi is only used for HTTP**, not for the phone link (that remains BLE / USB / serial as in the companion example).
- **What you run:** Deploy or run the Potato Mesh web app (see the `potato-mesh` repo README), set `API_TOKEN` there, and point the radio at that server’s origin with the same token.

### Building firmware with ingest

Potato ingest is enabled when **`POTATO_MESH_INGEST`** is defined (this repo sets it in the shared `arduino_base` section of `platformio.ini`). Companion builds that compile `helpers/esp32/*.cpp` link in **`PotatoMeshIngestor`** and **`PotatoMeshConfig`**.

For day‑to‑day development on Heltec LoRa32 v3, use the BLE companion environment (Wi‑Fi used only for ingest):

```bash
pio run -e Heltec_v3_companion_radio_ble
```

Optional **compile-time defaults** (seeded into NVS on first boot if NVS is empty): `POTATO_MESH_INGEST_URL`, `POTATO_MESH_API_TOKEN`, `POTATO_MESH_WIFI_SSID`, `POTATO_MESH_WIFI_PWD`. Other useful defines include `POTATO_MESH_INGEST_API_PATH` (default `/api/nodes`), queue depth, and HTTP timeouts—see `src/helpers/esp32/PotatoMeshIngestor.cpp` and `PotatoMeshConfig.cpp`.

### Configuring ingest (NVS + DMs)

Settings are stored in ESP32 **NVS** under the namespace `potatomesh`. Ingest is considered **ready** only when **all three** are set: Wi‑Fi SSID, ingest **origin** (scheme + host, optional port), and **API token** (sent as `Authorization: Bearer …`).

1. **From a MeshCore client:** Mark your **admin phone/user** as a **favorite** on this radio’s contact list (the companion app’s favorite flag). **Only favorited contacts** may send the slash commands below; everyone else gets a short refusal. Favoriting does **not** limit which mesh nodes are posted to Potato Mesh—**discovered nodes are ingested**; favorites gate **remote configuration** only.
2. **Over the mesh:** Send a **direct message** to the radio with one line starting with:

   | Command | Purpose |
   |--------|---------|
   | `/wifi <ssid> <password>` | Save STA credentials and reconnect |
   | `/auth <token>` | Save Potato Mesh `API_TOKEN` |
   | `/endpoint <url>` | Save ingest origin only, e.g. `http://192.168.1.10:41447` or `https://your-host` (path is fixed by `POTATO_MESH_INGEST_API_PATH`, default `/api/nodes`) |
   | `/pause` / `/resume` | Pause or resume HTTP POSTs (queue kept) |
   | `/debug` | Toggle extra `PotatoMesh:` serial logging (also see `POTATO_MESH_DEBUG` at build time) |
   | `/info` | Status: Wi‑Fi, ingest readiness, queue depth, radio stats |
   | `/help` | Short menu |

After `/wifi`, `/auth`, or `/endpoint`, the ingest queue is cleared and contacts are **re‑bootstrapped** from storage so nodes can be posted again.

### On-device hints

If the display build supports it, the UI can show that ingest **needs configuration** until SSID, URL, and token are all set.

---

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
