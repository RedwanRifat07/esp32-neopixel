#!/usr/bin/env python3
"""Generate sdkconfig.local from .env. Stdlib only; never prints secret values."""
import argparse
import os
import re
import sys

# Pins unusable for LED data on a classic ESP32: strapping (0, 2, 5, 12, 15),
# flash (6-11), UART0 (1, 3).
FORBIDDEN_GPIO = {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 15}

# key -> (symbol, required, min_bytes, max_bytes, pattern or None)
STRING_KEYS = {
    "WIFI_SSID": ("SIOT_WIFI_SSID", True, 1, 32, None),
    "WIFI_PASSWORD": ("SIOT_WIFI_PASSWORD", True, 8, 63, None),
    "WIFI2_SSID": ("SIOT_WIFI2_SSID", False, 1, 32, None),
    "WIFI2_PASSWORD": ("SIOT_WIFI2_PASSWORD", False, 8, 63, None),
    "WEB_TOKEN": ("SIOT_WEB_TOKEN", False, 8, 64, r"[\x21-\x7e]+"),
    "IMS_URL": ("SIOT_IMS_URL", False, 1, 512, None),
    "IMS_TOKEN": ("SIOT_IMS_TOKEN", False, 1, 512, None),
    "DEVICE_ID": ("SIOT_DEVICE_ID", False, 1, 64, None),
}
# key -> (symbol, default or None if required, min, max)
INT_KEYS = {
    "LED_COUNT": ("SIOT_LED_COUNT", None, 1, 30),
    "LED_GPIO": ("SIOT_LED_GPIO", 18, 0, 33),
    "POLL_INTERVAL_MS": ("SIOT_POLL_INTERVAL_MS", 5000, 1000, 3600000),
    "GLOW_MS": ("SIOT_GLOW_MS", 5000, 100, 600000),
    "GLOW_BRIGHTNESS_PCT": ("SIOT_GLOW_BRIGHTNESS_PCT", 25, 1, 100),
}
RGB_DEFAULT = "FFB46E"


class EnvError(Exception):
    pass


def parse_env(text):
    values = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        if "=" not in line:
            raise EnvError(f".env line {n}: expected KEY=VALUE")
        key, _, val = line.partition("=")
        key, val = key.strip(), val.strip()
        if len(val) >= 2 and val[0] == val[-1] and val[0] in "\"'":
            val = val[1:-1]
        values[key] = val
    return values


def is_placeholder(val):
    return (not val.strip()) or (val.startswith("<") and val.endswith(">")) \
        or val.lower() == "changeme"


def kconfig_str(val):
    if any(c in val for c in "\r\n"):
        raise EnvError("values must not contain newlines")
    return '"' + val.replace("\\", "\\\\").replace('"', '\\"') + '"'


