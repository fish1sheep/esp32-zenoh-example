"""
Zenoh publisher — publishes messages for the ESP32-S3 zenoh-pico subscriber.

Usage — send a one-shot message:

    python scripts/z_pub.py "Hello ESP32!"

Usage — interactive mode (type line by line):

    python scripts/z_pub.py

By default connects as client to tcp/192.168.1.26:7447, matching the C
subscriber in main/z_sub.c.  Use --mode peer for routerless P2P.
"""

import argparse     # command-line argument parsing
import sys          # stdin / stderr
import time         # timestamp for log lines

import zenoh        # zenoh-python library


def build_config(mode: str, connect: str, listen: str) -> zenoh.Config:
    """Build a Zenoh Config matching the ESP32 zenoh-pico subscriber setup."""
    cfg = zenoh.Config()
    cfg.insert_json5("mode", f'"{mode}"')
    if connect:
        cfg.insert_json5("connect/endpoints", f'["{connect}"]')
    if listen:
        cfg.insert_json5("listen/endpoints", f'["{listen}"]')
    return cfg


def main():
    # --- CLI ---
    parser = argparse.ArgumentParser(
        description="Zenoh publisher — sends messages to the ESP32-S3 zenoh-pico subscriber"
    )
    parser.add_argument(
        "message",
        nargs="*",
        help="Message text.  Omit for interactive line-by-line input.",
    )
    parser.add_argument(
        "--key",
        default="demo/example/zenoh-pico-pub",
        help="Key expression to publish on (default: %(default)s)",
    )
    parser.add_argument(
        "--mode",
        default="client",
        choices=["client", "peer"],
        help="Zenoh session mode (default: %(default)s)",
    )
    parser.add_argument(
        "--connect",
        default="tcp/192.168.1.26:7447",
        help="Endpoint to connect to (default: %(default)s)",
    )
    parser.add_argument(
        "--listen",
        default="",
        help="Endpoint to listen on in peer mode",
    )
    args = parser.parse_args()

    # --- Build config and open session ---
    config = build_config(args.mode, args.connect, args.listen)

    info = f"mode={args.mode}, key='{args.key}'"
    if args.connect:
        info += f", connect={args.connect}"
    if args.listen:
        info += f", listen={args.listen}"
    print(f"[Zenoh Publisher] {info}")

    try:
        with zenoh.open(config) as session:
            print(f"✓ Session opened. Publishing to '{args.key}'.\n")

            # ── One-shot mode: publish the message and exit ─────────
            if args.message:
                payload = " ".join(args.message)
                session.put(args.key, payload, encoding="text/plain")
                ts = time.strftime("%H:%M:%S")
                print(f"[{ts}] Published: {payload}")
                return

            # ── Interactive mode: read lines from stdin ─────────────
            print("Enter messages line by line.  Ctrl+D or empty line to quit.")
            try:
                for line in sys.stdin:
                    line = line.rstrip("\n")
                    if not line:
                        break
                    session.put(args.key, line, encoding="text/plain")
                    ts = time.strftime("%H:%M:%S")
                    print(f"[{ts}] Published: {line}")
            except KeyboardInterrupt:
                pass

    except Exception as e:
        print(f"✗ Failed: {e}", file=sys.stderr)
        sys.exit(1)


# ── Also export a simple publish helper for scripting ────────────────────
def publish(key: str, payload: str, connect: str = "tcp/192.168.1.26:7447"):
    """Single-shot helper: connect, publish, close.

    Example::

        from z_pub import publish
        publish("demo/example/hello", "Hello from script!")
    """
    cfg = build_config("client", connect, "")
    with zenoh.open(cfg) as session:
        session.put(key, payload, encoding="text/plain")


if __name__ == "__main__":
    main()
