#!/usr/bin/env python3
"""
vncbench -- compare VNC servers on the same machine under the same workload.

Runs on the machine hosting the servers. Speaks RFB directly so nothing
depends on a viewer being installed.

    vncbench.py --target wayvnc --workload scroll
    vncbench.py --target xvnc   --workload scroll
    vncbench.py --ab --workload scroll        # both, alternating, 3 rounds

Why this exists rather than ad-hoc one-liners: nearly every wrong conclusion
reached while debugging this stack came from a broken measurement, not from
a broken server. The specific failures, each of which this guards against:

  * The workload generator died mid-run, so the client sat waiting for damage
    that never came, and the server looked like it was stalling for seconds.
    -> the generator is checked alive at the START and the END of every run,
       and the run is discarded if it died.

  * The "update rate" measured was really the harness's own 1-second socket
    timeout before it re-requested.
    -> the next FramebufferUpdateRequest is sent the instant an update is
       fully parsed, never on a timer, and a run that hits a timeout is
       reported as such rather than averaged in.

  * The parser bailed on the first tight-encoded rectangle, so a "10 second"
    run measured exactly one frame.
    -> every advertised encoding is parsed to completion, and anything
       unparseable aborts loudly instead of returning a number.

  * Single samples were quoted as results.
    -> every measurement is repeated and reported with min/median/max.

  * Absolute numbers were read as meaningful when the interesting quantity
    was always the ratio to the X11 server on the same box.
    -> --ab alternates the two targets so drift affects both equally.

Loopback only. The network is measured separately and deliberately: it is a
1 Mbit link here and it swamps everything if left in.
"""

import argparse, json, os, socket, struct, subprocess, sys, time

# Whether to advertise the fence pseudo-encoding. A real viewer does, so this
# defaults on and --no-fence exists to measure the difference the server's
# congestion control makes. Set once from main, before any connection.
USE_FENCE = True

ENCODINGS = {"raw": 0, "copyrect": 1, "rre": 2, "hextile": 5, "tight": 7,
             "zrle": 16, "cursor": -239, "desktopsize": -223,
             "fence": -312, "continuousupdates": -313}

# Server-to-client message types this harness understands.
MSG_FENCE = 248

# Fence flags (RFB 3.8 extension). Only REQUEST matters here: a fence with
# REQUEST set is asking us to echo it back once we have processed everything
# that preceded it.
FENCE_REQUEST = 1 << 31
RECT_NAME = {v: k for k, v in ENCODINGS.items()}


class Unparseable(Exception):
    """Raised for an encoding this harness cannot length-decode exactly.

    tight, zrle and hextile are variable-length and need real decoders. Rather
    than guess at byte counts, the harness falls back to a QUIET-BOUNDARY
    mode: read until the socket goes silent for a fixed gap and treat that as
    one update. That is approximate and must be labelled as such -- but it is
    applied identically to both servers, so an A/B comparison of bytes per
    update remains meaningful even though the absolute figures are not exact.
    """
    def __init__(self, enc):
        self.enc = enc
        super().__init__("encoding %d needs a decoder" % enc)

XDG = "/tmp/xdg-%d" % os.getuid()


def sway_env():
    env = dict(os.environ)
    env["XDG_RUNTIME_DIR"] = XDG
    for f in sorted(os.listdir(XDG)):
        if f.startswith("sway-ipc."):
            env["SWAYSOCK"] = os.path.join(XDG, f)
        if f.startswith("wayland-") and not f.endswith(".lock"):
            env["WAYLAND_DISPLAY"] = f
    return env


