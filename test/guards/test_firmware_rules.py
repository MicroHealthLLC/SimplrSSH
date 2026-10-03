"""
Firmware rules from CLAUDE.md that can be checked in the source: settings access, task
stacks, collection bounds, logging hygiene, and the docs staying in step with the code.
"""

import glob
import os
import re
import unittest

import repo

SRC = {name: repo.strip_comments(text) for name, text in repo.sources().items()}


def grep(pattern, exclude=()):
    hits = []
    for name, code in SRC.items():
        if os.path.basename(name) in exclude:
            continue
        for i, line in enumerate(code.splitlines(), 1):
            if re.search(pattern, line):
                hits.append(f"{name}:{i}: {line.strip()}")
    return hits


class Build(unittest.TestCase):
    def test_cmake_lists_every_source(self):
        cmake = repo.read("main", "CMakeLists.txt")
        listed = set(re.findall(r'"([\w]+\.cpp)"', cmake))
        present = {os.path.basename(f) for f in glob.glob(os.path.join(repo.MAIN, "*.cpp"))}
        self.assertEqual(listed, present, "main/CMakeLists.txt SOURCES must match main/*.cpp")

    def test_claude_md_code_layout_covers_every_source(self):
        layout = repo.read("CLAUDE.md").split("## Code layout", 1)[1].split("\n## ", 1)[0]
        documented = set(re.findall(r"`main/([\w]+\.cpp)`", layout))
        # Small helpers without their own row: hardware glue and pure functions
        undocumented_ok = {"battery_measurement.cpp", "c3_keyboard.cpp"}
        present = {os.path.basename(f) for f in glob.glob(os.path.join(repo.MAIN, "*.cpp"))}
        self.assertEqual(sorted(present - documented - undocumented_ok), [],
                         "add new source files to the Code layout table in CLAUDE.md")
        self.assertEqual(sorted(documented - present), [], "Code layout lists files that no longer exist")


class Settings(unittest.TestCase):
    def test_namespaces_opened_through_settings_nvs(self):
        self.assertEqual(grep(r"\bnvs_open\s*\(", exclude=("settings_nvs.cpp",)), [],
                         "open namespaces with settings_nvs::open()")
        self.assertEqual(grep(r"NVS_DEFAULT_PART_NAME", exclude=("settings_nvs.cpp",)), [],
                         "use settings_nvs::partition()")

    def test_launcher_nvs_never_erased(self):
        self.assertEqual(grep(r"\bnvs_flash_erase\s*\(\s*\)"), [], "never erase the default nvs")
        self.assertEqual(grep(r"nvs_flash_erase_partition(_ptr)?\s*\(", exclude=("settings_nvs.cpp",)), [])

    def test_every_namespace_is_backed_up(self):
        backup = repo.read("main", "settings_backup.cpp")
        backed_up = set(re.findall(r'"(\w+)"', re.search(r"NAMESPACES\[\]\s*=\s*\{([^}]*)\}", backup).group(1)))
        used = set()
        for code in SRC.values():
            used |= set(re.findall(r'settings_nvs::open\(\s*"(\w+)"', code))
            used |= set(re.findall(r'(?:load|save)_records\(\s*"(\w+)"', code))
            used |= set(re.findall(r'static const char\s*\*\s*(?:NVS_NAMESPACE|NS_\w+)\s*=\s*"(\w+)"', code))
        never_backed_up = {"vault_dev"}   # Per-device key: a backup must not unlock passwords elsewhere
        self.assertTrue(used, "no namespaces found - update this test")
        self.assertEqual(sorted(used - backed_up - never_backed_up), [],
                         "list new NVS namespaces in settings_backup.cpp NAMESPACES")
        self.assertNotIn("vault_dev", backed_up)

    def test_legacy_names_still_read(self):
        self.assertIn('"pocketssh"', repo.read("main", "settings_nvs.cpp"))
        self.assertIn('"POCKETSSH-BACKUP 1"', repo.read("main", "settings_backup.cpp"))
        self.assertRegex(repo.read("main", "storage_menu.cpp"), r"/pocketssh")


class Tasks(unittest.TestCase):
    # CLAUDE.md: keypad 8 KB, SSH receive 8 KB, trackball 2 KB, ChatGPT worker 10 KB
    STACKS = {"keypad_task": 8192, "trackball_task": 2048, "ssh_rx": 8192, "chat": "WORKER_STACK",
              "models": "WORKER_STACK"}

    def test_task_stacks_match_budget(self):
        found = {}
        for code in SRC.values():
            for name, stack in re.findall(r'xTaskCreate\w*\(\s*\w+\s*,\s*"(\w+)"\s*,\s*(\w+)', code):
                found[name] = int(stack) if stack.isdigit() else stack
        self.assertEqual(found, self.STACKS, "a new or resized task needs CLAUDE.md updated (no long-lived tasks)")
        self.assertIn("WORKER_STACK = 10240", SRC["main/chat_app.cpp"])

    def test_no_busy_wait(self):
        self.assertEqual(grep(r"while\s*\(\s*(1|true)\s*\)\s*;"), [])


