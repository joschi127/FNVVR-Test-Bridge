"""Checks SteamVR's settings before a session with a headset.

Other projects' tests may leave SteamVR on its null driver (a virtual headset without a display). SteamVR then
shows nothing in the real headset, and FNVVR (OpenVR) talks to the null driver. This only reads the settings and
warns; it never changes them.

FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.
"""
import json
import os

try:
    import winreg
except ImportError:  # not Windows
    winreg = None


def settings_path():
    """steamvr.vrsettings in Steam's config folder (from the registry), or None."""
    if winreg is None:
        return None
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
            steam, _ = winreg.QueryValueEx(key, "SteamPath")
    except OSError:
        return None
    path = os.path.join(steam, "config", "steamvr.vrsettings")
    return path if os.path.exists(path) else None


def null_driver_warnings(path=None):
    """What in SteamVR's settings points at the null driver; an empty list when nothing does."""
    path = path or settings_path()
    if not path:
        return []
    try:
        with open(path, encoding="utf-8") as f:
            settings = json.load(f)
    except (OSError, ValueError) as e:
        return [f"could not read {path}: {e}"]
    warnings = []
    steamvr = settings.get("steamvr", {})
    if str(steamvr.get("forcedDriver", "")).lower() == "null":
        warnings.append(f'steamvr.forcedDriver is "null" ({path})')
    if settings.get("driver_null", {}).get("enable") is True:
        warnings.append(f"driver_null.enable is true ({path})")
    return warnings


def check(print_fn=print):
    """Prints a warning for each null driver setting; returns True when SteamVR is set for a real headset."""
    warnings = null_driver_warnings()
    for w in warnings:
        print_fn("WARNING: SteamVR is set to its null driver: " + w +
                 ". The headset shows nothing; switch it back before a headset session (not needed for the emulator).")
    return not warnings


if __name__ == "__main__":
    import sys
    sys.exit(0 if check() else 1)
