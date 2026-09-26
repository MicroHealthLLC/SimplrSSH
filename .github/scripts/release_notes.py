#!/usr/bin/env python3
"""
Build GitHub release notes for a firmware version.

Takes the "### v<version>" section from README.md's Version History and adds
flashing instructions and the image checksum. Exits 1 (with an annotation) if
the README has no section for the version, so a release never ships without notes.

Usage: release_notes.py <version> <image-file-name> <sha256> <output.md>
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


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    version, image, sha256, output = sys.argv[1:]

    with open("README.md", encoding="utf-8") as f:
        title, changes = version_section(f.read(), version)
    if not changes:
        print(f"::error file=README.md::No '### v{version}' section in the README Version History. "
              f"Add release notes for v{version} before releasing.")
        return 1

    notes = f"""## What's new in {title}

{changes}

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
"""
    with open(output, "w", encoding="utf-8") as f:
        f.write(notes)
    print(notes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