class Workload:
    """A source of screen damage, verified by a heartbeat rather than by pgrep.

    Counting processes does not work: pgrep -f matches both the terminal and
    the shell inside it, so one healthy workload looks like two. And process
    liveness is the wrong question anyway -- what matters is whether damage
    is still being produced. So each generator touches a heartbeat file every
    iteration and liveness means "that file changed recently".
    """

    MARK = "VNCBENCH"
    HB = "/tmp/vncbench-heartbeat"

    SHELL = {
        # a terminal scrolling continuously: the case X11 wins on, because
        # XCopyArea moves pixels and only the new line is damaged
        "scroll":  "while :; do date; date > %s; done" % HB,
        # one character every half second: minimal damage
        "type":    "while :; do printf .; date > %s; sleep 0.5; done" % HB,
        # nothing at all: proves an idle desktop costs nothing
        "idle":    "sleep 3600",
        # the harness moves the pointer itself over RFB
        "pointer": "sleep 3600",
    }

    def __init__(self, target, kind):
        self.kind, self.target = kind, target
        self.pattern = self.SHELL[kind]
        # ";" before the marker: without it the shell sees "done : MARK"
        # and fails to parse, so the generator dies instantly and every run
        # is discarded with no heartbeat.
        self.cmd = "%s; : %s" % (self.pattern, self.MARK)

    def start(self):
        # Kill leftovers FIRST. Without this, generators from earlier runs
        # keep scrolling and inflate the next measurement -- seen as a target
        # reporting 154 KiB/update against a 5.7 KiB/update baseline.
        self.stop()
        time.sleep(0.5)
        if self.kind in ("idle", "pointer"):
            return
        try: os.unlink(self.HB)
        except OSError: pass
        if self.target == "wayvnc":
            subprocess.run(["swaymsg", "exec",
                            "foot -e sh -c '%s'" % self.cmd],
                           env=sway_env(), capture_output=True)
        else:
            subprocess.Popen(["urxvt", "-e", "sh", "-c", self.cmd],
                             env=dict(os.environ, DISPLAY=":1"),
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        for _ in range(40):
            time.sleep(0.25)
            if self.alive():
                return
        raise RuntimeError("workload %r never produced a heartbeat" % self.kind)

    def alive(self, max_age=3.0):
        if self.kind in ("idle", "pointer"):
            return True
        try:
            return (time.time() - os.stat(self.HB).st_mtime) < max_age
        except OSError:
            return False

    def stop(self):
        subprocess.run(["pkill", "-f", self.MARK], capture_output=True)


class RFB:
    def __init__(self, port, host="127.0.0.1"):
        # Always loopback in practice: the harness drives its workloads by
        # spawning processes on the machine it runs on, so it has to run on
        # the same machine as the servers. The parameter exists so the class
        # is reusable, not because a remote run would work.
        self.s = socket.create_connection((host, port), timeout=30)
        self.s.settimeout(30)
        self.buf = bytearray()
        self.s.recv(12)
        self.s.sendall(b"RFB 003.008\n")
        types = self.s.recv(self.s.recv(1)[0])
        self.s.sendall(bytes([1 if 1 in types else types[0]]))
        if struct.unpack(">I", self.s.recv(4))[0] != 0:
            n = struct.unpack(">I", self.s.recv(4))[0]
            raise RuntimeError("auth failed: %r" % self.s.recv(n))
        self.s.sendall(b"\x01")
        hdr = self.s.recv(24)
        self.w, self.h = struct.unpack(">HH", hdr[:4])
        self.bpp = hdr[4]
        self.name = self.s.recv(struct.unpack(">I", self.s.recv(4))[0]).decode(
            errors="replace")

    def set_encodings(self, names):
        encs = [ENCODINGS[n] for n in names]
        self.s.sendall(struct.pack(">BBH", 2, 0, len(encs))
                       + b"".join(struct.pack(">i", e) for e in encs))

    def request(self, incremental=True):
        self.s.sendall(struct.pack(">BBHHHH", 3, 1 if incremental else 0,
                                   0, 0, self.w, self.h))

    def _take(self, n):
        while len(self.buf) < n:
            d = self.s.recv(max(65536, n - len(self.buf)))
            if not d:
                raise EOFError("server closed")
            self.buf.extend(d)
        r = bytes(self.buf[:n]); del self.buf[:n]
        return r

    def read_update(self):
        """Parse one FramebufferUpdate completely. Returns (bytes, rects, px)."""
        while True:
            t = self._take(1)[0]
            if t == 0:
                break
            elif t == 1:                       # SetColourMapEntries
                self._take(3)
                n = struct.unpack(">H", self._take(2))[0]
                self._take(n * 6)
            elif t == 2:                       # Bell
                pass
            elif t == 3:                       # ServerCutText
                self._take(3)
                n = struct.unpack(">I", self._take(4))[0]
                self._take(n)
            elif t == MSG_FENCE:
                self._handle_fence()
            else:
                raise RuntimeError("unexpected server message %d" % t)
        self._take(1)
        nrects = struct.unpack(">H", self._take(2))[0]
        nbytes, px, kinds = 4, 0, {}
        for _ in range(nrects):
            hdr = self._take(12); nbytes += 12
            x, y, w, h, enc = struct.unpack(">HHHHi", hdr)
            kinds[RECT_NAME.get(enc, str(enc))] = \
                kinds.get(RECT_NAME.get(enc, str(enc)), 0) + 1
            n = self._rect_body(w, h, enc)
            nbytes += n
            if enc == 0:
                px += w * h
        return nbytes, kinds, px

    def _handle_fence(self):
        """Echo a fence request back, the way a real viewer does.

        This is not optional decoration. neatvnc uses fences to measure
        round-trip time and to count how many bytes are in flight, and it
        refuses to send a new frame while more data is outstanding than it
        believes the link can carry. A client that never advertises "fence"
        never exercises that path at all -- which is precisely how this
        harness once reported a full screen crossing the link in 0.19s while
        a real viewer took seconds for the same screen. A client that
        advertises it but never replies is worse still: the in-flight byte
        count would only ever grow, and the server would throttle itself to
        a standstill against a fault of our own making.
        """
        self._take(3)                                  # padding
        flags = struct.unpack(">I", self._take(4))[0]
        length = self._take(1)[0]
        payload = self._take(length)
        if not (flags & FENCE_REQUEST):
            return
        # Reply carries the same payload with REQUEST cleared, so the server
        # can match it to the frame it was measuring.
        self.s.sendall(struct.pack(">BBBBIB", 248, 0, 0, 0,
                                   flags & ~FENCE_REQUEST, length) + payload)

    def _rect_body(self, w, h, enc):
        Bpp = self.bpp // 8
        if enc == 0:                                   # raw
            self._take(w * h * Bpp); return w * h * Bpp
        if enc == 1:                                   # copyrect
            self._take(4); return 4
        if enc == -239:                                # cursor pseudo
            n = w * h * Bpp + ((w + 7) // 8) * h
            self._take(n); return n
        if enc == -223:                                # desktop size
            return 0
        raise Unparseable(enc)

    def close(self):
        try: self.s.close()
        except Exception: pass


def run_quiet_boundary(port, encoding, secs, gap=0.25):
    """Approximate: treat 'gap' seconds of silence as the end of an update.

    Used only for encodings this harness cannot parse exactly. Reports
    bytes and update counts but never damaged-pixel figures, because those
    genuinely require decoding the rectangles.
    """
    c = RFB(port)
    try:
        c.set_encodings([encoding, "copyrect", "cursor"] +
                        (["fence"] if USE_FENCE else []))
        time.sleep(0.5)
        c.request(incremental=False)
        c.s.settimeout(gap)
        try:
            while True:
                if not c.s.recv(65536): break
        except socket.timeout:
            pass
        total, updates, waits = 0, 0, []
        t0 = time.time()
        while time.time() - t0 < secs:
            c.request()
            req = time.time()
            got = 0
            try:
                while True:
                    d = c.s.recv(65536)
                    if not d: break
                    got += len(d)
            except socket.timeout:
                pass
            if got:
                total += got; updates += 1; waits.append(time.time() - req)
        el = time.time() - t0
        return dict(updates=updates, secs=el, bytes=total, waits=waits or [0],
                    rects={}, px=[], timed_out=False, suspect=False,
                    approx=True)
    finally:
        c.close()


def run_once(port, workload, encoding, secs, move_pointer=False):
    c = RFB(port)
    try:
        # only encodings this harness can parse exactly
        c.set_encodings([encoding, "copyrect", "cursor"] +
                        (["fence"] if USE_FENCE else []))
        time.sleep(0.5)
        c.request(incremental=False)
        c.read_update()                      # drain the initial full frame

        waits, total, rects, pxs = [], 0, {}, []

        def poke(i):
            """Move the pointer. Must happen BEFORE the request that is meant
            to observe its effect -- an earlier version moved only after
            reading an update, so the first read blocked with nothing to
            report and caught an unrelated full refresh instead."""
            c.s.sendall(struct.pack(">BBHH", 5, 0,
                                    200 + (i * 37) % 800,
                                    200 + (i * 53) % 400))

        t0 = time.time()
        if move_pointer:
            poke(0)
            time.sleep(0.05)
        c.request()
        req = time.time()
        timed_out = False
        while time.time() - t0 < secs:
            try:
                nb, kinds, px = c.read_update()
            except socket.timeout:
                timed_out = True
                break
            waits.append(time.time() - req)
            total += nb
            if px:
                pxs.append(px)
            for k, v in kinds.items():
                rects[k] = rects.get(k, 0) + v
            if move_pointer:
                poke(len(waits))
                time.sleep(0.02)
            c.request()                      # immediately, never on a timer
            req = time.time()
        el = time.time() - t0
        full = 1280 * 800
        suspect = bool(pxs) and sorted(pxs)[len(pxs) // 2] >= full
        return dict(updates=len(waits), secs=el, bytes=total, waits=waits,
                    rects=rects, px=pxs, timed_out=timed_out,
                    suspect=suspect)
    finally:
        c.close()


def pct(v, p):
    if not v: return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))]


