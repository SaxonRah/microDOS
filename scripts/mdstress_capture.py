#!/usr/bin/env python3
"""Clean phase-by-phase MDSTRESS v2 capture for microDOS Pico firmware.

Unlike v1, this never profiles while MDSTRESS is blocked in DOS keyboard input.
Each phase is a separate COMMAND.COM invocation:

    Ctrl+] snapshot at A>
    MDSTRESS N
    wait for return to A>
    Ctrl+] snapshot at A>

The second snapshot's "since previous Ctrl+]" interval is therefore the phase.
The script also handles the initial MS-DOS date/time prompts on a fresh boot.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import pathlib
import re
import sys
import time

import serial
from serial import SerialException

PHASE_NAMES = {
    1: "ALU+FLAGS+SHIFT",
    2: "MEMORY+STORE",
    3: "REP-STRINGS",
    4: "CALL+RET+STACK",
    5: "MUL+DIV+ROTATE",
    6: "BRANCH+LFSR",
    7: "RARE-OPCODES",
    8: "DOS-SERVICES",
    9: "SELF-MODIFY",
}

PROMPT_RE = re.compile(rb"(?:^|\r?\n)A>")
DATE_PROMPT = b"Enter new date:"
TIME_PROMPT = b"Enter new time:"
DONE = b"MDSTRESS complete."

def emit(data: bytes, log) -> None:
    if not data:
        return
    log.write(data)
    log.flush()
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

def read_available(ser: serial.Serial, log) -> bytes:
    n = ser.in_waiting
    if not n:
        return b""
    data = ser.read(n)
    emit(data, log)
    return data

def wait_quiet(ser: serial.Serial, log, quiet=0.30, hard=5.0) -> bytes:
    start = last = time.monotonic()
    out = bytearray()
    while time.monotonic() - start < hard:
        data = read_available(ser, log)
        if data:
            out.extend(data)
            last = time.monotonic()
        elif time.monotonic() - last >= quiet:
            break
        else:
            time.sleep(0.01)
    return bytes(out)

def sync_prompt(ser: serial.Serial, log, timeout: float) -> None:
    """Reach COMMAND.COM's A> prompt, accepting default date/time if needed."""
    end = time.monotonic() + timeout
    buf = bytearray()

    # Wake either COMMAND.COM or the current date/time input.
    ser.write(b"\r")
    ser.flush()
    last_nudge = time.monotonic()

    while time.monotonic() < end:
        data = read_available(ser, log)
        if data:
            buf.extend(data)
            if len(buf) > 32768:
                del buf[:-16384]

            if DATE_PROMPT in buf:
                ser.write(b"\r")
                ser.flush()
                buf.clear()
                continue
            if TIME_PROMPT in buf:
                ser.write(b"\r")
                ser.flush()
                buf.clear()
                continue
            if PROMPT_RE.search(bytes(buf)):
                return
        elif time.monotonic() - last_nudge > 1.0:
            ser.write(b"\r")
            ser.flush()
            last_nudge = time.monotonic()
        time.sleep(0.01)

    raise TimeoutError("could not reach COMMAND.COM A> prompt")

def wait_for_prompt_after_run(ser: serial.Serial, log, timeout: float) -> bytes:
    end = time.monotonic() + timeout
    buf = bytearray()
    saw_done = False
    while time.monotonic() < end:
        data = read_available(ser, log)
        if data:
            buf.extend(data)
            if DONE in buf:
                saw_done = True
            if saw_done and PROMPT_RE.search(bytes(buf)):
                return bytes(buf)
            if len(buf) > 65536:
                del buf[:-32768]
        else:
            time.sleep(0.01)
    raise TimeoutError("MDSTRESS did not return to A> before timeout")

