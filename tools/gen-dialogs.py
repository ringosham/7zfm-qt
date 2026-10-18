#!/usr/bin/env python3
"""Compiles upstream 7-Zip DIALOG resources (.rc) into C++ tables.

src/w32ui builds Qt dialogs from these tables, so upstream dialog code
(CompressDialog.cpp, BenchmarkDialog.cpp, ...) runs with upstream layouts.
The .rc files are run through the C preprocessor (with fake windows.h /
CommCtrl.h from tools/rcinc) to resolve GuiCommon.rc's layout macros.

usage: gen-dialogs.py <7zip-src-root> <rcinc-dir> <output.inc>
"""
import os
import re
import subprocess
import sys

root, rcinc, out = sys.argv[1], sys.argv[2], sys.argv[3]
ui = os.path.join(root, 'CPP/7zip/UI')
dirs = [os.path.join(ui, d) for d in ('FileManager', 'GUI')]

TOKEN = re.compile(r'''\s*(?:
    (?P<str>L?"(?:[^"]|"")*") |
    (?P<num>0[xX][0-9a-fA-F]+[lLuU]*|\d+[lLuU]*) |
    (?P<id>[A-Za-z_][A-Za-z0-9_]*) |
    (?P<op>[,|+\-*/()~&]) )''', re.X)

TEXT_CONTROLS = {'LTEXT', 'RTEXT', 'CTEXT', 'PUSHBUTTON', 'DEFPUSHBUTTON', 'GROUPBOX',
                 'CHECKBOX', 'AUTOCHECKBOX', 'RADIOBUTTON', 'AUTORADIOBUTTON', 'ICON',
                 'STATE3', 'AUTO3STATE'}
ID_CONTROLS = {'EDITTEXT', 'COMBOBOX', 'LISTBOX', 'SCROLLBAR'}
KEYWORDS = TEXT_CONTROLS | ID_CONTROLS | {'CONTROL'}

def tokenize(s):
    pos, toks = 0, []
    while pos < len(s):
        m = TOKEN.match(s, pos)
        if not m:
            if s[pos:].strip() == '':
                break
            raise ValueError('bad token at: %r' % s[pos:pos + 40])
        pos = m.end()
        kind = m.lastgroup
        toks.append((kind, m.group(kind)))
    return toks

def unquote(s):
    if s.startswith('L'):
        s = s[1:]
    return s[1:-1].replace('""', '"')

def evaluate(toks):
    expr = []
    for kind, v in toks:
        if kind == 'num':
            expr.append(str(int(v.rstrip('lLuU'), 0)))
        elif kind == 'op':
            expr.append('//' if v == '/' else v)
        elif kind == 'id':
            if v == 'NOT':
                return 0   # "NOT WS_TABSTOP": style bits to clear; ignored
            raise ValueError('unknown identifier %s' % v)
        else:
            raise ValueError('string in expression')
    if not expr:
        return 0
    return int(eval(''.join(expr), {'__builtins__': {}})) & 0xFFFFFFFF

def split_args(toks):
    args, cur, depth = [], [], 0
    for t in toks:
        if t == ('op', '(') : depth += 1
        if t == ('op', ')') : depth -= 1
        if t == ('op', ',') and depth == 0:
            args.append(cur); cur = []
        else:
            cur.append(t)
    args.append(cur)
    return args

def arg_str(a):
    return unquote(a[0][1]) if a and a[0][0] == 'str' else None

def parse_controls(body):
    toks = tokenize(body)
    stmts, cur = [], None
    for t in toks:
        if t[0] == 'id' and t[1] in KEYWORDS:
            if cur: stmts.append(cur)
            cur = [t[1], []]
        elif cur is not None:
            cur[1].append(t)
    if cur: stmts.append(cur)
    items = []
    for kw, ts in stmts:
        a = split_args(ts)
        try:
            if kw in TEXT_CONTROLS:
                text = arg_str(a[0])
                if text is None:
                    text = ''
                cid, x, y, cx, cy = (evaluate(v) for v in a[1:6])
                style = evaluate(a[6]) if len(a) > 6 else 0
                items.append((kw, text, cid, '', style, x, y, cx, cy))
            elif kw in ID_CONTROLS:
                cid, x, y, cx, cy = (evaluate(v) for v in a[0:5])
                style = evaluate(a[5]) if len(a) > 5 else 0
                items.append((kw, '', cid, '', style, x, y, cx, cy))
            else:  # CONTROL "text", id, class, style, x, y, cx, cy
                text = arg_str(a[0]) or ''
                cid = evaluate(a[1])
                cls = arg_str(a[2]) or ''
                style, x, y, cx, cy = (evaluate(v) for v in a[3:8])
                items.append((kw, text, cid, cls, style, x, y, cx, cy))
        except Exception as e:
            raise ValueError('%s %r: %s' % (kw, ts[:6], e))
    return items

