#!/usr/bin/env python3
"""Beat Hub: turns the Beat Gateway's USB beats into Ableton Link, OSC and a live web page / JSON feed.

Plug a XIAO ESP32-S3 running the Beat Gateway firmware into this computer (a Pi, or a laptop) and run this script.
Every beat from the Beat Commander link then:
  - moves the Ableton Link session (tempo + bar phase): Ableton, Traktor, Resolume, Serato, phone apps...
  - sends OSC messages to the targets in config.ini (TouchDesigner, Resolume, QLC+, your own code)
  - shows on http://<this-machine>:8090 and in /api/state as JSON (TrackID reads this)
MIDI clock doesn't need this script: the gateway is a USB MIDI device by itself.

Built by Anubis Darkwatch with Claude. MIT license.
"""
import argparse
import asyncio
import configparser
import glob
import json
import logging
import os
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
    from serial.tools import list_ports
except ImportError:                      # pragma: no cover
    serial = None
try:
    import aalink
except ImportError:                      # Link is optional (no wheel for 32-bit ARM)
    aalink = None

VERSION = "1.1"
APP_DIR = os.path.dirname(os.path.abspath(__file__))
log = logging.getLogger("beat-hub")

DEFAULTS = {
    "gateway": {"port": "auto"},
    "link": {"enabled": "true", "quantum": "4"},
    "osc": {"targets": "", "prefix": "/beat"},
    "web": {"enabled": "true", "port": "8090"},
    "timing": {"offset_ms": "0"},
}


def load_config(path):
    cfg = configparser.ConfigParser(inline_comment_prefixes=("#", ";"))
    cfg.read_dict(DEFAULTS)
    cfg.read(path)
    return cfg


# ---------------------------------------------------------------------------------------------- OSC
def _osc_str(s):
    b = s.encode() + b"\0"
    return b + b"\0" * (-len(b) % 4)


def osc_message(addr, *args):
    tags, data = ",", b""
    for a in args:
        if isinstance(a, float):
            tags += "f"; data += struct.pack(">f", a)
        else:
            tags += "i"; data += struct.pack(">i", int(a))
    return _osc_str(addr) + _osc_str(tags) + data


class Osc:
    def __init__(self, targets, prefix):
        self.targets = []
        for t in targets.replace(";", ",").split(","):
            t = t.strip()
            if not t:
                continue
            host, _, port = t.rpartition(":")
            if host and port.isdigit():
                self.targets.append((host, int(port)))
            else:
                log.warning("osc target %r should look like 127.0.0.1:9000", t)
        self.prefix = "/" + prefix.strip("/") if prefix.strip("/") else ""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send(self, addr, *args):
        msg = osc_message(self.prefix + addr, *args)
        for t in self.targets:
            try:
                self.sock.sendto(msg, t)
            except OSError as exc:
                log.debug("osc %s: %s", t, exc)

    def beat(self, n, bpm, bar):
        if not self.targets:
            return
        self.send("", n, float(bpm))            # /beat  <1-4> <bpm>
        self.send(f"/{n}", 1)                    # /beat/1 ... /beat/4  (one trigger per beat number)
        self.send("/bpm", float(bpm))
        if n == 1:
            self.send("/bar", bar)               # /beat/bar <bar count>


# ---------------------------------------------------------------------------------------------- Link
class Link:
    """Keeps an Ableton Link session on the conductor's tempo and bar phase."""
    FIX_BEATS = 0.02                             # only move the session when it's off by more than this

    def __init__(self, quantum):
        self.link = None
        self.quantum = quantum
        if aalink is None:
            log.warning("Ableton Link off: the aalink package isn't installed (pip install aalink)")
            return
        self.link = aalink.Link(120.0)
        self.link.quantum = quantum
        self.link.enabled = True

    @property
    def peers(self):
        return self.link.num_peers if self.link else None

    def beat(self, n, bpm, since_s):
        """Beat number n (1..quantum) happened since_s seconds ago at bpm."""
        if not self.link:
            return
        if abs(self.link.tempo - bpm) > 0.01:
            self.link.tempo = bpm
        now_beat = self.link.beat
        ahead = since_s * bpm / 60.0             # beats elapsed since that beat
        q = self.quantum
        k = (n - 1) % q
        base = now_beat - ahead                  # where Link thinks that beat was
        target_at_beat = round((base - k) / q) * q + k
        target = target_at_beat + ahead
        if abs(target - now_beat) > self.FIX_BEATS:
            self.link.force_beat(target)


