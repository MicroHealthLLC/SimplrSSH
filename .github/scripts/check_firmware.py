#!/usr/bin/env python3
"""
Checks on the built firmware, used by the Tests workflow (build and network-surface jobs).

  check_firmware.py size <size.json> [--board tdeck|tab5] [--budget BYTES]
      Static internal RAM (DIRAM: .data + .bss + IRAM code placed in DIRAM), from
      `python -m esp_idf_size --format json`, must stay within the board's budget
      (AI_RULES.md, Memory).

  check_firmware.py warnings <build-log>
      No compiler warnings in main/, except KNOWN_WARNINGS (reported, not failed).

  check_firmware.py secrets <file>...
      No private keys or API keys in the images (nothing secret is ever compiled in).

  check_firmware.py surface <nm-output> <sdkconfig>
      The linked firmware has no listening sockets, servers, OTA, Bluetooth, mDNS or access
      point, and does have TLS certificate-bundle verification and SSH host key checks; the
      generated sdkconfig keeps the attack surface closed (AI_RULES.md, Scope and Security).
      The board comes from the sdkconfig (the Tab5's WiFi options have their own names).

Each prints GitHub annotations for problems and exits 1 if there are any.
"""

import json
import os
import re
import sys

# Static internal RAM ceiling. AI_RULES.md: "~115 KB of internal RAM is used statically; keep
# it there or lower". The small headroom absorbs toolchain noise; raise it only deliberately.
INTERNAL_RAM_BUDGET = 118 * 1024
# The Tab5 (ESP32-P4) has 576 KB of internal RAM, but the same discipline applies: about 118 KB
# is used statically today (the display, WiFi co-processor link and USB-free BSP included).
TAB5_INTERNAL_RAM_BUDGET = 122 * 1024
BUDGETS = {"tdeck": INTERNAL_RAM_BUDGET, "tab5": TAB5_INTERNAL_RAM_BUDGET}

WARNING_RE = re.compile(r"(?:^|/)(main/[^:\s]+):(\d+):\d+: warning: (.*?)(?: \[(-W[^\]]+)\])?$")

# Warnings accepted for now, each tied to an open issue. LVGL 9.6 deprecated these flag
# helpers; the build compiles 9.6.0~1 although dependencies.lock pins 9.4.0 (KNOWN_DRIFT in
# check_dependencies.py). Remove once the lock is fixed and the calls are migrated.
KNOWN_WARNINGS = [
    re.compile(r"'[^']*\blv_obj_(?:add|remove|has)_flag\(.*' is deprecated"),
]

KEY_MATERIAL = [
    # The header alone is a string in the PEM parsers (mbedTLS, libssh2); a key has a body
    (rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----(?:\\[nr]|\s)*[A-Za-z0-9+/]{40}",
     "PEM private key"),
    (rb"(?<![A-Za-z0-9_-])sk-(?:proj-|svcacct-|admin-)?[A-Za-z0-9_-]{32,}", "OpenAI-style API key"),
    (rb"(?<![A-Za-z0-9])AKIA[0-9A-Z]{16}(?![A-Za-z0-9])", "AWS access key id"),
    (rb"gh[pousr]_[A-Za-z0-9]{36}", "GitHub token"),
]

# Linked symbols that would open the attack surface AI_RULES.md keeps closed
FORBIDDEN_SYMBOLS = [
    (r"lwip_listen|lwip_accept|lwip_bind", "listening socket"),
    (r"httpd_\w+|esp_https_server\w*", "HTTP server"),
    (r"esp_https_ota\w*|esp_ota_begin|esp_ota_write", "OTA update"),
    (r"esp_hosted_slave_ota\w*|esp_hosted_\w*_ota_\w*", "WiFi co-processor OTA update"),
    (r"esp_bt_\w+|esp_ble_\w+|ble_hs_\w+|btc_\w+", "Bluetooth"),
    (r"mdns_\w+", "mDNS"),
    (r"dhcps_start|esp_netif_create_default_wifi_ap|esp_wifi_ap_get_sta_list", "WiFi access point"),
    (r"esp_now_\w+", "ESP-NOW"),
]
REQUIRED_SYMBOLS = [
    ("esp_crt_bundle_attach", "TLS verification with the CA bundle (api.openai.com, sign-in pages)"),
    ("libssh2_session_hostkey", "SSH host key check"),
    ("libssh2_hostkey_hash", "SSH host key fingerprint"),
    ("esp_netif_create_default_wifi_sta", "WiFi station mode"),
    ("esp_wifi_set_storage", "WiFi password kept out of the WiFi driver's flash"),
]

# Options that must hold in the generated sdkconfig (unset counts as "n")
REQUIRED_CONFIG = {
    "CONFIG_ESP_WIFI_SOFTAP_SUPPORT": "n",
    "CONFIG_LWIP_DHCPS": "n",
    "CONFIG_LWIP_IPV6": "n",
    "CONFIG_BT_ENABLED": "n",
    "CONFIG_LIBSSH2_DEBUG_ENABLE": "n",
    "CONFIG_ESP_WIFI_NVS_ENABLED": "n",
    "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE": "y",
    "CONFIG_ESP_TLS_INSECURE": "n",
    "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY": "n",
}


def error(message, file=None):
    where = f" file={file}" if file else ""
    print(f"::error{where}::{message}")


def summary(text):
    out = os.environ.get("GITHUB_STEP_SUMMARY")
    if out:
        with open(out, "a") as f:
            f.write(text + "\n")
    print(text)


