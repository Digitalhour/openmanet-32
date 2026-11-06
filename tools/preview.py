#!/usr/bin/env python3
"""Preview the meshpoint's web pages on a computer, without flashing a board.

Serves main/page.html (the dashboard), main/setup.html (the first-boot setup page) and their
files straight from the source folder, so an edit shows up on the next browser reload. A fake
device answers the API with made-up mesh data (nodes, signal, uptime) and the US channel
table.

    python3 tools/preview.py            # then open http://localhost:8765/
    python3 tools/preview.py --port 9000

Pages:
    /            the dashboard, signed in
    /?login      the dashboard's sign-in screen
    /setup       the setup wizard, signed in
    /setup?login the setup sign-in screen

Nothing is saved: Save buttons answer OK and the preview keeps running.
"""
import argparse
import json
import os
import random
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main")
STARTED = time.time() - 2 * 3600 - 17 * 60

# US channels from Morse Micro's regulatory database (the same rows as OpenMANET's
# /usr/share/morse-regdb/channels.csv): channel, centre frequency (Hz), width (MHz), op class.
US = [(1, 902500000, 1, 68), (3, 903500000, 1, 68), (5, 904500000, 1, 68), (7, 905500000, 1, 68),
      (9, 906500000, 1, 68), (11, 907500000, 1, 68), (13, 908500000, 1, 68), (15, 909500000, 1, 68),
      (17, 910500000, 1, 68), (19, 911500000, 1, 68), (21, 912500000, 1, 68), (23, 913500000, 1, 68),
      (25, 914500000, 1, 68), (27, 915500000, 1, 68), (29, 916500000, 1, 68), (31, 917500000, 1, 68),
      (33, 918500000, 1, 68), (35, 919500000, 1, 68), (37, 920500000, 1, 68), (39, 921500000, 1, 68),
      (41, 922500000, 1, 68), (43, 923500000, 1, 68), (45, 924500000, 1, 68), (47, 925500000, 1, 68),
      (49, 926500000, 1, 68), (51, 927500000, 1, 68), (2, 903000000, 2, 69), (6, 905000000, 2, 69),
      (10, 907000000, 2, 69), (14, 909000000, 2, 69), (18, 911000000, 2, 69), (22, 913000000, 2, 69),
      (26, 915000000, 2, 69), (30, 917000000, 2, 69), (34, 919000000, 2, 69), (38, 921000000, 2, 69),
      (42, 923000000, 2, 69), (46, 925000000, 2, 69), (50, 927000000, 2, 69), (8, 906000000, 4, 70),
      (16, 910000000, 4, 70), (24, 914000000, 4, 70), (32, 918000000, 4, 70), (40, 922000000, 4, 70),
      (48, 926000000, 4, 70), (12, 908000000, 8, 71), (28, 916000000, 8, 71), (44, 924000000, 8, 71)]
CHANNELS = [{"chan": c, "mhz": f / 1e6, "bw": bw, "op": op} for c, f, bw, op in US]

RADIO = {"chan": 28, "prim_bw": 0, "prim_idx": -1, "beacon": 1000, "tx_dbm": 8, "ap_chan": 6}

# A pretend speed test: about 0.2 Mbit/s from a node, like a relayed HaLow hop.
TEST = {"state": "idle", "url": "", "start": 0.0, "seconds": 10}


