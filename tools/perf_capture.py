"""Capture [PERF] lines over serial, or summarise a capture.

The bench protocol for the frame-time steps. `capture` resets the board (so
the capture starts at t=0 of a fresh boot) and writes every serial line with a
host timestamp; `stats` turns a capture into the min / median / max table the
steps ask for, plus the derived terms:

    emu_core = emu - scale - qstall      (emu contains both; see emulator_bridge.cpp)
    frame    = emu + apu                 (core 1's per-frame cost; push overlaps on core 0)

    python tools/perf_capture.py capture /dev/ttyUSB0 out.log 90 [--no-reset]
    python tools/perf_capture.py stats out.log [--skip 10] [--from SEC --to SEC]

Standard library plus pyserial. Run from anywhere.
"""

import argparse
import re
import statistics
import sys
import time

FIELDS = ("emu", "scale", "push", "qstall", "qovf", "apu", "await", "aunder", "aover", "fps")
PERF_RE = re.compile(r"\[PERF\] " + " ".join(r"%s=(\d+)(?:us)?" % f for f in FIELDS))
TS_RE = re.compile(r"^\[\s*([\d.]+)\] ")
BUDGET_US = 16_667


def capture(port, out, dur, reset=True):
    import serial

    ser = serial.Serial(port, 115200, timeout=0.2)
    if reset:
        # DTR low keeps IO0 high (normal boot); pulse RTS to drive EN low.
        ser.dtr = False
        ser.rts = True
        time.sleep(0.12)
        ser.reset_input_buffer()
        ser.rts = False
    t0 = time.time()
    with open(out, "w", buffering=1) as f:
        f.write("# capture start %s port=%s baud=115200 duration=%gs\n"
                % (time.strftime("%Y-%m-%dT%H:%M:%S%z"), port, dur))
        buf = b""
        while time.time() - t0 < dur:
            try:
                buf += ser.read(4096)
            except serial.SerialException as e:
                # The bench board's USB link drops while the board keeps
                # running. Reopen WITHOUT touching DTR/RTS: the kernel raises
                # both together on open, which leaves EN high. Setting
                # dtr=False before rts=False (pyserial's order) deasserts DTR
                # while RTS is still asserted, and that pulses EN - a reset per
                # reopen, which looked like a boot loop on the bench.
                f.write("[%8.3f] # link dropped: %s\n" % (time.time() - t0, e))
                try:
                    ser.close()
                except serial.SerialException:
                    pass
                time.sleep(0.5)
                while time.time() - t0 < dur:
                    try:
                        ser = serial.Serial(port, 115200, timeout=0.2)
                        f.write("[%8.3f] # link reopened (no reset)\n" % (time.time() - t0))
                        break
                    except serial.SerialException:
                        time.sleep(0.5)
                continue
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                f.write("[%8.3f] %s\n" % (time.time() - t0,
                                          line.decode("utf-8", "replace").rstrip("\r")))
        if buf:
            f.write("[%8.3f] %s\n" % (time.time() - t0, buf.decode("utf-8", "replace")))
        f.write("# capture end, elapsed %.1fs\n" % (time.time() - t0))
    ser.close()


def parse(path, skip, t_from, t_to):
    rows = []
    with open(path, errors="replace") as f:
        for line in f:
            m = PERF_RE.search(line)
            if not m:
                continue
            ts = TS_RE.match(line)
            t = float(ts.group(1)) if ts else None
            if t is not None and (t < t_from or t > t_to):
                continue
            r = dict(zip(FIELDS, map(int, m.groups())))
            r["t"] = t
            r["emu_core"] = r["emu"] - r["scale"] - r["qstall"]
            r["frame"] = r["emu"] + r["apu"]
            rows.append(r)
    return rows[skip:]


def stats(rows):
    if not rows:
        sys.exit("no [PERF] lines in range")
    cols = ("emu", "emu_core", "scale", "qstall", "push", "apu", "frame", "fps")
    print("n=%d lines, t=%.1f..%.1f s" % (len(rows), rows[0]["t"] or 0, rows[-1]["t"] or 0))
    print("%-9s %9s %9s %9s" % ("field", "min", "median", "max"))
    for c in cols:
        v = [r[c] for r in rows]
        print("%-9s %9d %9d %9d" % (c, min(v), statistics.median(v), max(v)))
    med = lambda c: statistics.median(r[c] for r in rows)
    print()
    print("frame budget: median emu+apu %d us vs %d us -> %s"
          % (med("frame"), BUDGET_US, "inside" if med("frame") < BUDGET_US else
             "OVER by %d us" % (med("frame") - BUDGET_US)))
    print("qovf final %d (delta over capture %d); aunder final %d (delta %d); aover final %d"
          % (rows[-1]["qovf"], rows[-1]["qovf"] - rows[0]["qovf"],
             rows[-1]["aunder"], rows[-1]["aunder"] - rows[0]["aunder"], rows[-1]["aover"]))
    q, e, s = med("qstall"), med("emu_core"), med("scale")
    if q > 1000:
        b = "display (qstall large)"
    elif e > s:
        b = "emulator (emu_core largest with qstall near zero)"
    else:
        b = "scaler (scale largest with qstall near zero)"
    print("bottleneck by decomposition: %s" % b)
    print("gnuboy gate (emu_core > 16,000 us): %s" % ("TRIPPED" if e > 16_000 else "not tripped"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("port")
    c.add_argument("out")
    c.add_argument("duration", type=float)
    c.add_argument("--no-reset", action="store_true",
                   help="attach to a running board instead of resetting it first")
    s = sub.add_parser("stats")
    s.add_argument("log")
    s.add_argument("--skip", type=int, default=10, help="drop the first N lines (default 10)")
    s.add_argument("--from", dest="t_from", type=float, default=0.0)
    s.add_argument("--to", dest="t_to", type=float, default=1e9)
    a = ap.parse_args()
    if a.cmd == "capture":
        capture(a.port, a.out, a.duration, reset=not a.no_reset)
    else:
        stats(parse(a.log, a.skip, a.t_from, a.t_to))


if __name__ == "__main__":
    main()