HEADER = re.compile(r'^\s*(\d+)\s+DIALOG(?:EX)?\s+(.*?)$', re.M)

dialogs = {}
for d in dirs:
    for fn in sorted(os.listdir(d)):
        if not fn.endswith('.rc'):
            continue
        res = subprocess.run(['cpp', '-P', '-undef', '-x', 'c', '-I', d, '-I', os.path.join(root, 'CPP/7zip'),
                              '-I', rcinc, '-DRC_INVOKED', '-DLANGUAGE=', os.path.join(d, fn)],
                             capture_output=True, text=True, encoding='latin-1')
        text = res.stdout
        for m in HEADER.finditer(text):
            did = int(m.group(1))
            if did in dialogs:
                continue
            rest = text[m.end():]
            # a body is BEGIN ... END or { ... } (both styles occur upstream)
            begin = re.search(r'^\s*(?:BEGIN|\{)\s*$', rest, re.M)
            if not begin:
                continue
            end = re.compile(r'^\s*(?:END|\})\s*$', re.M).search(rest, begin.end())
            if not end:
                continue
            nxt = HEADER.search(rest)
            if nxt and nxt.start() < begin.start():
                continue
            header = m.group(2) + '\n' + rest[:begin.start()]
            body = rest[begin.end():end.start()]
            hdr_toks = tokenize(re.split(r'\b(?:STYLE|FONT|CAPTION|EXSTYLE)\b', header)[0])
            dims = [evaluate(v) for v in split_args(hdr_toks)[:4]]
            sm = re.search(r'\bSTYLE\b(.*?)(?=\bFONT\b|\bCAPTION\b|\bEXSTYLE\b|$)', header, re.S)
            style = evaluate(tokenize(sm.group(1))) if sm else 0
            cm = re.search(r'\bCAPTION\s+(L?"(?:[^"]|"")*")', header)
            caption = unquote(cm.group(1)) if cm else ''
            try:
                items = parse_controls(body)
            except ValueError as e:
                print('gen-dialogs: skipping dialog %d in %s: %s' % (did, fn, e), file=sys.stderr)
                continue
            dialogs[did] = (caption, dims, style, items, fn)

def cstr(s):
    out = []
    for c in s:
        if c == '\\': out.append('\\\\')
        elif c == '"': out.append('\\"')
        elif c == '\n': out.append('\\n')
        elif c == '\t': out.append('\\t')
        elif ord(c) > 126 or ord(c) < 32: out.append('\\x%04x' % ord(c))
        else: out.append(c)
    return 'L"' + ''.join(out) + '"'

os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, 'w') as f:
    f.write('// Generated by tools/gen-dialogs.py from upstream .rc files -- do not edit.\n')
    for did in sorted(dialogs):
        caption, dims, style, items, fn = dialogs[did]
        f.write('// %s\nstatic const CDlgItemTemplate k_Dlg_%d[] = {\n' % (fn, did))
        for kw, text, cid, cls, st, x, y, cx, cy in items:
            f.write('  { "%s", %s, %d, "%s", 0x%X, %d, %d, %d, %d },\n' % (kw, cstr(text), cid - (1 << 32) if cid >= 1 << 31 else cid, cls, st, x, y, cx, cy))
        f.write('};\n')
    f.write('static const CDlgTemplate k_DialogTemplates[] = {\n')
    for did in sorted(dialogs):
        caption, dims, style, items, fn = dialogs[did]
        f.write('  { %d, %s, %d, %d, 0x%X, k_Dlg_%d, %d },\n' % (did, cstr(caption), dims[2], dims[3], style, did, len(items)))
    f.write('};\n')
print('gen-dialogs: %d dialogs' % len(dialogs))