def speedtest():
    if TEST["state"] == "idle":
        return {"state": "idle", "url": "", "bytes": 0, "ms": 0, "seconds": 0, "downloads": 0, "mbps": 0, "message": ""}
    ms = int(min(time.time() - TEST["start"], TEST["seconds"]) * 1000)
    mbps = 0.21 + random.uniform(-0.03, 0.03)
    bytes_ = int(mbps * 1e6 / 8 * ms / 1000)
    state = "running" if ms < TEST["seconds"] * 1000 else "done"
    return {"state": state, "url": TEST["url"], "bytes": bytes_, "ms": ms, "seconds": TEST["seconds"],
            "downloads": bytes_ // 221432, "mbps": round(mbps, 3), "message": ""}


PEERS = [("5c:ba:75:00:71:ec", -48, "peered"), ("5c:ba:75:00:72:07", -61, "peered"),
         ("5c:ba:75:00:73:10", -84, "listen")]


def status(setup):
    ch = next(c for c in CHANNELS if c["chan"] == RADIO["chan"])
    pbw = RADIO["prim_bw"] or (2 if ch["bw"] in (4, 8) else 1)
    pidx = RADIO["prim_idx"] if RADIO["prim_idx"] >= 0 else (ch["bw"] - 1) // 2
    st = {
        "can_edit": True, "setup": setup, "mesh_id": "" if setup else "MY-MESH", "mesh_secure": True,
        "ap_ssid": "Meshpoint-01C5", "saved": not setup, "uptime_s": int(time.time() - STARTED),
        "ap_ip": "192.168.4.1", "ap_clients": 1,
        "radio": {"chan": ch["chan"], "mhz": ch["mhz"], "bw": ch["bw"], "prim_bw": pbw, "prim_idx": pidx,
                  "ap_chan": RADIO["ap_chan"]},
        "peers": [], "nodes": [],
    }
    if not setup:
        st["mesh_ip"] = "10.41.250.251"
        st["peers"] = [{"mac": m, "rssi": r + random.randint(-3, 3), "state": s, "peered": s == "peered"}
                       for m, r, s in PEERS]
        st["nodes"] = [{"mac": m, "via": m, "mbps": 10} for m, _, s in PEERS if s == "peered"]
    return st


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def file(self, name, ctype):
        with open(os.path.join(MAIN, name), "rb") as f:
            self.send(200, f.read(), ctype)

    def referer(self):
        return self.headers.get("Referer") or ""

    def need_login(self):
        return "?login" in self.referer()

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/":
            return self.file("page.html", "text/html; charset=utf-8")
        if path == "/setup":
            return self.file("setup.html", "text/html; charset=utf-8")
        if path == "/board.webp":
            return self.file("board.webp", "image/webp")
        if path == "/radio.js":
            return self.file("radio.js", "text/javascript")
        if path in ("/api/status", "/api/radio"):
            if self.need_login():
                return self.send(401, json.dumps({"ok": False, "login": True, "message": "Sign in first"}))
            if path == "/api/radio":
                return self.send(200, json.dumps({"country": "US", "ap_max_chan": 11, "channels": CHANNELS,
                                                  "saved": RADIO}))
            return self.send(200, json.dumps(status("/setup" in self.referer())))
        if path == "/api/speedtest":
            return self.send(200, json.dumps(speedtest()))
        if path == "/api/speedtest/data":
            # Stream zeros at about 4 Mbit/s, like a phone on the 2.4 GHz hotspot.
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.end_headers()
            try:
                for _ in range(2000):
                    self.wfile.write(bytes(16384))
                    time.sleep(16384 * 8 / 4e6)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        self.send(404, "not found", "text/plain")

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length)
        if self.path == "/api/login":
            return self.send(401, json.dumps({"ok": False, "message":
                                              "Preview: sign-in isn't simulated. Open the page without ?login."}))
        if self.path == "/api/speedtest":
            try:
                req = json.loads(body or b"{}")
            except ValueError:
                req = {}
            ip = str(req.get("ip", ""))
            TEST.update(state="running", url="http://%s%s" % (ip, req.get("path") or "/"), start=time.time(),
                        seconds=int(req.get("seconds") or 10))
            return self.send(200, json.dumps({"ok": True, "message": "Started"}))
        if self.path == "/api/speedtest/sink":
            time.sleep(len(body) * 8 / 3e6)   # about 3 Mbit/s up
            return self.send(200, json.dumps({"ok": True, "bytes": len(body)}))
        if self.path == "/api/settings":
            try:
                radio = json.loads(body or b"{}").get("radio")
                if isinstance(radio, dict):
                    RADIO.update({k: v for k, v in radio.items() if k in RADIO})
            except ValueError:
                pass
        self.send(200, json.dumps({"ok": True, "message": "Preview: nothing was saved."}))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()
    print("Preview at http://localhost:%d/ (dashboard), /setup (setup page); add ?login for sign-in" % args.port)
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
