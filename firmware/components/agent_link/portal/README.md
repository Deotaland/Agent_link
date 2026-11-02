# WiFi provisioning pages

`portal.html` and `connecting.html` are embedded by the component's
[CMakeLists.txt](../CMakeLists.txt). Rebuild the selected WiFi profile after edits.

| File | Purpose |
| --- | --- |
| `portal.html` | Network selection and password form |
| `connecting.html` | Join progress and success/failure result |

The progress page always includes offline-readable next steps: restore phone
internet, read the device activation code, open the configured console and bind
an Agent. `{{PLATFORM_URL}}` is HTML-escaped and replaced by the WiFi backend's
configured platform origin; do not hardcode another platform into the page.
After 35 seconds without a confirmed result the page asks the user to check the
device; this is not a failed-join claim. Language changes also update the result.

Keep `{{SSIDS}}` in the network selector; firmware replaces it with scanned
options. Keep form field names `ssid` and `password`. The progress page polls
`GET /status` for `state`, `ssid`, `ip` and `reason`; reason codes include
`badpass`, `notfound` and `fail`.

The pages contain Chinese and English UI strings. A hidden `lang` checkbox and
CSS switch `.zh`/`.en` visibility without navigation. This works in captive-portal
browsers that restrict JavaScript. Join progress uses JavaScript; the device
continues connecting if the page cannot poll.

Keep all assets inline. A phone connected to the setup hotspot may have no
internet access, so external fonts, scripts and images are unavailable.
