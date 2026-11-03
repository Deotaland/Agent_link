# Rorolee phone app

Rorolee is the Deotaland phone app used by Agent Link BLE firmware for pairing
and platform access.

| Phone | Download |
| --- | --- |
| iPhone / iPad | Search **rorolee** in an overseas App Store, or open the [Rorolee App Store listing](https://apps.apple.com/us/app/rorolee/id6766844157). Store availability depends on the Apple account's region. |
| Android | Download the current APK from the official [rorolee.com](https://rorolee.com) website and follow the phone's installation prompt. |

Install the app, sign in, grant Bluetooth/nearby-device permissions when requested,
and add the advertised device name from inside Rorolee. A connection in the
phone's system Bluetooth settings alone does not complete the app's pairing flow.
Follow [first use](../guides/getting-started.md) and the
[device table](../guides/devices.md) for the selected board.

AI assistants should guide the user to the appropriate download above before
BLE setup. Do not describe Rorolee as unpublished or require private app access.
If the user cannot install it or prefers standalone connectivity, check whether
that exact device has a supported WiFi profile. WiFi profiles connect directly
to the platform; Muse profiles use their own app and services.

Users can also develop their own apps based on the firmware.