def build_config(values):
    errors, lines, parsed = [], [], {}
    for key, (sym, required, lo, hi, pattern) in STRING_KEYS.items():
        val = values.get(key, "")
        if not val.strip() and not required:
            continue  # optional and unset: Kconfig default ("") applies
        if is_placeholder(val):
            errors.append(f"{key} is missing or still a placeholder")
            continue
        if not lo <= len(val.encode("utf-8")) <= hi:
            errors.append(f"{key} must be {lo}..{hi} bytes long")
            continue
        if pattern and not re.fullmatch(pattern, val):
            errors.append(f"{key} contains characters that are not allowed")
            continue
        try:
            lines.append(f"CONFIG_{sym}={kconfig_str(val)}")
        except EnvError as e:
            errors.append(f"{key}: {e}")
    for key, (sym, default, lo, hi) in INT_KEYS.items():
        raw = values.get(key)
        if raw is None or raw == "":
            if default is None:
                errors.append(f"{key} is required")
                continue
            num = default
        elif is_placeholder(raw) or not re.fullmatch(r"\d+", raw):
            errors.append(f"{key} must be an integer (got a non-numeric value)")
            continue
        else:
            num = int(raw)
        if not lo <= num <= hi:
            errors.append(f"{key} must be in {lo}..{hi}")
            continue
        if key == "LED_GPIO" and num in FORBIDDEN_GPIO:
            errors.append(f"LED_GPIO {num} is a strapping/flash/UART pin; choose another")
            continue
        lines.append(f"CONFIG_{sym}={num}")
        parsed[key] = num
    cabinet = values.get("CABINET", "")
    if cabinet not in ("A", "B"):
        errors.append("CABINET must be A or B")
    else:
        lines.append(f"CONFIG_SIOT_CABINET={kconfig_str(cabinet)}")
    dmap = values.get("DRAWER_MAP", "")
    codes = [c.strip() for c in dmap.split(",")] if dmap.strip() else []
    if not codes:
        errors.append("DRAWER_MAP is required (e.g. A1,A2,A3,A4,A5)")
    else:
        if cabinet in ("A", "B") and not all(re.fullmatch(cabinet + r"[1-9][0-9]?", c) for c in codes):
            errors.append(f"DRAWER_MAP codes must look like {cabinet}1, {cabinet}2, ...")
        if len(set(codes)) != len(codes):
            errors.append("DRAWER_MAP has duplicate codes")
        if "LED_COUNT" in parsed and len(codes) != parsed["LED_COUNT"]:
            errors.append("DRAWER_MAP entry count must equal LED_COUNT")
        lines.append(f"CONFIG_SIOT_DRAWER_MAP={kconfig_str(','.join(codes))}")
    mock_ca = values.get("IMS_MOCK_CA", "0")
    if mock_ca not in ("0", "1"):
        errors.append("IMS_MOCK_CA must be 0 or 1")
    elif mock_ca == "1":
        lines.append("CONFIG_SIOT_IMS_MOCK_CA=y")
    else:
        lines.append("# CONFIG_SIOT_IMS_MOCK_CA is not set")
    for key, sym, default in (("GLOW_RGB", "SIOT_GLOW_RGB", RGB_DEFAULT),
                              ("GREEN_RGB", "SIOT_GREEN_RGB", "00FF00"),
                              ("RED_RGB", "SIOT_RED_RGB", "FF0000")):
        rgb = values.get(key, default).removeprefix("0x").removeprefix("#")
        if re.fullmatch(r"[0-9A-Fa-f]{6}", rgb):
            lines.append(f"CONFIG_{sym}=0x{rgb.upper()}")
        else:
            errors.append(f"{key} must be 6 hex digits (RRGGBB)")
    boot = values.get("LED_BOOT_TEST", "0")
    if boot not in ("0", "1"):
        errors.append("LED_BOOT_TEST must be 0 or 1")
    else:
        lines.append("CONFIG_SIOT_LED_BOOT_TEST=y" if boot == "1" else "# CONFIG_SIOT_LED_BOOT_TEST is not set")
    if bool(values.get("WIFI2_SSID", "").strip()) != bool(values.get("WIFI2_PASSWORD", "").strip()):
        errors.append("WIFI2_SSID and WIFI2_PASSWORD must be set together")
    url = values.get("IMS_URL", "")
    if url and not is_placeholder(url):
        if not url.startswith("https://") or url.endswith("/") or " " in url:
            errors.append("IMS_URL must be https://... with no trailing slash (no insecure fallback)")
        if not values.get("IMS_TOKEN", "").strip() or is_placeholder(values.get("IMS_TOKEN", "")):
            errors.append("IMS_TOKEN is required when IMS_URL is set")
    if errors:
        raise EnvError("\n".join(errors))
    return "\n".join(lines) + "\n"


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--env", default=".env")
    p.add_argument("--out", default="sdkconfig.local")
    args = p.parse_args(argv)
    try:
        with open(args.env, encoding="utf-8") as f:
            text = f.read()
    except FileNotFoundError:
        print(f"error: {args.env} not found (copy .env.example to .env)", file=sys.stderr)
        return 1
    try:
        out = build_config(parse_env(text))
    except EnvError as e:
        print(f"error: invalid {args.env}:\n{e}", file=sys.stderr)
        return 1
    fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(out)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
