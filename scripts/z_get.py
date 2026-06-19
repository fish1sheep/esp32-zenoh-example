"""
Zenoh querier — sends GET queries verifying the ESP32-S3 queryable.

Python counterpart of the ESP32-S3 ``main/z_queryable.c`` example.
Opens a Zenoh session, queries the key expression that the MCU
queryable listens on, and prints each reply.

Usage — query the default key expression::

    python scripts/z_get.py
    python scripts/z_get.py "Hello MCU, are you alive?"
    python scripts/z_get.py --mode peer --timeout 5.0
"""

import argparse     # command-line argument parsing
import sys          # stderr / exit code
import time         # timestamp formatting

import zenoh        # zenoh-python library


# ============================================================================
# Config builder — same signature as z_pub.py / z_sub.py
# ============================================================================


def build_config(mode: str, connect: str, listen: str) -> zenoh.Config:
    """Build a Zenoh Config from CLI parameters.

    Consistent with ``z_pub.build_config()`` and ``z_sub.build_config()``.
    """
    cfg = zenoh.Config()
    cfg.insert_json5("mode", f'"{mode}"')
    if connect:
        cfg.insert_json5("connect/endpoints", f'["{connect}"]')
    if listen:
        cfg.insert_json5("listen/endpoints", f'["{listen}"]')
    return cfg


# ============================================================================
# Exportable helper — for scripting / module use
# ============================================================================


def query(
    config: zenoh.Config,
    key: str,
    payload: str | None = None,
    timeout_sec: float = 3.0,
) -> int:
    """Send a Zenoh GET query and print replies.

    Returns the number of successful replies received.

    Example::

        from z_get import build_config, query

        cfg = build_config("client", "", "")
        n = query(cfg, "demo/example/zenoh-pico-queryable",
                  payload="ping", timeout_sec=3.0)
        print(f"Got {n} reply(s)")
    """
    replies_received = 0

    def _reply_handler(reply: zenoh.Reply):
        nonlocal replies_received
        ts = time.strftime("%H:%M:%S")

        if reply.ok:
            # Successful reply — extract and print payload
            sample = reply.result
            keystr = sample.key_expr
            payload_bytes = sample.payload
            payload_str = payload_bytes.to_bytes().decode(
                "utf-8", errors="replace"
            )
            print(f"[{ts}] Reply from {keystr}: {payload_str}")
            print(f"       replier ZID: {reply.replier_id}")
            replies_received += 1
        else:
            # Error reply (e.g. no queryable found, router not available)
            print(f"[{ts}] Error: {reply.err}")

    # Build a selector for session.get()
    selector = zenoh.Selector(key)

    print(f"Querying '{key}'...")
    try:
        with zenoh.open(config) as session:
            session.get(
                selector,
                handler=_reply_handler,
                payload=payload.encode("utf-8") if payload else None,
                timeout=timeout_sec,
            )
            # session.get() is non-blocking — keep the session alive
            # long enough for replies to arrive asynchronously.
            time.sleep(timeout_sec)
    except Exception as e:
        print(f"✗ Query failed: {e}", file=sys.stderr)
        return 0

    if replies_received == 0:
        print("No reply received (timeout or no queryable registered).")
    else:
        print(f"Done — {replies_received} reply(s) received.")

    return replies_received


# ============================================================================
# CLI entry point
# ============================================================================


def main():
    parser = argparse.ArgumentParser(
        description="Zenoh querier — verifies the ESP32-S3 queryable (z_queryable.c)"
    )
    parser.add_argument(
        "message",
        nargs="*",
        help="Optional query payload text.",
    )
    parser.add_argument(
        "--key",
        default="demo/example/zenoh-pico-queryable",
        help="Key expression to query (default: %(default)s)",
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
    parser.add_argument(
        "--timeout",
        type=float,
        default=3.0,
        help="Query timeout in seconds (default: %(default)s)",
    )
    args = parser.parse_args()

    config = build_config(args.mode, args.connect, args.listen)

    payload = " ".join(args.message) if args.message else None

    info = f"mode={args.mode}, key='{args.key}'"
    if args.connect:
        info += f", connect={args.connect}"
    if args.listen:
        info += f", listen={args.listen}"
    print(f"[Zenoh GET] {info}")

    try:
        count = query(
            config, args.key, payload=payload, timeout_sec=args.timeout
        )
        sys.exit(0 if count > 0 else 1)
    except Exception as e:
        print(f"✗ Failed: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
