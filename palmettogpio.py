#!/usr/bin/python3

import sys
from itertools import chain

terminators = [ "input", "output", "in/out", "unused" ]

for raw in sys.stdin.readlines():
    cooked = raw.strip()
    if "" == cooked:
        continue
    toks = cooked.split()
    row = []
    s = 0
    for tok in toks:
        row.append(tok)
        s += 1
        if s >= 5 and tok.lower() in terminators:
            # print(cooked)
            desc = [ " ".join(row[3:-1]) ]
            print(", ".join(chain(row[:3], desc, row[-1:])))
            row = []
            s = 0
