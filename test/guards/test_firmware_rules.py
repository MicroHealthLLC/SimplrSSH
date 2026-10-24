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
    # AI_RULES.md: keypad 8 KB, SSH receive 8 KB, trackball 2 KB, ChatGPT worker 10 KB,
    # sign-in page worker 10 KB
    STACKS = {"keypad_task": 8192, "trackball_task": 2048, "ssh_rx": 8192, "chat": "WORKER_STACK",
              "models": "WORKER_STACK", "portal": 10240}

    def test_task_stacks_match_budget(self):
        found = {}
        for code in SRC.values():
            for name, stack in re.findall(r'xTaskCreate\w*\(\s*\w+\s*,\s*"(\w+)"\s*,\s*(\w+)', code):
                found[name] = int(stack) if stack.isdigit() else stack
        self.assertEqual(found, self.STACKS, "a new or resized task needs AI_RULES.md updated (no long-lived tasks)")
        self.assertIn("WORKER_STACK = 10240", SRC["main/chat_app.cpp"])

    def test_no_busy_wait(self):
        self.assertEqual(grep(r"while\s*\(\s*(1|true)\s*\)\s*;"), [])

    def test_recording_loop_ends_when_the_microphone_returns_nothing(self):
        # mic_read() returns 0 on a read error; the loop would then spin without waiting
        code = SRC["main/chat_app.cpp"]
        self.assertRegex(code, r"size_t got = audio::mic_read\([^;]*;\s*if \(got == 0\) \{\s*break;")


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

    def test_sign_in_page_bounds(self):
        header = repo.read("main", "include", "portal.hpp")
        rules = repo.read("AI_RULES.md").replace("\n", " ")
        page_kb = int(re.search(r"MAX_PAGE_BYTES = (\d+) \* 1024;", header).group(1))
        fields = int(re.search(r"MAX_FIELDS = (\d+);", header).group(1))
        cookies = int(re.search(r"MAX_COOKIES = (\d+);", header).group(1))
        self.assertIn(f"sign-in pages {page_kb} KB in PSRAM / {fields} fields / {cookies} cookies", rules)
        self.assertIn("MALLOC_CAP_SPIRAM", SRC["main/portal_client.cpp"])


class KnownHosts(unittest.TestCase):
    def test_host_key_records_not_read_into_a_fixed_buffer(self):
        # A long host name made the record exceed a 160-byte buffer: the read failed, a changed
        # key then looked like a new server (Unknown) instead of a mismatch
        code = SRC[os.path.join("main", "known_hosts.cpp")]
        self.assertEqual(re.findall(r"char value\[\d+\]", code), [])


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


class Power(unittest.TestCase):
    """Sleep and power off (AI_RULES.md, UI model): never taken from an SSH session's input."""

    def test_shell_named_commands_only_without_ssh(self):
        terminal = SRC["main/ssh_terminal.cpp"]
        for word in ("sleep", "shutdown", "poweroff", "power"):
            lines = [line for line in terminal.splitlines() if f'cmd == "{word}"' in line]
            self.assertTrue(lines, f"'{word}' command missing")
            for line in lines:
                with self.subTest(command=word):
                    self.assertIn("!ssh_connected", line)

    def test_request_refused_while_connected(self):
        home = SRC["main/home_menu.cpp"]
        body = home.split("void SSHTerminal::request_power", 1)[1].split("\n}", 1)[0]
        self.assertIn("ssh_connected", body)

    def test_power_prepare_saves_and_stops(self):
        home = SRC["main/home_menu.cpp"]
        body = home.split("void SSHTerminal::power_prepare", 1)[1].split("\n}", 1)[0]
        for call in ("close_chat()", "save_history_to_nvs()", "wifi_sleep()", "vault::lock()"):
            with self.subTest(call=call):
                self.assertIn(call, body)


# Each board's defaults and the sdkconfig they must have reached (later files override earlier)
BOARD_CONFIGS = {
    "sdkconfig": ["sdkconfig.defaults"],
    "sdkconfig.tab5": ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4"],
}


