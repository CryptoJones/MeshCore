<p align="center"><em>Proudly Made in Nebraska. Go Big Red! 🌽 <a href="https://xkcd.com/2347/">https://xkcd.com/2347/</a></em></p>

# Fork note — Wio Tracker L1 companion with USB CLI *and* BLE at the same time

This is a fork of [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore).
It adds exactly one thing: a build environment for the Seeed Studio Wio Tracker L1
named **`WioTrackerL1_companion_radio_usb_ble`**, which enables the USB serial CLI
and Bluetooth LE **simultaneously**. Nothing else in the upstream tree is modified.

## Why this was needed

Upstream ships two separate companion builds for this board, and they are mutually
exclusive in practice:

| Shipped build | Defines | You get | You lose |
|---|---|---|---|
| `WioTrackerL1_companion_radio_usb` | `ENABLE_USB_INTERFACE` | `meshcore-cli` over USB serial | the phone app — BLE is gone |
| `WioTrackerL1_companion_radio_ble` | `BLE_PIN_CODE` | the phone app over BLE | scriptable CLI access |

So configuring the node from a laptop and then carrying it with the phone app meant
reflashing the board between the two, every time. On a device whose whole job is to
be picked up and taken outside, that is a genuine daily annoyance — and each reflash
is a 1200-baud touch, a UF2 drag, and a reboot.

The irritating part is that **this was never an architectural limitation.** It is a
packaging choice. `examples/companion_radio/main.cpp` does not pick a transport; it
registers every transport that was compiled in:

```c
MultiSerialInterface interface_manager;                                    // line 17
#if defined(BLE_PIN_CODE)         ... addInterface(InterfaceType::Bluetooth, ...)  // 188
#if defined(ENABLE_USB_INTERFACE) ... addInterface(InterfaceType::USB, ...)        // 214
```

Those are independent `#if` blocks — there is no `#elif` anywhere in the chain. And
`MultiSerialInterface` is explicitly built for concurrency:

```c
#define MAX_INTERFACES 4     // ble, usb, wifi, ethernet
```

`writeFrame()` fans a frame out to every enabled interface, and `checkRecvFrame()`
accepts a frame from whichever interface delivers one first. Two live clients on one
node is the intended design. The upstream envs simply each define one flag, and no
shipped environment in the tree turns on both (82 are BLE-only, 76 are USB-only).

This fork just defines both flags in one environment and compiles
`helpers/nrf52/SerialBLEInterface.cpp` alongside the USB path.

## The one real trap

**Do not add `BLE_DEBUG_LOGGING` to a build that also defines `ENABLE_USB_INTERFACE`.**

`BLE_DEBUG_LOGGING` expands to `Serial.printf("BLE: " ...)`, and
`ArduinoSerialInterface` uses that *same* `Serial` stream to carry binary frames.
Turning both on interleaves human-readable log text into the frame stream and
corrupts the CLI protocol. This is the identical hazard that already earns
`MESH_DEBUG` and `MESH_PACKET_LOGGING` their `; NOTE: DO NOT ENABLE` comments in the
stock `_usb` environment, and it is why upstream's own `build.sh` strips
`-UBLE_DEBUG_LOGGING` under `DISABLE_DEBUG=1`. The new env carries the same warning
comments so the next person does not have to rediscover it.

Note that `DISABLE_DEBUG=1` itself must **not** be used to build this target. It also
passes `-UCFG_DEBUG`, and the Adafruit nRF52 core needs that symbol
(`while(CFG_DEBUG) yield();` in `cores/nRF5/rtos.cpp`), so the build fails to compile.
Upstream's release workflow does not set it either — `firmware-builder.yml` passes only
`FIRMWARE_VERSION`. This environment defines no debug flags to begin with, so there is
nothing to strip.

## Does it fit?

Yes, with room to spare. The genuine open question was whether the BLE stack and the
USB interface would both fit in the board's 708,608-byte application budget. Measured
on the real hardware target (nRF52840, SoftDevice S140 7.3.0):

```
RAM:   62.7% (used 147604 bytes from 235520 bytes)
Flash: 60.8% (used 430824 bytes from 708608 bytes)
```

## Behaviour to expect

Because `writeFrame()` writes to *all* enabled interfaces, a phone connected over BLE
and a `meshcore-cli` session over USB will both see every frame. That is the intended
design, not a bug, but it does mean the two clients share one event stream rather than
getting isolated sessions.

## Building it yourself

```sh
FIRMWARE_VERSION=v1.17.1 sh build.sh build-firmware WioTrackerL1_companion_radio_usb_ble
```

Artifacts land in `out/`. To flash on macOS: `stty -f /dev/cu.usbmodemXXXX 1200` to drop
the board into its UF2 bootloader, wait for the `TRACKER L1` volume to mount, then copy
the `.uf2` onto it. The volume ejecting itself is the success signal. Radio settings
(node name, frequency, bandwidth, spreading factor, TX power) survive the reflash.

