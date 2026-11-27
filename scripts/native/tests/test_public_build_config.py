"""Public build configuration tests use inert fixtures, not real credentials."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("nickel_public_build_config", HERE / "public-build-config.py")
public = importlib.util.module_from_spec(spec)
spec.loader.exec_module(public)


class PublicBuildConfigTests(unittest.TestCase):
    def test_only_three_public_microsoft_fields_survive(self):
        document = {"accounts": ["private-fixture"], "microsoft": {
            "clientId": "fixture-client", "authority": "common", "redirectUri": "nickel://auth",
            "clientSecret": "private-fixture", "tenantId": "private-fixture", "refreshToken": "private-fixture"},
            "curseforge": {"apiKey": "private-fixture"}}
        projected = public.project(json.dumps(document))
        self.assertEqual(projected, {"microsoft": {"clientId": "fixture-client", "authority": "common",
                                                  "redirectUri": "nickel://auth"}})
        self.assertNotIn("private-fixture", public.header_for(projected))
        self.assertNotIn("apiKey", public.header_for(projected))

    def test_absent_and_example_clients_remain_unconfigured(self):
        for text in ("{}", "null", "[]", '{"microsoft":{}}', '{"microsoft":{"clientId":" "}}',
                     '{"microsoft":{"clientId":"00000000-0000-0000-0000-000000000000"}}'):
            with self.subTest(text=text):
                self.assertEqual(public.project(text), {})

    def test_defaults_match_the_actual_identity_consumer(self):
        self.assertEqual(public.project('{"microsoft":{"clientId":" fixture-client "}}'),
                         {"microsoft": {"clientId": "fixture-client", "authority": "consumers",
                                        "redirectUri": "nickel://auth"}})

    def test_environment_merges_before_projection_without_secret_lookup(self):
        calls = []
        values = {"NICKEL_MSA_CLIENT_ID": "fixture-environment-client", "NICKEL_MSA_AUTHORITY": "common",
                  "NICKEL_CURSEFORGE_API_KEY": "private-fixture", "NICKEL_BUILD_SECRETS": "private-fixture"}
        def lookup(name):
            calls.append(name)
            return values.get(name)
        projected = public.project('{"microsoft":{"redirectUri":"nickel://fixture"}}', lookup)
        self.assertEqual(projected["microsoft"], {"clientId": "fixture-environment-client", "authority": "common",
                                                  "redirectUri": "nickel://fixture"})
        self.assertEqual(set(calls), set(public.PUBLIC_ENVIRONMENT.values()))
        self.assertNotIn("private-fixture", public.header_for(projected))

    def test_empty_environment_does_not_erase_source(self):
        projected = public.project('{"microsoft":{"clientId":"fixture-client","authority":"common"}}',
                                   lambda name: " ")
        self.assertEqual(projected["microsoft"]["clientId"], "fixture-client")
        self.assertEqual(projected["microsoft"]["authority"], "common")

    def test_invalid_types_controls_and_surrogates_fail_without_payload(self):
        for name in public.FIELD_LIMITS:
            for invalid in (1, True, [], {}, "private-fixture\x00suffix", "private-fixture\u0085suffix", "\ud800"):
                with self.subTest(name=name, invalid=repr(invalid)):
                    document = {"microsoft": {"clientId": "fixture-client", name: invalid}}
                    with self.assertRaises(public.PublicBuildConfigError) as caught:
                        public.project(json.dumps(document))
                    self.assertNotIn("private-fixture", str(caught.exception))

    def test_field_limits_count_utf8_bytes_like_the_identity_parser(self):
        for name, limit in public.FIELD_LIMITS.items():
            section = {"clientId": "fixture-client", name: "a" * limit}
            self.assertEqual(public.project(json.dumps({"microsoft": section}))["microsoft"][name], "a" * limit)
            for too_large in ("a" * (limit + 1), "\u00e9" * (limit // 2 + 1)):
                with self.subTest(name=name):
                    with self.assertRaises(public.PublicBuildConfigError):
                        public.project(json.dumps({"microsoft": {"clientId": "fixture-client", name: too_large}}))

    def test_invalid_oversized_or_nonfinite_source_is_sanitized(self):
        for text in ('{"private-fixture": broken}', " " * (public.MAX_SOURCE_BYTES + 1), '{"other":NaN}',
                     '{"other":Infinity}', "\ud800"):
            with self.subTest(text=text[:24]):
                with self.assertRaises(public.PublicBuildConfigError) as caught:
                    public.project(text)
                self.assertNotIn("private-fixture", str(caught.exception))

    def test_header_is_ascii_cpp_literal_with_bound_json(self):
        config = public.project(json.dumps({"microsoft": {"clientId": "fixture-client", "redirectUri": 'nickel://\u00e9/"\\'}}))
        header = public.header_for(config)
        self.assertTrue(header.isascii())
        self.assertIn("namespace nickel::app::public_build", header)
        literal = header.split("microsoft_json = ", 1)[1].split(";\n", 1)[0]
        self.assertEqual(json.loads(json.loads(literal)), config)

    def test_default_generation_opens_no_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "PublicBuildConfig.hpp"
            public.generate(destination)
            self.assertEqual(destination.read_text(encoding="ascii"), public.header_for({}))

    def test_invalid_source_does_not_replace_a_previous_header(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "PublicBuildConfig.hpp"
            output.write_bytes(b"preserved-header")
            source = root / "public.json"
            source.write_bytes(b'{"private-fixture": broken}')
            with self.assertRaises(public.PublicBuildConfigError):
                public.generate(output, source)
            self.assertEqual(output.read_bytes(), b"preserved-header")
            self.assertFalse(any(path.name.startswith(".public-build-") for path in root.iterdir()))

    def test_identical_generation_preserves_header_timestamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "PublicBuildConfig.hpp"
            public.generate(output)
            stamp = output.stat().st_mtime_ns
            public.generate(output)
            self.assertEqual(output.stat().st_mtime_ns, stamp)

    def test_missing_explicit_source_fails_without_creating_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "PublicBuildConfig.hpp"
            with self.assertRaises(public.PublicBuildConfigError):
                public.generate(output, root / "absent-public.json")
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