def read_sdkconfig(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            m = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line)
            if m:
                values[m.group(1)] = m.group(2)
                continue
            m = re.match(r"^# (CONFIG_[A-Z0-9_]+) is not set$", line)
            if m:
                values[m.group(1)] = "n"
    return values


# Tab5 (ESP32-P4): WiFi runs on the ESP32-C6 through esp_hosted / esp_wifi_remote, whose
# options replace the ESP_WIFI_* ones above (those are absent there, so they'd pass by default)
REQUIRED_CONFIG_TAB5 = {
    "CONFIG_WIFI_RMT_SOFTAP_SUPPORT": "n",
    "CONFIG_WIFI_RMT_NVS_ENABLED": "n",
    "CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE": "n",
    "CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID": "n",
    "CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED": "y",
}


def required_config(values):
    if values.get("CONFIG_IDF_TARGET_ESP32P4") == "y":
        return {**REQUIRED_CONFIG, **REQUIRED_CONFIG_TAB5}
    return REQUIRED_CONFIG


def sdkconfig_problems(values):
    return [f"{option}={values.get(option, 'n')} (must be {wanted})"
            for option, wanted in required_config(values).items() if values.get(option, "n") != wanted]


def defined_symbols(nm_output):
    """Symbol names from `nm --defined-only` output."""
    names = set()
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            names.add(parts[-1])
    return names


def surface_problems(symbols, sdkconfig_values):
    problems = []
    for pattern, what in FORBIDDEN_SYMBOLS:
        hits = sorted(s for s in symbols if re.fullmatch(pattern, s))
        if hits:
            problems.append(f"{what} linked into the firmware: {', '.join(hits[:5])}")
    for name, what in REQUIRED_SYMBOLS:
        if name not in symbols:
            problems.append(f"missing {name} ({what})")
    problems += [f"sdkconfig: {p}" for p in sdkconfig_problems(sdkconfig_values)]
    return problems


def key_material(data):
    return [what for pattern, what in KEY_MATERIAL if re.search(pattern, data)]


def warning_problems(log):
    """(blocking, known) lists of 'file:line: message' for warnings in main/."""
    blocking, known = [], []
    seen = set()
    for line in log.splitlines():
        m = WARNING_RE.search(line.strip())
        if not m or m.group(0) in seen:
            continue
        seen.add(m.group(0))
        path, row, message, _flag = m.groups()
        entry = f"{path}:{row}: {message}"
        (known if any(k.search(message) for k in KNOWN_WARNINGS) else blocking).append(entry)
    return blocking, known


def cmd_warnings(args):
    with open(args[0], encoding="utf-8", errors="replace") as f:
        blocking, known = warning_problems(f.read())
    for entry in blocking:
        path, row, message = entry.split(":", 2)
        error(f"compiler warning: {message.strip()}", f"{path},line={row}")
    summary(f"## Compiler warnings (main/)\n\n{len(blocking)} blocking, {len(known)} known "
            "(LVGL 9.6 deprecations from the dependency drift, see check_dependencies.py).")
    return 1 if blocking else 0


def cmd_size(args):
    board = args[args.index("--board") + 1] if "--board" in args else "tdeck"
    budget = BUDGETS[board]
    if "--budget" in args:
        budget = int(args[args.index("--budget") + 1])
    with open(args[0]) as f:
        size = json.load(f)
    used = size["used_diram"]
    lines = [f"## Firmware size ({board})", "",
             "| Region | Used | Limit |", "|---|---|---|",
             f"| Internal RAM, static (DIRAM) | {used:,} bytes | {budget:,} bytes |",
             f"| IRAM | {size.get('used_iram', 0):,} bytes | {size.get('iram_total', 0):,} bytes |",
             f"| Flash (code + rodata) | {size.get('used_flash_non_ram', 0):,} bytes | app partition |"]
    summary("\n".join(lines))
    if used > budget:
        error(f"Static internal RAM {used} bytes exceeds the {budget}-byte budget (AI_RULES.md, Memory): "
              "move buffers to PSRAM or explain the increase and raise the board's budget")
        return 1
    return 0


def cmd_secrets(args):
    failed = 0
    for path in args:
        with open(path, "rb") as f:
            found = key_material(f.read())
        for what in found:
            error(f"{what} found in {path}: nothing secret may be compiled into the firmware", path)
        failed += bool(found)
    if not failed:
        print(f"No key material in {len(args)} file(s)")
    return 1 if failed else 0


def cmd_surface(args):
    with open(args[0], encoding="utf-8", errors="replace") as f:
        symbols = defined_symbols(f.read())
    if len(symbols) < 1000:
        error(f"only {len(symbols)} symbols read from {args[0]}: is it nm output of the firmware ELF?")
        return 1
    problems = surface_problems(symbols, read_sdkconfig(args[1]))
    for p in problems:
        error(p)
    summary("## Network surface\n\n" + ("No problems: station-only WiFi, no listening sockets, servers, OTA "
                                       "or Bluetooth; TLS and host key checks linked." if not problems else
                                       "\n".join(f"- {p}" for p in problems)))
    return 1 if problems else 0


def main():
    commands = {"size": cmd_size, "warnings": cmd_warnings, "secrets": cmd_secrets, "surface": cmd_surface}
    if len(sys.argv) < 3 or sys.argv[1] not in commands:
        print(__doc__)
        return 2
    return commands[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    sys.exit(main())