def snapshot(ser: serial.Serial, log, timeout: float) -> str:
    """Request Ctrl+] stats and collect until the output becomes quiet."""
    ser.write(b"\x1d")
    ser.flush()

    start = time.monotonic()
    seen_perf = False
    out = bytearray()
    last = start

    while time.monotonic() - start < timeout:
        data = read_available(ser, log)
        if data:
            out.extend(data)
            last = time.monotonic()
            if b"[perf] --- since previous Ctrl+] ---" in out:
                seen_perf = True
        elif seen_perf and time.monotonic() - last >= 0.35:
            break
        else:
            time.sleep(0.01)

    if not seen_perf:
        raise TimeoutError("Ctrl+] statistics did not arrive")
    return out.decode("ascii", errors="replace")

def previous_section(text: str, tag: str) -> str:
    marker = f"[{tag}] --- since previous Ctrl+] ---"
    i = text.find(marker)
    if i < 0:
        return ""
    s = text[i:]
    # stop at next section type after the desired previous section
    if tag == "perf":
        j = s.find("\n[jit] --- since boot ---")
        if j >= 0:
            s = s[:j]
    return s

def integer(pattern: str, text: str, default=0) -> int:
    m = re.search(pattern, text)
    return int(m.group(1)) if m else default

def floating(pattern: str, text: str, default=0.0) -> float:
    m = re.search(pattern, text)
    return float(m.group(1)) if m else default

def parse_snapshot(phase: int, text: str, app_text: str) -> dict:
    perf = previous_section(text, "perf")
    jit = previous_section(text, "jit")

    row = {
        "phase": phase,
        "name": PHASE_NAMES[phase],
        "active_s": floating(r"\[perf\] active\s+([0-9.]+) s", perf),
        "instructions": integer(r"\[perf\] instructions\s+(\d+)", perf),
        "mips": floating(r"active\s+([0-9.]+) MIPS", perf),
        "static_aot": integer(r"static-aot\s+(\d+)", perf),
        "jit_native": integer(r"jit-native\s+(\d+)", perf),
        "interpreted": integer(r"interpreted\s+(\d+)", perf),
        "bios": integer(r"interpreted split: bios\s+(\d+)", perf),
        "jit_owned": integer(r"jit-owned\s+(\d+)", perf),
        "jit_fallback": integer(r"fallback interpreted\s+(\d+)", perf),
        "native": integer(r"\[jit\] native=(\d+)", jit),
        "fallback": integer(r"fallback=(\d+)", jit),
        "control": integer(r"control=(\d+)", jit),
        "entry": integer(r"entry=(\d+)", jit),
        "compile": integer(r"compile=(\d+)", jit),
        "hit": integer(r"hit=(\d+)", jit),
        "miss": integer(r"miss=(\d+)", jit),
        "returns": integer(r"return=(\d+)", jit),
        "zero": integer(r"zero=(\d+)", jit),
        "cold": integer(r"cold=(\d+)", jit),
        "invalid": integer(r"invalid=(\d+)", jit),
        "flush": integer(r"flush=(\d+)", jit),
        "code_used": integer(r"code=(\d+)/\d+ B", jit),
        "code_size": integer(r"code=\d+/(\d+) B", jit),
        "checksum": "",
        "app_cs": "",
        "rep_instructions": integer(r"\[rep\] instructions\s+(\d+)", perf),
        "rep_elements": integer(r"\[rep\] instructions\s+\d+\s+elements\s+(\d+)", perf),
        "rep_payload_bytes": integer(r"payload\s+(\d+) B", perf),
        "rep_memory_bytes": integer(r"traffic\s+(\d+) B", perf),
        "rep_elements_mps": floating(r"rate elements\s+([0-9.]+) M/s", perf),
        "rep_payload_mib_s": floating(r"payload\s+([0-9.]+) MiB/s", perf),
        "rep_traffic_mib_s": floating(r"traffic\s+([0-9.]+) MiB/s", perf),
        "rep_floppy144_s": floating(r"1\.44MB-eq\s+([0-9.]+)/s", perf),
        "xip_accesses": integer(r"\[xip\] accesses\s+(\d+)", perf),
        "xip_hits": integer(r"\[xip\] accesses\s+\d+\s+hits\s+(\d+)", perf),
        "xip_misses": integer(r"misses\s+(\d+)", perf),
        "xip_hit_pct": floating(r"hit-rate\s+([0-9.]+)%", perf),
        "xip_miss_per_guest": floating(r"misses/guest\s+([0-9.]+)", perf),
    }
    m = re.search(r"checksum = 0x([0-9A-Fa-f]{4})", app_text)
    if m:
        row["checksum"] = m.group(1).upper()
    m = re.search(r"\[MDSTRESS\] CS=0x([0-9A-Fa-f]{4})", app_text)
    if m:
        row["app_cs"] = m.group(1).upper()
    return row

