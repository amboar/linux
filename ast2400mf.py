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

def parse_pd(toks):
    pat = r"^(SCU[A-F0-9]+|Strap|SIORD_30)\[([0-9:,]+)\]"
    mat = re.search(pat, toks[0])
    if not mat:
        return None, toks
    pd = pd_t(mat.group(1), mat.group(2))
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
        if filt and d["default"] not in filt:
            continue
        pins.append(pin_t(**d))
    return pins

def is_simple(expr):
    return expr.type in [ "=", "!=" ]

def is_compound(expr):
    return not is_simple(expr)

def get_simple_bits(expr):
    if is_simple(expr):
        return [ expr.a ]
    assert is_compound(expr)
    bits = []
    assert expr.a
    bits.extend(get_simple_bits(expr.a))
    assert expr.b
    bits.extend(get_simple_bits(expr.b))
    return bits

def get_compound_bits(expr):
    assert expr
    if is_simple(expr):
        return []
    assert is_compound(expr)
    return get_simple_bits(expr)

def get_all_compound_bits(pins):
    all_bits = []
    for pin in pins:
        if not pin.high.expr:
            pp(pin)
        assert pin.high.expr
        iters = [ (pin.high, get_compound_bits(pin.high.expr)) ]
        if pin.low:
            if not pin.low.expr:
                pp(pin)
            assert pin.low.expr
            iters.append((pin.low, get_compound_bits(pin.low.expr)))
        src = [ zip(repeat(sig), bits) for sig, bits in iters ]
        for sig, bits in chain(*src):
            if bits not in all_bits:
                all_bits.append(bits)
    return all_bits

def get_bits(expr):
    bits = []
    if not expr:
        return bits
    if expr.type in ["=", "!="]:
        bits.append((expr.a))
    else:
        bits.extend(get_bits(expr.a))
        bits.extend(get_bits(expr.b))
    return bits

def get_all_bits(pins):
    all_bits = {}
    for pin in pins:
        iters = [ (pin.high, get_bits(pin.high.expr)) ]
        if pin.low:
            iters.append((pin.low, get_bits(pin.low.expr)))
        src = [ zip(repeat(sig), bits) for sig, bits in iters ]
        for sig, bits in chain(*src):
            if bits not in all_bits:
                all_bits[bits] = []
            t = (pin.default, sig.sig)
            if t not in all_bits[bits]:
                all_bits[bits].append(t)
    return all_bits

def print_bits(bits):
    for k in sorted(bits.keys(), key=lambda x: len(bits[x]), reverse=True):
        print("{} ; {}".format(k, sorted(bits[k])))

def expr_uses_bit(expr, bit):
    if is_simple(expr):
        return expr.a == bit
    assert is_compound(expr)
    return expr_uses_bit(expr.a, bit) or expr_uses_bit(expr.b, bit)

def sig_uses_bit(sig, bit):
    return expr_uses_bit(sig.expr, bit)

def get_bit_sigs(pins, bit):
    sigs = []
    for pin in pins:
        if sig_uses_bit(pin.high, bit):
            sigs.append(pin.high)
        if pin.low:
            if sig_uses_bit(pin.low, bit):
                sigs.append(pin.low)
    return sigs

def get_sig_pin(pins, sig):
    for pin in pins:
        if sig in [ pin.high, pin.low ]:
            return pin

def find_networks(pins):
    cbits = get_all_compound_bits(pins)
    bsigs = { cbit : get_bit_sigs(pins, cbit) for cbit in cbits }
    nets = []
    for bit in cbits:
        if bit not in bsigs:
            continue
        q = bsigs[bit][:]
        rels = []
        while len(q):
            sig = q.pop()
            rels.append(sig)
            sigbits = get_simple_bits(sig.expr)
            nsigs = [ x for nbit in sigbits
                            for x in bsigs[nbit]
                                if not (x in q or x in rels) ]
            q.extend(nsigs)
        nets.append(rels)
    return frozenset([frozenset(rels) for rels in nets])

def print_networks(nets):
    allbits = set()
    for rels in sorted(nets):
        c = Counter()
        for sig in rels:
            c.update(get_bits(sig.expr))
        relbits = frozenset([bit for sig in rels
            for bit in get_bits(sig.expr)
                if c[bit] > 1 or sum(c.values()) == len(c)])
        common = [ (bit, c[bit]) for bit in relbits ]
        scommon = sorted(common, key=lambda x: x[0])
        scommon = sorted(scommon, key=lambda x: x[1], reverse=True)
        print("Common bits: {}".format(scommon))
        defrels = [ (get_sig_pin(pins, sig).other, sig) for sig in rels ]
        for default, sig in sorted(defrels, key=lambda x: x[0]):
            selbits = set(get_bits(sig.expr)).difference(relbits)
            print("('{}', '{}'): {}".format(default, sig.sig, selbits))
        assert allbits.isdisjoint(relbits)
        allbits.update(relbits)
        print()

if __name__ == "__main__":
    pinf = None
    if 2 <= len(sys.argv):
        with open(sys.argv[1]) as f:
            pinf = list(x.strip() for x in f.readlines())
    pins = gen_pins(sys.stdin.readlines(), pinf)
    nets = find_networks(pins)
    print_networks(nets)
