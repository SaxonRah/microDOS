#!/usr/bin/env python3
import argparse, csv, pathlib, re

QMI_RE = re.compile(
    r"\[qmi\]\s+(?P<cat>[a-z0-9-]+)\s+accesses\s+(?P<acc>\d+)\s+misses\s+(?P<miss>\d+)"
    r"\s+access/guest\s+(?P<apg>[0-9.]+)\s+miss/guest\s+(?P<mpg>[0-9.]+)", re.I)
INST_RE = re.compile(r"\[perf\] instructions\s+(\d+)\s+active\s+([0-9.]+) MIPS")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log', type=pathlib.Path)
    ap.add_argument('--label', default='')
    ap.add_argument('--csv', type=pathlib.Path)
    ns = ap.parse_args()
    text = ns.log.read_text(encoding='utf-8', errors='replace')
    starts = list(re.finditer(r"=== SPLIT INTERVAL: ([^=\r\n]+) ===", text))
    rows=[]
    for i,m in enumerate(starts):
        end = starts[i+1].start() if i+1 < len(starts) else len(text)
        sec = text[m.end():end]
        marker = '[perf] --- since previous Ctrl+] ---'
        p = sec.find(marker)
        if p < 0: continue
        body = sec[p+len(marker):]
        im = INST_RE.search(body)
        instr = int(im.group(1)) if im else 0
        mips = float(im.group(2)) if im else 0.0
        interval = m.group(1).strip()
        for q in QMI_RE.finditer(body):
            rows.append({
                'Engine':ns.label,'Interval':interval,'Instructions':instr,'MIPS':mips,
                'Category':q.group('cat'),'Accesses':int(q.group('acc')),'Misses':int(q.group('miss')),
                'AccessPerGuest':float(q.group('apg')),'MissPerGuest':float(q.group('mpg'))
            })
    print('\nQMI attribution summary:', ns.label or ns.log.name)
    print('interval        category      accesses       misses   access/g   miss/g')
    print('--------------  ------------  ------------  ----------  --------  -------')
    for r in rows:
        print(f"{r['Interval'][:14]:14}  {r['Category'][:12]:12}  {r['Accesses']:12d}  {r['Misses']:10d}  {r['AccessPerGuest']:8.4f}  {r['MissPerGuest']:7.4f}")
    if ns.csv:
        ns.csv.parent.mkdir(parents=True, exist_ok=True)
        with ns.csv.open('w',newline='',encoding='utf-8') as f:
            w=csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ['Engine','Interval','Instructions','MIPS','Category','Accesses','Misses','AccessPerGuest','MissPerGuest'])
            w.writeheader(); w.writerows(rows)
        print('CSV:', ns.csv)

if __name__ == '__main__': main()
