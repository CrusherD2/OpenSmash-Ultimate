#!/usr/bin/env python3
"""Compare two SSBU_FIGHT_HASH logs (docs/TESTING-RUNS.md): the first line that differs, its frame,
fighter and the fields that differ, or how many lines are the same.

Usage: fight_hash_diff.py <log A> <log B> [--context N]

Only whole hash lines are read ("F ..." to "...%)"): a run that was stopped leaves its last line
cut, and a log written to the standard output (SSBU_FIGHT_HASH=1) has the game's other lines in
it. The logs are compared over the lines both have, as two runs end a few frames apart. As
ssbu-recomp's tests/s11_run.py diff, with the fields named.

Exit status: 0 the same, 1 a line differs, 2 a log missing or without hash lines.
"""
import sys


def hash_lines(path):
    with open(path, errors='replace') as f:
        return [line.rstrip('\r\n') for line in f if line.startswith('F ') and line.rstrip('\r\n').endswith('%)')]


def fields(line):
    """The line's (name, value) pairs; the part in brackets is the same numbers for reading."""
    words = line.split('  (')[0].split()
    return list(zip(words[0::2], words[1::2]))


def first_difference(a, b):
    """(index, differing field names) of the first line that differs over the lines both have, or None."""
    for i, (la, lb) in enumerate(zip(a, b)):
        if la != lb:
            fa, fb = fields(la), fields(lb)
            names = [na for (na, va), (nb, vb) in zip(fa, fb) if na != 'H' and (na != nb or va != vb)]
            return i, names
    return None


def main(argv):
    args = list(argv)
    context = 2
    if '--context' in args:
        k = args.index('--context')
        try:
            context = int(args[k + 1])
        except (IndexError, ValueError):
            return usage()
        del args[k:k + 2]
    if len(args) != 2:
        return usage()
    logs = []
    for path in args:
        try:
            lines = hash_lines(path)
        except OSError as e:
            print(f'{path}: {e.strerror}', file=sys.stderr)
            return 2
        if not lines:
            print(f'{path}: no hash lines', file=sys.stderr)
            return 2
        logs.append(lines)
    (pa, pb), (a, b) = args, logs
    found = first_difference(a, b)
    if found is None:
        n = min(len(a), len(b))
        print(f'{pa} vs {pb}: the same over {n} lines ({len(a)} / {len(b)} lines; last frame {a[n - 1].split()[1]})')
        return 0
    i, names = found
    f = dict(fields(a[i]))
    what = 'fields ' + ' '.join(names) if names else 'only H (the generators\' hash, which the line shows folded to 32 bits)'
    print(f'{pa} vs {pb}: first difference at line {i + 1} of {len(a)} / {len(b)}: frame {f.get("F", "?")} fighter e {f.get("e", "?")}: {what}')
    for k in range(max(0, i - context), min(len(a), len(b), i + context + 1)):
        mark = '>' if k == i else ' '
        print(f'{mark}  A {a[k]}')
        print(f'{mark}  B {b[k]}')
    return 1


def usage():
    print(__doc__.split('\n\n')[1], file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
