"""
Network surface (AI_RULES.md, Scope and Security): station-only WiFi, IPv4, no listening
sockets, no servers, OTA or Bluetooth. The only fixed endpoints are api.openai.com over
verified TLS and the captive-portal probe, the one plain-HTTP address, fetched only while the
user signs in to a network. The built firmware is checked too, by check_firmware.py in CI.
"""

import re
import unittest

import repo

SRC = {name: repo.strip_comments(text) for name, text in repo.sources().items()}
ALLOWED_HOSTS = {"api.openai.com", "connectivitycheck.gstatic.com"}
PROBE = '"http://connectivitycheck.gstatic.com/generate_204"'


def find(pattern):
    return [f"{name}: {m}" for name, code in SRC.items() for m in re.findall(pattern, code)]


class SourceSurface(unittest.TestCase):
    def test_no_listening_sockets(self):
        self.assertEqual(find(r"\b(?:lwip_)?(?:listen|accept|bind)\s*\(\s*\w"), [])

    def test_station_mode_only(self):
        self.assertEqual(find(r"WIFI_MODE_(?:AP|APSTA)\b"), [])
        self.assertEqual(find(r"esp_netif_create_default_wifi_ap"), [])
        self.assertTrue(find(r"WIFI_MODE_STA\b"))

    def test_ipv4_only(self):
        self.assertEqual(find(r"\bAF_INET6\b|\bAF_UNSPEC\b"), [])

    def test_no_servers_ota_bluetooth_mdns(self):
        forbidden = r'#include\s*[<"](?:esp_http_server|esp_https_server|esp_https_ota|esp_ota_ops|' \
                    r'esp_bt|esp_gap|esp_gatt|nimble|mdns|esp_now|esp_wifi_ap|esp_local_ctrl)[^>"]*[>"]'
        hits = [h for h in find(forbidden) if "esp_ota_ops" not in h or "settings_nvs.cpp" not in h]
        self.assertEqual(hits, [], "esp_ota_ops is allowed only in settings_nvs.cpp (running partition)")

    def test_only_known_endpoints(self):
        for name, text in repo.sources().items():
            for host in re.findall(r"https?://([A-Za-z0-9.-]+)", text):
                with self.subTest(file=name, host=host):
                    self.assertIn(host, ALLOWED_HOSTS)

    def test_plain_http_only_for_the_portal_probe(self):
        hits = [(name, m) for name, text in repo.sources().items() for m in re.findall(r'"http://[^"]*"', text)]
        self.assertEqual(hits, [("main/portal_client.cpp", PROBE)],
                         "plain HTTP is used only for the captive-portal probe")
        self.assertNotIn("gstatic", SRC["main/openai_client.cpp"])

    def test_tls_verified_with_ca_bundle(self):
        for client in ("main/openai_client.cpp", "main/portal_client.cpp"):
            with self.subTest(file=client):
                self.assertIn("crt_bundle_attach = esp_crt_bundle_attach", SRC[client])
        for name, code in SRC.items():
            with self.subTest(file=name):
                self.assertNotRegex(code, r"skip_cert_common_name_check\s*=\s*true|use_global_ca_store|"
                                          r"MBEDTLS_SSL_VERIFY_NONE|MBEDTLS_SSL_VERIFY_OPTIONAL")


def function_body(code, name):
    """Body of SSHTerminal::name in `code` (comments already stripped)."""
    m = re.search(r"\b" + name + r"\s*\([^)]*\)\s*(?:const\s*)?\{", code)
    if not m:
        return None
    depth, i = 1, m.end()
    while depth and i < len(code):
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        i += 1
    return code[m.end():i]


