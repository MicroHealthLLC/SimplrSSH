"""
Persistence: the partition layouts that keep users' settings across flashing and updates
(AI_RULES.md, Persistence). Changing these offsets or sizes wipes settings in the field.
"""

import re
import unittest

import repo

FLASH = 16 * 1024 * 1024
NVS_OFFSET = 0xD10000          # Fixed forever: moving it wipes every user's settings
SETTINGS_SIZE = 0x40000        # 256 KB, never shrink


def by_name(rows):
    return {r["name"]: r for r in rows}


class DirectFlashLayout(unittest.TestCase):
    rows = repo.partitions("partitions.csv")
    parts = by_name(rows)

    def test_nvs_fixed_offset_and_size(self):
        nvs = self.parts["nvs"]
        self.assertEqual((nvs["type"], nvs["subtype"]), ("data", "nvs"))
        self.assertEqual(nvs["offset"], NVS_OFFSET, "never move nvs: it wipes users' settings")
        self.assertGreaterEqual(nvs["size"], SETTINGS_SIZE, "never shrink nvs")

    def test_nvs_after_app(self):
        app = self.parts["factory"]
        self.assertEqual((app["type"], app["subtype"]), ("app", "factory"))
        self.assertGreaterEqual(self.parts["nvs"]["offset"], app["offset"] + app["size"],
                                "nvs must sit after the app so flashing the merged image at 0x0 keeps it")

    def test_no_overlap_and_fits_flash(self):
        rows = sorted(self.rows, key=lambda r: r["offset"])
        self.assertGreaterEqual(rows[0]["offset"], 0x9000, "partition table lives at 0x8000")
        for a, b in zip(rows, rows[1:], strict=False):
            self.assertLessEqual(a["offset"] + a["size"], b["offset"], f"{a['name']} overlaps {b['name']}")
        self.assertLessEqual(rows[-1]["offset"] + rows[-1]["size"], FLASH)

    def test_app_offset_aligned(self):
        self.assertEqual(self.parts["factory"]["offset"] % 0x10000, 0)

    def test_sdkconfig_uses_this_table(self):
        cfg = repo.sdkconfig()
        self.assertEqual(cfg.get("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME"), '"partitions.csv"')
        self.assertEqual(cfg.get("CONFIG_PARTITION_TABLE_OFFSET"), "0x8000")
        self.assertEqual(cfg.get("CONFIG_ESPTOOLPY_FLASHSIZE"), '"16MB"')


class LauncherLayout(unittest.TestCase):
    rows = repo.partitions("partitions_launcher.csv")
    parts = by_name(rows)

    def test_settings_partition_label_and_type(self):
        own = self.parts.get("simplrssh")
        self.assertIsNotNone(own, "keep the 'simplrssh' label: launchers keep it across updates")
        self.assertEqual((own["type"], own["subtype"]), ("data", "spiffs"),
                         "launchers only create SPIFFS/LittleFS/FAT data partitions")
        self.assertGreaterEqual(own["size"], SETTINGS_SIZE)

    def test_settings_partition_inside_image(self):
        self.assertLess(self.parts["simplrssh"]["offset"], self.parts["factory"]["offset"],
                        "Launcher's web installer skips partitions past the end of the image")

    def test_app_offset_matches_cmake(self):
        cmake = repo.read("CMakeLists.txt")
        offset = int(re.search(r"set\(LAUNCHER_APP_OFFSET\s+(0x[0-9a-fA-F]+)\)", cmake).group(1), 16)
        self.assertEqual(self.parts["factory"]["offset"], offset)

    def test_same_app_size_as_direct_flash(self):
        direct = by_name(repo.partitions("partitions.csv"))["factory"]["size"]
        self.assertEqual(self.parts["factory"]["size"], direct)

    def test_no_launcher_nvs_in_image(self):
        self.assertNotIn("nvs", self.parts, "never ship a table that would replace the launcher's nvs")

    def test_no_overlap(self):
        rows = sorted(self.rows, key=lambda r: r["offset"])
        for a, b in zip(rows, rows[1:], strict=False):
            self.assertLessEqual(a["offset"] + a["size"], b["offset"])
        self.assertLessEqual(rows[-1]["offset"] + rows[-1]["size"], FLASH)


class FirmwareMatchesLayout(unittest.TestCase):
    def test_settings_code_knows_the_labels(self):
        code = repo.read("main", "settings_nvs.cpp")
        self.assertIn('OWN_LABEL = "simplrssh"', code)
        self.assertIn('LEGACY_LABEL = "pocketssh"', code)

    def test_settings_partition_is_nvs_sized(self):
        code = repo.read("main", "settings_nvs.cpp")
        min_size = int(re.search(r"MIN_SIZE = (0x[0-9a-fA-F]+)", code).group(1), 16)
        self.assertLessEqual(min_size, by_name(repo.partitions("partitions_launcher.csv"))["simplrssh"]["size"])


if __name__ == "__main__":
    unittest.main()