def parse_phases(value: str) -> list[int]:
    if value.lower() in ("all", "1-9"):
        return list(range(1,10))
    out=[]
    for part in value.split(","):
        n=int(part)
        if n not in PHASE_NAMES:
            raise argparse.ArgumentTypeError("phases must be 1..9")
        if n not in out:
            out.append(n)
    return out

def print_summary(rows: list[dict]) -> None:
    print("\n\nMDSTRESS v2 phase summary")
    print("phase  name                 active(s)      instr     MIPS   REP elem M/s  REP traffic MiB/s")
    print("-----  -------------------  ---------  ---------  -------  ------------  -----------------")
    for r in rows:
        print(f"{r['phase']:>5}  {r['name']:<19}  {r['active_s']:>9.3f}  "
              f"{r['instructions']:>9}  {r['mips']:>7.3f}  {r['rep_elements_mps']:>12.3f}  "
              f"{r['rep_traffic_mib_s']:>17.3f}")

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="Windows serial port, e.g. COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--phases", default="all",
                    help="all, or comma-separated phase numbers such as 1,4,9")
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--label", default="")
    ap.add_argument("--output", type=pathlib.Path,
                    help="base output path without extension")
    ns = ap.parse_args()

    phases = parse_phases(ns.phases)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    base = ns.output or pathlib.Path(f"mdstress-v2-{stamp}")
    raw_path = base.with_suffix(".txt")
    csv_path = base.with_suffix(".csv")
    json_path = base.with_suffix(".json")

    rows=[]
    try:
        with raw_path.open("wb") as log, serial.Serial(ns.port, ns.baud, timeout=0.05) as ser:
            ser.reset_input_buffer()
            sync_prompt(ser, log, ns.timeout)

            for phase in phases:
                print(f"\n[capture] phase {phase}: {PHASE_NAMES[phase]}", file=sys.stderr)

                # Establish a clean performance-counter boundary at COMMAND.COM.
                snapshot(ser, log, ns.timeout)

                command=f"MDSTRESS {phase}\r".encode("ascii")
                ser.write(command)
                ser.flush()
                app_bytes=wait_for_prompt_after_run(ser, log, ns.timeout)

                # This snapshot's "since previous" interval is exactly the
                # COMMAND.COM invocation + one phase + return-to-prompt path.
                stats=snapshot(ser, log, ns.timeout)
                rows.append(parse_snapshot(
                    phase,
                    stats,
                    app_bytes.decode("ascii",errors="replace")
                ))

    except SerialException as exc:
        print(f"serial error: {exc}", file=sys.stderr)
        print("Close miniterm or any other program using the COM port, then retry.",
              file=sys.stderr)
        return 2

    fields = list(rows[0].keys()) if rows else []
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w=csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)

    payload={
        "label": ns.label,
        "port": ns.port,
        "baud": ns.baud,
        "captured": stamp,
        "phases": rows,
    }
    json_path.write_text(json.dumps(payload,indent=2)+"\n",encoding="utf-8")

    print_summary(rows)
    print(f"\nraw:  {raw_path}")
    print(f"csv:  {csv_path}")
    print(f"json: {json_path}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
