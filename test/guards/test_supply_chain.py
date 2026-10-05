"""
Supply chain: every GitHub Action is pinned to a commit SHA, every container image to a
digest, and every ESP-IDF component to an exact version recorded in each board's lock file
(dependencies.lock for the T-Deck, dependencies.lock.esp32p4 for the Tab5).
"""

import re
import unittest

import repo

USES = re.compile(r"^\s*(?:-\s*)?uses:\s*([^\s#]+)", re.M)


class ActionsPinned(unittest.TestCase):
    def test_actions_pinned_to_commit_sha(self):
        for name, text in repo.workflows().items():
            for ref in USES.findall(text):
                if ref.startswith("./"):
                    continue                      # Reusable workflow in this repository
                with self.subTest(workflow=name, uses=ref):
                    self.assertRegex(ref, r"^[\w.-]+/[\w./-]+@[0-9a-f]{40}$",
                                     "pin actions to a full commit SHA (with a # vX.Y.Z comment)")

    def test_container_images_pinned_by_digest(self):
        image = re.compile(r"(?:docker (?:pull|run)[^\n]*?\s|esp_idf_version:\s*)"
                           r"((?:[\w.-]+/)*[\w.-]+:[\w.-]+(?:@sha256:[0-9a-f]{64})?)")
        for name, text in repo.workflows().items():
            for ref in image.findall(text):
                with self.subTest(workflow=name, image=ref):
                    self.assertRegex(ref, r"@sha256:[0-9a-f]{64}$", "pin images by digest")
            for ref in re.findall(r"esp_idf_version:\s*([^\s#]+)", text):
                with self.subTest(workflow=name, esp_idf_version=ref):
                    self.assertRegex(ref, r"@sha256:[0-9a-f]{64}$")

    def test_image_variables_and_known_images_pinned(self):
        for name, text in repo.workflows().items():
            code = re.sub(r"(?m)\s#\s.*$", "", text)          # Comments may name an image by tag
            for var, ref in re.findall(r"(?m)^\s*(\w*IMAGE\w*):\s*([\w.-]+/[^\s#]+:[^\s#]+)", code):
                with self.subTest(workflow=name, variable=var):
                    self.assertRegex(ref, r"@sha256:[0-9a-f]{64}$")
            for ref in re.findall(r"(?:espressif/idf|zricethezav/gitleaks|koalaman/shellcheck):[^\s'\"]*", code):
                with self.subTest(workflow=name, image=ref):
                    self.assertRegex(ref, r"@sha256:[0-9a-f]{64}$")

    def test_workflows_default_to_read_only_token(self):
        for name, text in repo.workflows().items():
            with self.subTest(workflow=name):
                self.assertRegex(text, r"(?m)^\s*permissions:", "declare token permissions")
                self.assertNotRegex(text, r"permissions:\s*write-all")

    def test_no_pull_request_target(self):
        for name, text in repo.workflows().items():
            with self.subTest(workflow=name):
                self.assertNotIn("pull_request_target", text)

    def test_dependabot_keeps_actions_current(self):
        self.assertIn("package-ecosystem: github-actions", repo.read(".github", "dependabot.yml"))


# Each board's lock file and the target its components are resolved for (CMakeLists.txt)
LOCKS = {"dependencies.lock": "esp32s3", "dependencies.lock.esp32p4": "esp32p4"}


def manifest_dependencies():
    """{name: (version, target or None)} from main/idf_component.yml: either `name: 'x.y.z'`
    or a block with `version:` and an optional `rules: - if: 'target == ...'`."""
    deps = {}
    current = None
    for line in repo.read("main", "idf_component.yml").splitlines():
        m = re.match(r"^  ([\w./-]+):\s*'([^']+)'", line)
        if m:
            deps[m.group(1)] = (m.group(2), None)
            current = None
            continue
        m = re.match(r"^  ([\w./-]+):\s*(?:#.*)?$", line)
        if m:
            current = m.group(1)
            deps[current] = (None, None)
            continue
        m = re.match(r"^    version:\s*'([^']+)'", line)
        if m and current:
            deps[current] = (m.group(1), deps[current][1])
        m = re.match(r"^\s+- if:\s*'target == (\w+)'", line)
        if m and current:
            deps[current] = (deps[current][0], m.group(1))
    return deps


def lock_entries(name="dependencies.lock"):
    """{name: {'version':..., 'component_hash':..., 'type':...}} from a lock file."""
    entries = {}
    current = None
    in_deps = False
    for line in repo.read(name).splitlines():
        if line == "dependencies:":
            in_deps = True
            continue
        if in_deps and re.match(r"^\S", line):
            in_deps = False
        if not in_deps:
            continue
        m = re.match(r"^  ([\w./-]+):$", line)
        if m:
            current = entries.setdefault(m.group(1), {})
            continue
        m = re.match(r"^    (version|component_hash):\s*'?([^'\s]+)'?$", line)
        if m and current is not None:
            current[m.group(1)] = m.group(2)
        m = re.match(r"^      type:\s*(\w+)$", line)
        if m and current is not None:
            current["type"] = m.group(1)
    return entries


class ComponentsPinned(unittest.TestCase):
    def test_direct_dependencies_exact(self):
        for name, (version, _target) in manifest_dependencies().items():
            if name == "idf":
                continue
            with self.subTest(component=name):
                self.assertRegex(version or "", r"^\d+\.\d+\.\d+(~\d+)?$", "pin components to an exact version")

    def test_board_rules_name_a_known_target(self):
        for name, (_version, target) in manifest_dependencies().items():
            with self.subTest(component=name):
                self.assertIn(target, (None, *LOCKS.values()))

    def test_direct_dependencies_locked_at_same_version(self):
        for lock_name, target in LOCKS.items():
            lock = lock_entries(lock_name)
            with self.subTest(lock=lock_name):
                self.assertIn(f"target: {target}", repo.read(lock_name))
            for name, (version, only) in manifest_dependencies().items():
                if name == "idf" or only not in (None, target):
                    continue
                with self.subTest(lock=lock_name, component=name):
                    self.assertIn(name, lock, "run idf.py update-dependencies for this board and commit its lock")
                    self.assertEqual(lock[name].get("version"), version)

    def test_registry_components_have_hashes(self):
        for lock_name in LOCKS:
            for name, info in lock_entries(lock_name).items():
                if name == "idf":
                    continue
                with self.subTest(lock=lock_name, component=name):
                    self.assertEqual(info.get("type"), "service", "only registry components (no local paths)")
                    self.assertRegex(info.get("component_hash", ""), r"^[0-9a-f]{64}$")

    def test_board_locks_selected_by_target(self):
        cmake = repo.read("CMakeLists.txt")
        self.assertRegex(cmake, r'IDF_TARGET STREQUAL "esp32p4"\)\s*\n\s*idf_build_set_property\(DEPENDENCIES_LOCK '
                                r'"\$\{CMAKE_CURRENT_LIST_DIR\}/dependencies.lock.esp32p4"\)')

    def test_ci_idf_version_matches_manifest(self):
        idf = re.search(r"(?m)^  idf:\s*\n\s+version:\s*'([^']+)'", repo.read("main", "idf_component.yml")).group(1)
        build = repo.read(".github", "workflows", "build.yml")
        ci = re.search(r"ESP_IDF_VERSION:\s*v([\d.]+)", build).group(1)
        self.assertTrue(idf.lstrip("~^=") == ci, f"manifest idf {idf} vs CI v{ci}")


if __name__ == "__main__":
    unittest.main()
