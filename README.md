# Ham Desk for the 3.5-inch CYD

A touch dashboard for APRS PropView and HamAlert. Configured for the battery-equipped E32R35T (ESP32-WROOM-32E, ST7796 320x480, 4 MB flash, XPT2046 touch). Display and touch are similar to the ESP32-3248S035R/C family, but battery and LED wiring differ. The app retains GT911 detection and resistive-touch calibration support.

### Browser flasher

Open [Ham Desk Web Flasher](https://rf-yvy.github.io/CYD-Hamdesk/) in desktop Chrome or Edge, connect the E32R35T with a USB data cable, and select its CH340 serial device.

- **Update firmware** writes only the application at `0x10000`. Leave **Erase device** unchecked to preserve settings. This option requires the existing Ham Desk E32R35T partition layout.
- **Erase & install** installs the complete firmware and erases saved settings. Use this for first setup or switching from another firmware.

The browser identifies the ESP32 chip family, not the display-board model. This build targets the E32R35T board with GPIO34 battery sensing and GPIO22 red LED. After installation, configure Wi-Fi and services on the CYD touchscreen; Improv Wi-Fi provisioning is not implemented.

## Battery and corrected LED wiring

The E32R35T battery connector has a built-in 100k/100k divider connected to GPIO34. The firmware averages 16 calibrated ADC samples every three seconds, multiplies the result by two, and smooths the displayed voltage. The banner displays a battery icon and approximate percentage prefixed by `~`; HEALTH shows voltage and the estimate. Invalid readings display `--` / unavailable. Percentage is a generic single-cell LiPo voltage estimate, affected by load and charging; it is not a fuel-gauge measurement or a charging/USB-detection indicator. A plausible reading cannot reliably detect whether a battery is physically connected.

The red LED uses GPIO22, green GPIO16, and blue GPIO17. GPIO4 is the audio-amplifier enable and is held HIGH to keep the unused amplifier off. Previous tests on GPIO4 did not test the red LED. The gradient cycles smoothly through red, yellow, green, cyan, blue, and magenta. Static swatches include red, orange, yellow, lime, green, cyan, sky blue, blue, violet, magenta, white, and off. SETUP → CASE LIGHT → TEST CHANNELS tests all channels using direct active-low GPIO drive.

USB serial diagnostics at 115200 baud: `battery` prints the measured voltage and estimated percentage; `redtest` opens the LED test page and drives only red for 45 seconds before restoring the case-light controls; `ledstop` ends a test immediately. Voltage-curve boundary/interpolation/monotonicity checks run at boot alongside the time-format checks.

Hardware references: [E32R35T pin assignments](https://www.lcdwiki.com/3.5inch_ESP32-32E_Display#ESP32_Pin_Assignment), [board schematic](https://www.lcdwiki.com/res/E32R35T/3.5inch_ESP32-32E_E32R35T_Schematic.pdf).

## October 2026 dashboard update

- Rounded navigation buttons, outlined content cards, and a centered AM/PM clock. The banner shows Wi-Fi, HamAlert, and NTP state; HOLD means the clock is running without a current connection or recent synchronization.
- SETUP → CLOCK selects a fixed UTC offset in 15-minute increments or US Eastern, Central, Mountain, or Pacific rules with automatic daylight saving. NIST (`time.nist.gov`) is the primary NTP source; `pool.ntp.org` is the fallback. SYNC NIST NOW requests an immediate resynchronization. Fixed offsets need manual daylight-saving adjustment.
- HamAlert retains 30 spots in RAM, with five rows per page, band/mode filters, and duplicate suppression. Tap BAND, MODE, or PAGE to cycle. Spot and receipt times are separately displayed in local AM/PM time. Unsupported or absent source timestamps are shown as unavailable. History resets after reboot.
- SETUP → CASE LIGHT offers STATIC or GRADIENT, three cycle speeds (60, 30, or 12 seconds), per-tab static colors, and brightness. The gradient rolls through the full RGB rainbow. Alert intensity respects brightness and fades back to the selected effect.
- SETUP → ALERTS controls spotlight duration (0 disables it), quiet hours, and a separate quiet-hour start/end page. Quiet hours suppress LED alerts and spotlight cards while continuing to collect spots.
- PropView shows data age and marks readings older than 45 seconds stale. HamAlert login progresses between loop iterations instead of waiting for each prompt. HTTP waits are bounded; requests can still briefly pause touch processing.
- HEALTH includes NOAA SWPC current radio-blackout (R), solar-radiation (S), and geomagnetic-storm (G) scales with local source timestamp and feed age. It refreshes every ten minutes while a dashboard is open, using verified HTTPS. Reference: [NOAA scales](https://www.swpc.noaa.gov/noaa-scales-explanation), [JSON feed](https://services.swpc.noaa.gov/products/noaa-scales.json).
- Clock/effect/quiet-hour preferences are also available through the authenticated web setup page. Existing saved connection settings and touch calibration are retained.

This device uses the CH340 USB serial port **COM13** ***Your COM port will likely be different***: `pio run -e cyd35 -t upload --upload-port COM13`.

## What it does

- **PropView:** polls `/api/status` and `/api/propagation` every 15 seconds and repaints only values that changed. The URL can be a reachable LAN instance or the local `tools/propview_relay.py`, which reaches remote PropView through Tailscale. The screen shows my-station and regional propagation, station counts, RF/APRS-IS connectivity, and a two-color graph of the last 60 successful readings (up to 15 minutes). The graph starts fresh after a reboot.
- **HamAlert:** logs in to `hamalert.org:7300` using your Telnet username/password, requests `set/json`, and lists the latest spots. Tap a spot for its station location, spotter, comment, spot time, and received timestamp in local AM/PM time. Fields not present in a particular HamAlert alert are labeled accordingly. A magenta dot marks the SPOTS tab when a new spot arrives while it is closed; opening the tab clears it.
- **ADS-B radar:** centers a 25, 50, or 100 nautical mile radar on the location reported by PropView or on saved manual coordinates. To use it without PropView, enter latitude and longitude under SETUP → RADAR and tap **USE MANUAL**. The same fields are on the setup web page. A radar line makes one sweep every 12 seconds, and aircraft move between reports based on their last known speed and heading. The CYD queries the public ADSB.lol aircraft API over Wi-Fi every 30 seconds while RADAR is open; estimated motion stops after 60 seconds without a fresh report. Tap an aircraft dot to see its flight, distance, bearing, altitude, and speed. This is an internet-fed view; receiving 1090 MHz signals directly would require separate radio hardware. API rate limits are dynamic, and the firmware backs off for three minutes after HTTP 429. Source: [ADSB.lol API](https://api.adsb.lol/docs).
- **Health:** shows Wi-Fi address and signal, PropView freshness, and HamAlert connection status.
- **Case light:** choose a steady color for each tab and brightness in 25% steps from 0% through 100%, or use the smooth full RGB gradient. Static choices span the RGB spectrum; diagnostic channel tests use the corrected GPIO22 red mapping. PropView alerts pulse green, HamAlert alerts blue, and optional Mesh alerts cyan. Alerts fade back to the selected effect. The touchscreen offers light on/off, color, opening score threshold, and alert switches. An opening alert fires on a transition after the initial successful poll. The TEST CHANNELS page under CASE LIGHT remains available for diagnosis.
- **On-device settings:** SETUP has Wi-Fi, PropView, HamAlert, case light, alerts, radar center, and touch calibration controls. WI-FI scans nearby networks once when you tap **SCAN NETWORKS**, and again only when you tap **SCAN AGAIN**. Selecting a result opens the password keyboard and then connects. DEVICE can change the shared setup AP and web password. The web setup remains available as a recovery path.
- **Display extras:** choose green, amber, or cyan in SETUP → DISPLAY. Night mode dims the screen and darkens the palette; optional automatic night mode runs 22:00–06:00 using the local time configured under SETUP → CLOCK. The optional idle cycle rotates PROP, SPOTS, and RADAR after two minutes without touch. Tap the PropView trend for a larger graph with peaks and opening markers. Aircraft leave short trails on radar, and a new HamAlert spot briefly opens a large spotlight card; tap it for details.

The CYD has no LoRa transceiver. The optional Mesh bridge endpoint remains available for a future external Meshtastic node. All integrations are receive only.

## Flash and first setup

### Browser flasher

Open [Ham Desk Web Flasher](https://rf-yvy.github.io/CYD-Hamdesk/) in desktop Chrome or Edge, connect the E32R35T with a USB data cable, and select its CH340 serial device.

- **Update firmware** writes only the application at `0x10000`. Leave **Erase device** unchecked to preserve settings. This option requires the existing Ham Desk E32R35T partition layout.
- **Erase & install** installs the complete firmware and erases saved settings. Use this for first setup or switching from another firmware.

The browser identifies the ESP32 chip family, not the display-board model. This build targets the E32R35T board with GPIO34 battery sensing and GPIO22 red LED. After installation, configure Wi-Fi and services on the CYD touchscreen; Improv Wi-Fi provisioning is not implemented.

GitHub Actions builds clean firmware, verifies merged-image offsets, generates manifests and SHA-256 checksums, and deploys the `web/` site to GitHub Pages on relevant pushes to `main`. Generated binary images and manifests are deployment artifacts, not committed files. Device flash backups are never used to produce public firmware.

To preview locally after `pio run -e cyd35`, install `esptool==5.4.0`, run `python tools/prepare_web_flasher.py --version local`, then `python -m http.server 8780 --directory web`. Open `http://localhost:8780`. The packager verifies the first application partition before creating the update manifest.

### PlatformIO setup

1. Install PlatformIO, connect the CYD over USB, and run `pio run -e cyd35 -t upload`. If PlatformIO cannot detect the serial port, pass `--upload-port <your-port>`.
2. On a new install, the device starts a Wi-Fi access point named `HamDesk-XXXX`. Open SETUP → WI-FI and enter your network name and password with the touch keyboard, then tap **CONNECT NOW**.
3. Open SETUP → PROPVIEW and enter the base URL, for example `http://192.168.1.20:14501` for a local instance or `http://<relay-LAN-IP>:18401` for a Tailscale relay. Optional HamAlert credentials go under SETUP → HAMALERT.
4. The web setup remains available at `http://<device-ip>/` with HTTP Basic authentication, including while connected to the setup AP. User: `admin`; password: the generated or custom password shown under SETUP → DEVICE. Reconnecting the setup AP is available there or by holding BOOT for 5 seconds.

The PropView URL should be reachable from the same Wi-Fi network as the CYD. Use a local IP or hostname; this firmware does not use HTTPS for the LAN PropView URL. Reserve the PropView host's LAN IP in the router if its DHCP address might change. HamAlert credentials are stored in ESP32 Preferences and sent to the HamAlert Telnet service using its existing plaintext Telnet protocol; use a distinct password for that service.

## Remote PropView through Tailscale

When PropView is on a different network from the CYD, the ESP32 cannot run the normal Tailscale client. Run `tools/propview_relay.py` on a Windows, Linux, or macOS device that **has Tailscale connected** and shares a local Wi-Fi network with the CYD. That could be a PC or a small travel Raspberry Pi. The CYD calls the relay on local Wi-Fi; the relay calls your PropView tailnet IP or MagicDNS name. Nothing needs to be publicly exposed. This relay is unnecessary when PropView and the CYD share a LAN.

1. On the gateway, check that `tailscale status` shows connected and `http://<PropView-tailnet-IP>:14501/api/status` is reachable. PropView's documented default web port is 14501; use its actual configured port if different.
2. Start the relay. On PowerShell, set `$env:HAMDESK_TOKEN = '<token from CYD setup page>'`; on Linux/macOS, use `export HAMDESK_TOKEN='<token>'`. Then run `python tools/propview_relay.py --upstream http://<PropView-tailnet-IP>:14501 --port 18401`. The relay prompts for the token if the environment variable is absent.
3. Enter `http://<gateway-LAN-IP>:18401` as the CYD's **PropView URL** in its setup page. The CYD sends its device token in a request header. Only `/api/status` and `/api/propagation` are exposed by the relay.

Keep the gateway running while the CYD is away from PropView's LAN. If you use a travel hotspot, connect both the CYD and gateway to that hotspot. For a permanent installation, run the relay as a service on an always-on gateway and allow TCP 18401 from your trusted local network in its firewall.

## Controls

Tap the bottom navigation for PROP, SPOTS, RADAR, HEALTH, and SETUP. On RADAR, tap an aircraft dot for details or use ± to change range. On SPOTS, tap a row for its full details. In SETUP → CASE LIGHT, choose a tab, tap a color swatch, set brightness, and toggle the light. SETUP → DEVICE can restart into touch calibration; tap the four targets and the result is saved. The BOOT button can also move to the next tab with a short press or reopen the setup AP when held for 5 seconds; neither action is required for normal use.

## Device backups

Full flash backups may contain saved Wi-Fi and service credentials. Keep them private; the `backups/` directory and binary images are excluded from Git. To restore a private full flash backup, use `python -m esptool --port <your-port> write-flash 0 <your-backup.bin>` after confirming the serial port and device match.