class SignInPages(unittest.TestCase):
    """Captive portal (AI_RULES.md, Scope): only while the user signs in, nothing kept."""

    PORTAL_FILES = ("main/portal.cpp", "main/portal_client.cpp", "main/portal_menu.cpp")

    def test_only_started_by_the_user(self):
        # Background work (boot, reconnects, the idle tick) never loads a sign-in page
        for name, function in (("main/wifi_menu.cpp", "wifi_auto_connect"), ("main/wifi_menu.cpp", "wifi_maintain"),
                               ("main/wifi_menu.cpp", "run_startup_tasks"), ("main/home_menu.cpp", "background_tick"),
                               ("main/home_menu.cpp", "power_resume")):
            body = function_body(SRC[name], function)
            with self.subTest(function=function):
                self.assertIsNotNone(body)
                self.assertNotIn("portal", body)
        callers = sorted({name for name, code in SRC.items() if re.search(r"handle_portal_command\s*\(", code)})
        self.assertEqual(callers, ["main/include/ssh_terminal.hpp", "main/portal_menu.cpp",
                                   "main/ssh_terminal.cpp", "main/wifi_menu.cpp"])
        fetchers = sorted({name for name, code in SRC.items() if re.search(r"portal::(?:fetch|probe)\s*\(", code)})
        self.assertEqual(fetchers, ["main/portal_menu.cpp"], "pages load only in the portal worker")
        worker = function_body(SRC["main/portal_menu.cpp"], "portal_worker")
        self.assertIn("portal::probe(", worker)
        self.assertIn("portal::fetch(", worker)

    def test_nothing_from_a_page_is_saved(self):
        for name in self.PORTAL_FILES:
            with self.subTest(file=name):
                self.assertNotRegex(SRC[name], r"settings_nvs|nvs_set|fopen|sdcard::|vault::put")

    def test_page_state_wiped_when_the_sign_in_ends(self):
        self.assertIn("portal_wipe();", function_body(SRC["main/profile_menu.cpp"], "wizard_reset"))
        wipe = function_body(SRC["main/portal_menu.cpp"], "portal_wipe")
        self.assertIn("portal::wipe(portal_page)", wipe)
        self.assertIn("portal_cookies.wipe()", wipe)
        self.assertIn("mbedtls_platform_zeroize", SRC["main/portal_client.cpp"])

    def test_portal_parser_is_host_tested(self):
        parser = SRC["main/portal.cpp"] + SRC["main/include/portal.hpp"]
        self.assertNotRegex(parser, r'#include\s*"(?:esp_|freertos|lvgl)')
        self.assertIn("portal.cpp", repo.read("test", "host", "CMakeLists.txt"))


# The same options check_firmware.py enforces on the sdkconfig the build generates
check_firmware = repo.ci_script("check_firmware")


class ConfigSurface(unittest.TestCase):
    def test_committed_sdkconfig(self):
        for name in ("sdkconfig", "sdkconfig.tab5"):
            with self.subTest(sdkconfig=name):
                self.assertEqual(check_firmware.sdkconfig_problems(repo.sdkconfig(name)), [])

    def test_tab5_checked_with_its_own_options(self):
        self.assertEqual(repo.sdkconfig("sdkconfig.tab5").get("CONFIG_IDF_TARGET_ESP32P4"), "y")
        self.assertIn("CONFIG_WIFI_RMT_NVS_ENABLED", check_firmware.required_config(repo.sdkconfig("sdkconfig.tab5")))

    def test_defaults_declare_the_surface(self):
        defaults = repo.read("sdkconfig.defaults")
        for option in ("CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n", "CONFIG_LWIP_DHCPS=n", "CONFIG_LWIP_IPV6=n"):
            self.assertIn(option, defaults)
        tab5 = repo.read("sdkconfig.defaults.esp32p4")
        for option in ("CONFIG_WIFI_RMT_SOFTAP_SUPPORT=n", "CONFIG_WIFI_RMT_NVS_ENABLED=n",
                       "CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=n", "CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID=n"):
            self.assertIn(option, tab5)

    def test_wifi_password_not_stored_by_the_driver(self):
        self.assertIn("esp_wifi_set_storage(WIFI_STORAGE_RAM)", SRC["main/ssh_terminal.cpp"])


if __name__ == "__main__":
    unittest.main()
