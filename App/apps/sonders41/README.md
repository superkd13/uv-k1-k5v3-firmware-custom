# Sonde RS41

Receive-only overlay application for standard Vaisala RS41 radiosondes. It
samples the BK4829 AF output on PA4, validates RS41 STATUS and GPS sub-blocks,
and displays the serial number, GPS position, altitude, vertical speed, and
the number of GPS satellites.

## Status

Version 0.2 implements the common 320-byte RS41 frame at 4800 bit/s. The
parser, CRC path, normal RF polarity, and inverted RF polarity have synthetic
host tests, and `test/model_rx.py` runs the demodulator through a model of the
radio's RAW audio path. Reception on a UV-K1 or UV-K5 V3 has been validated
with the generated Flipper Zero files in both RF polarities. Reception of a
real RS41 signal still needs field validation.

To fit the 4 KiB overlay, this first version deliberately does not implement:

- Reed-Solomon error correction;
- PTU temperature, humidity, and pressure conversion;
- extended or encrypted RS41 sub-blocks.

Consequently, a clean signal is required. A valid STATUS block is mandatory;
an invalid GPS block still counts as a received frame but shows no position.
Each valid received frame triggers two short flashes of the green LED.

## Radio setup

1. Tune the VFO to the RS41 frequency in FM wide mode. Operational weather
   sondes are normally found in the 400.15–406 MHz meteorological band.
2. Launch **Sonde RS41**.
3. If a nearby sonde overloads the receiver, use key `3` to increase front-end
   attenuation.

The receive path is:

```
RS41 RF -> BK4829 RAW, wide IF -> AF bypass -> PA4 ADC at 19.2 kHz
        -> 2 hysteresis slicers, each with its DPLL -> hard-bit sync
        -> de-whitening -> CRC -> telemetry
```

The RAW audio path cuts the low frequencies (corner near 1 kHz, as found with
POCSAG RX), so the NRZ level droops back to zero within a few bits and a plain
sign decision fails. Each slicer flips its level when the signal passes 2/8 or
4/8 of its peak and holds it in between. The sync word is matched on hard
bits, either polarity, with up to 6 wrong bits out of 64.

Model (`python3 -B App/apps/sonders41/test/model_rx.py`): the three Flipper
files are read correctly for an audio corner anywhere from 30 Hz to 1.5 kHz,
with a +/-1 % clock error and moderate noise; receiver noise alone gives no
false sync.

## Keys

| Key | Action |
|---|---|
| `1` | Speaker on/off |
| `2` / `F`+`2` | Tune +5 / -5 kHz, up to +/-50 kHz |
| `3` / `F`+`3` | Cycle attenuation forward / backward |
| `4` | Restore the VFO frequency |
| `EXIT` | Quit |

## Supported frame

- 64-bit RS41 synchronization word;
- 320-bit alternating preamble;
- 64-byte repeating whitening mask;
- common 320-byte frame;
- CRC-16/CCITT-FALSE STATUS (`0x79`) and GPS (`0x7B`) sub-blocks;
- eight-character sonde serial number;
- GPS ECEF position converted to latitude, longitude, and altitude;
- GPS ECEF velocity projected on the local vertical (`VS`, m/s, positive
  while climbing) and the number of satellites used (`SAT`).

## Build

From the repository root:

```bash
./compile-app.sh sonders41
```

The result is `build/Apps/SondeRS41.app` and must fit the 4 KiB overlay. The
firmware must include the overlay-app loader. The repository's plain `Fusion`
preset leaves that loader disabled; use the project's intended overlay-enabled
Fusion configuration when installing the app.

Host test:

```bash
App/apps/sonders41/test/run_tests.sh
```

Synthetic Flipper Zero bench files can be generated without a real RS41:

```bash
python3 -B App/apps/sonders41/test/flipper_rs41.py App/apps/sonders41/test/flipper
```

Tune the radio to 433.650 MHz FM wide for this bench test. The generated files
are synthetic SRD-band transmissions, not recordings of an operational sonde.
Use only a frequency permitted by the Flipper's configured region.

## Version

Current: **v0.2**.
