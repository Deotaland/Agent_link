# Korvo BLE

Profile: `korvo-ble`. The application lives in `apps/korvo-ble/`; shared hardware
initialization is in `boards/korvo/`. The device model and BLE name are `KORVO`.
It provides camera preview, agent voice and WAV recording to the TF card.
Use the catalog's left USB-C connector for flashing.

From `firmware/`:

```sh
python tools/build_firmware.py korvo-ble --jobs 2
```

## Recording

Startup scans the recording directory and continues after the highest
`rec-NNNN.wav` number, including case variants. Files are created exclusively;
a collision advances to another number. A scan failure disables recording but
leaves agent voice available. Recovery of a WAV interrupted by power loss is
not implemented.

## Log checks

A short camera frame or `EV-EOF-OVF` means an incomplete capture was dropped.
Frequent occurrences need camera/DMA scheduling analysis. A disconnect before
BLE encryption needs the corresponding app log to identify its cause.
