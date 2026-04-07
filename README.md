## PotatoCore

> A MeshScore ingestor for [Potato Mesh](https://github.com/l5yth/potato-mesh)

This project is a fork of MeshCore with Potato Mesh ingestor support built in. No laptop or python sidecar needed.

## Requirements

* A spare MeshCore device with WiFi (will become PotatoCore)
* A regular MeshCore companion node
* A BLE or USB connection 
* A Potato Mesh server

## Quickstart

You'll need two nodes for this! One for PotatoCore, and the other a standard MeshCore companion radio. 

**Step 1 - Flash PotatoCore to a spare device**

First, we're going to make the PotatoCore device. This is the one you'll leave plugged in all the time. It just sits around 24x7 monitoring and pushing data to your Potato Mesh server.

Go to https://meshforge.com/potatocore and flash. Choose any device that has WiFi.

**Step 2 - Favorite your client device from PotatoCore**

Open two instances of https://app.meshcore.nz. Connect one to your client device and one to your PotatoCore device.

Make sure you can see each other in your contact lists. Favorite your normal MeshCore client device from PotatoCore.

**Step 3 - Configure PotatoCore**

PotatoCore is configured over the mesh by sending DM's from a standard MeshCore client device.

PotatoCore needs a few pieces of information to get started:

1. WiFi SSID and password
2. Potato Mesh API endpoint (URL)
3. Potato Mesh API key (shared secret)

DM these slash commands individually to your PotatoCore device:

```
/wifi <SSID> <WPA Password>
```

```
/auth <Potato Mesh API token>
```

```
/endpoint <Potato Mesh API URL>
```

That's it. You should start seeing your nodes populating on your Potato Mesh instance.

## Device Testing (please contribute)

MeshCore in this tree builds for many **WiFi-capable** boards (ESP32 / ESP32-C6 targets under `meshcore/variants/`, plus Raspberry Pi Pico W). Use this matrix for **Potato Mesh HTTP ingest / PotatoCore** on WiFi.

| Device | Status |
| ------ | :----: |
| Ebyte EORA S3 | ❓ |
| Generic E22 (ESP32) | ❓ |
| Generic ESP32-C3 (ESP-NOW / devkit) | ❓ |
| Heltec CT62 | ❓ |
| Heltec E213 ePaper | ❓ |
| Heltec E290 | ❓ |
| Heltec LoRa32 v2 | ❓ |
| Heltec LoRa32 v3 / WSL3 | ✅ |
| Heltec LoRa32 v4 / V4 TFT | ❓ |
| Heltec T190 | ❓ |
| Heltec Wireless Paper | ❓ |
| Heltec Wireless Tracker | ❓ |
| Heltec Wireless Tracker v2 | ❓ |
| LilyGo T-Beam 1W | ❓ |
| LilyGo T-Beam (SX1262) | ❓ |
| LilyGo T-Beam (SX1276) | ❓ |
| LilyGo T-Beam S3 Supreme (SX1262) | ❓ |
| LilyGo T-Deck | ❓ |
| LilyGo T-LoRa (ESP32-C6) | ❓ |
| LilyGo T-LoRa v2.1 | ❓ |
| LilyGo T3-S3 | ❓ |
| LilyGo T3-S3 (SX1276) | ❓ |
| M5Stack Unit C6L | ❓ |
| Meshadventurer | ❓ |
| Nibble Screen Connect | ❓ |
| RAK3112 | ❓ |
| Raspberry Pi Pico W | ❓ |
| Seeed XIAO ESP32-C3 | ❓ |
| Seeed XIAO ESP32-C6 | ❓ |
| Seeed XIAO ESP32S3 + Wio SX1262 | ❓ |
| SenseCAP Indicator (ESP-NOW) | ❓ |
| Station G2 | ❓ |
| Tenstar C3 | ❓ |
| ThinkNode M2 | ❓ |
| ThinkNode M5 | ❓ |

