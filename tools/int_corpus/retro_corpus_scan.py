#!/usr/bin/env python3
from __future__ import annotations
import argparse,csv,hashlib,json,re,shutil,struct,subprocess,tempfile,zipfile
from collections import Counter
from pathlib import Path
EXEC_EXT={".com",".exe",".bin"}; YEAR_RE=re.compile(r"\((19\d{2}|20\d{2})\)"); LAUNCH_RE=re.compile(r"(?i)([A-Z0-9_$!#%&'()@^`{}~.,+-]+)\.(COM|EXE)\b")
def find7(x):
    c=[Path(x)] if x else []
    for n in ("7z.exe","7zz.exe","7za.exe","7z","7zz","7za"):
        q=shutil.which(n)
        if q:c.append(Path(q))
    c += [Path(r"C:\Program Files\7-Zip\7z.exe"),Path(r"C:\Program Files (x86)\7-Zip\7z.exe")]
    for q in c:
        if q.exists(): return q
    raise SystemExit('7-Zip not found; pass --sevenzip "C:\\Program Files\\7-Zip\\7z.exe"')
def run7(z,args,binary=False):
    cp=subprocess.run([str(z),*args],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=not binary,encoding=None if binary else "utf-8",errors=None if binary else "replace")
    if cp.returncode not in (0,1): raise RuntimeError(f"7-Zip failed {cp.returncode}: {' '.join(args)}")
    return cp.stdout
def listing(z,a):
    t=run7(z,["l","-slt","-ba",str(a)]); out=[]; cur={}
    for line in t.splitlines()+[""]:
        if not line.strip():
            if cur.get("Path"): out.append(cur)
            cur={}; continue
        if " = " in line:
            k,v=line.split(" = ",1); cur[k.strip()]=v.strip()
    return out
def extract(z,a,m): return run7(z,["x","-so","-y",str(a),m],True)
def parts(m): return [p for p in m.replace("\\","/").split("/") if p]
def inc(m): return any(p.casefold()=="c" for p in parts(m))
def rawcd(d):
    c=Counter()
    for i in range(len(d)-1):
        if d[i]==0xCD:c[d[i+1]]+=1
    return c
def mz(d):
    if len(d)<28 or d[:2]!=b"MZ":return ""
    try:v=struct.unpack_from("<14H",d,0)
    except:return ""
    return f"{v[11]:04X}:{v[10]:04X}"
def batrefs(d):
    s=d.decode("cp437","replace"); return {m.group(1).upper()+"."+m.group(2).upper() for m in LAUNCH_RE.finditer(s)}
