# Analog voice board firmware (Teensy 4.1)

Runs the Teensy 4.1 on the EIII / Wave Rev A analog board. The emulator (`eiii-standalone --board /dev/ttyACM0`) sends the
16 voice lanes at 96 kHz and the CV words over USB (`host/eiii_board_link.h`). This firmware:

* buffers the lanes (2048 frames) and plays them to both CS42448 codecs as 96 kHz TDM on SAI1, voices 0-7 on codec A
  and 8-15 on codec B;
* writes each CV word to its DAC8420 channel when its frame plays, following `kicad_sm/docs/dac_write_sequence.md`
  (SPI mode 3 at 4 MHz, /DEN only while SCK is high, one LD per channel round);
* powers up as `mute_operation.md` describes: muted, waits for RAIL_OK, enables the buses, releases /CLR, primes every
  DAC with start-up codes, settles, starts the codecs, and unmutes only once a stream has played for 30 ms. If RAIL_OK
  drops it opens the relay first, then stops;
* reports its buffer state every millisecond, which paces the emulator, and passes MIDI IN, the pedal and the
  footswitches to the emulator.

`src/board_logic.*` holds the sequencing and buffering without any Teensy code. `tests/board_fw.cpp` checks it against
the board contracts on a PC (`make tests/board_fw && ./tests/board_fw` at the top of the repo).
`src/firmware.cpp` holds the pins, SPI, clocks, SAI/DMA and codec setup.

## Build and upload

    pio run -t upload        # PlatformIO (platformio.ini)

`make TEENSY_CORE=... TEENSY_SPI=...` compiles against checkouts of
PaulStoffregen/cores and /SPI with `arm-none-eabi-gcc`; use it as a compile check only.

## Not yet run on the board

The hardware parts are written from the docs and the i.MX RT1062 / CS42448 register maps but have not run. Check these
at bring-up:

* clocks: MCLK 24.576 MHz on pin 23; DAC BCLK 24.576 MHz and LRCK 96 kHz on pins 36/37; ADC BCLK 12.288 MHz and
  LRCK 48 kHz on pins 21/20;
* DAC8420 transfers on a scope: /CSk falls and rises only while SCK is high, 16 bits per transfer, an LD pulse after
  each round;
* the CS42448 SPI chip address (`0x9E`) and the Functional Mode, Interface Formats and ADC Control values in
  `codecs_start`;
* the TDM slot order (`tdm_slot` in `board_logic.h`; `codec_channel_map.csv` marks it VERIFY) and footswitch polarity;
* the sampling input from the codec ADCs, which is not handled yet.
