"""
The CI scripts in .github/scripts: release planning, release notes, warning reports,
dependency version ordering and the firmware checks (with negative controls, so a check
that passes everything cannot stay green).
"""

import io
import json
import os
import subprocess
import sys
import tempfile
import types
import unittest
from contextlib import redirect_stdout

import repo

plan_release = repo.ci_script("plan_release")
release_notes = repo.ci_script("release_notes")
check_firmware = repo.ci_script("check_firmware")
check_audit = repo.ci_script("check_audit")
check_dependencies = repo.ci_script("check_dependencies")

TAGS = {(1, 4, 0), (1, 4, 1)}
FLOOR = (1, 4, 0)


def plan(**env):
    with redirect_stdout(io.StringIO()):
        return plan_release.plan(env, TAGS, FLOOR)


class PlanRelease(unittest.TestCase):
    def test_push_to_main_releases_next_patch(self):
        r = plan(GITHUB_EVENT_NAME="push", GITHUB_REF="refs/heads/main")
        self.assertEqual((r["version"], r["release"], r["prerelease"]), ("1.4.2", True, False))

    def test_push_to_integration_builds_only(self):
        r = plan(GITHUB_EVENT_NAME="push", GITHUB_REF="refs/heads/integration", GITHUB_SHA="abcdef123456")
        self.assertEqual((r["version"], r["release"]), ("1.4.2-dev.abcdef1", False))

    def test_pull_request_builds_only(self):
        r = plan(GITHUB_EVENT_NAME="pull_request", GITHUB_REF="refs/pull/9/merge", GITHUB_SHA="1234567")
        self.assertFalse(r["release"])

    def test_tag_push_releases_that_version(self):
        r = plan(GITHUB_EVENT_NAME="push", GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v2.0.0")
        self.assertEqual((r["version"], r["release"]), ("2.0.0", True))

    def test_bad_tag_rejected(self):
        with self.assertRaises(SystemExit), redirect_stdout(io.StringIO()):
            plan_release.plan({"GITHUB_EVENT_NAME": "push", "GITHUB_REF_TYPE": "tag",
                               "GITHUB_REF_NAME": "release-2"}, TAGS, FLOOR)

    def test_manual_bumps(self):
        self.assertEqual(plan(GITHUB_EVENT_NAME="workflow_dispatch", INPUT_BUMP="minor")["version"], "1.5.0")
        self.assertEqual(plan(GITHUB_EVENT_NAME="workflow_dispatch", INPUT_BUMP="major")["version"], "2.0.0")

    def test_manual_integration_is_prerelease(self):
        r = plan(GITHUB_EVENT_NAME="workflow_dispatch", INPUT_BRANCH="integration")
        self.assertTrue(r["prerelease"])

    def test_manual_exact_version_must_be_new(self):
        with self.assertRaises(SystemExit), redirect_stdout(io.StringIO()):
            plan_release.plan({"GITHUB_EVENT_NAME": "workflow_dispatch", "INPUT_VERSION": "1.4.1"}, TAGS, FLOOR)

    def test_floor_wins_over_older_tags(self):
        with redirect_stdout(io.StringIO()):
            r = plan_release.plan({"GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/main"},
                                  {(1, 3, 9)}, (1, 4, 0))
        self.assertEqual(r["version"], "1.4.0")

    def test_first_release_uses_floor(self):
        self.assertEqual(plan_release.next_version(set(), (1, 4, 0), "patch"), (1, 4, 0))

    def test_release_tags_parsing(self):
        listing = ("aaa\trefs/tags/v1.4.0\nbbb\trefs/tags/v1.4.0^{}\nccc\trefs/tags/v1.5.0-rc1\n"
                   "ddd\trefs/tags/latest\n")
        self.assertEqual(plan_release.release_tags(listing), {(1, 4, 0)})

    def test_project_ver_floor_present(self):
        with redirect_stdout(io.StringIO()):
            self.assertIsNotNone(plan_release.project_ver(repo.path("CMakeLists.txt")))


class ReleaseNotes(unittest.TestCase):
    def test_version_section(self):
        readme = "# X\n\n### v1.4.2 (2026-10-01)\n- Fixed a thing\n\n### v1.4.1\n- Older\n## Other\n"
        title, body = release_notes.version_section(readme, "1.4.2")
        self.assertEqual(title, "v1.4.2 (2026-10-01)")
        self.assertEqual(body, "- Fixed a thing")

    def test_no_section(self):
        self.assertEqual(release_notes.version_section("# X\n", "9.9.9"), (None, None))

    def test_notes_cover_both_boards(self):
        notes = release_notes.build_notes("# X\n", "1.5.0", "SimplrSSH-v1.5.0-release.bin", "a" * 64,
                                          "SimplrSSH-v1.5.0-launcher.bin", "b" * 64,
                                          ("SimplrSSH-Tab5-v1.5.0-release.bin", "c" * 64,
                                           "SimplrSSH-Tab5-v1.5.0-launcher.bin", "d" * 64))
        for text in ("SimplrSSH-v1.5.0-release.bin", "--chip esp32s3", "SimplrSSH-Tab5-v1.5.0-release.bin",
                     "--chip esp32p4", "SimplrSSH-Tab5-v1.5.0-launcher.bin", "c" * 64, "d" * 64):
            self.assertIn(text, notes)
        t_deck_only = release_notes.build_notes("# X\n", "1.5.0", "i", "s", "l", "ls")
        self.assertNotIn("Tab5", t_deck_only)

    def test_version_is_not_a_regex(self):
        self.assertEqual(release_notes.version_section("### v1x4x2\n- no\n", "1.4.2"), (None, None))


class ReportWarnings(unittest.TestCase):
    def run_report(self, log):
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(log)
        env = dict(os.environ, GITHUB_STEP_SUMMARY="")
        out = subprocess.run([sys.executable, repo.path(".github", "scripts", "report_warnings.py"), f.name],
                             capture_output=True, text=True, env=env)
        os.unlink(f.name)
        return out

    def test_project_warnings_annotated(self):
        out = self.run_report("/app/x/main/ssh_terminal.cpp:12:5: warning: unused variable 'a' [-Wunused-variable]\n"
                              "/opt/esp/idf/components/lwip/x.c:1:1: warning: not ours [-Wfoo]\n")
        self.assertEqual(out.returncode, 0)
        self.assertIn("::warning file=main/ssh_terminal.cpp,line=12,col=5", out.stdout)
        self.assertNotIn("lwip", out.stdout)

    def test_missing_log_is_fine(self):
        self.assertEqual(self.run_report("").returncode, 0)


class OutdatedVersionOrder(unittest.TestCase):
    def test_version_key(self):
        # version_key needs no YAML; stand in for PyYAML so this runs on any Python
        sys.modules.setdefault("yaml", types.ModuleType("yaml"))
        key = repo.ci_script("check_outdated").version_key
        self.assertLess(key("9.4.0"), key("9.6.0~1"))
        self.assertLess(key("9.6.0"), key("9.6.0~1"))
        self.assertLess(key("2.0.0-rc1"), key("2.0.0"))
        self.assertLess(key("v5.5.1"), key("v5.5.2"))


class FirmwareChecks(unittest.TestCase):
    def test_key_material_detected(self):
        # Assembled at run time so the source holds nothing that looks like a key (secret-scan)
        marker = b"OPENSSH " + b"PRIVATE KEY-----"
        pem = b"-----BEGIN " + marker + b"\n" + b"QUJD" * 13 + b"\n-----END " + marker + b"\n"
        self.assertEqual(check_firmware.key_material(b"\x00junk" + pem), ["PEM private key"])
        self.assertEqual(check_firmware.key_material(b"x sk-proj-" + b"A" * 40 + b" y"), ["OpenAI-style API key"])

    def test_parser_strings_are_not_keys(self):
        parser = b"-----BEGIN RSA PRIVATE KEY-----\x00-----END RSA PRIVATE KEY-----\x00sk-\x00"
        self.assertEqual(check_firmware.key_material(parser), [])

    def test_surface_negative_controls(self):
        good = {"esp_crt_bundle_attach", "libssh2_session_hostkey", "libssh2_hostkey_hash",
                "esp_netif_create_default_wifi_sta", "esp_wifi_set_storage", "lwip_connect"}
        cfg = dict(check_firmware.REQUIRED_CONFIG)
        self.assertEqual(check_firmware.surface_problems(good, cfg), [])
        for bad in ("lwip_listen", "httpd_start", "esp_https_ota_begin", "esp_bt_controller_init",
                    "mdns_init", "esp_netif_create_default_wifi_ap", "esp_ota_begin"):
            with self.subTest(symbol=bad):
                self.assertTrue(check_firmware.surface_problems(good | {bad}, cfg))
        self.assertTrue(check_firmware.surface_problems(good - {"esp_crt_bundle_attach"}, cfg))
        self.assertTrue(check_firmware.surface_problems(good, dict(cfg, CONFIG_BT_ENABLED="y")))
        self.assertTrue(check_firmware.surface_problems(good, dict(cfg, CONFIG_LWIP_IPV6="y")))
        missing_bundle = {k: v for k, v in cfg.items() if k != "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE"}
        self.assertTrue(check_firmware.surface_problems(good, missing_bundle))
        self.assertTrue(check_firmware.surface_problems(good | {"esp_hosted_slave_ota_begin"}, cfg))
        self.assertTrue(check_firmware.surface_problems(good - {"esp_wifi_set_storage"}, cfg))

    def test_surface_tab5_wifi_coprocessor_options(self):
        good = {"esp_crt_bundle_attach", "libssh2_session_hostkey", "libssh2_hostkey_hash",
                "esp_netif_create_default_wifi_sta", "esp_wifi_set_storage"}
        cfg = dict(check_firmware.REQUIRED_CONFIG, **check_firmware.REQUIRED_CONFIG_TAB5,
                   CONFIG_IDF_TARGET_ESP32P4="y")
        self.assertEqual(check_firmware.surface_problems(good, cfg), [])
        for option, bad in (("CONFIG_WIFI_RMT_SOFTAP_SUPPORT", "y"), ("CONFIG_WIFI_RMT_NVS_ENABLED", "y"),
                            ("CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE", "y"),
                            ("CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED", "n")):
            with self.subTest(option=option):
                self.assertTrue(check_firmware.surface_problems(good, dict(cfg, **{option: bad})))
        # The T-Deck's sdkconfig doesn't need the Tab5's options
        self.assertEqual(check_firmware.surface_problems(good, dict(check_firmware.REQUIRED_CONFIG)), [])

    def test_warnings_gate(self):
        log = ("/project/main/ssh_terminal.cpp:368:22: warning: 'void lv_obj_remove_flag(lv_obj_t*, "
               "lv_obj_flag_t)' is deprecated: Use setters [-Wdeprecated-declarations]\n"
               "/opt/esp/idf/components/lwip/x.c:1:1: warning: not ours [-Wfoo]\n")
        blocking, known = check_firmware.warning_problems(log)
        self.assertEqual((len(blocking), len(known)), (0, 1))
        blocking, _ = check_firmware.warning_problems(
            log + "/project/main/chat_app.cpp:10:5: warning: unused variable 'x' [-Wunused-variable]\n")
        self.assertEqual(blocking, ["main/chat_app.cpp:10: unused variable 'x'"])

    def test_size_budget(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump({"used_diram": check_firmware.INTERNAL_RAM_BUDGET + 1}, f)
        try:
            with redirect_stdout(io.StringIO()):
                self.assertEqual(check_firmware.cmd_size([f.name]), 1)
                self.assertEqual(check_firmware.cmd_size([f.name, "--budget", str(10 ** 9)]), 0)
                self.assertEqual(check_firmware.cmd_size([f.name, "--board", "tab5"]), 0)
            with open(f.name, "w") as g:
                json.dump({"used_diram": check_firmware.TAB5_INTERNAL_RAM_BUDGET + 1}, g)
            with redirect_stdout(io.StringIO()):
                self.assertEqual(check_firmware.cmd_size([f.name, "--board", "tab5"]), 1)
        finally:
            os.unlink(f.name)



def cve(cve_id, severity, vulnerable="YES", kev=""):
    return {"cve_id": cve_id, "cvss_base_severity": severity, "vulnerable": vulnerable, "kev_added": kev,
            "pkg_name": "mbed_tls", "pkg_version": "3.6.4", "cvss_base_score": 9.0}


class Audit(unittest.TestCase):
    def test_new_high_or_critical_fails(self):
        failures, _, _ = check_audit.evaluate([cve("CVE-2099-0001", "CRITICAL"), cve("CVE-2099-0002", "HIGH")], {})
        self.assertEqual(len(failures), 2)

    def test_baselined_passes(self):
        failures, warnings, _ = check_audit.evaluate([cve("CVE-2099-0001", "HIGH")], {"CVE-2099-0001": "why"})
        self.assertEqual((failures, warnings), ([], []))

    def test_known_exploited_always_fails(self):
        failures, _, _ = check_audit.evaluate([cve("CVE-2099-0001", "MEDIUM", kev="2099-01-01")],
                                              {"CVE-2099-0001": "why"})
        self.assertEqual(len(failures), 1)

    def test_medium_and_maybe_warn(self):
        failures, warnings, _ = check_audit.evaluate(
            [cve("CVE-2099-0003", "MEDIUM"), cve("CVE-2099-0004", "CRITICAL", vulnerable="MAYBE"),
             cve("CVE-2099-0005", "CRITICAL", vulnerable="EXCLUDED"), cve("", "", vulnerable="NO")], {})
        self.assertEqual(failures, [])
        self.assertEqual(len(warnings), 2)

    def test_repository_baseline_is_well_formed(self):
        accepted = check_audit.load_baseline(repo.path(".github", "audit-baseline.txt"))
        for cve_id, reason in accepted.items():
            with self.subTest(cve=cve_id):
                self.assertTrue(reason.strip())


RECONFIGURE_LOG = """NOTICE: Processing 3 dependencies:
NOTICE: [1/3] espressif/button (4.2.1)
NOTICE: [2/3] lvgl/lvgl (9.4.0)
NOTICE: [3/3] idf (5.5.1)
-- something else
NOTICE: Processing 3 dependencies:
NOTICE: [1/3] espressif/button (4.2.1) (/project/managed_components/espressif__button)
NOTICE: [2/3] lvgl/lvgl (9.6.0~1)
NOTICE: [3/3] idf (5.5.1)
"""


class Dependencies(unittest.TestCase):
    def test_first_pass_only(self):
        self.assertEqual(check_dependencies.first_install_pass(RECONFIGURE_LOG),
                         {"espressif/button": "4.2.1", "lvgl/lvgl": "9.4.0"})

    def test_missing_pass(self):
        self.assertIsNone(check_dependencies.first_install_pass("nothing here"))

    def test_repository_lock_parses(self):
        locked = check_dependencies.lock_versions(repo.read("dependencies.lock"))
        self.assertIn("lvgl/lvgl", locked)
        self.assertNotIn("idf", locked)
        for name, (lock_v, _built) in check_dependencies.KNOWN_DRIFT.items():
            with self.subTest(component=name):
                self.assertEqual(locked.get(name), lock_v, "KNOWN_DRIFT must match dependencies.lock")

    def run_check(self, compiled, lock_text, defaults="CONFIG_A=y\n", generated="CONFIG_A=y\n", board="tdeck",
                  board_defaults=""):
        with tempfile.TemporaryDirectory() as root:
            for name, version in compiled.items():
                folder = os.path.join(root, "managed_components", name.replace("/", "__"))
                os.makedirs(folder)
                with open(os.path.join(folder, "idf_component.yml"), "w") as f:
                    f.write(f"version: '{version}'\n")
            files = {"lock": lock_text, "sdkconfig.defaults": defaults, "gen": generated, "committed": generated,
                     "sdkconfig.defaults.esp32p4": board_defaults}
            for name, text in files.items():
                with open(os.path.join(root, name), "w") as f:
                    f.write(text)
            with redirect_stdout(io.StringIO()):
                return check_dependencies.check(root, RECONFIGURE_LOG, os.path.join(root, "lock"),
                                                os.path.join(root, "committed"), os.path.join(root, "gen"),
                                                board)[0]

    LOCK = ("dependencies:\n  espressif/button:\n    version: 4.2.1\n  lvgl/lvgl:\n    version: 9.4.0\n"
            "  idf:\n    version: 5.5.1\ndirect_dependencies:\n- lvgl/lvgl\n")

    def test_matching_build_passes(self):
        self.assertEqual(self.run_check({"espressif/button": "4.2.1", "lvgl/lvgl": "9.4.0"}, self.LOCK), [])

    def test_known_drift_passes_new_drift_fails(self):
        self.assertEqual(self.run_check({"espressif/button": "4.2.1", "lvgl/lvgl": "9.6.0~1"}, self.LOCK), [])
        problems = self.run_check({"espressif/button": "4.2.1", "lvgl/lvgl": "9.7.0"}, self.LOCK)
        self.assertTrue(any("9.7.0" in p for p in problems))

    def test_install_must_match_lock(self):
        lock = self.LOCK.replace("version: 4.2.1", "version: 4.3.0")
        problems = self.run_check({"espressif/button": "4.3.0", "lvgl/lvgl": "9.4.0"}, lock)
        self.assertTrue(any("installed 4.2.1" in p for p in problems))

    def test_tab5_defaults_override_shared_ones(self):
        compiled = {"espressif/button": "4.2.1", "lvgl/lvgl": "9.6.0~1"}   # Known drift on the Tab5 too
        self.assertEqual(self.run_check(compiled, self.LOCK, defaults="CONFIG_A=n\n", generated="CONFIG_A=y\n",
                                        board="tab5", board_defaults="CONFIG_A=y\n"), [])
        problems = self.run_check(compiled, self.LOCK, defaults="CONFIG_A=y\n", generated="CONFIG_A=y\n",
                                  board="tab5", board_defaults="CONFIG_A=n\n")
        self.assertTrue(any("CONFIG_A" in p for p in problems))

    def test_sdkconfig_default_must_take_effect(self):
        problems = self.run_check({"espressif/button": "4.2.1", "lvgl/lvgl": "9.4.0"}, self.LOCK,
                                  defaults="CONFIG_A=n\n", generated="CONFIG_A=y\n")
        self.assertTrue(any("CONFIG_A" in p for p in problems))


if __name__ == "__main__":
    unittest.main()
