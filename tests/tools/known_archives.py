#!/usr/bin/env python3
"""Prints core/data/known_archives.txt from the _archives/index.csv of a full
unpack made with --scan (the archive column: paths relative to the installation).

    python3 tests/tools/known_archives.py OUT/_archives/index.csv > core/data/known_archives.txt
"""
import csv
import sys

with open(sys.argv[1], newline="", encoding="utf-8") as f:
    archives = sorted({row["archive"].replace("\\", "/") for row in csv.DictReader(f) if row["archive"]},
                      key=str.lower)
print("""# The archives of a complete Magic Awakened installation, one per line, relative
# to the installation folder ('/' or '\\' separators). Compiled into npkextpp:
# without --scan, the app only checks which of these exist instead of walking
# the whole installation, and lists the ones that don't as missing. While none
# of them exists in an installation (another game version), the app scans.
#
# Generated from the _archives/index.csv of a full unpack:
#
#   python3 tests/tools/known_archives.py OUT/_archives/index.csv > core/data/known_archives.txt
#""")
for archive in archives:
    print(archive)
