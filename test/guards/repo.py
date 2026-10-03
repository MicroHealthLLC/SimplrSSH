"""
Helpers shared by the guard tests: paths and small readers for the repository's files.
Guard tests keep the rules in AI_RULES.md from regressing; they read files, never build.
"""

import glob
import importlib.util
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MAIN = os.path.join(ROOT, "main")


def path(*parts):
    return os.path.join(ROOT, *parts)


def read(*parts):
    with open(path(*parts), encoding="utf-8") as f:
        return f.read()


def sources():
    """Firmware sources as {relative path: text}."""
    files = sorted(glob.glob(os.path.join(MAIN, "*.cpp")) + glob.glob(os.path.join(MAIN, "include", "*.h*")))
    return {os.path.relpath(f, ROOT): open(f, encoding="utf-8").read() for f in files}


def strip_comments(code):
    """C/C++ source without comments and string contents (keeps the quotes)."""
    code = re.sub(r"/\*.*?\*/", " ", code, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)
    return code


def workflows():
    files = sorted(glob.glob(path(".github", "workflows", "*.yml")) + glob.glob(path(".github", "workflows", "*.yaml")))
    return {os.path.relpath(f, ROOT): open(f, encoding="utf-8").read() for f in files}


def sdkconfig(name="sdkconfig"):
    """{option: value} from an sdkconfig file; 'is not set' lines become 'n'."""
    values = {}
    with open(path(name) if not os.path.isabs(name) else name, encoding="utf-8") as f:
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


def partitions(name):
    """Rows of a partition CSV as dicts with integer offset/size."""
    rows = []
    for line in read(name).splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        rows.append({"name": cols[0], "type": cols[1], "subtype": cols[2],
                     "offset": parse_size(cols[3]), "size": parse_size(cols[4])})
    return rows


def parse_size(text):
    text = text.strip()
    if text.upper().endswith("M"):
        return int(text[:-1], 0) * 1024 * 1024
    if text.upper().endswith("K"):
        return int(text[:-1], 0) * 1024
    return int(text, 0)


def ci_script(name):
    """Imports .github/scripts/<name>.py as a module."""
    spec = importlib.util.spec_from_file_location(name, path(".github", "scripts", name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
