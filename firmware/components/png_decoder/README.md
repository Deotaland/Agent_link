# PNG decoder

Pinned upstream LodePNG source is recorded in `upstream.json`. Original license
headers are retained. Agent Link adds private symbol aliases and supplies bounded
PSRAM allocators; encoding, files and C++ wrappers are disabled. The decoder is
independent of LVGL's memory pool, so cover workers can decode safely while the
UI renders. Other profiles do not link this component.
