# OJBadge local friends

Build from the repository root using ESP-IDF 5.5.x or 6.1.x:

```sh
python firmware/tools/build_firmware.py ojbadge-wifi --jobs 2
```

The 240 x 240 Chinese UI fits the circular display. The home screen shows battery
and charge state. Tap **Nearby friends** or short-press SW2 to start discovery. The local
identity is `Link-XXXXXX` (last three WiFi MAC bytes), matching the shared protocol.
Up to six discovered peers appear across pages of two. Tap a name to invite it.

On both devices compare the six-digit pairing code and tap **Codes match** only if
the codes agree. Either side can reject; invitations expire automatically.
After mutual confirmation tap **Send greeting** or short-press SW2. A sent count means
the peer acknowledged the greeting; incoming greetings show a toast and update
the received count. **Find another partner** disconnects and resumes discovery. **Back**, or
holding SW2 for one second, disconnects and stops the local radio. Pairing is
ephemeral and is not saved as a persistent friends list.

For cross-board testing, run `waveshare-amoled-175-wifi` on the existing Waveshare
board and open its launcher → **Friends**. Button names here describe the Chinese UI.
OJBadge uses the same channel 6, packet
version, ECDH key exchange, code confirmation, encrypted peer setup, authenticated
greetings, acknowledgements and timeout handling. No AP/router or platform
activation is required on OJBadge. Waveshare temporarily pauses its cloud WiFi
while in friends mode and restores it after leaving.

Both devices need the updated shared friends module, which derives the same
PMK from their shared session secret before installing encrypted peers. Older
builds with per-device random PMKs can discover one another and display pairing
codes but cannot complete encrypted confirmations. Updating only one endpoint
does not correct an older peer. Five-second radio diagnostics distinguish local
and remote confirmation, transmit completion/failures and receive/authentication
failures without printing keys or pairing codes.

This version deliberately has no Agent Link platform integration, hotspot,
voice service or audio initialization. The product identity is stored only in
build metadata. Include the `anim_pack` Chinese font asset in a USB package;
updating the application alone does not install a missing font partition.

Hardware acceptance is pending: LCD colors/inversion/orientation, all touch
targets, SW2 debounce/hold, battery/charging readings, discovery in both
directions, equal codes, acceptance/rejection, greetings/ACK in both directions,
peer loss, timeout, repeated exit/re-entry and Waveshare cloud WiFi restoration.
No device is flashed by the build command.
