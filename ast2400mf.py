#!/usr/bin/python3

from collections import namedtuple as nt, Counter
import re
from pprint import pprint as pp
import sys
from itertools import chain, repeat

pd_t = nt("pd_t", "reg mask")
bop_t = nt("bop_t", "type a b")
sig_t = nt("sig_t", "sig expr")
pin_t = nt("pin", "pin default high low other")

def get_mask_bits(mask):
    if "," in mask:
        bits = set()
        for disjoint in mask.split(","):
            bits.update(get_mask_bits(disjoint))
        return bits
    if ":" in mask:
        r = [ int(x) for x in mask.split(":") ]
        s = set(range(r[1], r[0] + 1))
        return s
    return set( [ int(mask) ] )

def parse_pd(toks):
    pat = r"^(SCU[A-F0-9]+|Strap|SIORD_30)\[([0-9:,]+)\]"
    mat = re.search(pat, toks[0])
    if not mat:
        return None, toks
    bits = get_mask_bits(mat.group(2))
    pd = pd_t(mat.group(1), frozenset(bits))
    _toks = [ toks[0].replace(mat.group(0), "") ]
    _toks.extend(toks[1:])
    return pd, _toks

def parse_lit(toks):
    pat = r"^([0-9]+)$"
    mat = re.search(pat, toks[0])
    if not mat:
        return None, toks
    lit = mat.group(1)
    return lit, toks[1:]

def parse_arg(toks):
    if not toks:
        return None, None
    arg, toks = parse_pd(toks)
    if not arg:
        return parse_lit(toks)
    return arg, toks

def parse_bop(toks):
    if not toks:
        return None, None
    pat = r"(!?=|[&|])"
    mat = re.search(pat, toks[0])
    if not mat:
        return None, toks
    op = mat.group(1)
    _toks = toks[1:]
    if op in [ "=", "!=" ]:
        _toks = [ toks[0].replace(mat.group(1), "") ]
        _toks.extend(toks[1:])
    return op, _toks

def parse_expr(toks, a=None):
    if not toks:
        return None, None
    if not a:
        a, _toks = parse_arg(toks)
        if not a:
            return None, toks
        a, _ = parse_expr(_toks[:1], a)
        toks = toks[1:]
    bop, toks = parse_bop(toks)
    if not bop:
        return a, toks
    b, _toks = parse_arg(toks)
    if not b:
        return None, toks
    if _toks:
        b, _ = parse_expr(_toks[:1], b)
    expr = bop_t(bop, a, b)
    _expr, toks = parse_expr(toks[1:], expr)
    return (expr if not _expr else _expr), toks

def parse_sig(toks):
    if not toks:
        return None, None
    sig = toks[0]
    bop, toks = parse_expr(toks[1:])
    return sig_t(sig, bop), toks

def gen_pins(src, filt=None):
    pins = []
    for line in (x for x in src if "" != x.strip()):
        toks = line.strip().split(" ")
        d = {}
        d["pin"] = toks[0]
        d["default"] = toks[1]
        d["other"] = toks[-1]
        d["high"], toks = parse_sig(toks[2:-1])
        d["low"], toks = parse_sig(toks)
        if filt and d["other"] not in filt:
            continue
        pins.append(pin_t(**d))
    return pins

def is_simple(expr):
    return expr.type in [ "=", "!=" ]

def is_compound(expr):
    return not is_simple(expr)

def get_simple_pds(expr):
    if is_simple(expr):
        return [ expr.a ]
    assert is_compound(expr)
    bits = []
    assert expr.a
    bits.extend(get_simple_pds(expr.a))
    assert expr.b
    bits.extend(get_simple_pds(expr.b))
    return bits

def get_compound_pds(expr):
    assert expr
    if is_simple(expr):
        return []
    assert is_compound(expr)
    return get_simple_pds(expr)

def get_all_compound_pds(pins):
    all_pds = []
    for pin in pins:
        if not pin.high.expr:
            pp(pin)
        assert pin.high.expr
        iters = [ (pin.high, get_compound_pds(pin.high.expr)) ]
        if pin.low:
            if not pin.low.expr:
                pp(pin)
            assert pin.low.expr
            iters.append((pin.low, get_compound_pds(pin.low.expr)))
        src = [ zip(repeat(sig), bits) for sig, bits in iters ]
        for sig, bits in chain(*src):
            if bits not in all_pds:
                all_pds.append(bits)
    return all_pds

def get_pds(expr):
    pds = []
    if not expr:
        return pds
    if expr.type in ["=", "!="]:
        pds.append((expr.a))
    else:
        pds.extend(get_pds(expr.a))
        pds.extend(get_pds(expr.b))
    return pds

