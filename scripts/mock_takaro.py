#!/usr/bin/env python3
"""
Tiny stub of the Takaro game-server API for local end-to-end testing.

Endpoints implemented (matches what TakaroBridge talks to):
  POST /gameserver/register                 -> issue identityToken + gameServerId
  POST /gameserver/<id>/events              -> accept event batch
  GET  /gameserver/<id>/poll                -> return queued operations
  POST /gameserver/<id>/operation/<op>/result -> accept operation result

Operations can be queued via the admin endpoints:
  POST /admin/queue                          body = {action, args}
  GET  /admin/state                          dump received events + results
  POST /admin/clear                          reset state

Run: python3 mock_takaro.py [--port 8088]
"""
import argparse
import json
import threading
import time
import uuid
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

state_lock = threading.Lock()
state = {
    "events_received": [],         # list of TakaroEventBatch payloads
    "operations_queue": deque(),   # operations waiting to be polled
    "operation_results": {},       # opId -> result payload
    "registered_servers": {},      # gameServerId -> info
}


class Handler(BaseHTTPRequestHandler):
    server_version = "MockTakaro/0.1"

    def _read_body(self):
        n = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(n) if n else b""

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _text(self, code, msg=""):
        body = msg.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *a):
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] {self.command} {self.path} -> {fmt % a}")

    def do_GET(self):
        path = self.path.rstrip("/")
        with state_lock:
            if path == "/admin/state":
                snap = {
                    "events_received": state["events_received"],
                    "operations_pending": list(state["operations_queue"]),
                    "operation_results": state["operation_results"],
                    "registered_servers": state["registered_servers"],
                }
                self._json(200, snap)
                return

            if path.startswith("/gameserver/") and path.endswith("/poll"):
                ops = []
                while state["operations_queue"]:
                    ops.append(state["operations_queue"].popleft())
                self._json(200, {"operations": ops})
                return

        self._text(404, "Not Found")

    def do_POST(self):
        path = self.path.rstrip("/")
        body = self._read_body()
        try:
            payload = json.loads(body) if body else {}
        except json.JSONDecodeError:
            payload = {"_raw": body.decode("utf-8", "replace")}

        with state_lock:
            if path == "/admin/clear":
                state["events_received"].clear()
                state["operations_queue"].clear()
                state["operation_results"].clear()
                self._json(200, {"ok": True})
                return

            if path == "/admin/queue":
                op = {
                    "operationId": payload.get("operationId") or uuid.uuid4().hex,
                    "action": payload["action"],
                    "argsJson": json.dumps(payload.get("args", {})),
                }
                state["operations_queue"].append(op)
                self._json(200, {"ok": True, "operationId": op["operationId"]})
                return

            if path == "/gameserver/register":
                gid = "srv_" + uuid.uuid4().hex[:12]
                tok = "id_" + uuid.uuid4().hex
                state["registered_servers"][gid] = {
                    "registrationToken": payload.get("registrationToken"),
                    "serverName": payload.get("serverName"),
                    "gameType": payload.get("gameType"),
                    "registeredAt": time.time(),
                }
                self._json(200, {"identityToken": tok, "gameServerId": gid})
                return

            if path.startswith("/gameserver/") and path.endswith("/events"):
                state["events_received"].append({
                    "ts": time.time(),
                    "path": path,
                    "payload": payload,
                })
                self._json(200, {"ok": True})
                return

            if path.startswith("/gameserver/") and "/operation/" in path and path.endswith("/result"):
                # /gameserver/<id>/operation/<opId>/result
                parts = path.split("/")
                op_id = parts[-2]
                state["operation_results"][op_id] = {
                    "ts": time.time(),
                    "payload": payload,
                }
                self._json(200, {"ok": True})
                return

        self._text(404, "Not Found")


def run(port: int) -> None:
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"Mock Takaro listening on 0.0.0.0:{port}")
    print(f"  Admin: GET  /admin/state       — see what we've received")
    print(f"  Admin: POST /admin/queue       — queue an operation for the bridge")
    print(f"  Admin: POST /admin/clear       — reset state")
    srv.serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8088)
    args = ap.parse_args()
    run(args.port)
