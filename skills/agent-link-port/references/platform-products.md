# Platform product lookup

`GET /api/products/all` lists products registered on the platform. It returns
`data[]` records with `id`, `name`, `product_key` and `chip_type`. The lookup helper
also accepts a complete `data.items` response with `data.total`, and rejects
incomplete pages rather than selecting from a partial catalog.
It selects a product identity; device activation still uses
`POST /api/device-gateway/claim-code` through the SDK.

Use a valid account token from the environment variable `DEOTALAND_TOKEN`.
The helper sends both `Authorization: Bearer ...` and `X-Token`, reads the
checkout's `firmware/boards/common/platform_config.h` and makes no platform writes.
Never put this account token in firmware, profiles, documentation or logs.
For a standalone skill outside a checkout, pass `--base-url https://PLATFORM_HOST`.

To switch platforms, change the shared header and rebuild. Query the target
platform again: its account token and product IDs may differ. Update console
links in the documentation and refresh installed skills as needed.

Record verified products and profile-to-product-key mappings in
`firmware/profiles/products.json`, including the matching platform origin.
The build generates the selected product ID; board headers use
`AGENT_LINK_PRODUCT_ID`, never a numeric fallback or a separate Kconfig ID.
Register an unknown WiFi product before building; do not reuse an old platform's ID.

```sh
python skills/agent-link-port/scripts/find_product.py --list --json
python skills/agent-link-port/scripts/find_product.py "FoloToy AI Passport" --chip esp32-c3 --json
python skills/agent-link-port/scripts/find_product.py --compatible --chip esp32-s3 --json
```

- **Official board:** match the registered name or `product_key` and verify the
  chip. A unique exact match returns `selected_id`; use it directly. Resolve
  partial names against the board catalog and returned keys. Ask only when the
  hardware or product remains ambiguous, rather than asking for a numeric ID.
- **Custom board:** choose a registered product that supports the intended
  connection and behavior. `--compatible` lists same-chip candidates only; the
  API does not describe peripherals or capabilities. Review those separately,
  then report which reference ID is reused. Keep the actual hardware settings,
  `Model()` string and OTA checks. Do not invent an ID when no suitable product exists.

Exit code 0 means a match or list was returned; 2 means no unique exact match;
1 means the request or response failed. For 401/403, check the URL and obtain a
valid token. Do not guess an ID or silently switch hosts after a failed query.
