"""Read boot diagnostics without printing saved credentials or service URLs."""
import time
import serial

with serial.Serial('COM13', 115200, timeout=0.25) as port:
    port.dtr = False
    port.rts = True
    time.sleep(0.12)
    port.rts = False
    until = time.monotonic() + 45
    markers = ('Battery', 'Time checks:', 'Radar canvas:', 'Ham Desk ready',
               'Wi-Fi connection started', 'Wi-Fi IP:', 'NTP time synchronized',
               'NOAA', 'Guru Meditation', 'Backtrace:',
               'Wi-Fi connection timed out', 'rst:')
    while time.monotonic() < until:
        line = port.readline().decode('utf-8', errors='replace').strip()
        if any(marker in line for marker in markers):
            print(line, flush=True)
