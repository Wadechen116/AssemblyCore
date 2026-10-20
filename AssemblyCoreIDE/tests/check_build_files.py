#!/usr/bin/env python3
"""Checks that the CMake and qmake file lists agree, and that every Q_OBJECT header is given to moc by qmake.

  python3 tests/check_build_files.py        (from anywhere; exit status 1 on a mismatch)
"""
import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
ide = os.path.dirname(here)
root = os.path.dirname(ide)


def cmake_list(text, var):
    m = re.search(r"set\(%s(.*?)\)" % var, text, re.S)
    return sorted(re.sub(r"\$\{CMAKE_CURRENT_LIST_DIR\}/", "", x) for x in m.group(1).split())


def pri_list(text, var, prefix_var):
    m = re.search(r"^%s\s*=\s*((?:.*\\\n)*.*)$" % var, text, re.M)
    items = [x for x in m.group(1).replace("\\", " ").split()]
    return sorted(x.replace("$$%s/" % prefix_var, "") for x in items)


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


bad = 0


def compare(name, a, b):
    global bad
    if a != b:
        bad += 1
        print("MISMATCH in %s" % name)
        for x in sorted(set(a) - set(b)):
            print("  only in CMake: " + x)
        for x in sorted(set(b) - set(a)):
            print("  only in qmake: " + x)


cm = read(os.path.join(ide, "sources.cmake"))
pri = read(os.path.join(ide, "sources.pri"))
compare("services", cmake_list(cm, "ACIDE_SERVICE_SOURCES"), pri_list(pri, "SERVICE_SOURCES", "IDE_DIR"))
compare("ui", cmake_list(cm, "ACIDE_UI_SOURCES"), pri_list(pri, "UI_SOURCES", "IDE_DIR"))

top = read(os.path.join(root, "CMakeLists.txt"))
m = re.search(r"add_library\(asmcore STATIC(.*?)\)", top, re.S)
core_cmake = sorted(x[len("core/src/"):] for x in m.group(1).split())
core_pri = pri_list(read(os.path.join(root, "core", "core.pri")), "CORE_SOURCES", "ASMCORE_DIR")
compare("core", core_cmake, [x[len("src/"):] for x in core_pri])

# every source file on disk is listed somewhere, and every Q_OBJECT header is in UI_MOC_HEADERS
listed = set(cmake_list(cm, "ACIDE_SERVICE_SOURCES")) | set(cmake_list(cm, "ACIDE_UI_SOURCES"))
for sub in ("services", "ui"):
    for f in sorted(os.listdir(os.path.join(ide, sub))):
        if f.endswith(".cpp") and f != "main.cpp" and (sub + "/" + f) not in listed:
            bad += 1
            print("not listed in any build file: %s/%s" % (sub, f))
moc = set(pri_list(pri, "UI_MOC_HEADERS", "IDE_DIR"))
for f in sorted(os.listdir(os.path.join(ide, "ui"))):
    if f.endswith(".h") and "Q_OBJECT" in read(os.path.join(ide, "ui", f)) and ("ui/" + f) not in moc:
        bad += 1
        print("Q_OBJECT header missing from UI_MOC_HEADERS: ui/" + f)

print("build files agree" if not bad else "%d problem(s)" % bad)
sys.exit(1 if bad else 0)
