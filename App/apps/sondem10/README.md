# Sonde M10

Receive-only overlay application for standard Meteomodem M10 radiosondes. It
samples the BK4829 AF output on PA4, validates the M10 checksum, and displays
the serial number, GPS position, altitude, and outside temperature.

## Status

Version 0.2 implements the standard M10 `0x9F` telemetry frame and outside
temperature conversion. The parser and soft-input path have a synthetic host
test. Live reception has also been validated in the field with an operational
M10 radiosonde transmitted from Trappes, France.

Not implemented yet:

- M10 humidity conversion;
- M10+ `0xAF` frames;
- periodic `0x49` satellite-level frames.

## Radio setup

1. Tune the VFO to the M10 frequency in FM wide mode. Operational weather
   sondes are normally found in the 400–406 MHz meteorological band.
2. Launch **Sonde M10**.
3. If a nearby sonde overloads the receiver, use key `3` to increase front-end
   attenuation.

The application uses the same proven receive path as Sonde M20:

```
M10 RF -> BK4829 RAW, wide IF -> AF bypass -> PA4 ADC at 19.2 kHz
       -> edge DPLL -> differential Manchester -> checksum -> telemetry
```

M10 nominal timing is about 9615 chips/s, so 19.2 kHz provides just under two
samples per chip. The decoder DPLL tracks this small difference.

## Keys

| Key | Action |
|---|---|
| `1` | Speaker on/off |
| `2` / `F`+`2` | Tune +5 / -5 kHz, up to +/-50 kHz |
| `3` / `F`+`3` | Cycle attenuation forward / backward |
| `4` | Restore the VFO frequency |
| `EXIT` | Quit |

## Supported frame

- raw synchronization header `0x99994C99`;
- differential Manchester;
- frame lengths `0x64`, `0x66`, and `0x76` with type `0x9F`;
- M10 linear 16-bit checksum;
- Trimble GPS packet `0x20` position and altitude;
- standard five-byte M10 serial number.
- Shibaura PB5-41E thermistor temperature, converted with a compact lookup
  table and integer interpolation.

Frames with an invalid GPS fix still count as valid received frames and show
their serial number, but no position is displayed.

Each valid received frame triggers two short flashes of the green LED.

## Build

From the repository root:

```bash
./compile-app.sh sondem10
```

The result is `build/Apps/SondeM10.app` and must fit the 4 KiB overlay. The
firmware must include the overlay-app loader. At present, the repository's
plain `Fusion` preset leaves that loader disabled; use the project's intended
overlay-enabled firmware configuration when installing the app.

Host test:

```bash
App/apps/sondem10/test/run_tests.sh
```

Synthetic Flipper Zero bench files can be generated without using a real M10:

```bash
python3 -B App/apps/sondem10/test/flipper_m10.py App/apps/sondem10/test/flipper
```

Tune the radio to 433.650 MHz FM wide for this bench test. The generated files
are synthetic SRD-band transmissions, not recordings of an operational sonde.
Use only a frequency permitted by the Flipper's configured region.

## Version

Current: **v0.2**.
