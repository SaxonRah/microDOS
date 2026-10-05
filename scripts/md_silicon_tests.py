#!/usr/bin/env python3
"""
microDOS hardware-silicon 8086/8088 conformance driver v4.

Downloads SingleStepTests JSON.gz vectors into an ignored build directory,
filters vectors that require a stale hardware prefetch queue (unless explicitly
requested), converts the remaining tests to compact MDV1, and runs the actual
microDOS interpreter.

No external Python packages are required.
"""

from __future__ import annotations

import argparse
import gzip
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import urllib.request

REG_NAMES = [
    "ax", "bx", "cx", "dx",
    "cs", "ss", "ds", "es",
    "sp", "bp", "si", "di",
    "ip", "flags",
]
ARCH_FLAG_MASK = 0x0FD5
OF = 0x0800
PREFIXES = {0x26, 0x2E, 0x36, 0x3E, 0xF0, 0xF2, 0xF3}

CPU_INFO = {
    "8086": {
        "repo": "SingleStepTests/8086",
        "branch": "main",
        "dir": "v1",
        "tag": 8086,
    },
    "8088": {
        "repo": "SingleStepTests/8088",
        "branch": "main",
        "dir": "v2",
        "tag": 8088,
    },
}

SMOKE_OPCODES = {
    "00","01","29","3B","40","48","50","58",
    "74","75","81","83","89","8B","8E","9C",
    "9D","A4","A5","AD","C3","CD","CF","D1",
    "D3","E2","E8","EB","F6","F7","FF",
}

QUICK_OPCODES = SMOKE_OPCODES | {
    # v4: always sample the full-run correctness families found in October 2026.
    "27","2F",
    "60","61","62","63","64","65","66","67",
    "68","69","6A","6B","6C","6D","6E","6F",
    "C0","C1","C8","C9",
    "02","03","04","05","08","09","0A","0B",
    "10","11","12","13","18","19","1A","1B",
    "20","21","22","23","28","2A","2B","30",
    "31","32","33","38","39","3A","41","49",
    "51","59","70","71","72","73","76","77",
    "78","79","7A","7B","7C","7D","7E","7F",
    "80","84","85","86","87","88","8A","8C",
    "8D","8F","90","98","99","9A","9E","9F",
    "A0","A1","A2","A3","A6","A7","AA","AB",
    "AC","AE","AF","B8","B9","BA","BB","BE",
    "BF","C2","C4","C5","C6","C7","CB","CC",
    "CE","D0","D2","D4","D5","D7","E0","E1",
    "E3","E4","E5","E9","EA","EC","ED","F5",
    "F8","F9","FA","FB","FC","FD","FE",
}

def http_json(url: str):
    req = urllib.request.Request(url, headers={"User-Agent": "microDOS-conformance/4"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.load(r)

def download(url: str, dest: Path) -> None:
    if dest.exists() and dest.stat().st_size:
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "microDOS-conformance/4"})
    print(f"[download] {url}")
    with urllib.request.urlopen(req, timeout=300) as r, open(tmp, "wb") as f:
        while True:
            b = r.read(1024 * 1024)
            if not b:
                break
            f.write(b)
    tmp.replace(dest)

def list_files(cpu: str, cache: Path) -> tuple[dict, list[dict]]:
    info = CPU_INFO[cpu]
    base = f"https://raw.githubusercontent.com/{info['repo']}/{info['branch']}/{info['dir']}"
    meta_path = cache / cpu / "metadata.json"
    download(base + "/metadata.json", meta_path)
    metadata = json.loads(meta_path.read_text(encoding="utf-8"))

    listing_path = cache / cpu / "listing.json"
    if listing_path.exists():
        listing = json.loads(listing_path.read_text(encoding="utf-8"))
    else:
        api = f"https://api.github.com/repos/{info['repo']}/contents/{info['dir']}?ref={info['branch']}"
        listing = http_json(api)
        listing_path.parent.mkdir(parents=True, exist_ok=True)
        listing_path.write_text(json.dumps(listing, indent=2), encoding="utf-8")
    files = [x for x in listing if x.get("type") == "file" and x["name"].endswith(".json.gz")]
    return metadata, files

def decode_opcode(raw: list[int]) -> tuple[int | None, int | None]:
    i = 0
    while i < len(raw) and raw[i] in PREFIXES:
        i += 1
    if i >= len(raw):
        return None, None
    op = raw[i]
    reg = None
    if i + 1 < len(raw):
        reg = (raw[i + 1] >> 3) & 7
    return op, reg