class Bounds(unittest.TestCase):
    def test_collection_bounds(self):
        header = repo.read("main", "include", "connection_profiles.hpp")
        terminal = repo.read("main", "ssh_terminal.cpp")
        claude = repo.read("CLAUDE.md")
        profiles = int(re.search(r"#define PROFILE_MAX_COUNT (\d+)", header).group(1))
        networks = int(re.search(r"#define NETWORK_MAX_COUNT (\d+)", header).group(1))
        keys = int(re.search(r"MAX_KEYS = (\d+);", terminal).group(1))
        key_kb = int(re.search(r"MAX_TOTAL_BYTES = (\d+) \* 1024;", terminal).group(1))
        scrollback = int(re.search(r"SCROLLBACK_MAX = (\d+);", terminal).group(1))
        history = re.findall(r"command_history\.size\(\) > (\d+)", terminal)
        self.assertEqual(history, ["100"])
        bounds = re.search(r"Bound every collection[^(]*\(([^)]*)\)", claude.replace("\n", " ")).group(1)
        self.assertIn("history 100", bounds)
        self.assertIn(f"profiles {profiles}", bounds)
        self.assertIn(f"networks {networks}", bounds)
        self.assertIn(f"SD keys {keys} / {key_kb} KB", bounds)
        self.assertIn(f"terminal text ~{scrollback // 1024} KB", bounds)


class Logging(unittest.TestCase):
    LOG = re.compile(r"ESP_LOG[EWIDV]\s*\(\s*(\w+)\s*,\s*([^;]*)\)\s*;")
    SENSITIVE = re.compile(r"\b\w*(password|passwd|passphrase|pin|secret|api_?key|private_?key|"
                           r"current_input|command|cmd|ssid|username|plain)\w*\b", re.I)

    def test_log_formats_are_literals(self):
        for name, code in SRC.items():
            for _tag, args in self.LOG.findall(code):
                with self.subTest(file=name, args=args[:60]):
                    self.assertTrue(args.lstrip().startswith('"'), "no format strings built from data")

    def test_no_secrets_or_user_input_logged(self):
        for name, code in SRC.items():
            for _tag, args in self.LOG.findall(code):
                values = args.split('"')[-1] if '"' in args else args   # Arguments after the format
                values = re.sub(r"[\w.]+\.(size|empty|length)\(\)", "", values)   # Counts are fine
                for word in self.SENSITIVE.findall(values):
                    with self.subTest(file=name, log=args[:80]):
                        self.fail(f"logs '{word}': never log user input, commands or secrets")

    def test_libssh2_trace_disabled(self):
        self.assertIn("CONFIG_LIBSSH2_DEBUG_ENABLE=n", repo.read("sdkconfig.defaults"))
        self.assertEqual(grep(r"libssh2_trace\s*\("), [])


class Secrets(unittest.TestCase):
    def test_host_key_checked_before_session_use(self):
        terminal = SRC["main/ssh_terminal.cpp"]
        self.assertIn("check_host_key", terminal)
        self.assertIn("ssh_begin", terminal)
        self.assertIn("ssh_finish", terminal)

    def test_redaction_used_for_echo_and_history(self):
        terminal = SRC["main/ssh_terminal.cpp"]
        self.assertGreaterEqual(terminal.count("redact_secrets("), 2)

    def test_secrets_wiped(self):
        self.assertIn("mbedtls_platform_zeroize", SRC["main/secret_vault.cpp"])
        self.assertGreater(sum(code.count("vault::wipe(") for code in SRC.values()), 3)


class SdkconfigDefaults(unittest.TestCase):
    def test_every_setting_explained(self):
        lines = repo.read("sdkconfig.defaults").splitlines()
        for i, line in enumerate(lines):
            if not line.startswith("CONFIG_"):
                continue
            j = i - 1
            while j >= 0 and lines[j].startswith("CONFIG_"):
                j -= 1
            with self.subTest(option=line):
                self.assertTrue(j >= 0 and lines[j].startswith("#"), "comment each setting in sdkconfig.defaults")

    def test_defaults_applied_to_committed_sdkconfig(self):
        committed = repo.sdkconfig()
        for line in repo.read("sdkconfig.defaults").splitlines():
            m = re.match(r"^(CONFIG_\w+)=(.*)$", line)
            if not m or m.group(1) not in committed:
                continue
            with self.subTest(option=m.group(1)):
                self.assertEqual(committed[m.group(1)], m.group(2),
                                 "sdkconfig overrides sdkconfig.defaults: update both")


if __name__ == "__main__":
    unittest.main()
