## PotatoCore

> A MeshScore ingestor for [Potato Mesh](https://github.com/l5yth/potato-mesh)

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

