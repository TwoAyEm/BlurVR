"""Tiny C++ lexer: splits source text into ('code'|'string'|'comment', text) segments so renames never touch string literals."""
import re

def segments(text):
    out = []
    i = 0
    n = len(text)
    buf_start = 0
    while i < n:
        c = text[i]
        if c == '"' or (c == "'" ):
            # char / string literal
            if i > 0 and text[i-1].isalnum() and c == "'":
                i += 1; continue
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == '\\': j += 1
                j += 1
            j += 1
            if i > buf_start: out.append(('code', text[buf_start:i]))
            out.append(('string', text[i:j]))
            i = j; buf_start = i
        elif text.startswith('//', i):
            j = text.find('\n', i)
            if j < 0: j = n
            if i > buf_start: out.append(('code', text[buf_start:i]))
            out.append(('comment', text[i:j]))
            i = j; buf_start = i
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            if i > buf_start: out.append(('code', text[buf_start:i]))
            out.append(('comment', text[i:j]))
            i = j; buf_start = i
        else:
            i += 1
    if buf_start < n: out.append(('code', text[buf_start:]))
    return out

def map_code(text, fn, comments=False):
    res = []
    for kind, seg in segments(text):
        if kind == 'code' or (comments and kind == 'comment'):
            res.append(fn(seg))
        else:
            res.append(seg)
    return ''.join(res)