def get_all_pds(pins):
    all_pds = {}
    for pin in pins:
        iters = [ (pin.high, get_pds(pin.high.expr)) ]
        if pin.low:
            iters.append((pin.low, get_pds(pin.low.expr)))
        src = [ zip(repeat(sig), pds) for sig, pds in iters ]
        for sig, pds in chain(*src):
            if pds not in all_pds:
                all_pds[pds] = []
            t = (pin.other, sig.sig)
            if t not in all_pds[pds]:
                all_pds[pds].append(t)
    return all_pds

def print_pds(pds):
    for k in sorted(pds.keys(), key=lambda x: len(pds[x]), reverse=True):
        print("{} ; {}".format(k, sorted(pds[k])))

def expr_uses_bits(expr, bit):
    if is_simple(expr):
        return (set() != expr.a.mask.intersection(bit.mask)
                and expr.a.reg == bit.reg)
    assert is_compound(expr)
    return expr_uses_bits(expr.a, bit) or expr_uses_bits(expr.b, bit)

def sig_uses_bits(sig, bit):
    return expr_uses_bits(sig.expr, bit)

def get_bit_sigs(pins, bit):
    sigs = []
    for pin in pins:
        if not pin.high.expr:
            print(pin)
            sys.exit(1)
        if sig_uses_bits(pin.high, bit):
            sigs.append(pin.high)
        if pin.low:
            if sig_uses_bits(pin.low, bit):
                sigs.append(pin.low)
    return sigs

def get_sig_pin(pins, sig):
    for pin in pins:
        if sig in [ pin.high, pin.low ]:
            return pin

def explode_pd(pd):
    return [ pd_t(pd.reg, frozenset([bit])) for bit in pd.mask ]

def gen_bit_sigs_lookup(pds):
    lookup = {}
    for pd in pds:
        sigs = get_bit_sigs(pins, pd)
        for bit in explode_pd(pd):
            lookup[bit] = sigs
    return lookup

def find_networks(pins):
    pds = get_all_pds(pins)
    bsigs = gen_bit_sigs_lookup(pds)
    nets = []
    for pd in pds:
        q = []
        for bit in explode_pd(pd):
            q.extend(bsigs[bit])
        rels = []
        while len(q):
            sig = q.pop()
            rels.append(sig)
            sigpds = get_simple_pds(sig.expr)
            nsigs = []
            for pd in sigpds:
                for bit in explode_pd(pd):
                    for nsig in bsigs[bit]:
                        if not (nsig in q or nsig in rels):
                            nsigs.append(nsig)
            q.extend(nsigs)
        nets.append(rels)
    return frozenset([frozenset(rels) for rels in nets])

def print_networks(nets):
    allbits = set()
    for rels in sorted(nets):
        if len(rels) < 2:
            continue
        blah = {}
        for sig in rels:
            pin = get_sig_pin(pins, sig)
            if pin not in blah:
                blah[pin] = []
            blah[pin].append(sig)
        c = Counter()
        for pin, sigs in blah.items():
            pds = []
            for sig in sigs:
                pds.extend(get_pds(sig.expr))
            c.update(frozenset(pds))
        relbits = frozenset([bit for sig in rels
            for bit in get_pds(sig.expr)
                if c[bit] > 1 or sum(c.values()) == len(c)])
        common = [ (bit, c[bit]) for bit in relbits ]
        scommon = sorted(common, key=lambda x: x[0])
        scommon = sorted(scommon, key=lambda x: x[1], reverse=True)
        print("Common bits: {}".format(scommon))
        defrels = blah.items()
        for pin, sigs in sorted(defrels, key=lambda x: x[0].other):
            pds = []
            for sig in sigs:
                pds.extend(get_pds(sig.expr))
            selbits = set(pds).difference(relbits)
            print("('{}', '{}'): {}".format(pin.other,
                sorted([ x.sig for x in sigs ]), selbits))
        assert allbits.isdisjoint(relbits)
        allbits.update(relbits)
        print()

def find_sig(pins, name):
    for pin in pins:
        if pin.high and pin.high.sig == name:
            return pin.high
        if pin.low and pin.low.sig == name:
            return pin.low
    return None

if __name__ == "__main__":
    pinf = None
    if 2 <= len(sys.argv):
        with open(sys.argv[1]) as f:
            pinf = list(x.strip() for x in f.readlines())
    pins = gen_pins(sys.stdin.readlines(), pinf)
    nets = find_networks(pins)
    print_networks(nets)
