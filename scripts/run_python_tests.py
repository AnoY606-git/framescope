#!/usr/bin/env python3
# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Run the actual standard-library tests and retain their outcomes as JUnit XML."""
import argparse
from pathlib import Path
import sys
import time
import unittest
import xml.etree.ElementTree as ET


def test_ids(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from test_ids(item)
        else:
            yield item.id()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("artifacts/test-results/python/unittest.xml"))
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.discover(str(Path(__file__).resolve().parent / "tests"))
    identifiers = list(test_ids(suite))
    if not identifiers:
        raise SystemExit("No Python tests discovered")
    started = time.monotonic()
    result = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite)
    report = ET.Element("testsuite", name="FrameScope.scripts", tests=str(result.testsRun),
                        failures=str(len(result.failures)), errors=str(len(result.errors)),
                        skipped=str(len(result.skipped)), time=f"{time.monotonic() - started:.6f}")
    cases = {}
    for identifier in identifiers:
        classname, _, name = identifier.rpartition(".")
        cases[identifier] = ET.SubElement(report, "testcase", classname=classname, name=name)
    for tag, outcomes in (("failure", result.failures), ("error", result.errors), ("skipped", result.skipped)):
        for test, detail in outcomes:
            identifier = test.id()
            if identifier not in cases:
                cases[identifier] = ET.SubElement(report, "testcase", name=identifier)
            ET.SubElement(cases[identifier], tag).text = detail
    args.output.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(report).write(args.output, encoding="utf-8", xml_declaration=True)
    raise SystemExit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