def effective_meta(metadata: dict, raw: list[int]) -> dict:
    op, reg = decode_opcode(raw)
    if op is None:
        return {}
    entry = metadata.get("opcodes", {}).get(f"{op:02X}", {})
    out = dict(entry)
    regmap = entry.get("reg")
    if isinstance(regmap, dict) and reg is not None and str(reg) in regmap:
        out.update(regmap[str(reg)])
    return out

def status_allowed(status: str, include_undocumented: bool, include_undefined: bool) -> bool:
    if status in ("normal", "alias", ""):
        return True
    if status == "undocumented":
        return include_undocumented
    if status == "undefined":
        return include_undefined
    return False

def parse_mask(value) -> int:
    if value is None:
        return 0xFFFF
    if isinstance(value, int):
        return value & 0xFFFF
    if isinstance(value, str):
        return int(value, 0) & 0xFFFF
    return 0xFFFF

def dynamic_flag_mask(meta: dict, raw: list[int], initial_regs: dict) -> int:
    mask = parse_mask(meta.get("flags-mask")) & ARCH_FLAG_MASK
    op, _ = decode_opcode(raw)

    # Metadata masks OF for D2/D3 because OF is undefined for counts >1.
    # Real 8086 still defines OF for count 0/1.
    if op in (0xD2, 0xD3):
        cl = initial_regs.get("cx", 0) & 0xFF
        if cl <= 1:
            mask |= OF
    return mask

def full_expected_regs(initial: dict, final: dict) -> list[int]:
    merged = dict(initial)
    merged.update(final)
    return [int(merged.get(k, 0)) & 0xFFFF for k in REG_NAMES]

def initial_regs(initial: dict) -> list[int]:
    return [int(initial.get(k, 0)) & 0xFFFF for k in REG_NAMES]

def code_address(cs: int, ip: int, index: int) -> int:
    # Each fetched instruction byte increments 16-bit IP before segmentation.
    off = (int(ip) + int(index)) & 0xFFFF
    return (((int(cs) & 0xFFFF) << 4) + off) & 0xFFFFF

def prefetch_conflicts(test: dict) -> list[dict]:
    """
    Find cases that cannot be reproduced by a no-prefetch CPU core.

    SingleStepTests may execute bytes that are already resident in the 8086
    instruction queue even though backing RAM at CS:IP has since changed.
    microDOS intentionally has no bus/prefetch-queue model and fetches from RAM,
    so such vectors are not valid architectural comparisons for this core.
    """
    raw = [int(x) & 0xFF for x in test.get("bytes", [])]
    initial = test.get("initial", {})
    regs = initial.get("regs", {})
    cs = int(regs.get("cs", 0)) & 0xFFFF
    ip = int(regs.get("ip", 0)) & 0xFFFF
    ram = {int(a) & 0xFFFFF: int(v) & 0xFF for a, v in initial.get("ram", [])}
    queue = [int(x) & 0xFF for x in initial.get("queue", [])]

    conflicts = []
    for i, expected in enumerate(raw):
        if i < len(queue) and queue[i] != expected:
            conflicts.append({
                "kind": "queue-vs-bytes",
                "byte_index": i,
                "address": code_address(cs, ip, i),
                "expected_instruction_byte": expected,
                "queue_byte": queue[i],
            })

        addr = code_address(cs, ip, i)
        if addr in ram and ram[addr] != expected:
            conflicts.append({
                "kind": "ram-vs-prefetched-instruction",
                "byte_index": i,
                "address": addr,
                "expected_instruction_byte": expected,
                "ram_byte": ram[addr],
                "queue_byte": queue[i] if i < len(queue) else None,
            })
    return conflicts

def write_u16(f, v: int): f.write(struct.pack("<H", v & 0xFFFF))
def write_u32(f, v: int): f.write(struct.pack("<I", v & 0xFFFFFFFF))

