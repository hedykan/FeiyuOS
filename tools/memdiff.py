import json, sys, collections
a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2]))
ha,hb=a['heap'],b['heap']
print('heap      ', ' '.join(f"{k}={ha[k]}->{hb[k]}({hb[k]-ha[k]:+})" for k in ha))
print('buckets (used_b / free_b / used_n):')
for x,y in zip(a['buckets'],b['buckets']):
    print(f"  <= {str(x['le']):>6}: used {x['used_b']:>7}->{y['used_b']:>7} ({y['used_b']-x['used_b']:+7})  n {x['used_n']:>4}->{y['used_n']:>4}   free {x['free_b']:>7}->{y['free_b']:>7}")
ta={t['name']:t for t in a['tasks']}; tb={t['name']:t for t in b['tasks']}
print('tasks:')
for n in sorted(set(ta)|set(tb)):
    x,y=ta.get(n),tb.get(n)
    if not x: print(f"  + {n} stack={y['stack']} free_min={y['stack_free_min']}")
    elif not y: print(f"  - {n} stack={x['stack']}")
    else: print(f"    {n:16} stack={y['stack']:>6} free_min={x['stack_free_min']}->{y['stack_free_min']}")
ka={(x['a'],x['s']) for x in a['blocks'] if x['u']}
new=[x for x in b['blocks'] if x['u'] and (x['a'],x['s']) not in ka]
gone=[x for x in a['blocks'] if x['u'] and (x['a'],x['s']) not in {(y['a'],y['s']) for y in b['blocks'] if y['u']}]
print(f"used blocks >=512 new: {len(new)} = {sum(x['s'] for x in new)} B; gone: {len(gone)} = {sum(x['s'] for x in gone)} B")
c=collections.Counter(x['s'] for x in new)
for s,n in sorted(c.items(), key=lambda kv:-kv[0]*kv[1])[:30]:
    tag={x.get('stack_of') for x in new if x['s']==s}-{None}
    print(f"  {s:>6} x{n}  {','.join(tag)}")
print('largest free blocks B:', sorted([x['s'] for x in b['blocks'] if not x['u']], reverse=True)[:8])
