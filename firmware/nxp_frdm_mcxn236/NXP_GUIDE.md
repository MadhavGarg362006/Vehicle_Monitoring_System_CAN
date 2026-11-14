# FRDM-MCXN236 node: GPS + onboard accelerometer -> CAN

Status: `vhms_logic.h` (NMEA parser + CAN packers) is unit tested on a PC
(`gcc -Wall -Wextra -o test_logic test_logic.c && ./test_logic`).
`main.c` / `app.h` are NOT compiled here (no SDK). SDK function and macro names follow
SDK 26.9 conventions but are unverified: if the build fails, paste the errors.

## 1. Where are D18 / D19?

They are the last two pins of the **outer (Arduino) row of header J2** (the 2x10 header).
Silkscreen most likely says **SDA** and **SCL**, not D18/D19 (on Arduino R3 layouts D18/D19 = SDA/SCL).

| Header pin | Arduino name | MCU pin | Use for GPS |
|-----------|--------------|---------|-------------|
| J2 pin 18 | SDA (D18)    | P1_16   | MCU RX  <- GPS TX |
| J2 pin 20 | SCL (D19)    | P1_17   | MCU TX  -> GPS RX (optional) |

Pin 1 is marked on the board; pins 1/2 are on the same end, and even pins are one row.
If unsure, with the board powered, use a multimeter in continuity mode from a pin to the
SDA/SCL label's via or look at the silkscreen near the AREF/GND pins: the SDA/SCL pair is
at the far end of J2 from D8.

## 2. GPS wiring (3.3 V logic)

| GPS module | FRDM-MCXN236 |
|-----------|--------------|
| TX  | J2 pin 18 (SDA / P1_16) |
| RX  | J2 pin 20 (SCL / P1_17) |
| GND | any GND, e.g. J3 pin 12 or 14 |
| VCC | 3.3 V (J3 pin 4 or 8). Use 5 V (J3 pin 10) only if your module has its own regulator |

Most NEO-6M / NEO-M8N modules default to 9600 baud NMEA. The GPS needs a sky view; first
fix can take minutes. Indoors you will see "RMC" count rising but fix=no.

## 3. Pins (done in code, no Config Tools needed)

I could not find the pin-mode numbers (kPORT_MuxAltN) for P1_16/P1_17 (GPS) and
P4_0/P4_1 (accelerometer). So `main.c` tries Alt2..Alt7 at start-up and keeps the first that works:
- accelerometer: the one where the FXLS8974 answers WHO_AM_I (0x86)
- GPS: the one where valid NMEA arrives (up to 3 s per try, only while the GPS is wired and powered)

It prints which one worked, e.g. `Accel: found with kPORT_MuxAlt2 on P4_0/P4_1`.
Once you know the numbers, set `VHMS_FORCE_MUX_FC5` / `VHMS_FORCE_MUX_FC2` in `main.c` to skip the search.
Start-up can take up to about 20 s with the GPS connected but silent.
While searching, a wrong mode may briefly connect a pin to another peripheral signal; this is a
short test at low drive strength, not expected to damage anything, but do not leave it running if
a wrong mode were to drive the GPS TX line for a long time. Connect the GPS only after the accelerometer
is found if you want to be extra careful.

Also add `fsl_lpuart` and `fsl_lpi2c` drivers to CMakeLists/prj.conf like `fsl_flexcan`.

## 4. Files

Copy into your working project's `source` folder (replace your `main.c` and `app.h`):
- `main.c`, `app.h`, `vhms_logic.h`

`app.h` keeps your working CAN settings (`CAN1`, `TX_MCXN_MB 2U`, FlexCAN clock 1).
The receive mailboxes are not used any more: this node only sends.

## 5. Expected serial output (1 line per second)

```
Accel: address 0x18 answered, WHO_AM_I = 0x86
Accel: FXLS8974 running at 50 Hz, +-2 g
GPS bytes=1432 RMC=2 fix=no | accel mg X=12 Y=-4 Z=1001 | CAN sent=5 timeout=0 drop=0
```
- Flat on the table, Z should be about +-1000 mg and X/Y near 0.
- `GPS bytes` stays 0: wiring TX/RX swapped, no power, or pins not muxed to LPUART5.
- `CAN timeout` rises: no other node acknowledging (the Arduino/STM32 must be on and
  the bus terminated: about 60 ohm between CANH and CANL).
- `Accel: FXLS8974 not found`: I2C pins not muxed on FC2, or the board variant uses
  another address; the log prints any address that answers.
- On the Arduino gateway (or any CAN monitor) you should see ID 0x10A every second, and
  0x105 / 0x10B once the GPS has a fix.

## 6. Notes
- GPS frames are only sent with a valid fix, so the dashboard won't show a bogus 0,0.
- Accelerometer axes depend on how the board sits; no calibration is applied.
- Speed is derived from the RMC sentence (knots x 1.852).
