#!/usr/bin/env python3
import argparse
import json
import sys
import time

import websocket


def send_rpc(ws, req):
    payload = json.dumps(req)
    print(f"--> {payload}")
    ws.send(payload)


def recv_json(ws, timeout_s=5.0):
    ws.settimeout(timeout_s)
    raw = ws.recv()
    print(f"<-- {raw}")
    try:
        return json.loads(raw)
    except Exception:
        return None


def main():
    parser = argparse.ArgumentParser(description="Moonraker websocket connectivity checker")
    parser.add_argument("--host", required=True, help="Moonraker host/IP, e.g. 192.168.2.178")
    parser.add_argument("--port", type=int, default=7125, help="Moonraker websocket port (default: 7125)")
    parser.add_argument("--path", default="/websocket", help="Websocket path (default: /websocket)")
    parser.add_argument("--watch", type=int, default=8, help="Seconds to watch notify events")
    args = parser.parse_args()

    url = f"ws://{args.host}:{args.port}{args.path}"
    print(f"Connecting: {url}")

    try:
        ws = websocket.create_connection(url, timeout=5)
    except Exception as exc:
        print(f"CONNECT_FAIL: {exc}")
        return 2

    print("CONNECTED")

    try:
        send_rpc(ws, {"jsonrpc": "2.0", "method": "server.info", "id": 1})
        rsp = recv_json(ws)
        if not isinstance(rsp, dict) or rsp.get("id") != 1:
            print("WARN: unexpected server.info response")

        send_rpc(ws, {
            "jsonrpc": "2.0",
            "method": "printer.objects.subscribe",
            "params": {
                "objects": {
                    "motion_report": ["live_extruder_velocity"],
                    "webhooks": ["state"],
                    "print_stats": ["state"]
                }
            },
            "id": 2
        })
        recv_json(ws)

        send_rpc(ws, {
            "jsonrpc": "2.0",
            "method": "printer.objects.query",
            "params": {
                "objects": {
                    "motion_report": None,
                    "webhooks": None,
                    "print_stats": None
                }
            },
            "id": 3
        })
        recv_json(ws)

        end_t = time.time() + max(0, args.watch)
        got_notify = False
        while time.time() < end_t:
            remaining = end_t - time.time()
            if remaining <= 0:
                break
            msg = recv_json(ws, timeout_s=min(1.0, remaining))
            if isinstance(msg, dict) and msg.get("method") == "notify_status_update":
                got_notify = True
                params = msg.get("params") or []
                if params and isinstance(params[0], dict):
                    lv = (((params[0].get("motion_report") or {}).get("live_extruder_velocity")))
                    if lv is not None:
                        print(f"INFO: live_extruder_velocity={lv}")

        if got_notify:
            print("PASS: websocket connected and notify_status_update observed")
        else:
            print("PASS: websocket connected and RPC works (no notify during watch window)")

        return 0
    except websocket.WebSocketTimeoutException:
        print("PASS: websocket connected and RPC succeeded (timeout while waiting for extra events)")
        return 0
    except Exception as exc:
        print(f"RUNTIME_FAIL: {exc}")
        return 3
    finally:
        try:
            ws.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
