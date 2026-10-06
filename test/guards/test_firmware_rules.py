"""
Firmware rules from AI_RULES.md that can be checked in the source: settings access, task
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

    def test_ai_rules_code_layout_covers_every_source(self):
        layout = repo.read("AI_RULES.md").split("## Code layout", 1)[1].split("\n## ", 1)[0]
        documented = set(re.findall(r"`main/([\w]+\.cpp)`", layout))
        # Small helpers without their own row: hardware glue and pure functions
        undocumented_ok = {"battery_measurement.cpp", "c3_keyboard.cpp", "battery_tab5.cpp"}
        present = {os.path.basename(f) for f in glob.glob(os.path.join(repo.MAIN, "*.cpp"))}
        self.assertEqual(sorted(present - documented - undocumented_ok), [],
                         "add new source files to the Code layout table in AI_RULES.md")
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
    # AI_RULES.md: keypad 8 KB, SSH receive 8 KB, trackball 2 KB, ChatGPT worker 10 KB
    STACKS = {"keypad_task": 8192, "trackball_task": 2048, "ssh_rx": 8192, "chat": "WORKER_STACK",
              "models": "WORKER_STACK"}

    def test_task_stacks_match_budget(self):
        found = {}
        for code in SRC.values():
            for name, stack in re.findall(r'xTaskCreate\w*\(\s*\w+\s*,\s*"(\w+)"\s*,\s*(\w+)', code):
                found[name] = int(stack) if stack.isdigit() else stack
        self.assertEqual(found, self.STACKS, "a new or resized task needs AI_RULES.md updated (no long-lived tasks)")
        self.assertIn("WORKER_STACK = 10240", SRC["main/chat_app.cpp"])

    def test_no_busy_wait(self):
        self.assertEqual(grep(r"while\s*\(\s*(1|true)\s*\)\s*;"), [])


class Bounds(unittest.TestCase):
    def test_collection_bounds(self):
        header = repo.read("main", "include", "connection_profiles.hpp")
        terminal = repo.read("main", "ssh_terminal.cpp")
        rules = repo.read("AI_RULES.md")
        profiles = int(re.search(r"#define PROFILE_MAX_COUNT (\d+)", header).group(1))
        networks = int(re.search(r"#define NETWORK_MAX_COUNT (\d+)", header).group(1))
        keys = int(re.search(r"MAX_KEYS = (\d+);", terminal).group(1))
        key_kb = int(re.search(r"MAX_TOTAL_BYTES = (\d+) \* 1024;", terminal).group(1))
        scrollback = int(re.search(r"SCROLLBACK_MAX = (\d+);", terminal).group(1))
        history = re.findall(r"command_history\.size\(\) > (\d+)", terminal)
        self.assertEqual(history, ["100"])
        bounds = re.search(r"Bound every collection[^(]*\(([^)]*)\)", rules.replace("\n", " ")).group(1)
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


# Each board's defaults and the sdkconfig they must have reached (later files override earlier)
BOARD_CONFIGS = {
    "sdkconfig": ["sdkconfig.defaults"],
    "sdkconfig.tab5": ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4"],
}


class SdkconfigDefaults(unittest.TestCase):
    def test_every_setting_explained(self):
        for name in ("sdkconfig.defaults", "sdkconfig.defaults.esp32p4"):
            lines = repo.read(name).splitlines()
            for i, line in enumerate(lines):
                if not line.startswith("CONFIG_"):
                    continue
                j = i - 1
                while j >= 0 and lines[j].startswith("CONFIG_"):
                    j -= 1
                with self.subTest(file=name, option=line):
                    self.assertTrue(j >= 0 and lines[j].startswith("#"), f"comment each setting in {name}")

    def test_defaults_applied_to_committed_sdkconfig(self):
        for config, defaults_files in BOARD_CONFIGS.items():
            committed = repo.sdkconfig(config)
            wanted = {}
            for name in defaults_files:
                wanted.update(re.findall(r"(?m)^(CONFIG_\w+)=(.*)$", repo.read(name)))
            for option, value in wanted.items():
                if option not in committed:
                    continue
                with self.subTest(sdkconfig=config, option=option):
                    self.assertEqual(committed[option], value, f"{config} overrides its defaults: update both")

    def test_main_task_stack_fits_boot(self):
        # app_main brings up SD, display, touch and the terminal screen: the 3.5 KB default
        # overflowed on the Tab5 (black screen, reboot loop)
        for config in BOARD_CONFIGS:
            with self.subTest(sdkconfig=config):
                self.assertGreaterEqual(int(repo.sdkconfig(config)["CONFIG_ESP_MAIN_TASK_STACK_SIZE"]), 12288)

    def test_tab5_sdkconfig_is_for_the_tab5(self):
        cfg = repo.sdkconfig("sdkconfig.tab5")
        self.assertEqual(cfg.get("CONFIG_IDF_TARGET"), '"esp32p4"')
        self.assertEqual(repo.sdkconfig().get("CONFIG_IDF_TARGET"), '"esp32s3"')


class Boards(unittest.TestCase):
    """One source tree: shared code stays board-independent, board code sits behind board.hpp."""
    BOARD_FILES = {
        "esp32s3": {"board_tdeck.cpp", "c3_keyboard.cpp", "battery_measurement.cpp", "audio_tdeck.cpp"},
        "esp32p4": {"board_tab5.cpp", "tab5_keyboard.cpp", "tab5_keymap.cpp", "battery_tab5.cpp",
                    "audio_tab5.cpp"},
    }

    def test_board_sources_listed_per_target(self):
        cmake = repo.read("main", "CMakeLists.txt")
        tab5, tdeck = cmake.split('if(IDF_TARGET STREQUAL "esp32p4")', 1)[1].split("else()", 1)
        tdeck = tdeck.split("endif()", 1)[0]
        self.assertEqual(set(re.findall(r'"(\w+\.cpp)"', tab5)), self.BOARD_FILES["esp32p4"])
        self.assertEqual(set(re.findall(r'"(\w+\.cpp)"', tdeck)), self.BOARD_FILES["esp32s3"])

    def test_shared_code_has_no_board_pins(self):
        board_files = set().union(*self.BOARD_FILES.values()) | {"sd_card.cpp"}   # sd_card: per-board #if
        for name, code in SRC.items():
            if os.path.basename(name) in board_files or not name.endswith(".cpp"):
                continue
            with self.subTest(file=name):
                self.assertNotRegex(code, r'#include\s*"utilities\.h"|lv_font_montserrat_\d+|BOARD_\w+',
                                    "board pins and fonts belong in the board files (board::ui())")

    def test_both_boards_implement_the_board_interface(self):
        header = repo.read("main", "include", "board.hpp")
        functions = set(re.findall(r"^\s+(?:[\w:*&]+\s+)+?(\w+)\(", header, re.M)) - {"post_input"}
        for board in ("board_tdeck.cpp", "board_tab5.cpp"):
            code = SRC[f"main/{board}"]
            for fn in functions:
                with self.subTest(board=board, function=fn):
                    self.assertRegex(code, rf"board::{fn}\(")


if __name__ == "__main__":
    unittest.main()
