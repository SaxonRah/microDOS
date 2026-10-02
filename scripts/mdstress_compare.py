#!/usr/bin/env python3
"""Compare one or more MDSTRESS v2 CSV captures phase-by-phase."""
from __future__ import annotations
import argparse
import csv
import pathlib

def load(path: pathlib.Path):
    with path.open(newline="",encoding="utf-8") as f:
        rows=list(csv.DictReader(f))
    return {int(r["phase"]):r for r in rows}

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("csv",nargs="+",type=pathlib.Path)
    ns=ap.parse_args()
    sets=[(p.stem,load(p)) for p in ns.csv]
    phases=sorted(set().union(*(set(s.keys()) for _,s in sets)))
    header=["phase","name"]+[name for name,_ in sets]
    widths=[5,19]+[max(12,len(x)) for x in header[2:]]
    print("  ".join(h.ljust(w) for h,w in zip(header,widths)))
    print("  ".join("-"*w for w in widths))
    for ph in phases:
        anyrow=next(s[ph] for _,s in sets if ph in s)
        vals=[str(ph),anyrow["name"]]
        for _,s in sets:
            if ph not in s:
                vals.append("-")
            else:
                r=s[ph]
                vals.append(f"{float(r['mips']):.3f} MIPS / {float(r['active_s']):.3f}s")
        print("  ".join(v.ljust(w) for v,w in zip(vals,widths)))
    return 0

if __name__=="__main__":
    raise SystemExit(main())
