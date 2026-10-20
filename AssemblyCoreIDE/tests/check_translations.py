#!/usr/bin/env python3
"""Checks the translation source (ui/translations/*.ts) against the program text.

  * every string passed to tr() / QT_TR_NOOP() / QObject::tr() in ui/*.cpp and *.h is in the .ts file,
  * nothing is unfinished, obsolete or empty,
  * the placeholders (%1 %2 ...) and the line breaks / tabs of a translation agree with the original.

Exit status 1 on any problem. Run from anywhere:  python3 tests/check_translations.py
"""
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET

here = os.path.dirname(os.path.abspath(__file__))
ide = os.path.dirname(here)
ui = os.path.join(ide, "ui")

LIT = r'"((?:[^"\\]|\\.)*)"'
CALL = re.compile(r'(?:\btr|\bQT_TR_NOOP|QObject::tr|QT_TRANSLATE_NOOP\(\s*"[A-Za-z:]+"\s*,)\(?\s*' + LIT)


def unescape(s):
    return (s.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"').replace("\\\\", "\\"))


used = {}
for path in sorted(glob.glob(os.path.join(ui, "*.cpp")) + glob.glob(os.path.join(ui, "*.h"))):
    text = open(path, encoding="utf-8").read()
    for m in CALL.finditer(text):
        # adjacent literals "a" "b" are one string
        end = m.end()
        s = unescape(m.group(1))
        while True:
            n = re.match(r'\s*' + LIT, text[end:])
            if not n:
                break
            s += unescape(n.group(1))
            end += n.end()
        used.setdefault(s, os.path.basename(path))

bad = 0
for ts in sorted(glob.glob(os.path.join(ui, "translations", "*.ts"))):
    root = ET.parse(ts).getroot()
    have = {}
    for m in root.iter("message"):
        src = m.find("source").text or ""
        tr = m.find("translation")
        text = tr.text or ""
        have[src] = text
        kind = tr.attrib.get("type")
        if kind in ("unfinished", "obsolete", "vanished"):
            print("%s: %s translation: %r" % (os.path.basename(ts), kind, src)); bad += 1
        if not text.strip() and src.strip():
            print("%s: empty translation: %r" % (os.path.basename(ts), src)); bad += 1
        if sorted(re.findall(r"%\d", src)) != sorted(re.findall(r"%\d", text)):
            print("%s: placeholders differ: %r -> %r" % (os.path.basename(ts), src, text)); bad += 1
        if src.count("\n") != text.count("\n") or src.count("\t") != text.count("\t"):
            print("%s: line breaks or tabs differ: %r -> %r" % (os.path.basename(ts), src, text)); bad += 1
    for s, where in sorted(used.items()):
        if s not in have:
            print("%s: not in the .ts file (run the acide_update_translations target): %r (%s)" % (os.path.basename(ts), s, where)); bad += 1
    for s in sorted(set(have) - set(used)):
        print("%s: no longer used in the sources: %r" % (os.path.basename(ts), s)); bad += 1
    print("%s: %d messages, %d strings in the sources" % (os.path.basename(ts), len(have), len(used)))

print("translations are complete" if not bad else "%d problem(s)" % bad)
sys.exit(1 if bad else 0)
