# WebP decoder

Decoder-only subset of Google's libwebp 1.6.0, for the production community's
WebP covers. `upstream.json` records the official release and archive SHA-256.
The original source headers, COPYING, PATENTS and AUTHORS are preserved.
Only upstream dec/common DSP/common utility C sources are compiled. Encoders,
command-line tools, SIMD and worker threads are not included in this build.
The component is selected from the resolved community-store profile; other
profiles do not link it. Distribution includes the license and patent grant.