class Dictation(unittest.TestCase):
    # AI_RULES.md: in an SSH session the talk key types speech at the cursor, without Enter
    @staticmethod
    def body(name):
        return SRC["main/chat_app.cpp"].split(f"SSHTerminal::{name}(", 1)[1].split("\n}", 1)[0]

    def test_transcript_is_one_printable_line(self):
        # Untrusted text from the network: printable ASCII, bounded, no Enter or control codes
        self.assertIn("portal::printable(heard, MAX_DICTATION)", self.body("dictate_job"))

    def test_typed_only_into_the_session_it_was_spoken_in(self):
        tick = self.body("dictate_tick")
        self.assertIn("ssh_generation == dictate_ssh_generation", tick)
        self.assertNotIn("\\r", tick)
        self.assertNotIn("\\n", tick)

    def test_slow_ctrl_key_drops_the_recording(self):
        self.assertIn("INPUT_HOLD_CANCEL", SRC["main/tab5_keymap.cpp"])
        self.assertIn("cancel_talk()", SRC["main/deck_base.cpp"])


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
                    "audio_tab5.cpp", "vterm.cpp", "term_view.cpp"},
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

    def test_both_boards_define_every_hint(self):
        hints = set(re.findall(r"extern const char\* const (HINT_\w+);", repo.read("main", "include", "board.hpp")))
        self.assertIn("HINT_PICK", hints)
        for board in ("board_tdeck.cpp", "board_tab5.cpp"):
            for hint in hints:
                with self.subTest(board=board, hint=hint):
                    self.assertRegex(SRC[f"main/{board}"], rf'const char\* const board::{hint} = "')


class Ui(unittest.TestCase):
    """UI model (AI_RULES.md): every menu and prompt is a page; LVGL lives in PSRAM; taps do no work."""
    MENU_FILES = sorted({os.path.relpath(f, repo.ROOT) for f in glob.glob(os.path.join(repo.MAIN, "*_menu.cpp"))}
                        | {"main/chat_app.cpp", "main/known_hosts.cpp"})

    def test_no_numbered_text_menus(self):
        numbered = re.compile(r'append_text\(\s*"(?:\\n)?\s*\d+\)')
        for name in self.MENU_FILES:
            with self.subTest(file=name):
                self.assertNotRegex(SRC[name], numbered, "menus are pages (ui::row), not numbered text")

    def test_every_step_prompt_shows_a_page(self):
        found = 0
        for name, code in SRC.items():
            for m in re.finditer(r"bool SSHTerminal::(\w+_step_prompt)\(\)\s*\{", code):
                found += 1
                body = code[m.end():].split("\n}", 1)[0]
                with self.subTest(file=name, function=m.group(1)):
                    self.assertIn("show_page(", body, "prompts and menus are drawn as pages")
        self.assertGreaterEqual(found, 8, "no *_step_prompt() found - update this test")

    def test_lvgl_allocates_in_psram(self):
        self.assertIn("CONFIG_LV_USE_CUSTOM_MALLOC=y", repo.read("sdkconfig.defaults").splitlines())
        for config in BOARD_CONFIGS:
            with self.subTest(sdkconfig=config):
                self.assertEqual(repo.sdkconfig(config).get("CONFIG_LV_USE_CUSTOM_MALLOC"), "y")
        mem = repo.strip_comments(repo.read("main", "lv_mem_psram.c"))
        malloc = mem.split("lv_malloc_core", 1)[1].split("\n}", 1)[0]
        self.assertTrue("MALLOC_CAP_SPIRAM" in malloc or "PSRAM_CAPS" in malloc)
        self.assertRegex(mem, r"PSRAM_CAPS\s*=\s*MALLOC_CAP_SPIRAM")
        self.assertIn('"lv_mem_psram.c"', repo.read("main", "CMakeLists.txt"))

    def test_taps_only_post_an_input_event(self):
        terminal = SRC["main/ssh_terminal.cpp"]
        for callback in ("on_tap", "on_back"):
            with self.subTest(callback=callback):
                self.assertIn(f"view.{callback} = [", terminal)
                body = terminal.split(f"view.{callback} = [", 1)[1].split("\n    };", 1)[0]
                self.assertIn("post_input(board::INPUT_ACTIVATE)", body)
                for work in ("wizard_handle_input", "page_answer", "connect", "scan", "execute_command",
                             "handle_key_input", "save"):
                    self.assertNotIn(work, body, "LVGL callbacks only record the tap; keypad_task does the work")
        self.assertIn("INPUT_ACTIVATE", SRC["main/deck_base.cpp"])

    def test_page_model_is_host_tested(self):
        for name in ("main/ui_page.cpp", "main/include/ui_page.hpp"):
            with self.subTest(file=name):
                self.assertNotRegex(SRC[name], r'#include\s*[<"](lvgl\.h|esp_|freertos/)',
                                    "keep the page model free of LVGL / ESP-IDF headers")
        self.assertIn("${MAIN_DIR}/ui_page.cpp", repo.read("test", "host", "CMakeLists.txt"))

    def test_sources_are_ascii(self):
        # On-screen text must be ASCII (LV_SYMBOL_* icons are escape sequences); the generated
        # terminal font is binary data
        files = (glob.glob(os.path.join(repo.MAIN, "*.cpp")) + glob.glob(os.path.join(repo.MAIN, "*.c"))
                 + glob.glob(os.path.join(repo.MAIN, "include", "*.h*")))
        for f in sorted(files):
            if os.path.basename(f) == "font_term_mono_18.c":
                continue
            with open(f, "rb") as fh:
                data = fh.read()
            for i, line in enumerate(data.split(b"\n"), 1):
                if any(b > 0x7F for b in line):
                    with self.subTest(file=os.path.relpath(f, repo.ROOT), line=i):
                        self.fail("non-ASCII byte: use plain ASCII or an escape sequence")


