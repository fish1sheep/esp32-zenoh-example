"""
Zenoh subscriber — receives messages published by ESP32-S3 (zenoh-pico).

Default mode is "client" with scouting, requiring a zenohd router.
For direct ESP32-to-PC communication (no router), switch both sides to
peer mode:

    # ESP32 app_main.c: CLIENT_OR_PEER = 1
    # This script:
    python scripts/z_sub.py --mode peer
"""

import argparse  # command-line argument parsing
import sys       # stderr output
import time      # timestamp formatting

import zenoh     # zenoh-python library


def callback(sample: zenoh.Sample):
    """Print received message with a timestamp."""
    ts = time.strftime("%H:%M:%S")                             # format as HH:MM:SS
    payload = sample.payload.to_bytes().decode("utf-8", errors="replace")  # bytes → str
    print(f"[{ts}] {payload}")


def build_config(mode: str, connect: str, listen: str) -> zenoh.Config:
    """Build a Zenoh Config from CLI parameters.

    - client + empty connect → scouting (needs zenohd)
    - client + connect       → connect to a specific endpoint
    - peer                   → multicast discovery / explicit listen
    """
    cfg = zenoh.Config()

    # session mode: "client" or "peer"
    cfg.insert_json5("mode", f'"{mode}"')

    # endpoint(s) to connect to (JSON array — supports multiple fallbacks)
    if connect:
        cfg.insert_json5("connect/endpoints", f'["{connect}"]')
    # endpoint(s) to listen on (peer mode)
    if listen:
        cfg.insert_json5("listen/endpoints", f'["{listen}"]')

    return cfg


def main():
    # --- CLI argument parser ---
    parser = argparse.ArgumentParser(
        description="Zenoh subscriber — receives messages from ESP32-S3 (zenoh-pico)"
    )
    parser.add_argument(
        "--key",
        default="demo/example/zenoh-pico-pub",
        help="Key expression to subscribe to (default: %(default)s)",
    )
    parser.add_argument(
        "--mode",
        default="client",
        choices=["client", "peer"],
        help="Zenoh session mode (default: %(default)s — requires zenohd router)",
    )
    parser.add_argument(
        "--connect",
        default="",
        help="Endpoint to connect to in client mode (empty = scouting)",
    )
    parser.add_argument(
        "--listen",
        default="",
        help="Endpoint to listen on in peer mode (empty = default multicast)",
    )
    args = parser.parse_args()

    # --- Build config and open session ---
    config = build_config(args.mode, args.connect, args.listen)

    # Log startup params for diagnostics
    info = f"mode={args.mode}, key='{args.key}'"
    if args.connect:
        info += f", connect={args.connect}"
    if args.listen:
        info += f", listen={args.listen}"
    print(f"[Zenoh Subscriber] {info}")

    try:
        # Open session and declare subscriber
        with zenoh.open(config) as session:
            sub = session.declare_subscriber(args.key, callback)
            print(f"✓ Subscribed to '{args.key}'. Press Ctrl+C to stop.")

            # Main loop: sleep forever; callback handles incoming messages
            try:
                while True:
                    time.sleep(1)
            except KeyboardInterrupt:
                print("\n⏹  Shutting down…")
                sub.undeclare()   # unsubscribe cleanly
    except Exception as e:
        # Connection failed (timeout, unreachable endpoint, etc.)
        print(f"✗ Connection failed: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