def convert_vector_file(cpu: str, metadata: dict, src: Path, dest: Path,
                        limit: int, include_undocumented: bool,
                        include_undefined: bool,
                        include_prefetch_conflicts: bool,
                        sample_limit: int = 12):
    with gzip.open(src, "rt", encoding="utf-8") as f:
        tests = json.load(f)

    chosen = []
    metadata_filtered = 0
    prefetch_filtered = 0
    prefetch_samples = []

    for source_index, t in enumerate(tests):
        raw = [int(x) & 0xFF for x in t.get("bytes", [])]
        meta = effective_meta(metadata, raw)
        status = str(meta.get("status", "normal"))

        if not status_allowed(status, include_undocumented, include_undefined):
            metadata_filtered += 1
            continue

        conflicts = prefetch_conflicts(t)
        if conflicts and not include_prefetch_conflicts:
            prefetch_filtered += 1
            if len(prefetch_samples) < sample_limit:
                prefetch_samples.append({
                    "source_index": source_index,
                    "idx": t.get("idx"),
                    "hash": t.get("hash"),
                    "name": t.get("name", ""),
                    "bytes": raw,
                    "initial_queue": t.get("initial", {}).get("queue", []),
                    "conflicts": conflicts,
                })
            continue

        chosen.append((source_index, t, meta))
        if limit and len(chosen) >= limit:
            break

    dest.parent.mkdir(parents=True, exist_ok=True)
    with open(dest, "wb") as out:
        out.write(b"MDV1")
        write_u32(out, len(chosen))
        write_u16(out, CPU_INFO[cpu]["tag"])
        write_u16(out, 0)

        for source_index, t, meta in chosen:
            raw = bytes(int(x) & 0xFF for x in t.get("bytes", []))
            # Include upstream source ordinal in the visible failure name.
            base_name = str(t.get("name", ""))
            name_text = f"[src={source_index}] {base_name}"
            name = name_text[:512].encode("utf-8", "replace")
            init = t["initial"]
            final = t["final"]
            iregs = initial_regs(init["regs"])
            eregs = full_expected_regs(init["regs"], final.get("regs", {}))
            mask = dynamic_flag_mask(meta, list(raw), init["regs"])
            init_ram = [(int(a) & 0xFFFFF, int(v) & 0xFF) for a, v in init.get("ram", [])]
            final_ram = [(int(a) & 0xFFFFF, int(v) & 0xFF) for a, v in final.get("ram", [])]

            # Use the source ordinal as the test number. Some historical files
            # omit or repeat idx=0; the ordinal is always unique within a file.
            write_u32(out, source_index)
            write_u16(out, mask)
            write_u16(out, len(name))
            out.write(struct.pack("<B", len(raw)))
            out.write(b"\x00\x00\x00")
            for v in iregs: write_u16(out, v)
            for v in eregs: write_u16(out, v)
            write_u32(out, len(init_ram))
            write_u32(out, len(final_ram))
            out.write(name)
            out.write(raw)
            for a, v in init_ram:
                write_u32(out, a); out.write(bytes([v]))
            for a, v in final_ram:
                write_u32(out, a); out.write(bytes([v]))

    return {
        "selected": len(chosen),
        "metadata_filtered": metadata_filtered,
        "prefetch_filtered": prefetch_filtered,
        "prefetch_samples": prefetch_samples,
    }

SUMMARY_RE = re.compile(r"\[mdv-summary\]\s+total=(\d+)\s+passed=(\d+)\s+failed=(\d+)")

def run_one(runner: Path, mdv: Path, max_details: int) -> tuple[int,int,int,int]:
    proc = subprocess.run([str(runner), str(mdv), str(max_details)],
                          text=True, capture_output=True)
    if proc.stdout:
        print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, file=sys.stderr, end="")
    m = SUMMARY_RE.search(proc.stdout)
    if not m:
        return proc.returncode, 0, 0, 1
    total, passed, failed = map(int, m.groups())
    return proc.returncode, total, passed, failed