---

## About MeshCore

MeshCore is a lightweight, portable C++ library that enables multi-hop packet routing for embedded projects using LoRa and other packet radios. It is designed for developers who want to create resilient, decentralized communication networks that work without the internet.

## 🔍 What is MeshCore?

MeshCore now supports a range of LoRa devices, allowing for easy flashing without the need to compile firmware manually. Users can flash a pre-built binary using tools like Adafruit ESPTool and interact with the network through a serial console.
MeshCore provides the ability to create wireless mesh networks, similar to Meshtastic and Reticulum but with a focus on lightweight multi-hop packet routing for embedded projects. Unlike Meshtastic, which is tailored for casual LoRa communication, or Reticulum, which offers advanced networking, MeshCore balances simplicity with scalability, making it ideal for custom embedded solutions, where devices (nodes) can communicate over long distances by relaying messages through intermediate nodes. This is especially useful in off-grid, emergency, or tactical situations where traditional communication infrastructure is unavailable.

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

- Watch the [MeshCore QuickStart Playlist](https://www.youtube.com/watch?v=iaFltojJrAc&list=PLshzThxhw4O4WU_iZo3NmNZOv6KMrUuF9) by The Comms Channel
- Watch the [MeshCore Technical Presentation](https://www.youtube.com/watch?v=OwmkVkZQTf4) by Liam Cottle.
- Read through our [Frequently Asked Questions](./docs/faq.md) and [Documentation](https://docs.meshcore.io).
- Flash the MeshCore firmware on a supported device.
- Connect with a supported client.

For developers:

- Install [PlatformIO](https://docs.platformio.org) in [Visual Studio Code](https://code.visualstudio.com).
- Clone and open the MeshCore repository in Visual Studio Code.
- See the example applications you can modify and run:
  - [Companion Radio](./examples/companion_radio) - For use with an external chat app, over BLE, USB or Wi-Fi.
  - [KISS Modem](./examples/kiss_modem) - Serial KISS protocol bridge for host applications. ([protocol docs](./docs/kiss_modem_protocol.md))
  - [Simple Repeater](./examples/simple_repeater) - Extends network coverage by relaying messages.
  - [Simple Room Server](./examples/simple_room_server) - A simple BBS server for shared Posts.
  - [Simple Secure Chat](./examples/simple_secure_chat) - Secure terminal based text communication between devices.
  - [Simple Sensor](./examples/simple_sensor) - Remote sensor node with telemetry and alerting.

The Simple Secure Chat example can be interacted with through the Serial Monitor in Visual Studio Code, or with a Serial USB Terminal on Android.

## ⚡️ MeshCore Flasher

We have prebuilt firmware ready to flash on supported devices.

- Launch https://meshcore.io/flasher
- Select a supported device
- Flash one of the firmware types:
  - Companion, Repeater or Room Server
- Once flashing is complete, you can connect with one of the MeshCore clients below.

## 📱 MeshCore Clients

**Companion Firmware**

The companion firmware can be connected to via BLE, USB or Wi-Fi depending on the firmware type you flashed.

- Web: https://app.meshcore.nz
- Android: https://play.google.com/store/apps/details?id=com.liamcottle.meshcore.android
- iOS: https://apps.apple.com/us/app/meshcore/id6742354151?platform=iphone
- NodeJS: https://github.com/liamcottle/meshcore.js
- Python: https://github.com/fdlamotte/meshcore-cli

**Repeater and Room Server Firmware**

The repeater and room server firmware can be set up via USB in the web config tool.

- https://config.meshcore.io

They can also be managed via LoRa in the mobile app by using the Remote Management feature.

## 🛠 Hardware Compatibility

MeshCore is designed for devices listed in the [MeshCore Flasher](https://meshcore.io/flasher)

## 📜 License

MeshCore is open-source software released under the MIT License. You are free to use, modify, and distribute it for personal and commercial projects.

## Contributing

Please submit PR's using 'dev' as the base branch!
For minor changes just submit your PR and we'll try to review it, but for anything more 'impactful' please open an Issue first and start a discussion. It is better to sound out what it is you want to achieve first, and try to come to a consensus on what the best approach is, especially when it impacts the structure or architecture of this codebase.

Here are some general principles you should try to adhere to:
* Keep it simple. Please, don't think like a high-level lang programmer. Think embedded, and keep code concise, without any unnecessary layers.
* No dynamic memory allocation, except during setup/begin functions.
* Use the same brace and indenting style that's in the core source modules. (A .clang-format is probably going to be added soon, but please do NOT retroactively re-format existing code. This just creates unnecessary diffs that make finding problems harder)

Help us prioritize! Please react with thumbs-up to issues/PRs you care about most. We look at reaction counts when planning work.

### Running unit tests

To run unit tests, run the following command:

```bash
pio test --environment native --verbose
```

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
- Join [MeshCore Discord](https://meshcore.gg) to chat with the developers and get help from the community.
