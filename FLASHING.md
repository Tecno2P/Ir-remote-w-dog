# IR Remote Web GUI v6.0.0 — Flashing Instructions

## Quick Start

Flash **4 files** in the correct order using `esptool.py`.

### Windows (one command)

```bat
esptool.py --chip esp32 --port COM3 --baud 921600 ^
  write_flash ^
  0x1000   bootloader.bin ^
  0x8000   partitions.bin ^
  0x10000  firmware.bin ^
  0x350000 littlefs.bin
```

### macOS / Linux

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash \
  0x1000   bootloader.bin \
  0x8000   partitions.bin \
  0x10000  firmware.bin \
  0x350000 littlefs.bin
```

> Replace `COM3` / `/dev/ttyUSB0` with your actual serial port.

> ⚠️ **Important:** `littlefs.bin` flashes to `0x350000` in v6.0.
> The old addresses `0x390000` (v4.x) or `0x2D0000` (v5.x) will corrupt the filesystem.

---

## Flash Map (4 MB ESP32 — Partition Layout v6.0)

| File             | Address      | Description                              |
|------------------|--------------|------------------------------------------|
| `bootloader.bin` | `0x001000`   | ESP32 first-stage bootloader             |
| `partitions.bin` | `0x008000`   | Custom partition table                   |
| `firmware.bin`   | `0x010000`   | Application firmware (IR Remote v6.0.0)  |
| `littlefs.bin`   | **`0x350000`** | Web GUI files + LittleFS storage       |

---

## Partition Layout v6.0

```
Name       Type   SubType   Offset     Size       Notes
nvs        data   nvs       0x009000    20 KB     WiFi credentials, settings
otadata    data   ota       0x00E000     8 KB     OTA boot selector
app0       app    ota_0     0x010000  1664 KB     Active firmware slot
app1       app    ota_1     0x1B0000  1664 KB     OTA update target slot
spiffs     data   spiffs    0x350000   704 KB     LittleFS (Web GUI + data)
```

**Total used: 3596 KB / 4096 KB** (500 KB reserved per OTA slot for growth)

**OTA headroom:** Each firmware slot is 1664 KB. Current firmware ~1.1 MB.
~300 KB free per OTA slot. OTA is rejected automatically if binary exceeds 1380 KB.

---

## Building from Source (PlatformIO)

### Prerequisites

- [PlatformIO Core](https://platformio.org/install/cli) or PlatformIO IDE (VSCode extension)
- Python 3.8+
- USB driver for CP2102 / CH340 chip on your ESP32 board

### Build & Flash Steps

```bash
# Clone or extract the project
cd ir-ota-update

# Build firmware only
pio run -e esp32dev

# Build filesystem (LittleFS) image only
pio run -e esp32dev -t buildfs

# Upload firmware via USB
pio run -e esp32dev -t upload

# Upload filesystem via USB (required on first flash)
pio run -e esp32dev -t uploadfs

# Monitor serial output
pio device monitor --baud 115200
```

> **Important:** The filesystem (`uploadfs`) must be flashed at least once.
> The firmware will start, but the Web GUI will be missing until `littlefs.bin` is flashed.

---

## OTA (Over-The-Air) Update

Once the device is running, update firmware or filesystem wirelessly:

1. Connect to the `IR-Remote` Wi-Fi network (password: `irremote123`)
2. Open `http://192.168.4.1` in your browser
3. Go to **Settings → OTA Firmware Update**
4. Select target: `Firmware` or `Filesystem / LittleFS`
5. Drag & drop the `.bin` file
6. Click **⬆ Flash** and wait — device reboots automatically

**OTA safety features (v6.0):**
- Pre-upload flash space validation (rejects oversized firmware)
- Temporary LittleFS files cleaned before OTA
- Chunk timeout watchdog (aborts stale uploads)
- Rollback: old firmware remains in app0 if OTA fails

---

## First Boot

1. Power on the ESP32.
2. It creates a Wi-Fi access point:
   - **SSID:** `IR-Remote`
   - **Password:** `irremote123`
   - **URL:** `http://192.168.4.1`
3. Open `http://192.168.4.1` in any browser.

---

## Connecting to Your Router (STA Mode)

AP mode stays active at all times. To also connect to your home router:

1. Open **Settings → Wi-Fi Configuration**
2. Enable **"Connect to Router (STA mode)"**
3. Click **📡 Scan** to find your network, or type the SSID manually
4. Enter your Wi-Fi password
5. Click **💾 Save Config**

The ESP32 connects immediately. The AP stays live — access is never lost.
Once connected, the STA IP appears in Settings → System Status.

---

## Hardware Wiring

| ESP32 GPIO | Component                   | Notes                           |
|------------|-----------------------------|---------------------------------|
| GPIO 14    | TSOP4838 DATA pin           | IR Receiver (default)           |
| GPIO 27    | NPN transistor base via 1kΩ | IR Emitter (default)            |
| GND        | TSOP4838 GND                | Also NPN emitter                |
| 3.3V       | TSOP4838 VCC                |                                 |

GPIO assignments can be changed live in **Settings → GPIO Pin Configuration**.

---

## v6.0.0 Changes

| Change                          | Details                                              |
|---------------------------------|------------------------------------------------------|
| NetMon module removed           | Freed ~35 KB flash, ~8 KB RAM                       |
| Ultra Pro Watchdog              | Loop freeze detection, thermal protection, boot counter |
| OTA storage fix                 | 1664 KB slots, pre-flash validation, temp cleanup   |
| Partition layout v6.0           | LittleFS enlarged to 704 KB (was 448 KB)            |
| Flash address changed           | LittleFS now at `0x350000` (was `0x390000`)         |
| Firmware version                | Bumped to 5.0.0                                     |

---

## Troubleshooting

| Symptom                         | Fix                                                                        |
|---------------------------------|----------------------------------------------------------------------------|
| Web GUI blank / "FS not flashed" | Flash `littlefs.bin` at `0x350000` (not 0x390000)                       |
| Can't connect to AP             | Check SSID `IR-Remote`, password `irremote123`                            |
| STA never connects              | Check SSID/password; router must be 2.4 GHz                               |
| No IR received                  | Check TSOP4838 wiring; verify RX GPIO in Settings → GPIO                  |
| IR TX not working               | Check NPN circuit; verify TX GPIO in Settings → GPIO                      |
| Scheduler not firing            | Ensure NTP is synced (needs STA + internet); check timezone offset         |
| OTA failed: "too large"         | Binary exceeds 1380 KB — rebuild with `-Os` flag                          |
| OTA failed: "not enough flash"  | Filesystem full; delete old logs in Settings → Logs                       |
| Device in safe mode             | >5 consecutive crash boots detected; check crash log at `/api/v1/watchdog/crashes` |

---

## Serial Debug Output

At 115200 baud:

```
╔══════════════════════════════════════════════╗
║   IR Remote Web GUI                          ║
║   ESP32-WROOM-32  ·  Full Feature Build      ║
╚══════════════════════════════════════════════╝
Version: 5.0.0
Chip: ESP32-D0WD-V3 rev3 240MHz  Flash:4MB  Heap:298976
[IR] LittleFS OK: total=752KB  used=62KB
[WDT] Started v3.0 — HW:ON  SafeMode:no  BootFails:0  Heap:271336  Temp:42.3C
[IR] AP ON: SSID='IR-Remote' IP=192.168.4.1
[IR] STA: connecting to 'MyRouter'...
[IR] Ready v6.0.0  AP: http://192.168.4.1
[IR] RX=GPIO14  TX-active=1  Groups=0  Schedules=0  Heap=248320
```
