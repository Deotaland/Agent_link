#!/usr/bin/env python3
"""Look up registered platform products without storing account credentials."""

import argparse
import json
import os
from pathlib import Path
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

MAX_RESPONSE_BYTES = 512 * 1024


class ProductError(ValueError):
    pass


def platform_base_url():
    for root in (Path.cwd(), *Path.cwd().parents, *Path(__file__).resolve().parents):
        config = root / "firmware/boards/common/platform_config.h"
        if config.is_file():
            match = re.search(r'^#define DEOTALAND_PLATFORM_BASE_URL "([^"]+)"$',
                              config.read_text(encoding="utf-8"), re.MULTILINE)
            if match:
                return match[1]
            raise ProductError("The shared platform URL definition is missing.")
    raise ProductError("Run in the Agent Link checkout or provide --base-url.")


def normalize(value):
    return re.sub(r"[^\w]", "", value.casefold(), flags=re.UNICODE).replace("_", "")


def parse_products(payload):
    if not isinstance(payload, dict) or payload.get("success") is False:
        raise ProductError("The platform did not return a successful product list.")
    if payload.get("code", 0) not in (0, 200):
        raise ProductError("The platform rejected the product-list request.")
    rows = payload.get("data")
    if isinstance(rows, dict):
        total = rows.get("total")
        rows = rows.get("items")
        if type(total) is not int or not isinstance(rows, list) or total != len(rows):
            raise ProductError("Expected the complete product catalog; paginated results are incomplete.")
    if not isinstance(rows, list):
        raise ProductError("Expected the product list in response.data.")
    products, ids = [], set()
    for item in rows:
        if not isinstance(item, dict):
            raise ProductError("Invalid product record.")
        product_id = item.get("id")
        if type(product_id) is not int or not 0 < product_id <= 2147483647:
            raise ProductError("A product has an invalid ID.")
        if product_id in ids:
            raise ProductError("The platform returned duplicate product IDs.")
        record = {"id": product_id}
        for field in ("name", "product_key", "chip_type"):
            value = item.get(field)
            if not isinstance(value, str) or not value.strip():
                raise ProductError("A product is missing its name, key or chip type.")
            record[field] = value.strip()
        ids.add(product_id)
        products.append(record)
    return products


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def fetch_products(base_url, token):
    parts = urllib.parse.urlsplit(base_url)
    if (parts.scheme != "https" or not parts.hostname or parts.username or
            parts.password or parts.path not in ("", "/") or parts.query or parts.fragment):
        raise ProductError("Use an HTTPS platform origin without a path or credentials.")
    if not token or any(char.isspace() for char in token):
        raise ProductError("Set DEOTALAND_TOKEN to a valid platform account token.")
    origin = base_url.rstrip("/")
    request = urllib.request.Request(origin + "/api/products/all", headers={
        "Authorization": "Bearer " + token,
        "X-Token": token,
        "Accept": "application/json",
        "User-Agent": "Agent-Link-Product-Lookup/1.0",
        "Origin": origin,
        "Referer": origin + "/",
    })
    opener = urllib.request.build_opener(NoRedirect())
    try:
        with opener.open(request, timeout=20) as response:
            raw = response.read(MAX_RESPONSE_BYTES + 1)
    except urllib.error.HTTPError as error:
        status = error.code
        error.close()
        if status in (401, 403):
            raise ProductError(f"HTTP {status}: check the platform URL and account token.") from None
        raise ProductError(f"HTTP {status}: product lookup failed; redirects are not followed.") from None
    except (urllib.error.URLError, TimeoutError, OSError):
        raise ProductError("Could not reach the platform product endpoint.") from None
    if len(raw) > MAX_RESPONSE_BYTES:
        raise ProductError("The product response is too large.")
    try:
        products = parse_products(json.loads(raw))
    except (UnicodeError, json.JSONDecodeError):
        raise ProductError("The platform returned invalid JSON.") from None
    # Only public product fields leave this function, even if the API grows.
    for product in products:
        for field in ("name", "product_key", "chip_type"):
            if token in product[field]:
                raise ProductError("The response contains account credentials.")
    return products


def select_products(products, query=None, chip=None, compatible=False):
    candidates = [p for p in products if not chip or normalize(p["chip_type"]) == normalize(chip)]
    if compatible:
        return {"status": "candidates", "selected_id": None, "products": candidates}
    if not query:
        return {"status": "list", "selected_id": None, "products": candidates}
    key = normalize(query)
    exact = [p for p in candidates if key in (normalize(p["name"]), normalize(p["product_key"]))]
    if len(exact) == 1:
        return {"status": "matched", "selected_id": exact[0]["id"], "products": exact}
    hints = exact or [p for p in candidates if key and any(
        key in normalize(p[field]) for field in ("name", "product_key"))]
    return {"status": "ambiguous" if exact else "not_found", "selected_id": None, "products": hints}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("query", nargs="?", help="registered product name or product_key")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--list", action="store_true", help="list registered products")
    modes.add_argument("--compatible", action="store_true", help="list same-chip reference products for a custom board")
    parser.add_argument("--chip", help="exact chip family, such as esp32-s3 or esp32-c3")
    parser.add_argument("--base-url", help="override the checkout's shared platform HTTPS origin")
    parser.add_argument("--json", action="store_true", help="print structured matching results")
    args = parser.parse_args(argv)
    if args.compatible and not args.chip:
        parser.error("--compatible requires --chip")
    if args.query and (args.list or args.compatible):
        parser.error("use a product query, --list or --compatible")
    if not (args.query or args.list or args.compatible):
        parser.error("provide a product query, --list or --compatible")
    try:
        products = fetch_products(args.base_url or platform_base_url(), os.environ.get("DEOTALAND_TOKEN", ""))
        result = select_products(products, args.query, args.chip, args.compatible)
    except ProductError as error:
        print(str(error), file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print(result["status"])
        for product in result["products"]:
            print("{id}: {name} ({product_key}, {chip_type})".format(**product))
        if args.compatible:
            print("Same chip only: review product behavior before reusing an ID.")
    return 2 if result["status"] in ("ambiguous", "not_found") else 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