def probe(exe,d,label):
    if not exe.exists() or len(d)>0xFF00:return {},{}
    with tempfile.TemporaryDirectory() as td:
        p=Path(td)/"x.com"; j=Path(td)/"x.json"; p.write_bytes(d)
        cp=subprocess.run([str(exe),"--input",str(p),"--base","0x100","--entry","0x100","--label",label,"--json",str(j),"--top","64"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        if cp.returncode or not j.exists():return {},{}
        o=json.loads(j.read_text(encoding="utf-8")); return o.get("interrupt_vectors",{}),o.get("opcode_counts",{})
def scanrar(z,rar,isoname,dosprobe,files,games,agg,maxfiles,nested=""):
    es=listing(z,rar); game=(Path(nested).name if nested else rar.name); game=game[:-4] if game.lower().endswith(".rar") else game
    ym=YEAR_RE.search(game); year=ym.group(1) if ym else ""
    bats=[e["Path"] for e in es if inc(e.get("Path","")) and Path(e["Path"]).suffix.lower()==".bat"]
    bins=[e["Path"] for e in es if inc(e.get("Path","")) and Path(e["Path"]).suffix.lower() in EXEC_EXT]
    refs=set()
    for b in bats:
        try: refs|=batrefs(extract(z,rar,b))
        except: pass
    gr=Counter(); gv=Counter(); go=Counter(); n=0
    for m in bins:
        if maxfiles and n>=maxfiles: break
        n+=1
        try:d=extract(z,rar,m)
        except Exception as e:
            files.append({"iso":isoname,"game":game,"year":year,"archive":nested or str(rar),"member":m,"ext":Path(m).suffix.lower(),"bytes":"","sha256":"","launch_ref":"","raw_cd_total":"","raw_vectors":"","reachable_vectors":"","reachable_opcodes":"","mz_cs_ip":"","error":str(e)}); continue
        rv=rawcd(d); gr.update(rv); agg["raw"].update(rv); reach={}; ops={}; ext=Path(m).suffix.lower()
        if ext==".com":
            reach,ops=probe(dosprobe,d,f"{game}:{m}")
            for k,v in reach.items(): gv[int(k,16)]+=int(v); agg["reach"][int(k,16)]+=int(v)
            for k,v in ops.items(): go[int(k,16)]+=int(v); agg["ops"][int(k,16)]+=int(v)
        files.append({"iso":isoname,"game":game,"year":year,"archive":nested or str(rar),"member":m,"ext":ext,"bytes":len(d),"sha256":hashlib.sha256(d).hexdigest(),"launch_ref":1 if Path(m).name.upper() in refs else 0,"raw_cd_total":sum(rv.values()),"raw_vectors":" ".join(f"{k:02X}:{v}" for k,v in rv.most_common(16)),"reachable_vectors":" ".join(f"{k}:{v}" for k,v in sorted(reach.items(),key=lambda x:-int(x[1]))[:16]),"reachable_opcodes":" ".join(f"{k}:{v}" for k,v in sorted(ops.items(),key=lambda x:-int(x[1]))[:20]),"mz_cs_ip":mz(d),"error":""})
    games.append({"iso":isoname,"game":game,"year":year,"executables":len(bins),"scanned":n,"launch_refs":" ".join(sorted(refs)),"raw_cd_total":sum(gr.values()),"raw_vectors":" ".join(f"{k:02X}:{v}" for k,v in gr.most_common(16)),"reachable_vectors":" ".join(f"{k:02X}:{v}" for k,v in gv.most_common(16)),"reachable_opcodes":" ".join(f"{k:02X}:{v}" for k,v in go.most_common(20))}); agg["games"]+=1; agg["files"]+=n
def main():
    ap=argparse.ArgumentParser(); g=ap.add_mutually_exclusive_group(required=True); g.add_argument("--mounted"); g.add_argument("--iso-dir"); ap.add_argument("--sevenzip"); ap.add_argument("--dosprobe",default=r"build-host\Release\dosprobe.exe"); ap.add_argument("--out",required=True); ap.add_argument("--max-games",type=int); ap.add_argument("--max-files-per-game",type=int); ns=ap.parse_args()
    z=find7(ns.sevenzip); dp=Path(ns.dosprobe); files=[]; games=[]; agg={"games":0,"files":0,"raw":Counter(),"reach":Counter(),"ops":Counter()}; print("7-Zip:",z); print("dosprobe:",dp,dp.exists())
    if ns.mounted:
        rars=sorted(Path(ns.mounted).rglob("*.rar")); rars=rars[:ns.max_games] if ns.max_games else rars
        for i,r in enumerate(rars,1): print(f"[{i}/{len(rars)}] {r.name}",flush=True); scanrar(z,r,Path(ns.mounted).name or ns.mounted,dp,files,games,agg,ns.max_files_per_game)
    else:
        isos=sorted(Path(ns.iso_dir).rglob("*.iso")); count=0
        with tempfile.TemporaryDirectory() as td:
            for iso in isos:
                for e in listing(z,iso):
                    m=e.get("Path","")
                    if Path(m).suffix.lower()!=".rar":continue
                    if ns.max_games and count>=ns.max_games: break
                    count+=1; print(f"[{count}] {iso.name} :: {Path(m).name}",flush=True); rp=Path(td)/f"g{count:05d}.rar"; rp.write_bytes(extract(z,iso,m)); scanrar(z,rp,iso.name,dp,files,games,agg,ns.max_files_per_game,f"{iso}!{m}"); rp.unlink(missing_ok=True)
                if ns.max_games and count>=ns.max_games: break
    out=Path(ns.out); out.parent.mkdir(parents=True,exist_ok=True); fc=out.with_name(out.name+"_files.csv"); gc=out.with_name(out.name+"_games.csv"); sj=out.with_name(out.name+"_summary.json")
    ff=["iso","game","year","archive","member","ext","bytes","sha256","launch_ref","raw_cd_total","raw_vectors","reachable_vectors","reachable_opcodes","mz_cs_ip","error"]; gf=["iso","game","year","executables","scanned","launch_refs","raw_cd_total","raw_vectors","reachable_vectors","reachable_opcodes"]
    with fc.open("w",newline="",encoding="utf-8") as f:w=csv.DictWriter(f,fieldnames=ff);w.writeheader();w.writerows(files)
    with gc.open("w",newline="",encoding="utf-8") as f:w=csv.DictWriter(f,fieldnames=gf);w.writeheader();w.writerows(games)
    s={"games":agg["games"],"files":agg["files"],"raw_vectors":{f"{k:02X}":v for k,v in agg["raw"].most_common()},"reachable_com_vectors":{f"{k:02X}":v for k,v in agg["reach"].most_common()},"reachable_com_opcodes":{f"{k:02X}":v for k,v in agg["ops"].most_common()}}; sj.write_text(json.dumps(s,indent=2),encoding="utf-8"); print(json.dumps({"games":s["games"],"files":s["files"],"top_vectors":dict(list(s["raw_vectors"].items())[:20]),"top_reachable":dict(list(s["reachable_com_vectors"].items())[:20]),"top_opcodes":dict(list(s["reachable_com_opcodes"].items())[:30])},indent=2)); print(fc);print(gc);print(sj)
if __name__=="__main__":main()
