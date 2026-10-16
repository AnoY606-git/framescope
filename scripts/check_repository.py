#!/usr/bin/env python3
# Copyright (C) 2026 AnoY606-git and FrameScope contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Check selected source paths, credential heuristics, pinned dependencies and legal files."""
import argparse
from pathlib import Path
import subprocess
from release_support import source_paths, validate_sources


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--include-untracked", action="store_true",
                        help="Also check unignored new files before the first commit")
    args = parser.parse_args()
    try:
        files = source_paths(args.root, args.include_untracked)
        validate_sources(args.root, files)
    except subprocess.CalledProcessError as error:
        raise SystemExit("Unable to read Git source file names") from error
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(str(error)) from error
    print(f"Repository policy passed: {len(files)} selected files; this is not a full secret/security audit")


if __name__ == "__main__":
    main()