def select_files(mode: str, files: list[dict], explicit_opcode: str | None) -> list[dict]:
    if explicit_opcode:
        wanted = {explicit_opcode.upper().replace("0X", "").zfill(2)}
    elif mode == "smoke":
        wanted = SMOKE_OPCODES
    elif mode == "quick":
        wanted = QUICK_OPCODES
    else:
        wanted = None

    out = []
    for x in files:
        base = x["name"].split(".", 1)[0].upper()
        if wanted is None or base in wanted:
            out.append(x)
    return sorted(out, key=lambda x: x["name"])

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", required=True, type=Path)
    ap.add_argument("--cache", required=True, type=Path)
    ap.add_argument("--report", required=True, type=Path)
    ap.add_argument("--cpu", choices=["8086","8088","both"], default="8086")
    ap.add_argument("--mode", choices=["smoke","quick","full","hard"], default="quick")
    ap.add_argument("--opcode")
    ap.add_argument("--max-failure-details", type=int, default=12)
    ap.add_argument("--report-only", action="store_true")
    ap.add_argument("--include-prefetch-conflicts", action="store_true",
                    help="run vectors whose prefetched instruction bytes differ from backing RAM")
    args = ap.parse_args()

    if not args.runner.exists():
        print(f"runner not found: {args.runner}", file=sys.stderr)
        return 2

    cpus = ["8086","8088"] if args.cpu == "both" else [args.cpu]
    include_undocumented = args.mode == "hard"
    include_undefined = False
    limit = 32 if args.mode == "smoke" else (250 if args.mode == "quick" else 0)

    report = {
        "version": 4,
        "mode": args.mode,
        "cpu": args.cpu,
        "include_undocumented": include_undocumented,
        "include_undefined": include_undefined,
        "include_prefetch_conflicts": args.include_prefetch_conflicts,
        "files": [],
        "total": 0,
        "passed": 0,
        "failed": 0,
        "metadata_filtered": 0,
        "prefetch_filtered": 0,
        "prefetch_samples": [],
    }

    for cpu in cpus:
        metadata, files = list_files(cpu, args.cache)
        selected = select_files(args.mode, files, args.opcode)
        print(f"[suite] cpu={cpu} mode={args.mode} files={len(selected)} "
              f"limit/file={limit or 'all'} "
              f"prefetch-conflicts={'included' if args.include_prefetch_conflicts else 'filtered'}")

        base_raw = f"https://raw.githubusercontent.com/{CPU_INFO[cpu]['repo']}/{CPU_INFO[cpu]['branch']}/{CPU_INFO[cpu]['dir']}"
        for x in selected:
            src = args.cache / cpu / x["name"]
            download(x.get("download_url") or f"{base_raw}/{x['name']}", src)
            mdv = args.cache / cpu / "mdv-v3" / (x["name"][:-8] + ".mdv")
            cv = convert_vector_file(
                cpu, metadata, src, mdv, limit,
                include_undocumented, include_undefined,
                args.include_prefetch_conflicts
            )

            report["metadata_filtered"] += cv["metadata_filtered"]
            report["prefetch_filtered"] += cv["prefetch_filtered"]
            for s in cv["prefetch_samples"]:
                if len(report["prefetch_samples"]) < 40:
                    sample = dict(s)
                    sample["cpu"] = cpu
                    sample["file"] = x["name"]
                    report["prefetch_samples"].append(sample)

            count = cv["selected"]
            if count == 0:
                continue

            print(f"\n[opcode] {cpu} {x['name']} selected={count} "
                  f"metadata-filtered={cv['metadata_filtered']} "
                  f"prefetch-filtered={cv['prefetch_filtered']}")
            if cv["prefetch_filtered"]:
                for sample in cv["prefetch_samples"][:3]:
                    print(f"  [prefetch-filter] src={sample['source_index']} "
                          f"name={sample['name']!r} conflicts={len(sample['conflicts'])}")

            rc, total, passed, failed = run_one(args.runner, mdv, args.max_failure_details)
            report["files"].append({
                "cpu": cpu, "file": x["name"],
                "selected": count,
                "metadata_filtered": cv["metadata_filtered"],
                "prefetch_filtered": cv["prefetch_filtered"],
                "total": total, "passed": passed, "failed": failed,
                "runner_rc": rc,
            })
            report["total"] += total
            report["passed"] += passed
            report["failed"] += failed

    report["filtered"] = report["metadata_filtered"] + report["prefetch_filtered"]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print("\n=== microDOS SILICON CONFORMANCE SUMMARY v4 ===")
    print(f"mode={args.mode} cpu={args.cpu}")
    print(f"tested={report['total']} passed={report['passed']} failed={report['failed']}")
    print(f"metadata-filtered={report['metadata_filtered']} "
          f"prefetch-filtered={report['prefetch_filtered']} "
          f"filtered-total={report['filtered']}")
    if report["prefetch_filtered"]:
        print("prefetch-filter note: these vectors require stale bytes already resident "
              "in the physical 8086 instruction queue; microDOS has no queue model.")
    print(f"report={args.report}")

    if report["failed"] and not args.report_only:
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
