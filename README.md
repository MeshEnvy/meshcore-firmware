## PotatoCore

**PotatoCore** is a MeshCore fork wired for [Potato Mesh](https://github.com/l5yth/potato-mesh): a federated dashboard for LoRa mesh nodes. On **ESP32 companion radios that also have Wi‑Fi**, the firmware can **POST discovered mesh nodes** to your Potato Mesh server over HTTP, so you do not need a separate computer on your LAN running the Python ingestor.

**Wi‑Fi is only used to reach Potato Mesh** (HTTP). Your phone or laptop still talks to the companion over **BLE / USB / serial**, like any other MeshCore companion.

### You need two nodes on the mesh

Remote configuration uses **direct messages** to the ingestor radio. The firmware **only accepts Potato admin commands from contacts you have marked as favorites** on **that** radio.

That means you need **two distinct MeshCore identities** that can reach each other over LoRa:

1. **Ingestor node** — the PotatoCore companion (e.g. Heltec LoRa32 v3) that will join Wi‑Fi and POST to Potato Mesh.
2. **Admin node** — a **second** MeshCore device (another companion, or any node you can use from a MeshCore app to send a DM **to** the ingestor). This is often “your everyday radio” paired to the same phone app, while the ingestor sits in a window or on the roof.

**Workflow:** Pair/configure the ingestor as usual. On the **ingestor’s** contact list, add the **admin node** and turn on **favorite** for that contact. From the MeshCore client connected to the **admin** node, send **direct messages** to the ingestor containing the slash commands below. If you only ever had one radio, you would have no trusted sender the ingestor could accept commands from.

**Favorites vs the map:** Favoriting only gates **who may send `/wifi`, `/auth`, etc.** It does **not** restrict which overheard nodes get sent to Potato Mesh; discovery still drives ingest.

### Potato Mesh server

Run or deploy Potato Mesh (see the **`potato-mesh`** repo README). You will use the same **`API_TOKEN`** (or equivalent shared secret) on the server and in `/auth` on the radio. The radio posts to **`POST /api/nodes`** on your instance (same shape as the official ingestor).

### Configure the ingestor (after favoriting the admin node)

Ingest starts only when **all three** are stored on the device: **Wi‑Fi SSID + password**, **ingest base URL** (scheme + host and optional port, **no path**), and **API token**.

Send each line as a **DM** to the ingestor from your **favorited admin** node:

| Command | Purpose |
|--------|---------|
| `/wifi <ssid> <password>` | Save Wi‑Fi credentials and reconnect |
| `/auth <token>` | Save the Potato Mesh API token (`Bearer` on HTTP) |
| `/endpoint <url>` | Base URL only, e.g. `http://192.168.1.10:41447` or `https://your-host` — the path is added by firmware (default `/api/nodes`) |
| `/pause` / `/resume` | Stop or resume HTTP uploads (queue is kept) |
| `/debug` | Toggle extra `PotatoMesh:` lines on USB serial |
| `/info` | Wi‑Fi status, whether ingest is ready, queue depth, radio counters |
| `/help` | Short command list |

After **`/wifi`**, **`/auth`**, or **`/endpoint`**, the device clears the ingest queue and **re-posts stored contacts** so nothing is stuck after a settings change.

If the build has a display, you may see a hint that ingest **still needs configuration** until all three values are set.

### Development and local testing

For **building** PotatoCore yourself, ingest is compiled when **`POTATO_MESH_INGEST`** is set (this repo enables it in the shared `arduino_base` block in `platformio.ini`). Companion environments that include **`helpers/esp32/*.cpp`** link **`PotatoMeshIngestor`** and **`PotatoMeshConfig`** (e.g. Heltec `Heltec_v3_companion_radio_ble` or `_wifi`; USB-only companion targets may omit those sources).

```bash
pio run -e Heltec_v3_companion_radio_ble
```

**Local Potato Mesh:** Run the web app on your machine, note host and port, set `API_TOKEN`, then `/endpoint http://<your-LAN-IP>:<port>` and `/auth <same token>` from the mesh. For HTTPS tunnels (e.g. ngrok), use the tunnel origin as `/endpoint`; the client adds a header for ngrok where needed.

**Compile-time NVS seeds** (written on first boot if the corresponding NVS keys are still empty): `POTATO_MESH_INGEST_URL`, `POTATO_MESH_API_TOKEN`, `POTATO_MESH_WIFI_SSID`, `POTATO_MESH_WIFI_PWD`. Other knobs: `POTATO_MESH_INGEST_API_PATH`, queue depth, HTTP timeouts — see `src/helpers/esp32/PotatoMeshIngestor.cpp` and `PotatoMeshConfig.cpp`. Persisted settings live in ESP32 **NVS** namespace **`potatomesh`**.

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
