# Quntis RGB Pro+ Monitor Light Bar — ESPHome & RF Protocol

Complete reverse-engineering and **ESPHome (Home Assistant)** integration for the **[Quntis RGB Pro+ Monitor Light Bar](https://www.quntis.com/de/products/quntis-rgb-pro-monitor-light-bar-with-remote-15-modes-gaming-backlight-4-colors-brightness-dimmable-fronlight-20-ra95-eye-care-monitor-lamp-no-screen-glare-upgraded-clip-for-all-monitor)** (and likely others using the Touch Remote with 14-LED ring and 15-mode RGB backlight) using an **ESP32** and a **nRF24L01+** module.

<p align="center">
  <img src="https://www.quntis.com/cdn/shop/files/20251125-162341.jpg?v=1764061284&width=900" alt="Quntis RGB Pro+ Monitor Light Bar with Touch Remote" width="720">
</p>

---

## Features

* **Full Front Light Control:** On/Off, stepless Brightness (`10%`–`100%`), and stepless Color Temperature (`3000K` Warm $\leftrightarrow$ `6500K` Cool) exposed as a native Home Assistant CCT Light entity.
* **Full RGB Backlight Control:** Dedicated switch for turning the rear RGB backlight On/Off, a `1–15` Mode selector, and a Cycle Mode button.
* **Live Two-Way Sync with the Physical Remote:** The ESP32 + nRF24L01+ listens continuously on `2419 MHz` when idle. Touching any button on the physical Quntis remote updates Home Assistant's state in real time.
* **Built-In Remote Address Sniffer (No SDR / HackRF Required):** Includes a **"Pair / Sniff Remote"** config switch in Home Assistant that automatically discovers and logs your physical remote's 5-byte address and 4-byte header if your unit uses a different factory ID.

---

## Repository Structure

```text
├── esphome/
│   ├── quntis_lightbar.yaml   # ESPHome device configuration (ESP32 & ESP32-S3)
│   └── quntis_rf.h            # nRF24L01+ XN297L transceiver & sniffer driver
├── docs/
│   ├── PROTOCOL.md            # Deep-dive 2.419 GHz XN297L protocol specification
│   └── images/
│       └── fsk_packet_structure.png
├── tools/
│   └── decode_quntis.py       # Python FSK demodulator, decoder & IQ synthesizer
└── captures/                  # Reference 10 MSps HackRF IQ captures (.complex16s)
```

---

## Hardware Wiring (`nRF24L01+` $\rightarrow$ `ESP32` / `ESP32-S3`)

> [!IMPORTANT]
> Connect the `nRF24L01+` **VCC** pin to **`3.3V`** (never `5V`!). Adding a small `10 µF` capacitor across `VCC` and `GND` on the nRF24L01+ module is recommended for clean TX bursts.

| nRF24L01+ Pin | Basic ESP32 (`esp32dev`) | ESP32-S3 (`ESP32-S3-WROOM-1`) |
| :--- | :---: | :---: |
| **VCC** | `3.3V` | `3.3V` |
| **GND** | `GND` | `GND` |
| **CE** | `GPIO 4` | `GPIO 9` |
| **CSN** | `GPIO 5` | `GPIO 10` |
| **SCK** | `GPIO 18` | `GPIO 12` |
| **MISO** | `GPIO 19` | `GPIO 13` |
| **MOSI** | `GPIO 23` | `GPIO 11` |
| **IRQ** | *Not connected* | *Not connected* |

---

## Installation (ESPHome in Home Assistant)

1. Copy [`esphome/quntis_rf.h`](esphome/quntis_rf.h) into your Home Assistant `/config/esphome/` folder (using the **File Editor**, **Studio Code Server**, or **Samba** add-on).
2. In the **ESPHome Dashboard**, create a new device (or edit an existing one) and paste the contents of [`esphome/quntis_lightbar.yaml`](esphome/quntis_lightbar.yaml) (keeping your own `api:` encryption key, `ota:` password, and Wi-Fi credentials).
3. If using an `ESP32-S3`, uncomment the `ESP32-S3` pin substitutions and `esp32:` board block at the top of `quntis_lightbar.yaml`.
4. Click **Install** to compile and flash your ESP32.

---

## Will I Need to Sniff My Remote's Unique ID?

Each Quntis packet contains two identifier fields:
* **5-byte XN297 Pipe Address:** `AA 41 5F 98 E7` (default in `quntis_lightbar.yaml`)
* **4-byte Payload Header (`p[0..3]`):** `83 18 5B 01` (default in `quntis_lightbar.yaml`)

Depending on whether Quntis assigns a unique ID per remote at the factory (as on older Quntis models so multiple lightbars in the same room don't cross-trigger):
1. **Try the defaults first:** Flash the firmware and toggle **Front Light** in Home Assistant. If your lightbar responds, your unit shares the same ID!
2. **If only the 4-byte Header (`p[0..3]`) differs:** Simply press any button on your physical remote. In normal operation, the ESP32 will automatically print a warning in the ESPHome logs with your remote's 4 header bytes:
   ```text
   [W][quntis_rf]: Received valid Quntis packet with different header: 0xXX, 0xXX, 0xXX, 0x01
   ```
3. **If the 5-byte Pipe Address also differs (Built-in Sniffer Mode):**
   * Turn ON the **"Pair / Sniff Remote"** configuration switch in Home Assistant.
   * Hold your physical Quntis remote within **2–5 cm** of the `nRF24L01+` antenna and press any button once or twice.
   * The ESP32 will capture the raw XN297 preamble (`71 0F 55`), validate the packet using both the 16-bit XN297 CRC and 8-bit payload checksum, **immediately adopt your remote's address & header in RAM**, and print them to the ESPHome logs:
     ```text
     [W][quntis_rf]: === DISCOVERED QUNTIS REMOTE ===
     [W][quntis_rf]:   Address (pipe): 0xAA, 0xXX, 0xXX, 0xXX, 0xXX
     [W][quntis_rf]:   Header p[0..3]: 0xXX, 0xXX, 0xXX, 0x01
     ```
   * Copy those values into the `remote_addr_*` and `remote_hdr_*` substitutions at the top of [`esphome/quntis_lightbar.yaml`](esphome/quntis_lightbar.yaml) and re-flash so they persist across reboots.

---

## Protocol Overview

![FSK Burst and Packet Structure](docs/images/fsk_packet_structure.png)

The remote transmits on **2.419 GHz** (nRF24 Channel `19`) using **1 Mbps GFSK** and a **Panchip XN297L** transceiver. Rather than sending relative "step up/down" commands, the remote tracks the lightbar's state internally (to display on its own 14-LED touch ring) and transmits a **complete state snapshot** in every packet:

```text
On-Air Frame: [71 0F 55] [5B Scrambled Address] [11B Scrambled LSB Payload] [2B CRC-16]
```

After descrambling (`xn297_scramble`) and bit-reversing each payload byte (`LSB-first`), the 11-byte payload `p[0..10]` is:

| Byte | Field | Description |
| :---: | :--- | :--- |
| `p[0..3]` | **Header** | `83 18 5B 01` |
| `p[4]` | **Command ID** | `0x31` = Power Toggle, `0x32` = RGB On/Cycle, `0x35` = Bri Up, `0x36` = Bri Down, `0x39` = Color Temp, `0x3D` = RGB Off |
| `p[5]` | **Brightness** | `0x00` (Off) or `0x1A` (`10%`) .. `0xFF` (`100%`) |
| `p[6]` | **Color Temp** | `0x47` (Warm) .. `0xA2` (Neutral) .. `0xFF` (Cool) |
| `p[7]` | **Enable Flag** | `0xFF` = Target Light ON, `0x00` = Target Light OFF |
| `p[8]` | **RGB Mode** | `0x81` (Mode 1) .. `0x8F` (Mode 15) |
| `p[9]` | **Seq Counter** | Increments by `+1` on every button press |
| `p[10]` | **Checksum** | `sum(p[0..9]) & 0xFF` |

For the full physical layer breakdown, whitening tables, CRC-16 math, and burst timing measurements, see **[`docs/PROTOCOL.md`](docs/PROTOCOL.md)**.

---

## SDR Analysis Tool (`tools/decode_quntis.py`)

If you have a HackRF One or other SDR and want to inspect the reference `10 MSps` `.complex16s` captures in [`captures/`](captures/) (or synthesize new `.complex16s` files to transmit via Universal Radio Hacker / HackRF), run:

```bash
python3 tools/decode_quntis.py
```

