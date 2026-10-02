# Ham Desk for the 3.5-inch CYD

A touch dashboard for APRS PropView and HamAlert. Built for the common ESP32-3248S035R/C family (ESP32, ST7796 320x480, 4 MB flash). The app detects GT911 capacitive touch or calibrates XPT2046 resistive touch on first boot.

## What it does

- **PropView:** polls `/api/status` and `/api/propagation` every 15 seconds and repaints only values that changed. The URL can be a reachable LAN instance or the local `tools/propview_relay.py`, which reaches remote PropView through Tailscale. The screen shows my-station and regional propagation, station counts, RF/APRS-IS connectivity, and a two-color graph of the last 60 successful readings (up to 15 minutes). The graph starts fresh after a reboot.
- **HamAlert:** logs in to `hamalert.org:7300` using your Telnet username/password, requests `set/json`, and lists the latest spots. Tap a spot for its station location, spotter, comment, spot time, and received UTC timestamp. Fields not present in a particular HamAlert alert are labeled accordingly. A magenta dot marks the SPOTS tab when a new spot arrives while it is closed; opening the tab clears it.
- **ADS-B radar:** centers a 25, 50, or 100 nautical mile radar on the location reported by PropView. A radar line makes one sweep every 12 seconds, and aircraft move between reports based on their last known speed and heading. The CYD queries the public ADSB.lol aircraft API over Wi-Fi every 30 seconds while RADAR is open; estimated motion stops after 60 seconds without a fresh report. Tap an aircraft dot to see its flight, distance, bearing, altitude, and speed. This is an internet-fed view; receiving 1090 MHz signals directly would require separate radio hardware. API rate limits are dynamic, and the firmware backs off for three minutes after HTTP 429. Source: [ADSB.lol API](https://api.adsb.lol/docs).
- **Health:** shows Wi-Fi address and signal, PropView freshness, HamAlert connection status, and battery voltage/estimated charge when a voltage sensor is wired and enabled. Otherwise it clearly shows that sensing is unavailable.
- **Case light:** choose a steady color for each tab and brightness in 25% steps from 0% through 100%. On this unit, the red LED channel remains dark even when GPIO4 is driven directly, so the color choices use the working green and blue channels. PropView alerts pulse green, HamAlert alerts blue, and optional Mesh alerts cyan. Alerts fade back to the selected tab color. The touchscreen offers light on/off, color, opening score threshold, and alert switches. An opening alert fires on a transition after the initial successful poll. The TEST CHANNELS page under CASE LIGHT remains available for diagnosis.
- **On-device settings:** SETUP has Wi-Fi, PropView, HamAlert, case light, alerts, and touch calibration controls. WI-FI can scan nearby networks; selecting one opens the password keyboard and then connects. DEVICE can change the shared setup AP and web password. The web setup remains available as a recovery path.
- **Display extras:** choose green, amber, or cyan in SETUP → DISPLAY. Night mode dims the screen and darkens the palette; optional automatic night mode runs 22:00–06:00 using the UTC offset set there. The optional idle cycle rotates PROP, SPOTS, and RADAR after two minutes without touch. Tap the PropView trend for a larger graph with peaks and opening markers. Aircraft leave short trails on radar, and a new HamAlert spot briefly opens a large spotlight card; tap it for details.

The CYD has no LoRa transceiver. The optional Mesh bridge endpoint remains available for a future external Meshtastic node. All integrations are receive only.

## Flash and first setup

1. Install PlatformIO, connect the CYD over USB, and run `pio run -e cyd35 -t upload`. If PlatformIO cannot detect the serial port, pass `--upload-port <your-port>`.
2. On a new install, the device starts a Wi-Fi access point named `HamDesk-XXXX`. Open SETUP → WI-FI and enter your network name and password with the touch keyboard, then tap **CONNECT NOW**.
3. Open SETUP → PROPVIEW and enter the base URL, for example `http://192.168.1.20:14501` for a local instance or `http://<relay-LAN-IP>:18401` for a Tailscale relay. Optional HamAlert credentials go under SETUP → HAMALERT.
4. The web setup remains available at `http://<device-ip>/` with HTTP Basic authentication, including while connected to the setup AP. User: `admin`; password: the generated or custom password shown under SETUP → DEVICE. Reconnecting the setup AP is available there or by holding BOOT for 5 seconds.

## Battery monitoring

The board's battery connector supplies power through its charger/regulator, but the documented pinout does not show a battery-voltage signal connected to the ESP32. The regulated input voltage cannot reveal battery charge. The battery badge therefore shows `BAT --` and Health shows `SENSOR NOT WIRED` by default.

For battery readings, add a voltage divider from battery positive to GPIO35 on the P3 header, with the lower resistor to ground. A pair of 100 kΩ resistors gives a 2:1 divider and keeps a 4.2 V cell below the ESP32 ADC input range. After verifying the wiring, enable **Battery voltage sensor on GPIO35** in the web setup and set the actual divider ratio. The displayed percentage is a rough voltage estimate, especially while charging or under load. Do not connect the battery directly to GPIO35.

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
