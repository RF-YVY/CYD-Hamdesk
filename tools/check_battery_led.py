"""Boot the CYD, print safe power diagnostics, and run a timed red-only test."""
import time
from pathlib import Path
import serial

lines = []
with serial.Serial('COM13', 115200, timeout=0.2) as port:
    port.dtr = False
    port.rts = True
    time.sleep(0.12)
    port.rts = False
    started = time.monotonic()
    red_started = False
    battery_requests = 0
    while time.monotonic() - started < 72:
        elapsed = time.monotonic() - started
        if battery_requests < 3 and elapsed >= 9 + battery_requests * 3:
            port.write(b'battery\n')
            battery_requests += 1
        if elapsed >= 20 and not red_started:
            port.write(b'redtest\n')
            red_started = True
        line = port.readline().decode('utf-8', errors='replace').strip()
        markers = ('Battery', 'Time checks:', 'Radar canvas:', 'Ham Desk ready',
                   'Wi-Fi IP:', 'NTP time synchronized', 'NOAA', 'RED TEST',
                   'Guru Meditation', 'Backtrace:', 'rst:')
        if any(marker in line for marker in markers):
            lines.append(line)
            print(line, flush=True)

Path('backups/battery-led-check.txt').write_text('\n'.join(lines) + '\n')
