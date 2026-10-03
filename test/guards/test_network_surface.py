"""
Network surface (CLAUDE.md, Scope and Security): station-only WiFi, IPv4, no listening
sockets, no servers, OTA or Bluetooth, and the only fixed endpoint is api.openai.com over
verified TLS. The built firmware is checked too, by check_firmware_surface.py in CI.
"""

import re
import unittest

import repo

SRC = {name: repo.strip_comments(text) for name, text in repo.sources().items()}
ALLOWED_HOSTS = {"api.openai.com"}


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

    def test_only_openai_endpoint(self):
        for name, text in repo.sources().items():
            for host in re.findall(r"https?://([A-Za-z0-9.-]+)", text):
                with self.subTest(file=name, host=host):
                    self.assertIn(host, ALLOWED_HOSTS)
            with self.subTest(file=name):
                self.assertNotRegex(text, r'"http://', "plain HTTP is never used")

    def test_tls_verified_with_ca_bundle(self):
        client = SRC["main/openai_client.cpp"]
        self.assertIn("crt_bundle_attach = esp_crt_bundle_attach", client)
        for name, code in SRC.items():
            with self.subTest(file=name):
                self.assertNotRegex(code, r"skip_cert_common_name_check\s*=\s*true|use_global_ca_store|"
                                          r"MBEDTLS_SSL_VERIFY_NONE|MBEDTLS_SSL_VERIFY_OPTIONAL")


# The same options check_firmware.py enforces on the sdkconfig the build generates
check_firmware = repo.ci_script("check_firmware")


class ConfigSurface(unittest.TestCase):
    def test_committed_sdkconfig(self):
        self.assertEqual(check_firmware.sdkconfig_problems(repo.sdkconfig()), [])

    def test_defaults_declare_the_surface(self):
        defaults = repo.read("sdkconfig.defaults")
        for option in ("CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n", "CONFIG_LWIP_DHCPS=n", "CONFIG_LWIP_IPV6=n"):
            self.assertIn(option, defaults)


if __name__ == "__main__":
    unittest.main()
