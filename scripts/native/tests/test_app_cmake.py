"""Exercise complete application CMake blocks without compiling a product."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
IMAGE_PLUGINS = (
    "Qt6::QGifPlugin", "Qt6::QICOPlugin", "Qt6::QJpegPlugin",
    "Qt6::QSvgPlugin", "Qt6::QWebpPlugin", "Qt6::QSvgIconPlugin",
)
PLATFORM_PLUGINS = {
    "Windows": "Qt6::QWindowsIntegrationPlugin",
    "Darwin": "Qt6::QCocoaIntegrationPlugin",
    "Linux": "Qt6::QXcbIntegrationPlugin",
}


class AppCMakeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = shutil.which("cmake")
        if cls.cmake is None:
            raise unittest.SkipTest("CMake is required for application branch tests")
        text = (ROOT / "native/app/CMakeLists.txt").read_text(encoding="utf-8")
        begin = text.index("get_target_property(_nickel_qt_core_type Qt6::Core TYPE)")
        end = text.index("if(WIN32)\n    enable_language(RC)", begin)
        cls.qt_branch = text[begin:end]
        begin = text.index('if(_nickel_qt_core_type STREQUAL "SHARED_LIBRARY")', end)
        cls.deploy_branch = text[begin:]
        cls.app_text = text

    def configure_branch(self, system, library_type, missing=()):
        with tempfile.TemporaryDirectory(prefix="pd-cmake-contract-") as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            plugins = [plugin for plugin in IMAGE_PLUGINS if plugin not in missing]
            platform = PLATFORM_PLUGINS.get(system)
            if platform is not None and platform not in missing:
                plugins.append(platform)
            flags = {
                "Windows": "set(WIN32 TRUE)\nset(APPLE FALSE)\nset(UNIX FALSE)",
                "Darwin": "set(WIN32 FALSE)\nset(APPLE TRUE)\nset(UNIX TRUE)",
                "Linux": "set(WIN32 FALSE)\nset(APPLE FALSE)\nset(UNIX TRUE)",
            }.get(system, "set(WIN32 FALSE)\nset(APPLE FALSE)\nset(UNIX FALSE)")
            definitions = "\n".join(
                f"add_library({plugin} STATIC IMPORTED)" for plugin in plugins
            )
            if library_type == "INTERFACE":
                core = "add_library(Qt6::Core INTERFACE IMPORTED)"
            else:
                core = f"add_library(Qt6::Core {library_type} IMPORTED)"
            fixture = f'''cmake_minimum_required(VERSION 3.25)
project(PdApplicationBranchContract LANGUAGES NONE)
{flags}
set(CMAKE_SYSTEM_NAME "{system}")
{core}
{definitions}
function(qt_import_plugins target)
    file(APPEND "${{CMAKE_BINARY_DIR}}/imports.txt" "${{target}};${{ARGN}}\\n")
endfunction()
function(qt_generate_deploy_app_script)
    if("NO_UNSUPPORTED_PLATFORM_ERROR" IN_LIST ARGN)
        message(FATAL_ERROR "Unsupported deployment must not be suppressed")
    endif()
    cmake_parse_arguments(ARG "" "TARGET;OUTPUT_SCRIPT" "" ${{ARGN}})
    if(NOT ARG_TARGET STREQUAL "ni-launcher" OR NOT ARG_OUTPUT_SCRIPT)
        message(FATAL_ERROR "Deployment must use the production target")
    endif()
    file(WRITE "${{CMAKE_BINARY_DIR}}/deploy.cmake" "# Logic fixture only\\n")
    set(${{ARG_OUTPUT_SCRIPT}} "${{CMAKE_BINARY_DIR}}/deploy.cmake" PARENT_SCOPE)
endfunction()
{self.qt_branch}
{self.deploy_branch}
file(WRITE "${{CMAKE_BINARY_DIR}}/branch-complete.txt" "${{_nickel_qt_core_type}}")
'''
            (source / "CMakeLists.txt").write_text(fixture, encoding="ascii")
            result = subprocess.run(
                [self.cmake, "-S", str(source), "-B", str(root / "build"), "-G", "Ninja"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding="utf-8", errors="replace", timeout=30,
            )
            build = root / "build"
            imports = (build / "imports.txt").read_text() if (build / "imports.txt").exists() else ""
            install = (build / "cmake_install.cmake").read_text() if (build / "cmake_install.cmake").exists() else ""
            return result.returncode, result.stdout, imports, install

    def test_static_desktop_imports_all_required_plugins(self):
        for system, platform in PLATFORM_PLUGINS.items():
            with self.subTest(system=system):
                code, output, imports, install = self.configure_branch(system, "STATIC")
                self.assertEqual(code, 0, output)
                for plugin in (*IMAGE_PLUGINS, platform):
                    self.assertIn(plugin, imports)
                self.assertEqual(len(imports.splitlines()), 2)
                self.assertNotIn("deploy.cmake", install)

    def test_non_windows_shared_qt_needs_no_static_plugin_targets(self):
        missing = (*IMAGE_PLUGINS, *PLATFORM_PLUGINS.values())
        for system in ("Darwin", "Linux"):
            with self.subTest(system=system):
                code, output, imports, install = self.configure_branch(system, "SHARED", missing)
                self.assertEqual(code, 0, output)
                self.assertEqual(imports, "")
                self.assertIn("deploy.cmake", install)

    def test_windows_shared_qt_is_rejected(self):
        code, output, _, _ = self.configure_branch("Windows", "SHARED")
        self.assertNotEqual(code, 0)
        self.assertIn("Windows single-EXE builds require actual static Qt", output)

    def test_missing_static_image_or_icon_plugin_is_rejected(self):
        for plugin in IMAGE_PLUGINS:
            with self.subTest(plugin=plugin):
                code, output, _, _ = self.configure_branch("Windows", "STATIC", (plugin,))
                self.assertNotEqual(code, 0)
                self.assertIn(f"Required static image plugin is missing: {plugin}", output)

    def test_missing_static_platform_plugin_is_rejected(self):
        for system, plugin in PLATFORM_PLUGINS.items():
            with self.subTest(system=system):
                code, output, _, _ = self.configure_branch(system, "STATIC", (plugin,))
                self.assertNotEqual(code, 0)
                self.assertIn(f"Required static platform plugin is missing: {plugin}", output)

    def test_other_qt_target_type_and_unknown_static_platform_are_rejected(self):
        code, output, _, _ = self.configure_branch("Linux", "INTERFACE")
        self.assertNotEqual(code, 0)
        self.assertIn("actual Qt6::Core target must be static or shared", output)
        code, output, _, _ = self.configure_branch("UnsupportedDesktop", "STATIC")
        self.assertNotEqual(code, 0)
        self.assertIn("No supported static desktop platform plugin", output)

    def test_metadata_is_a_configure_dependency_and_controls_visible_names(self):
        root_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS\n'
                      '    "${CMAKE_CURRENT_SOURCE_DIR}/native/metadata.json")', root_text)
        metadata = json.loads((ROOT / "native/metadata.json").read_text(encoding="utf-8"))
        self.assertEqual(metadata["name"], "pd-launcher")
        self.assertEqual(metadata["displayName"], "Palladium")
        self.assertEqual(metadata["executableName"], "pd-launcher")
        self.assertEqual(metadata["applicationId"], "studio.nickel.launcher")
        self.assertEqual(metadata["version"], "0.1.0")
        begin = self.app_text.index("foreach(_field displayName applicationId executableName license versionLabel)")
        end = self.app_text.index('include("${CMAKE_SOURCE_DIR}/cmake/NickelPublicBuildConfig.cmake")', begin)
        with tempfile.TemporaryDirectory(prefix="pd-build-info-") as directory:
            root = Path(directory)
            script = root / "configure.cmake"
            script.write_text(
                f'set(CMAKE_CURRENT_BINARY_DIR "{root.as_posix()}")\n'
                f'file(READ "{(ROOT / "native/metadata.json").as_posix()}" _nickel_metadata)\n'
                'string(JSON PROJECT_VERSION GET "${_nickel_metadata}" version)\n'
                + self.app_text[begin:end], encoding="ascii",
            )
            result = subprocess.run(
                [self.cmake, "-P", str(script)], cwd=ROOT / "native/app",
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            header = (root / "BuildInfo.hpp").read_text()
            self.assertIn('display_name = "Palladium"', header)
            self.assertIn('executable_name = "pd-launcher"', header)
            self.assertIn('application_id = "studio.nickel.launcher"', header)
            self.assertIn('version = "0.1.0"', header)
        self.assertIn('OUTPUT_NAME "${_nickel_executableName}"', self.app_text)
        self.assertNotIn("NO_UNSUPPORTED_PLATFORM_ERROR", self.deploy_branch)

    def test_root_requires_all_three_production_action_test_parents(self):
        text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        begin = text.index("foreach(_action_domain app_instance_actions app_resource_actions app_launch_actions)")
        end = text.index("endforeach()", begin) + len("endforeach()")
        branch = text[begin:end]
        with tempfile.TemporaryDirectory(prefix="pd-action-parents-") as directory:
            root = Path(directory)
            script = root / "check.cmake"
            script.write_text(
                'function(nickel_add_domain_tests source binary)\n'
                '    if(NOT source STREQUAL binary)\n'
                '        message(FATAL_ERROR "Action test parent cannot be replaced")\n'
                '    endif()\n'
                '    list(APPEND visited "${source}")\n'
                '    set(visited "${visited}" PARENT_SCOPE)\n'
                'endfunction()\n' + branch + '\n'
                'if(NOT visited STREQUAL "app_instance_actions;app_resource_actions;app_launch_actions")\n'
                '    message(FATAL_ERROR "The complete action inventory is required")\n'
                'endif()\n', encoding="ascii")
            result = subprocess.run([self.cmake, "-P", str(script)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_normalize_before_registering_configure_dependency_for_ninja(self):
        ninja = shutil.which("ninja")
        if ninja is None:
            self.skipTest("Ninja is required for configure-dependency regression")
        with tempfile.TemporaryDirectory(prefix="pd-ninja-dependencies-") as directory:
            root = Path(directory)
            source = root / "source"
            tests = source / "native/tests/fixture"
            assets = source / "native/assets/brand"
            tests.mkdir(parents=True)
            assets.mkdir(parents=True)
            (assets / "functional-resources.qrc").write_text("<RCC/>\n", encoding="ascii")
            helper = (ROOT / "cmake/NickelDomains.cmake").as_posix()
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\nproject(PdConfigurePaths LANGUAGES NONE)\n'
                'set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS\n'
                '    "${CMAKE_SOURCE_DIR}/native/assets/brand/functional-resources.qrc")\n'
                f'include("{helper}")\nnickel_add_domain_tests(fixture fixture)\n', encoding="ascii")
            for normalized in (False, True):
                expression = '"${CMAKE_CURRENT_LIST_DIR}/../../assets/brand/functional-resources.qrc"'
                definition = (f'get_filename_component(template {expression} ABSOLUTE)\n' if normalized
                              else f'set(template {expression})\n')
                (tests / "CMakeLists.txt").write_text(definition +
                    'set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${template}")\n'
                    'add_custom_target(domain-fixture COMMAND "${CMAKE_COMMAND}" -E true)\n', encoding="ascii")
                build = root / ("normalized" if normalized else "raw")
                result = subprocess.run([self.cmake, "-S", str(source), "-B", str(build), "-G", "Ninja"],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([ninja, "-C", str(build), "-t", "targets"],
                                        capture_output=True, text=True, timeout=30)
                if not normalized:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertTrue("defined as an output multiple times" in result.stderr
                                    or "multiple rules generate" in result.stderr, result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([self.cmake, "--build", str(build), "--target", "domain-fixture"],
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_same_graph_smoke_includes_the_actual_application_and_generated_manifest(self):
        helper = ROOT / "cmake/NickelProductionSmoke.cmake"
        self.assertIn("include(cmake/NickelProductionSmoke.cmake)",
                      (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
        cases = ((True, True, True, False, True, True),
                 (False, True, True, False, True, False),
                 (True, False, True, False, True, False),
                 (True, True, False, False, True, False),
                 (True, True, True, True, True, False),
                 (True, True, True, False, False, False))
        for ui, tests, app_target, portal, manifest, succeeds in cases:
            with self.subTest(ui=ui, tests=tests, app_target=app_target,
                              portal=portal, manifest=manifest):
                with tempfile.TemporaryDirectory(prefix="pd-same-graph-") as directory:
                    root = Path(directory)
                    harness = root / "tests/native-production-smoke"
                    harness.mkdir(parents=True)
                    (harness / "CMakeLists.txt").write_text(
                        'file(SHA256 "${NICKEL_PRODUCTION_SMOKE_WINDOWS_MANIFEST}" actual)\n'
                        'if(NOT actual STREQUAL NICKEL_PRODUCTION_SMOKE_WINDOWS_MANIFEST_SHA256)\n'
                        '    message(FATAL_ERROR "Manifest binding differs from the real application")\n'
                        'endif()\n'
                        'file(WRITE "${CMAKE_BINARY_DIR}/consumer-included.txt" "${actual}")\n',
                        encoding="ascii")
                    text = 'cmake_minimum_required(VERSION 3.25)\nproject(SmokeGraph LANGUAGES NONE)\n'
                    text += 'set(WIN32 TRUE)\nset(NICKEL_PRODUCTION_SMOKE_SAME_GRAPH ON)\n'
                    text += f'set(NICKEL_BUILD_UI {"ON" if ui else "OFF"})\n'
                    text += f'set(NICKEL_BUILD_TESTS {"ON" if tests else "OFF"})\n'
                    text += f'set(NICKEL_PRODUCTION_SMOKE_LINK_PORTAL {"ON" if portal else "OFF"})\n'
                    if app_target:
                        text += 'add_library(app INTERFACE)\nadd_library(Nickel::AppServices ALIAS app)\n'
                    if manifest:
                        text += 'file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/native/app")\n'
                        text += 'file(WRITE "${CMAKE_BINARY_DIR}/native/app/windows.manifest" "actual-fixture-manifest")\n'
                    text += f'include("{helper.as_posix()}")\n'
                    (root / "CMakeLists.txt").write_text(text, encoding="ascii")
                    result = subprocess.run([self.cmake, "-S", str(root), "-B", str(root / "build"),
                                             "-G", "Ninja"], capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode == 0, succeeds, result.stdout + result.stderr)
                    self.assertEqual((root / "build/consumer-included.txt").exists(), succeeds)



if __name__ == "__main__":
    unittest.main()
