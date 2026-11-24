#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
# sgu-dump-chains.py MARKS (docs/ios27/GPU-PASSES.md): the marks file's first line is 'log <console log>'; then per knob, the sgu_dump chains printed after each round's start (until the next round's start):
# fragment chains >= 4 ms (sorted), the sum of fragment and vertex/tiler time per dump, the long fragment chains' mean.
import re,sys,collections
M=open(sys.argv[1]).read().splitlines(); L=M[0].split()[1]
log=[l for l in open(L,'rb').read().decode('latin-1').splitlines() if 'STARLTE GPU DUMP' in l]
rounds=[(l.split()[0],l.split()[1]) for l in M[1:] if l.endswith('start')]
res=collections.OrderedDict()
for i,(t,k) in enumerate(rounds):
    nxt=rounds[i+1][0] if i+1<len(rounds) else '99:99:99'
    ch=[]
    for l in log:
        ts=l.split()[0]
        if t<=ts<nxt: ch+=re.findall(r'([vf])(\d+)\+(\d+)',l.split('DUMP',1)[1])
    res.setdefault(k,[]).append(ch)
for k,rs in res.items():
    longs=[];fs=0;vs=0;n=0;mid=[]
    for ch in rs:
        n+=len(ch); fs+=sum(int(x[1]) for x in ch if x[0]=='f'); vs+=sum(int(x[1]) for x in ch if x[0]=='v')
        longs+=[int(x[1]) for x in ch if x[0]=='f' and int(x[1])>=4000]
        mid+=[int(x[1]) for x in ch if x[0]=='f' and 1000<=int(x[1])<4000]
    big=[x for x in longs if x>=8000]; med=[x for x in longs if x<8000]
    mean=lambda a: sum(a)/len(a)/1000 if a else 0
    print(f"{k}: dumps {len(rs)} chains {n} | f>=8ms n{len(big)} mean {mean(big):.2f} | f 4-8ms n{len(med)} mean {mean(med):.2f} | f 1-4ms n{len(mid)} mean {mean(mid):.2f} | f sum {fs/1000:.0f} v sum {vs/1000:.0f} ms | longs {sorted(longs)}")