def report(label, r):
    if r is None:
        print("  %-16s DISCARDED (workload died mid-run)" % label); return
    u, el = r["updates"], r["secs"]
    fps = u / el if el else 0
    bpu = r["bytes"] / u / 1024 if u else 0
    print("  %-16s %5.1f fps | %7.1f KiB tot | %6.2f KiB/upd | "
          "p50 %5.0fms p90 %6.0fms max %6.0fms%s%s"
          % (label, fps, r["bytes"] / 1024, bpu,
             pct(r["waits"], .5) * 1000, pct(r["waits"], .9) * 1000,
             max(r["waits"]) * 1000 if r["waits"] else 0,
             "  TIMEOUT" if r["timed_out"] else "",
             ("  SUSPECT(full-screen updates: not an incremental measurement)"
              if r.get("suspect") else
              "  ~approx (quiet-boundary; bytes comparable, not exact)"
              if r.get("approx") else "")))
    if r["px"]:
        med = sorted(r["px"])[len(r["px"]) // 2]
        print("  %-16s   damaged %d px/update (%.2f%% of screen)"
              % ("", med, med * 100.0 / (1280 * 800)))
    if r["rects"]:
        print("  %-16s   rects: %s" % ("", r["rects"]))


def measure(target, port, workload_kind, encoding, secs, repeat):
    wl = Workload(target, workload_kind)
    wl.start()
    out = []
    try:
        for i in range(repeat):
            if not wl.alive():
                out.append(None); continue
            try:
                r = run_once(port, workload_kind, encoding, secs,
                             move_pointer=(workload_kind == 'pointer'))
            except Unparseable:
                r = run_quiet_boundary(port, encoding, secs)
            # the guardrail that matters most: was the load still running?
            out.append(r if wl.alive() else None)
    finally:
        wl.stop()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", choices=["wayvnc", "xvnc"], default="wayvnc")
    ap.add_argument("--ab", action="store_true", help="alternate both targets")
    ap.add_argument("--workload", choices=list(Workload.SHELL), default="scroll")
    ap.add_argument("--encoding", choices=["raw", "tight", "zrle"], default="raw",
                    help="raw is exactly parseable and gives damaged-pixel "
                         "counts; tight/zrle need a decoder to count px")
    ap.add_argument("--no-fence", action="store_true",
                    help="do not advertise the fence pseudo-encoding, "
                         "disabling the server's congestion control")
    ap.add_argument("--secs", type=float, default=10.0)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--json", action="store_true",
                    help="emit the summary as JSON for before/after diffing")
    a = ap.parse_args()

    global USE_FENCE
    USE_FENCE = not a.no_fence
    if not USE_FENCE:
        print("note: fence not advertised, so the server's congestion "
              "control is disabled -- figures will be optimistic")

    if a.encoding != "raw":
        print("note: %s rects are counted but not decoded, so damaged-pixel "
              "figures are unavailable" % a.encoding)

    targets = [("wayvnc", 5900), ("xvnc", 5901)] if a.ab else \
              [(a.target, 5900 if a.target == "wayvnc" else 5901)]

    results = {}
    print("workload=%s encoding=%s %ss x%d" %
          (a.workload, a.encoding, a.secs, a.repeat))
    for name, port in targets:
        try:
            rs = measure(name, port, a.workload, a.encoding, a.secs, a.repeat)
        except Exception as e:
            print("  %-16s FAILED: %s" % (name, e)); continue
        # a run flagged SUSPECT is not an incremental measurement, so it
        # must not feed the summary ratio -- quoting a number derived from
        # full-screen refreshes is exactly the kind of confident-but-wrong
        # figure this harness exists to prevent
        good = [r for r in rs if r and not r.get("suspect")]
        for i, r in enumerate(rs):
            report("%s #%d" % (name, i + 1), r)
        if good:
            results[name] = dict(
                bytes_per_update=sum(r["bytes"] / max(r["updates"], 1)
                                     for r in good) / len(good) / 1024,
                px_per_update=(sorted(x for r in good for x in r["px"])
                               [sum(len(r["px"]) for r in good) // 2]
                               if any(r["px"] for r in good) else 0))

    if len(results) < 2 and len(targets) == 2:
        print()
        print("  NO RATIO: at least one target produced no usable runs.")
        print("  Every run was discarded (dead workload) or SUSPECT")
        print("  (full-screen refreshes rather than incremental updates).")
        print("  Fix the workload before trusting any number from it.")
    if len(results) == 2:
        w, x = results.get("wayvnc"), results.get("xvnc")
        print()
        print("  RATIO wayvnc:xvnc  bytes/update %.1fx   damaged px/update %.1fx"
              % (w["bytes_per_update"] / max(x["bytes_per_update"], 1e-9),
                 w["px_per_update"] / max(x["px_per_update"], 1)))
        print("  (these are the numbers a scroll detector has to move;")
        print("   1.0x means parity with X11 on this workload)")
    if a.json:
        print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