class Contrast(unittest.TestCase):
    """WCAG 2.2 AAA 1.4.6: theme text colors reach 7:1 against the fills they sit on."""

    @staticmethod
    def theme():
        text = repo.read("main", "include", "ui_view.hpp")
        return {k: int(v, 16) for k, v in re.findall(r"uint32_t (\w+) = 0x([0-9A-Fa-f]{6});", text)}

    @staticmethod
    def lum(h):
        chan = [((h >> s) & 255) / 255 for s in (16, 8, 0)]
        chan = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4 for c in chan]
        return 0.2126 * chan[0] + 0.7152 * chan[1] + 0.0722 * chan[2]

    def ratio(self, a, b):
        hi, lo = sorted((self.lum(a), self.lum(b)), reverse=True)
        return (hi + 0.05) / (lo + 0.05)

    def test_text_pairs_reach_7_to_1(self):
        t = self.theme()
        white = 0xFFFFFF
        pairs = [(fg, bg) for fg in ("TEXT", "DIM", "DANGER", "OK", "WARN") for bg in ("BG", "SURFACE", "DIVIDER")
                 ]
        for fg, bg in pairs:
            with self.subTest(fg=fg, bg=bg):
                self.assertGreaterEqual(self.ratio(t[fg], t[bg]), 7.0)
        for fg in (white, 0xE6EDF3):  # white and the selected row's dim text on the accent fill
            with self.subTest(fg=hex(fg), bg="ACCENT_BG"):
                self.assertGreaterEqual(self.ratio(fg, t["ACCENT_BG"]), 7.0)
        self.assertGreaterEqual(self.ratio(t["ACCENT"], t["BG"]), 3.0, "focus outline needs 3:1 (1.4.11)")

    def test_terminal_ansi_colors_reach_7_to_1_on_black(self):
        text = repo.read("main", "term_view.cpp")
        block = re.search(r"ANSI\[16\] = \{(.*?)\};", text, re.S).group(1)
        colors = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{6})", block)]
        self.assertEqual(len(colors), 16)
        for i, c in enumerate(colors[1:], 1):
            with self.subTest(index=i):
                self.assertGreaterEqual(self.ratio(c, 0), 7.0)


if __name__ == "__main__":
    unittest.main()
