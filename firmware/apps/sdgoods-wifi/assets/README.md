# Default Agent_link cover

`agent_link_cover.png` is the user-supplied 256x256 RGB artwork. Keep this
original for future conversions. `agent_link_cover.raw` uses the existing
132x132 RGB565LE+A8 contract, with a circular alpha mask, CRC and the embedded
application identity `agent.status` (52,336 bytes total).

Regenerate it with [the reference encoder](../../../tests/local_app_store/make_app_cover.py),
passing absolute input/output paths and `--app-id agent.status`.

The initial animation package includes the cover in the preinstalled app's
reserved slot tail. Its offset follows the existing recovery normalization:
round `(app image bytes + cover reserve)` up to 64 KiB, then subtract the cover
reserve. The package validates identity, length and CRC before merging. It does
not change the app binary, flash layout, recovery or cover-loader interfaces.