# ---------------------------------------------------------------------------------------------- state
class Hub:
    def __init__(self, cfg):
        self.cfg = cfg
        self.lock = threading.Lock()
        self.offset = cfg.getfloat("timing", "offset_ms") / 1000.0
        self.osc = Osc(cfg.get("osc", "targets"), cfg.get("osc", "prefix"))
        self.link = None
        self.connected = False
        self.port = ""
        self.fw = ""
        self.status = "WAIT"
        self.clock = "STOP"
        self.bridge = "-"
        self.bpm = 0.0
        self.beat_n = 0
        self.beat_t = 0.0                        # time.monotonic() of the last beat
        self.source = "-"
        self.beats = 0
        self.bars = 0
        self.rssi = None
        self.started = time.time()

    # called on the asyncio loop
    def on_line(self, line, t_read):
        p = line.split(",")
        if p[0] == "B" and len(p) >= 5:
            try:
                n, bpm, src, age_us = int(p[1]), float(p[2]), p[3], int(p[4])
            except ValueError:
                return
            t_beat = t_read - age_us / 1e6 + self.offset
            with self.lock:
                self.bpm, self.beat_n, self.beat_t, self.source = bpm, n, t_beat, src
                self.beats += 1
                if n == 1:
                    self.bars += 1
                if src == "AIR" and len(p) >= 8:
                    self.rssi = int(p[7])
                if self.status == "WAIT":
                    self.status = "LOCK"
                bars = self.bars
            if self.link:
                self.link.beat(n, bpm, time.monotonic() - t_beat)
            self.osc.beat(n, bpm, bars)
        elif p[0] == "S" and len(p) >= 7:
            with self.lock:
                self.status, self.clock, self.bridge = p[1], p[5], p[6]
                if p[1] == "WAIT":
                    self.bpm = 0.0
        elif line.startswith("# Beat Gateway v"):
            self.fw = line[2:].split("|")[0].strip()
            log.info("%s", line[2:])
        elif line.startswith("#"):
            log.info("gateway: %s", line[2:])

    def state(self):
        with self.lock:
            age = time.monotonic() - self.beat_t if self.beat_t else None
            locked = self.connected and self.status in ("LOCK", "FREE") and age is not None and age < 3.0
            return {
                "version": VERSION, "gateway": {"connected": self.connected, "port": self.port, "firmware": self.fw},
                "status": self.status if self.connected else "NO GATEWAY", "locked": locked,
                "bpm": round(self.bpm, 1) if locked else None, "beat": self.beat_n if locked else None,
                "beat_age_ms": round(age * 1000) if locked else None, "source": self.source if locked else None,
                "rssi": self.rssi, "beats": self.beats, "bars": self.bars, "midi_clock": self.clock,
                "bridge": self.bridge,
                "link": {"enabled": bool(self.link and self.link.link), "peers": self.link.peers if self.link else None},
                "osc_targets": [f"{h}:{p}" for h, p in self.osc.targets],
            }


# ---------------------------------------------------------------------------------------------- serial
def find_port():
    if serial is None:
        return None
    for p in list_ports.comports():
        text = " ".join(str(x) for x in (p.product, p.description, p.manufacturer, p.interface) if x)
        if "Beat Gateway" in text or "Anubis Darkwatch" in text:
            return p.device
    ids = glob.glob("/dev/serial/by-id/*Beat_Gateway*")
    if ids:
        return ids[0]
    # T-Dongle C5 Beat Light: the C5's built-in USB serial has Espressif's generic name. Any one will do as a
    # fallback; set [gateway] port in the config to pin a specific device if more than one ESP32 is plugged in.
    for p in list_ports.comports():
        if p.vid == 0x303A and "JTAG" in " ".join(str(x) for x in (p.product, p.description) if x):
            return p.device
    return None


def serial_reader(hub, loop, port_cfg, stop):
    while not stop.is_set():
        port = find_port() if port_cfg == "auto" else port_cfg
        if not port:
            time.sleep(2)
            continue
        try:
            with serial.Serial(port, 115200, timeout=1) as s:
                with hub.lock:
                    hub.connected, hub.port = True, port
                log.info("gateway on %s", port)
                s.write(b"?\n")
                while not stop.is_set():
                    raw = s.readline()
                    t = time.monotonic()
                    if raw:
                        line = raw.decode(errors="replace").strip()
                        if line:
                            loop.call_soon_threadsafe(hub.on_line, line, t)
        except (serial.SerialException, OSError) as exc:
            log.warning("gateway %s: %s", port, exc)
        with hub.lock:
            hub.connected = False
        time.sleep(2)


