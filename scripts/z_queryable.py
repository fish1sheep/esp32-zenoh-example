"""
Zenoh queryable — responds to GET queries from the ESP32-S3 Get client.

Python counterpart of ``main/z_queryable.c`` on the MCU side, but used
to VERIFY ``main/z_get.c`` — the ESP32's Get client sends ``z_get()``
every 5 seconds on ``demo/example/**``, and THIS script replies.

Usage — respond to queries from the ESP32::

    # Default: scouting (needs zenohd router in the network)
    python scripts/z_queryable.py

    # Direct TCP connection to the same router the ESP32 uses
    python scripts/z_queryable.py --connect tcp/<ROUTER_IP>:7447

    # Peer mode (no router)
    python scripts/z_queryable.py --mode peer

Typical flow::

    ESP32 (z_get.c)  ── GET("demo/example/**") ──→  PC (this script)
         ↑                                            │
         └────────── reply("Hello from PC!") ──────────┘

    ESP32 serial output:
        Sending Query 'demo/example/**'...
     >> Received ('demo/example/**': 'Hello from PC!')
"""

import argparse     # command-line argument parsing
import sys          # stderr / exit code
import time         # sleep() / timestamp formatting

import zenoh        # zenoh-python library


# ============================================================================
# Config builder — same signature as sibling scripts
# ============================================================================


def build_config(mode: str, connect: str, listen: str) -> zenoh.Config:
    """Build a Zenoh Config from CLI parameters.

    Consistent with ``z_pub.build_config()`` / ``z_sub.build_config()``.
    """
    cfg = zenoh.Config()
    cfg.insert_json5("mode", f'"{mode}"')
    if connect:
        cfg.insert_json5("connect/endpoints", f'["{connect}"]')
    if listen:
        cfg.insert_json5("listen/endpoints", f'["{listen}"]')
    return cfg


# ============================================================================
# Query handler
# ============================================================================


def _query_handler(query: zenoh.Query):
    """Called every time the ESP32's Get client sends a GET query.

    ``zenoh.Query`` provides:
      - ``query.key_expr``  — the key expression the query targeted
      - ``query.payload``   — optional payload sent with the query
      - ``query.reply(payload)`` — send a reply back
    """
    ts = time.strftime("%H:%M:%S")

    # ── Extract query info ───────────────────────────────────────────
    keystr = query.key_expr

    # If the query carried a payload, decode it
    payload_str = ""
    if query.payload:
        try:
            payload_str = query.payload.to_bytes().decode("utf-8", errors="replace")
        except Exception:
            payload_str = repr(query.payload)

    # ── Log ──────────────────────────────────────────────────────────
    if payload_str:
        print(f"[{ts}] ⇐ Query on '{keystr}' with payload: '{payload_str}'")
    else:
        print(f"[{ts}] ⇐ Query on '{keystr}' (no payload)")

    # ── Build a reply ────────────────────────────────────────────────
    reply_text = f"[Python] Hello from PC! Received your query on '{keystr}' @ {ts}"

    # query.reply() sends the response back to the Get client
    query.reply(f"demo/example/reply", reply_text.encode("utf-8"))

    print(f"[{ts}] ⇒ Replied: '{reply_text}'")


# ============================================================================
# Exportable helper — for scripting / module use
# ============================================================================


def serve(
    config: zenoh.Config,
    key: str = "demo/example/**",
) -> None:
    """Declare a queryable and block forever (responds to GET queries).

    Example::

        from z_queryable import build_config, serve

        cfg = build_config("client", "tcp/<ROUTER_IP>:7447", "")
        serve(cfg, key="demo/example/**")
    """
    print(f"Declaring Queryable on '{key}'...")

    with zenoh.open(config) as session:
        queryable = session.declare_queryable(key, _query_handler)
        print(f"✓ Queryable declared on '{key}'.")
        print("  Waiting for GET queries from ESP32 (z_get.c)...")
        print("  Press Ctrl+C to stop.\n")

        try:
            # Block forever — _query_handler runs on each incoming query
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("\n⏹  Shutting down…")
            queryable.undeclare()

    print("✓ Closed.")


# ============================================================================
# CLI entry point
# ============================================================================


def main():
    parser = argparse.ArgumentParser(
        description="Zenoh queryable — responds to GET queries from "
                    "the ESP32-S3 Get client (z_get.c)"
    )
    parser.add_argument(
        "--key",
        default="demo/example/**",
        help="Key expression to answer (default: %(default)s). "
             "Must match the ESP32's Get client key 'demo/example/**'.",
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
        help="Endpoint to connect to (default: empty = scouting). "
             "Use the same router IP the ESP32 connects to, e.g. "
             "tcp/<ROUTER_IP>:7447",
    )
    parser.add_argument(
        "--listen",
        default="",
        help="Endpoint to listen on in peer mode",
    )
    args = parser.parse_args()

    config = build_config(args.mode, args.connect, args.listen)

    info = f"mode={args.mode}, key='{args.key}'"
    if args.connect:
        info += f", connect={args.connect}"
    if args.listen:
        info += f", listen={args.listen}"
    print(f"[Zenoh Queryable] {info}")

    try:
        serve(config, key=args.key)
    except Exception as e:
        print(f"✗ Failed: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
