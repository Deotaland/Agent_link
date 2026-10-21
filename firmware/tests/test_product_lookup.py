#!/usr/bin/env python3
"""Host checks for product identity selection and account-token handling."""

import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "find_product", ROOT / "skills/agent-link-port/scripts/find_product.py")
lookup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lookup)

PRODUCTS = [
    {"id": 15, "name": "FoloToy-AI-Passport", "product_key": "folotoy-ai-passport", "chip_type": "esp32-c3"},
    {"id": 16, "name": "paper-s31", "product_key": "paper-s31", "chip_type": "esp32-s31"},
    {"id": 17, "name": "M5stack-stopWatch", "product_key": "m5stack-stopwatch", "chip_type": "esp32-s3"},
]


class SelectionTests(unittest.TestCase):
    def test_official_board_matches_name_or_key_without_numeric_input(self):
        for query in ("FoloToy AI Passport", "folotoy-ai-passport"):
            result = lookup.select_products(PRODUCTS, query, "esp32c3")
            self.assertEqual(result["selected_id"], 15)

    def test_partial_name_is_only_a_hint(self):
        result = lookup.select_products(PRODUCTS, "Passport")
        self.assertEqual(result["status"], "not_found")
        self.assertIsNone(result["selected_id"])
        self.assertEqual(result["products"][0]["id"], 15)

    def test_duplicate_names_require_a_choice(self):
        duplicate = dict(PRODUCTS[0], id=21)
        result = lookup.select_products(PRODUCTS + [duplicate], "folotoy-ai-passport")
        self.assertEqual(result["status"], "ambiguous")
        self.assertIsNone(result["selected_id"])

    def test_wrong_chip_never_selects_official_id(self):
        result = lookup.select_products(PRODUCTS, "folotoy-ai-passport", "esp32-s3")
        self.assertIsNone(result["selected_id"])
        self.assertEqual(result["products"], [])

    def test_custom_s3_does_not_match_s31_or_select_automatically(self):
        result = lookup.select_products(PRODUCTS, chip="ESP32S3", compatible=True)
        self.assertEqual([p["id"] for p in result["products"]], [17])
        self.assertIsNone(result["selected_id"])

    def test_unknown_chip_has_no_reference_candidates(self):
        self.assertEqual(lookup.select_products(PRODUCTS, chip="esp32-p4", compatible=True)["products"], [])


class ApiTests(unittest.TestCase):
    def test_items_envelope_and_partial_pages(self):
        self.assertEqual(lookup.parse_products({"code": 0, "data": {
            "total": len(PRODUCTS), "items": PRODUCTS}}), PRODUCTS)
        with self.assertRaisesRegex(lookup.ProductError, "incomplete"):
            lookup.parse_products({"data": {"total": len(PRODUCTS) + 1, "items": PRODUCTS}})

    def test_api_contract_only_exports_public_product_fields(self):
        rows = [dict(p, enterprise_id=123, private_field="discard") for p in PRODUCTS]
        self.assertEqual(lookup.parse_products({"code": 0, "success": True, "data": rows}), PRODUCTS)

    def test_rejected_or_malformed_payload_cannot_select_a_product(self):
        for payload in ({"code": 401, "data": PRODUCTS}, {"success": False, "data": PRODUCTS},
                        {"data": {}}, [], {"data": [dict(PRODUCTS[0], chip_type=None)]},
                        {"data": [PRODUCTS[0], PRODUCTS[0]]}):
            with self.subTest(payload=payload), self.assertRaises(lookup.ProductError):
                lookup.parse_products(payload)

    def test_ids_fit_the_firmware_integer(self):
        for value in (True, 0, -1, "15", 2147483648):
            with self.subTest(value=value), self.assertRaises(lookup.ProductError):
                lookup.parse_products({"data": [dict(PRODUCTS[0], id=value)]})

    def test_get_uses_both_auth_headers_and_correct_origin(self):
        token = "fixture-access-token"
        raw = json.dumps({"code": 0, "data": PRODUCTS}).encode()
        with patch.object(lookup.urllib.request, "build_opener") as factory:
            factory.return_value.open.return_value = io.BytesIO(raw)
            self.assertEqual(lookup.fetch_products("https://platform.example/", token), PRODUCTS)
            request = factory.return_value.open.call_args.args[0]
            self.assertEqual(request.full_url, "https://platform.example/api/products/all")
            self.assertEqual(request.get_method(), "GET")
            self.assertEqual(request.get_header("Authorization"), "Bearer " + token)
            self.assertEqual(request.get_header("X-token"), token)
            self.assertEqual(request.get_header("Origin"), "https://platform.example")
            self.assertEqual(factory.return_value.open.call_args.kwargs["timeout"], 20)
            self.assertIsInstance(factory.call_args.args[0], lookup.NoRedirect)

    def test_auth_errors_do_not_echo_response_or_token(self):
        token = "fixture-access-token"
        error = urllib.error.HTTPError("https://platform.example", 401, token, {}, io.BytesIO(token.encode()))
        with patch.object(lookup.urllib.request, "build_opener") as factory:
            factory.return_value.open.side_effect = error
            with self.assertRaises(lookup.ProductError) as caught:
                lookup.fetch_products("https://platform.example", token)
            self.assertNotIn(token, str(caught.exception))
            self.assertIn("401", str(caught.exception))

    def test_redirect_handler_does_not_forward_credentials(self):
        request = urllib.request.Request("https://platform.example/api/products/all")
        self.assertIsNone(lookup.NoRedirect().redirect_request(
            request, None, 302, "Found", {}, "https://other.example"))

    def test_invalid_origins_and_tokens_fail_before_network(self):
        for origin, token in (("http://platform.example", "fixture"),
                              ("https://user:pass@platform.example", "fixture"),
                              ("https://platform.example/api", "fixture"),
                              ("https://platform.example", ""),
                              ("https://platform.example", "header\ninjection")):
            with self.subTest(origin=origin), patch.object(lookup.urllib.request, "build_opener") as factory:
                with self.assertRaises(lookup.ProductError):
                    lookup.fetch_products(origin, token)
                factory.assert_not_called()

    def test_response_size_is_bounded(self):
        with patch.object(lookup.urllib.request, "build_opener") as factory:
            factory.return_value.open.return_value = io.BytesIO(b"x" * (lookup.MAX_RESPONSE_BYTES + 1))
            with self.assertRaisesRegex(lookup.ProductError, "too large"):
                lookup.fetch_products("https://platform.example", "fixture")

    def test_json_escaped_token_is_not_exposed_as_a_product_field(self):
        token = "fixture-access-token"
        row = dict(PRODUCTS[0], name=token)
        raw = json.dumps({"data": [row]}).replace("f", "\\u0066").encode()
        with patch.object(lookup.urllib.request, "build_opener") as factory:
            factory.return_value.open.return_value = io.BytesIO(raw)
            with self.assertRaisesRegex(lookup.ProductError, "credentials"):
                lookup.fetch_products("https://platform.example", token)


if __name__ == "__main__":
    unittest.main()
