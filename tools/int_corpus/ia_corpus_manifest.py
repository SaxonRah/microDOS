#!/usr/bin/env python3
"""Build a metadata-only Internet Archive DOS corpus manifest."""
from __future__ import annotations
import argparse,csv,json,urllib.parse,urllib.request
from pathlib import Path

COLLECTIONS={"games":"softwarelibrary_msdos_games","apps":"softwarelibrary_msdos_apps"}

def fetch(url):
    req=urllib.request.Request(url,headers={"User-Agent":"microDOS-corpus-profiler/1.0"})
    with urllib.request.urlopen(req,timeout=60) as r:
        return json.load(r)

def query(collection,rows,year_min,year_max):
    q=f"collection:{collection}"
    if year_min is not None:
        q+=f" AND year:[{year_min} TO {year_max or 9999}]"
    params=[("q",q),("fl[]","identifier"),("fl[]","title"),("fl[]","year"),
            ("fl[]","date"),("fl[]","downloads"),("rows",str(rows)),
            ("page","1"),("output","json"),("sort[]","downloads desc")]
    url="https://archive.org/advancedsearch.php?"+urllib.parse.urlencode(params)
    return fetch(url)["response"]["docs"]

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--games",type=int,default=120)
    ap.add_argument("--apps",type=int,default=80)
    ap.add_argument("--year-min",type=int,default=1981)
    ap.add_argument("--year-max",type=int,default=1992)
    ap.add_argument("--out",default="corpus_manifest")
    ns=ap.parse_args()
    rows=[]
    for kind,n in [("games",ns.games),("apps",ns.apps)]:
        if n<=0: continue
        for d in query(COLLECTIONS[kind],n,ns.year_min,ns.year_max):
            ident=d.get("identifier","")
            rows.append({
                "kind":kind,"identifier":ident,"title":d.get("title",""),
                "year":d.get("year",""),"date":d.get("date",""),
                "downloads":d.get("downloads",0),
                "item_url":"https://archive.org/details/"+ident,
                "metadata_url":"https://archive.org/metadata/"+ident,
            })
    out=Path(ns.out); out.parent.mkdir(parents=True,exist_ok=True)
    jf=out.with_suffix(".json"); cf=out.with_suffix(".csv")
    jf.write_text(json.dumps(rows,indent=2,ensure_ascii=False),encoding="utf-8")
    fields=["kind","identifier","title","year","date","downloads","item_url","metadata_url"]
    with cf.open("w",newline="",encoding="utf-8") as f:
        w=csv.DictWriter(f,fieldnames=fields); w.writeheader(); w.writerows(rows)
    print(f"wrote {len(rows)} items")
    print(jf); print(cf)
if __name__=="__main__": main()
