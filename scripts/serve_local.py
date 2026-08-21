"""Fieldline local bridge — run the dashboard on http://localhost:8123

Serves index.html + images locally and proxies /data to the ESP32 node,
finding the node automatically (last-known IP, then mDNS, then a scan of
every local subnet). If the node's IP changes, the bridge notices the
failures and rediscovers it — the browser never needs to know an IP.

Usage:  python scripts/serve_local.py        (or double-click run_dashboard.bat)
"""
import json
import socket
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PORT = 8123
IP_FILE = ROOT / ".node_ip"          # remembers the last node address

node_ip = None                        # current node address, None while searching
_lock = threading.Lock()
_fails = 0


def is_node(ip, timeout=1.5):
    """True if http://ip/data answers with Fieldline JSON."""
    try:
        with urllib.request.urlopen(f"http://{ip}/data", timeout=timeout) as r:
            d = json.loads(r.read())
            return isinstance(d, dict) and "node" in d
    except Exception:
        return False


def local_subnets():
    """/24 prefixes of every non-loopback IPv4 interface on this machine."""
    prefixes = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith(("127.", "169.254.")):
                prefixes.add(ip.rsplit(".", 1)[0])
    except socket.gaierror:
        pass
    return sorted(prefixes)


def port80_open(ip):
    try:
        s = socket.create_connection((ip, 80), timeout=0.5)
        s.close()
        return ip
    except OSError:
        return None


def discover():
    """Find the node: saved IP -> mDNS -> scan all local subnets."""
    candidates = []
    if IP_FILE.exists():
        candidates.append(IP_FILE.read_text().strip())
    candidates.append("fieldline-node.local")
    for c in candidates:
        if c and is_node(c):
            return c
    for prefix in local_subnets():
        print(f"[bridge] scanning {prefix}.0/24 ...")
        ips = [f"{prefix}.{i}" for i in range(1, 255)]
        with ThreadPoolExecutor(64) as ex:
            hosts = [r for r in ex.map(port80_open, ips) if r]
        for ip in hosts:
            if is_node(ip):
                return ip
    return None


def watcher():
    """Keep node_ip valid; rediscover whenever the node stops answering."""
    global node_ip, _fails
    while True:
        if node_ip is None:
            found = discover()
            if found:
                with _lock:
                    node_ip = found
                    _fails = 0
                IP_FILE.write_text(found)
                print(f"[bridge] node found at {found}")
            else:
                print("[bridge] no node found, retrying in 5 s")
                time.sleep(5)
        else:
            time.sleep(5)
            if not is_node(node_ip, timeout=2):
                with _lock:
                    _fails += 1
                    if _fails >= 3:
                        print(f"[bridge] lost {node_ip}, rediscovering")
                        node_ip = None
            else:
                with _lock:
                    _fails = 0


STATIC = {
    "/": ("index.html", "text/html", "no-cache"),
    "/index.html": ("index.html", "text/html", "no-cache"),
    "/bg.webp": ("bg.webp", "image/webp", "max-age=604800"),
    "/crop.webp": ("crop.webp", "image/webp", "max-age=604800"),
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype, cache="no-store"):
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", cache)
            self.end_headers()
            self.wfile.write(body)
        except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
            pass  # browser gave up on the request (e.g. poll timeout) — harmless

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in STATIC:
            name, ctype, cache = STATIC[path]
            self._send(200, (ROOT / name).read_bytes(), ctype, cache)
        elif path == "/data":
            ip = node_ip
            if not ip:
                self._send(503, b'{"error":"searching for node"}', "application/json")
                return
            try:
                with urllib.request.urlopen(f"http://{ip}/data", timeout=2) as r:
                    self._send(200, r.read(), "application/json")
            except Exception:
                self._send(502, b'{"error":"node not responding"}', "application/json")
        elif path == "/status":
            body = json.dumps({"node": node_ip}).encode()
            self._send(200, body, "application/json")
        else:
            self._send(404, b"not found", "text/plain")


if __name__ == "__main__":
    threading.Thread(target=watcher, daemon=True).start()
    print(f"[bridge] dashboard on http://localhost:{PORT}  (Ctrl+C to stop)")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
