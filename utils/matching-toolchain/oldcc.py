#!/usr/bin/env python3
"""Minimal gcc driver for an old Allegrex cc1: oldcc.py <candidate-dir> -c src.c -o out.o [flags]

<candidate-dir>/env.sh-like config is read from <candidate-dir>/cc1.cmd (one line: the command that runs cc1,
e.g. with a loader prefix) and <candidate-dir>/include (the gcc private include dir).
Preprocesses+compiles with the old cc1 (-S), then assembles with the current pspdev psp-as
(binutils 2.44 allegrex), as uOFW does, or <candidate-dir>/bin/psp-as if present, or $PSP_AS.
"""
import sys, os, subprocess, tempfile, shlex
cand = sys.argv[1]; args = sys.argv[2:]
cc1 = shlex.split(open(os.path.join(cand, 'cc1.cmd')).read().strip())
inc = os.path.join(cand, 'include')
defs = shlex.split(open(os.path.join(cand, 'defines')).read()) if os.path.exists(os.path.join(cand, 'defines')) else ['-DPSP=1', '-D__psp__=1', '-D_PSP=1']
src = out = None; flags = []; G = '-G0'; S_only = False
i = 0
while i < len(args):
    a = args[i]
    if a == '-c': pass
    elif a == '-S': S_only = True
    elif a == '-o': out = args[i + 1]; i += 1
    elif a.endswith(('.c', '.i')) and not a.startswith('-'): src = a
    elif a.startswith('-W') and not a.startswith('-Wa,'): pass  # old cc1 does not know the modern warnings
    elif a == '-nostdlib' or a.startswith('-Wa,') or a.startswith('-specs'): pass
    else:
        if a.startswith('-G'): G = a
        flags.append(a)
    i += 1
asm = out if S_only else tempfile.mktemp(suffix='.s')
sysinc = os.path.join(cand, 'sysinclude')
cmd = cc1 + ['-quiet', '-isystem', inc] + (['-isystem', sysinc] if os.path.exists(sysinc) else []) + defs + flags + ['-w', src, '-o', asm]
r = subprocess.run(cmd)
if r.returncode:
    if not S_only and os.path.exists(asm): os.unlink(asm)  # no stale /tmp/tmp*.s after a failed compile
    sys.exit(r.returncode)
if S_only: sys.exit(0)
cand_as = os.path.join(cand, 'bin', 'psp-as')  # candidate-specific (patched) assembler, if any
pspas = os.environ.get('PSP_AS') or (cand_as if os.path.exists(cand_as) else os.path.expanduser('~/pspdev/bin/psp-as'))
r = subprocess.run([pspas, '-EL', G, '-march=allegrex', '-mabi=eabi', '-mno-pdr', asm, '-o', out])
os.unlink(asm)
sys.exit(r.returncode)
