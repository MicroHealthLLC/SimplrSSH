#!/usr/bin/env python3
"""
Build GitHub release notes for a firmware version.

Uses a "### v<version>" section from README.md when there is one (optional; the
README normally has none); otherwise (e.g. automatic patch releases) the notes point to the change list
GitHub generates from the commits and pull requests since the previous release,
which the workflow appends below. Adds install instructions and the image checksums.

Usage: release_notes.py <version> <image> <sha256> <launcher-image> <launcher-sha256>
                        [<tab5-image> <tab5-sha256> <tab5-launcher-image> <tab5-launcher-sha256>]
                        <output.md>
(the T-Deck's images first, then optionally the M5Stack Tab5's)
"""

import re
import sys


def version_section(readme, version):
    """Lines under '### v<version> ...' up to the next '### ' or '## ' heading."""
    lines = readme.splitlines()
    heading = re.compile(r"^###\s+v?" + re.escape(version) + r"(\s|$)")
    for i, line in enumerate(lines):
        if heading.match(line):
            body = []
            for next_line in lines[i + 1:]:
                if next_line.startswith("### ") or next_line.startswith("## "):
                    break
                body.append(next_line)
            return lines[i][4:].strip(), "\n".join(body).strip()
    return None, None


def tab5_section(image, sha256, launcher_image, launcher_sha256):
    return f"""
## M5Stack Tab5 (with the Tab5 Keyboard)

Download **`{image}`** and flash it to the Tab5 at offset **`0x0`** (ESP32-P4, 16 MB). To
enter download mode, hold the Tab5's reset button for about 2 seconds, until the green LED
flashes quickly.

- **Browser**: open https://espressif.github.io/esptool-js/, connect the Tab5 over USB-C,
  add `{image}` at address `0x0`, and click *Program*.
- **Command line**:
  ```
  esptool.py --chip esp32p4 --baud 921600 write_flash 0x0 {image}
  ```

SHA-256: `{sha256}` (also in `{image}.sha256`)

With a launcher, install **`{launcher_image}`** from the SD card instead.
SHA-256: `{launcher_sha256}` (also in `{launcher_image}.sha256`)

Tab5 support is new and has not been tested on a Tab5 yet: please report what you find.
"""


def build_notes(readme, version, image, sha256, launcher_image, launcher_sha256, tab5=None):
    """Release notes text; tab5 = (image, sha256, launcher_image, launcher_sha256) or None."""
    title, changes = version_section(readme, version)
    if changes:
        whats_new = f"## What's new in {title}\n\n{changes}\n"
    else:
        whats_new = (f"## SimplrSSH v{version}\n\n"
                     "The changes in this release are listed under *What's Changed* below.\n")

    notes = f"""{whats_new}
## Install

Download **`{image}`** below and flash it to the LilyGO T-Deck / T-Deck Plus at offset **`0x0`**
(ESP32-S3, DIO, 80 MHz, 16 MB):

- **Browser**: open https://espressif.github.io/esptool-js/, connect the T-Deck over USB,
  add `{image}` at address `0x0`, and click *Program*.
- **Command line**:
  ```
  esptool.py --chip esp32s3 --baud 921600 write_flash 0x0 {image}
  ```

SHA-256: `{sha256}` (also in `{image}.sha256`; check with `sha256sum -c {image}.sha256`)

### With a launcher (bmorcelli/Launcher)

If the T-Deck runs a launcher, copy **`{launcher_image}`** to its SD card and install it from
the launcher's SD menu (or its web UI). The launcher puts the app in a free slot and creates
SimplrSSH's own `simplrssh` settings partition, which is kept when you install a newer version
the same way. Without a launcher, use `{image}` above.

SHA-256: `{launcher_sha256}` (also in `{launcher_image}.sha256`)
"""
    if tab5:
        notes += tab5_section(*tab5)
    return notes


def main():
    args = sys.argv[1:]
    if len(args) not in (6, 10):
        print(__doc__)
        return 2
    version, image, sha256, launcher_image, launcher_sha256 = args[:5]
    tab5 = tuple(args[5:9]) if len(args) == 10 else None
    output = args[-1]

    with open("README.md", encoding="utf-8") as f:
        notes = build_notes(f.read(), version, image, sha256, launcher_image, launcher_sha256, tab5)
    with open(output, "w", encoding="utf-8") as f:
        f.write(notes)
    print(notes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
