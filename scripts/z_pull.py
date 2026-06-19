"""
Zenoh batch publisher — tests the ESP32-S3 pull subscriber (z_pull.c).

The MCU's ``z_pull.c`` subscribes to ``demo/example/**`` with a ring
channel (capacity 3) and polls every 5 seconds.  This script sends a
burst of messages, then waits, letting you observe the ring channel's
behaviour — oldest messages dropped when full, all pending drained
on the next poll.

Usage — send a burst of 5 messages, 1 second apart::

    python scripts/z_pull.py
    python scripts/z_pull.py -n 10 -d 0.5 "Batch test"
    python scripts/z_pull.py --mode peer
"""

import argparse     # command-line argument parsing
import sys          # stderr / exit code
import time         # sleep() / timestamp

import zenoh        # zenoh-python library


# ============================================================================
# Config builder — same signature as z_pub.py / z_sub.py
# ============================================================================


def build_config(mode: str, connect: str, listen: str) -> zenoh.Config:
    """Build a Zenoh Config from CLI parameters."""
    cfg = zenoh.Config()
    cfg.insert_json5("mode", f'"{mode}"')
    if connect:
        cfg.insert_json5("connect/endpoints", f'["{connect}"]')
    if listen:
        cfg.insert_json5("listen/endpoints", f'["{listen}"]')
    return cfg


# ============================================================================
# Exportable helper
# ============================================================================


def publish_burst(
    config: zenoh.Config,
    key: str,
    payload_prefix: str = "pull-test",
    count: int = 5,
    delay_sec: float = 1.0,
) -> int:
    """Publish a burst of messages, one per ``delay_sec``.

    Returns the number of messages successfully published.
    The MCU's ring channel (capacity 3) will buffer them; excess
    messages may be dropped if the MCU hasn't polled in time.

    Example::

        from z_pull import build_config, publish_burst

        cfg = build_config("client", "", "")
        n = publish_burst(cfg, "demo/example/zenoh-pico-pub",
                          payload_prefix="hello", count=10, delay_sec=0.5)
        print(f"Published {n} messages")
    """
    published = 0
    try:
        with zenoh.open(config) as session:
            for i in range(count):
                payload = f"{payload_prefix} #{i + 1}"
                session.put(key, payload.encode("utf-8"))
                ts = time.strftime("%H:%M:%S")
                print(f"[{ts}] Published #{i + 1}: {payload}")
                published += 1
                if i < count - 1:
                    time.sleep(delay_sec)
    except Exception as e:
        print(f"✗ Publish failed: {e}", file=sys.stderr)
        return published

    print(f"Done — {published} message(s) published to '{key}'.")
    print("(MCU polls every 5 s and drains all buffered messages at once)")
    return published


# ============================================================================
# CLI entry point
# ============================================================================


def main():
    parser = argparse.ArgumentParser(
        description="Zenoh batch publisher — tests the ESP32-S3 "
                    "pull subscriber (z_pull.c)"
    )
    parser.add_argument(
        "message",
        nargs="*",
        help="Message prefix for each publication (default: 'pull-test'). "
             "Numbers are appended automatically.",
    )
    parser.add_argument(
        "--key",
        default="demo/example/zenoh-pico-pub",
        help="Key expression to publish on (default: %(default)s). "
             "The MCU subscribes to 'demo/example/**' so any "
             "sub-path matches.",
    )
    parser.add_argument(
        "-n",
        "--count",
        type=int,
        default=5,
        help="Number of messages to send (default: %(default)s)",
    )
    parser.add_argument(
        "-d",
        "--delay",
        type=float,
        default=1.0,
        help="Delay between messages in seconds (default: %(default)s)",
    )
    parser.add_argument(
        "--mode",
        default="client",
        choices=["client", "peer"],
        help="Zenoh session mode (default: %(default)s)",
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

    config = build_config(args.mode, args.connect, args.listen)

    payload_prefix = " ".join(args.message) if args.message else "pull-test"

    info = f"mode={args.mode}, key='{args.key}', count={args.count}"
    if args.connect:
        info += f", connect={args.connect}"
    if args.listen:
        info += f", listen={args.listen}"
    print(f"[Zenoh Pull-Test] {info}")

    count = publish_burst(
        config, args.key, payload_prefix=payload_prefix,
        count=args.count, delay_sec=args.delay,
    )
    sys.exit(0 if count > 0 else 1)


if __name__ == "__main__":
    main()
