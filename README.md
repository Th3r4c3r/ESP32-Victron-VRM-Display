# ESP32-Victron-VRM-Display

A live **Victron VRM energy-flow dashboard** on the **ESP32-2432S028R** — the cheap 2.8"
touch TFT board widely known as the **"Cheap Yellow Display" (CYD)**.

It reproduces the VRM portal tile view — **Grid · Inverter · AC Loads · Battery · DC Loads · Solar** —
reading live values from the **Victron VRM API** over WiFi. No Raspberry Pi, no broker, no PC: just the CYD.

![dashboard](docs/dashboard.jpg)

---

## Features

- Live **Grid / AC Loads / DC Loads / Solar (PV)** power, **Inverter system state**
- **Battery**: SoC %, charge bar, state (charging/…), voltage / current / power, temperature
- Energy-flow connectors between tiles, VRM-style dark theme
- NTP clock + WiFi signal indicator
- Polls the VRM API every 20 s (configurable)
- Everything runs on the ESP32 — credentials kept out of git via `secrets.h`

## Hardware

- **ESP32-2432S028R** (CYD) — ILI9341 240×320 TFT + XPT2046 touch (touch unused here)
- Micro-USB cable, 2.4 GHz WiFi (the ESP32 has no 5 GHz)

## Libraries (Arduino)

Install via Library Manager (or `arduino-cli lib install`):

- **TFT_eSPI** (Bodmer)
- **ArduinoJson** (Benoit Blanchon) — v7+
- **StreamUtils** (Benoit Blanchon)

Plus the **ESP32 board package** (`esp32` by Espressif). Board: *ESP32 Dev Module*.

## 1. Configure TFT_eSPI for the CYD

TFT_eSPI needs the right pins. Create `User_Setups/Setup_CYD.h` in the TFT_eSPI library
folder and select it in `User_Setup_Select.h`, **or** paste this into `User_Setup.h`:

```c
#define ILI9341_2_DRIVER        // if the screen stays blank, try #define ILI9341_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
#define SPI_FREQUENCY       55000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
```

> **Colors inverted / black shows as white?** Some CYD panels are inverted. This sketch calls
> `tft.invertDisplay(true)` in `setup()`. If your colors look wrong, change it to `false`.

## 2. Get your VRM token and installation id

1. **Personal Access Token** — VRM Portal → *Preferences → Integrations → Access tokens* → create one.
2. **Installation id (`idSite`)** — it's in the VRM URL (`.../installation/<idSite>/...`), or call:
   ```
   GET https://vrmapi.victronenergy.com/v2/users/{idUser}/installations
   Header:  x-authorization: Token <your-token>
   ```

## 3. Configure credentials

```bash
cd VictronCYD
cp secrets.example.h secrets.h     # then edit secrets.h with your values
```

`secrets.h` is git-ignored. Set `SECRET_WIFI_*`, `SECRET_VRM_TOKEN`, `SECRET_VRM_SITE`,
`SECRET_SITE_NAME`. Optionally tweak `TZOFF` and `POLL_MS` at the top of `VictronCYD.ino`.

## 4. Build & flash

**Arduino IDE:** open `VictronCYD/VictronCYD.ino`, select *ESP32 Dev Module*, Upload.

**arduino-cli:**
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 VictronCYD
arduino-cli upload  --fqbn esp32:esp32:esp32 -p COM3 VictronCYD   # use your port
```

---

## Technical notes (the parts that are easy to get wrong)

Reading the VRM **`/diagnostics`** endpoint on an ESP32 is the tricky bit — the response is
~129 KB of JSON over HTTPS. Three things are needed together:

1. **`http.useHTTP10(true)`** — otherwise the response is *chunked*, and ArduinoJson reads the
   first hex chunk-size as a JSON number, "succeeds", and returns **0 records**.
2. **ArduinoJson `Filter`** — the full body won't fit in RAM (a ~129 KB `String` allocation fails
   alongside the TLS buffers). The filter keeps only `code` / `rawValue` / `formattedValue` per
   record, so memory stays tiny while streaming.
3. **A blocking `Stream` wrapper** — the default `Stream::read()` is non-blocking and returns -1
   between TLS records; ArduinoJson treats that as end-of-input and stops at the first ~16 KB
   (~38 records). The `BlockingStream` in the sketch waits for more bytes until the connection
   truly closes, so the whole document is parsed.

Auth uses the header **`x-authorization: Token <PAT>`** (note: `Bearer` returns 401).

### VRM diagnostics codes used

| Tile | code | | Tile | code |
|---|---|---|---|---|
| Grid power | `g1` | | Battery V/A/W | `bv` / `bc` / `bp` |
| AC Loads | `a1` | | Battery SoC | `bs` |
| DC Loads | `dc` | | Battery temp | `bT` |
| Inverter state | `ss` | | Battery state | `bst` |
| Solar (PV) | `PVP` | | | |

---

## Customizing

- **Refresh rate:** `POLL_MS` in `VictronCYD.ino`.
- **Timezone:** `TZOFF` (seconds offset from UTC).
- **More/other values:** add a `code` to the parse loop in `fetchData()` and a tile to draw it.

## Credits

Built for the ESP32-2432S028R "Cheap Yellow Display". Uses the
[Victron VRM API](https://vrm-api-docs.victronenergy.com/). Not affiliated with Victron Energy.

## License

MIT — see [LICENSE](LICENSE).
