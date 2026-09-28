"""Forward received Meshtastic events to Ham Desk without transmitting on RF."""

import argparse
import json
import time
from urllib import request


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="Ham Desk IP address")
    parser.add_argument("--token", required=True, help="Token from Ham Desk setup page")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--port", help="Meshtastic serial port, e.g. COM11")
    group.add_argument("--host", help="Meshtastic node TCP IP address")
    args = parser.parse_args()

    try:
        from pubsub import pub
        from meshtastic.serial_interface import SerialInterface
        from meshtastic.tcp_interface import TCPInterface
    except ImportError as exc:
        raise SystemExit("Install dependencies: python -m pip install meshtastic pypubsub") from exc

    url = f"http://{args.device}/api/mesh/event"

    def send(event: dict) -> None:
        payload = json.dumps(event).encode("utf-8")
        req = request.Request(
            url,
            payload,
            headers={"Content-Type": "application/json", "X-HamDesk-Token": args.token},
            method="POST",
        )
        try:
            with request.urlopen(req, timeout=3) as response:
                if response.status != 204:
                    print(f"Ham Desk returned HTTP {response.status}")
        except Exception as exc:
            print(f"Ham Desk unreachable: {exc}")

    def received(packet: dict, interface=None) -> None:
        decoded = packet.get("decoded") or {}
        body = decoded.get("text")
        if not isinstance(body, str) or not body.strip():
            return
        sender = packet.get("fromId") or str(packet.get("from", "unknown"))
        print(f"{sender}: {body}")
        snr = packet.get("rxSnr")
        rssi = packet.get("rxRssi")
        signal = f"{snr:+g} dB" if isinstance(snr, (int, float)) else ""
        if not signal and isinstance(rssi, (int, float)):
            signal = f"{rssi:g} dBm"
        send({"type": "message", "from": sender, "text": body, "signal": signal})

    def node_updated(node: dict) -> None:
        if not isinstance(node, dict):
            return
        user = node.get("user") or {}
        name = user.get("shortName") or user.get("longName") or node.get("num") or "Node"
        signal = node.get("snr")
        signal_text = f"{signal:+g} dB" if isinstance(signal, (int, float)) else ""
        send({"type": "node", "from": str(name), "text": "Node update", "signal": signal_text})

    pub.subscribe(received, "meshtastic.receive.text")
    pub.subscribe(node_updated, "meshtastic.node.updated")
    interface = SerialInterface(devPath=args.port) if args.port else TCPInterface(hostname=args.host)
    print(f"Forwarding received Mesh text to {args.device}. Press Ctrl+C to stop.")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        interface.close()


if __name__ == "__main__":
    main()