# ---------------------------------------------------------------------------------------------- web
PAGE = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Beat Hub</title>
<link href="https://fonts.googleapis.com/css2?family=Orbitron:wght@500;800&family=Share+Tech+Mono&display=swap" rel="stylesheet">
<style>
:root{--bg:#050505;--amber:#ffb000;--green:#39ff14;--dim:#6b5a2e;--line:#2a2210}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--amber);font-family:'Share Tech Mono',monospace;
min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}
.card{width:100%;max-width:520px;border:1px solid var(--line);border-radius:14px;padding:22px;background:#0a0804}
h1{font-family:Orbitron,sans-serif;font-weight:800;font-size:18px;letter-spacing:.18em;margin:0 0 18px;display:flex;justify-content:space-between}
h1 span{color:var(--dim);font-size:12px;letter-spacing:.1em;align-self:center}
.bpm{font-family:Orbitron,sans-serif;font-weight:800;font-size:clamp(64px,22vw,120px);line-height:1;text-align:center;color:var(--green);
text-shadow:0 0 18px rgba(57,255,20,.35)}.bpm.off{color:var(--dim);text-shadow:none}
.unit{text-align:center;color:var(--dim);letter-spacing:.3em;margin:4px 0 18px}
.dots{display:flex;gap:10px;justify-content:center;margin-bottom:20px}.dot{width:44px;height:14px;border-radius:7px;background:var(--line);transition:background .08s}
.dot.on{background:var(--amber);box-shadow:0 0 14px var(--amber)}.dot.on.one{background:var(--green);box-shadow:0 0 14px var(--green)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px 16px;font-size:14px}.grid div{display:flex;justify-content:space-between;
border-bottom:1px solid var(--line);padding:6px 0}.grid b{color:#ffd27a;font-weight:normal}
.foot{margin-top:16px;color:var(--dim);font-size:12px;text-align:center}
</style></head><body><div class="card">
<h1>BEAT HUB <span>&#x130E3; ANUBIS DARKWATCH</span></h1>
<div class="bpm off" id="bpm">---</div><div class="unit">BPM</div>
<div class="dots"><div class="dot one"></div><div class="dot"></div><div class="dot"></div><div class="dot"></div></div>
<div class="grid">
<div>STATUS <b id="st">-</b></div><div>SOURCE <b id="src">-</b></div>
<div>LINK PEERS <b id="peers">-</b></div><div>MIDI CLOCK <b id="clk">-</b></div>
<div>BRIDGE <b id="br">-</b></div><div>BARS <b id="bars">0</b></div>
</div><div class="foot" id="foot">waiting for the gateway</div></div>
<script>
const $=id=>document.getElementById(id),dots=[...document.querySelectorAll('.dot')];let s=null,got=0;
async function poll(){try{const r=await fetch('api/state',{cache:'no-store'});s=await r.json();got=performance.now();paint()}catch(e){}}
function paint(){$('st').textContent=s.status;$('src').textContent=s.source||'-';
$('peers').textContent=s.link.enabled?s.link.peers:'off';$('clk').textContent=s.midi_clock;$('br').textContent=s.bridge;
$('bars').textContent=s.bars;$('bpm').textContent=s.bpm?s.bpm.toFixed(1):'---';$('bpm').classList.toggle('off',!s.locked);
$('foot').textContent=s.gateway.connected?(s.gateway.firmware||'Beat Gateway')+' on '+s.gateway.port:'plug in the Beat Gateway';}
function frame(){if(s&&s.locked){const per=60000/s.bpm,el=s.beat_age_ms+(performance.now()-got),k=Math.floor(el/per),
n=((s.beat-1+k)%4+4)%4,ph=(el%per)/per;dots.forEach((d,i)=>d.classList.toggle('on',i===n&&ph<0.35))}
else dots.forEach(d=>d.classList.remove('on'));requestAnimationFrame(frame)}
poll();setInterval(poll,400);frame();
</script></body></html>"""


def start_web(hub, port):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?")[0]
            if path == "/api/state":
                self._send(200, json.dumps(hub.state()).encode(), "application/json")
            elif path in ("/", "/index.html"):
                self._send(200, PAGE.encode(), "text/html; charset=utf-8")
            else:
                self._send(404, b"not found", "text/plain")

    srv = ThreadingHTTPServer(("0.0.0.0", port), H)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log.info("web page on http://%s:%d", socket.gethostname(), port)


# ---------------------------------------------------------------------------------------------- main
async def main_async(cfg):
    hub = Hub(cfg)
    if cfg.getboolean("link", "enabled"):
        hub.link = Link(max(1, cfg.getint("link", "quantum")))
    if hub.osc.targets:
        log.info("OSC to %s (prefix %s)", ", ".join(f"{h}:{p}" for h, p in hub.osc.targets), hub.osc.prefix or "/")
    if cfg.getboolean("web", "enabled"):
        start_web(hub, cfg.getint("web", "port"))
    if serial is None:
        log.error("pyserial missing: pip install pyserial (or apt install python3-serial)")
        return
    stop = threading.Event()
    loop = asyncio.get_running_loop()
    threading.Thread(target=serial_reader, args=(hub, loop, cfg.get("gateway", "port").strip(), stop),
                     daemon=True).start()
    log.info("Beat Hub v%s waiting for the Beat Gateway", VERSION)
    try:
        while True:
            await asyncio.sleep(3600)
    finally:
        stop.set()


def main():
    ap = argparse.ArgumentParser(description="Beat Hub: Beat Gateway -> Ableton Link / OSC / web")
    ap.add_argument("-c", "--config", default=os.path.join(APP_DIR, "config.ini"))
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, format="%(asctime)s %(message)s",
                        datefmt="%H:%M:%S")
    try:
        asyncio.run(main_async(load_config(a.config)))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
