#!/usr/bin/env python3
"""Static INT-vector scan for a local DOS corpus.

Raw CD imm8 scanning works on COM/EXE/BIN and may include data.
For COM files, the patched microDOS dosprobe adds recursive-descent reachable
INT-vector counts. Dynamic M29a telemetry is still authoritative.
"""
from __future__ import annotations
import argparse,csv,json,subprocess,tempfile,zipfile
from collections import Counter
from pathlib import Path

EXTS={".com",".exe",".bin"}

def raw_cd(data):
    c=Counter()
    for i in range(len(data)-1):
        if data[i]==0xCD: c[data[i+1]]+=1
    return c

def iter_files(root):
    for p in root.rglob("*"):
        if not p.is_file(): continue
        if p.suffix.lower() in EXTS:
            try: yield str(p),p.read_bytes(),p.suffix.lower()
            except Exception: pass
        elif p.suffix.lower()==".zip":
            try:
                with zipfile.ZipFile(p) as z:
                    for n in z.namelist():
                        ext=Path(n).suffix.lower()
                        if ext in EXTS and not n.endswith("/"):
                            try: yield str(p)+"!"+n,z.read(n),ext
                            except Exception: pass
            except Exception: pass

def reachable_com(dosprobe,label,data):
    with tempfile.TemporaryDirectory() as td:
        src=Path(td)/"x.com"; js=Path(td)/"x.json"
        src.write_bytes(data)
        cp=subprocess.run([str(dosprobe),"--input",str(src),"--base","0x100",
                           "--entry","0x100","--label",label,"--json",str(js)],
                          stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        if cp.returncode!=0 or not js.exists(): return {}
        return json.loads(js.read_text(encoding="utf-8")).get("interrupt_vectors",{})

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--dosprobe",default=r"build-host\Release\dosprobe.exe")
    ap.add_argument("--out",default="int_corpus_scan")
    ns=ap.parse_args()
    root=Path(ns.root); dosprobe=Path(ns.dosprobe)
    raw_total=Counter(); reach_total=Counter(); rows=[]
    for label,data,ext in iter_files(root):
        raw=raw_cd(data); raw_total.update(raw)
        reach={}
        if ext==".com" and dosprobe.exists():
            reach=reachable_com(dosprobe,label,data)
            for k,v in reach.items(): reach_total[int(k,16)]+=int(v)
        rows.append({
            "path":label,"bytes":len(data),"raw_cd_total":sum(raw.values()),
            "raw_vectors":" ".join(f"{k:02X}:{v}" for k,v in raw.most_common(12)),
            "reachable_vectors":" ".join(f"{k}:{v}" for k,v in
                sorted(reach.items(),key=lambda x:-int(x[1]))[:12]),
        })
    out=Path(ns.out); out.parent.mkdir(parents=True,exist_ok=True)
    cf=out.with_suffix(".csv"); jf=out.with_suffix(".json")
    with cf.open("w",newline="",encoding="utf-8") as f:
        w=csv.DictWriter(f,fieldnames=["path","bytes","raw_cd_total","raw_vectors","reachable_vectors"])
        w.writeheader(); w.writerows(rows)
    summary={"files":len(rows),
             "raw_vectors":{f"{k:02X}":v for k,v in raw_total.most_common()},
             "reachable_com_vectors":{f"{k:02X}":v for k,v in reach_total.most_common()}}
    jf.write_text(json.dumps(summary,indent=2),encoding="utf-8")
    print(json.dumps(summary,indent=2)); print(cf); print(jf)
if __name__=="__main__": main()
