#!/usr/bin/env python3
"""_paths.py - shared repository-root resolution for harness entry points.

The repository root is the NTPERS_ROOT environment variable when it is set and
non-empty. Otherwise it is the checkout that contains this file, so a checkout
works from any location without editing. WSL commands need the same root as a
WSL path, which wsl_repo_root() derives with windows_to_wsl_path().
"""
import os
import pathlib
import re

_DRIVE_RE = re.compile(r"^([A-Za-z]):[\\/](.*)$")


def windows_to_wsl_path(path: str) -> str:
    """Translate a Windows drive path to its WSL mount path (C:/x/y becomes /mnt/c/x/y).

    POSIX paths are returned with only separators normalised.
    """
    m = _DRIVE_RE.match(path)
    if m:
        rest = m.group(2).replace("\\", "/")
        return f"/mnt/{m.group(1).lower()}/{rest}"
    return path.replace("\\", "/")


def repo_root() -> str:
    """Return NTPERS_ROOT if set and non-empty, else the repository root of this checkout."""
    env = os.environ.get("NTPERS_ROOT")
    if env:
        return env
    return str(pathlib.Path(__file__).resolve().parents[2])


def wsl_repo_root() -> str:
    """Return the repository root as a WSL path, for commands run inside Ubuntu on Windows."""
    return windows_to_wsl_path(repo_root())
