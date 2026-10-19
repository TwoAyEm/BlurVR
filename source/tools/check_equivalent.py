"""Checks that the split tree contains exactly the code of renamed.cpp: compares top-level items token by token."""
import re, sys, os, glob, collections
sys.path.insert(0, os.path.dirname(__file__))
from lexer import segments

TOK = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|\d[\w.]*|->|::|<<|>>|<=|>=|==|!=|&&|\|\||\+\+|--|[-+*/%&|^]=|\S')

def tokens(text):
    out = []
    for kind, seg in segments(text):
        if kind == 'comment':
            continue
        if kind == 'string':
            out.append(seg); continue
        out += TOK.findall(seg)
    return out

def strip_preproc(text):
    lines = []
    cont = False
    for ln in text.split('\n'):
        if cont or ln.lstrip().startswith('#'):
            cont = ln.rstrip().endswith('\\')
            continue
        lines.append(ln)
    return '\n'.join(lines)

def items(toks):
    res, cur, depth = [], [], 0
    i = 0
    for t in toks:
        cur.append(t)
        if t in '{(':   # paren counted too so for(;;) is safe
            depth += 1
        elif t in '})':
            depth -= 1
            if t == '}' and depth == 0 and not (len(cur) >= 2 and cur[-2] == '='):
                nxt = None
                res.append(cur); cur = []
                continue
        elif t == ';' and depth == 0:
            res.append(cur); cur = []
    if cur: res.append(cur)
    # glue "struct X {...}" + ";" and "= {...}" + ";"
    out = []
    for it in res:
        if it == [';'] and out:
            out[-1] = out[-1] + [';']
        elif out and out[-1] and out[-1][-1] == '}' and it[0] in (';',):
            out[-1] += it
        else:
            out.append(it)
    return out

def normalise(it):
    it = list(it)
    s = ' '.join(it)
    s = re.sub(r'^template < typename F > ', '', s)
    s = re.sub(r'\bF draw\b', 'const DrawCall & draw', s)
    s = re.sub(r'^static ', '', s)
    s = re.sub(r'^extern ', '', s)
    return s

def collect(paths):
    c = collections.Counter()
    for p in paths:
        t = strip_preproc(open(p).read())
        for it in items(tokens(t)):
            c[normalise(it)] += 1
    return c

orig = collect([sys.argv[1]])
new = collect(sorted(glob.glob(sys.argv[2] + '/*.cpp') + glob.glob(sys.argv[2] + '/*.h')))
only_o = orig - new
only_n = new - orig
print('items: orig', sum(orig.values()), 'new', sum(new.values()))
print('--- only in original:', sum(only_o.values()))
for k in only_o: print('  ', k[:230])
print('--- only in new:', sum(only_n.values()))
for k in only_n: print('  ', k[:230])

# ---- per-function diff for items with bodies -----------------------------------------------------------------
import difflib
def fname(k):
    m = re.match(r'^[^({=;]*?(\w+) \(', k)
    return m.group(1) if m else None
def bodies(c):
    d = collections.defaultdict(list)
    for k in c:
        if '{' in k and fname(k) and not k.startswith(('struct', 'enum', 'class', 'typedef')):
            d[fname(k)].append(k)
    return d
bo, bn = bodies(only_o), bodies(only_n)
for name in sorted(set(bo) | set(bn)):
    a = bo.get(name, ['']); b = bn.get(name, [''])
    for x, y in zip(a, b):
        xs, ys = x.split(), y.split()
        sm = difflib.SequenceMatcher(None, xs, ys, autojunk=False)
        print('== function', name)
        for op, i1, i2, j1, j2 in sm.get_opcodes():
            if op != 'equal':
                print('   ', op, ' '.join(xs[max(0,i1-3):i2+2])[:200], '=>', ' '.join(ys[max(0,j1-3):j2+2])[:200])

# ---- initialised globals and struct/enum/typedef definitions ---------------------------------------------------
def inits(c):
    res = collections.Counter()
    for k in c:
        if k.startswith(('struct', 'enum', 'class', 'typedef', 'template')) or ('(' in k.split('=')[0] and '{' in k):
            continue
        toks = k.split()
        depth = 0
        for i, t in enumerate(toks):
            if t in '([{': depth += 1
            elif t in ')]}': depth -= 1
            elif t == '=' and depth == 0:
                j = i + 1; d = 0; init = []
                while j < len(toks):
                    u = toks[j]
                    if u in '([{': d += 1
                    elif u in ')]}': d -= 1
                    if d == 0 and u in (',', ';'): break
                    init.append(u); j += 1
                k2 = i - 1
                res[(toks[k2] if toks[k2] != ']' else 'ARR', ' '.join(init))] += 1
    return res
io, in_ = inits(orig), inits(new)
print('--- global initialisers only in orig:', dict(io - in_))
print('--- global initialisers only in new:', dict(in_ - io))
def typedefs(c):
    return collections.Counter(k for k in c if k.startswith(('struct', 'enum', 'typedef', 'class')))
to, tn = typedefs(orig), typedefs(new)
print('--- types only in orig:'); [print('  ', k[:150]) for k in (to - tn)]
print('--- types only in new:'); [print('  ', k[:150]) for k in (tn - to)]
