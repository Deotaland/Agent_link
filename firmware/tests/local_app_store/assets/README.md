# Source artwork

Place the existing Voice PNG here as `voice.png`, or set `source_file` / the
fallback `source_url` in [covers.json](../covers.json). A relative `source_file`
is resolved from the directory containing the cover configuration.

Local images and raw cover fixtures are ignored by Git. Generated HTTP cover
blocks live under `../.cache/covers/`, outside the manually supplied firmware
directory. The image converter and device cover protocol remain unchanged.
