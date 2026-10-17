// Analog voice board firmware, Teensy 4.1 side: pins (kicad_sm/docs/teensy_pin_allocation.csv), the DAC8420 bus
// (dac_write_sequence.md), the CS42448 codecs over SPI, 96 kHz TDM to both codecs on SAI1, the USB link to the
// emulator, MIDI IN, pedal and footswitches.  The sequencing and buffering live in board_logic.
//
// NOT YET RUN ON THE BOARD.  Bring-up must check, with a scope: MCLK 24.576 MHz on pin 23, DAC BCLK 24.576 MHz / LRCK
// 96 kHz on pins 36/37, ADC BCLK 12.288 MHz / LRCK 48 kHz on pins 21/20, the DAC8420 transfer timing (mode 3, CS only
// while SCK is high), and the CS42448 SPI chip address and register values below against the datasheet.
#include <Arduino.h>
#include <SPI.h>
#include "board_logic.h"

namespace pin {
constexpr uint8_t cs_codec_a = 10, cs_codec_b = 24, fs1 = 25, pedal = 26, fs2 = 27, ld = 30, clr = 31, rly = 34,
	bus_en = 35, rail_ok = 38, da0 = 14, da1 = 15, den = 16, da4 = 17, da2 = 40, da3 = 41, codec_rst = 22;
}

static fw::board_logic *g_logic;

// ---- the hardware interface for board_logic ----
struct teensy_hal : fw::hal
{
	SPISettings dac_spi{ 4000000, MSBFIRST, SPI_MODE3 }, codec_spi{ 6000000, MSBFIRST, SPI_MODE3 };
	uint32_t da_mask = CORE_PIN14_BITMASK | CORE_PIN15_BITMASK | CORE_PIN40_BITMASK | CORE_PIN41_BITMASK | CORE_PIN17_BITMASK;

	uint32_t millis() override { return ::millis(); }
	bool rail_ok() override { return digitalReadFast(pin::rail_ok); }
	void set_relay(bool on) override { digitalWriteFast(pin::rly, on); }
	void set_bus(bool enabled) override
	{
		if (enabled)
		{
			// every bus pin a defined output before the buffers open
			digitalWriteFast(pin::ld, HIGH); digitalWriteFast(pin::den, HIGH);
			SPI.begin();
			SPI.beginTransaction(dac_spi);   // SCK now idles high (mode 3)
			SPI.endTransaction();
		}
		digitalWriteFast(pin::bus_en, enabled);
	}
	void set_clear(bool asserted) override { digitalWriteFast(pin::clr, !asserted); }
	void dac_word(unsigned chip, uint16_t word) override
	{
		// dac_write_sequence.md steps 1-6
		SPI.beginTransaction(dac_spi);
		uint32_t const bits = (chip & 1 ? CORE_PIN14_BITMASK : 0) | (chip & 2 ? CORE_PIN15_BITMASK : 0) |
			(chip & 4 ? CORE_PIN40_BITMASK : 0) | (chip & 8 ? CORE_PIN41_BITMASK : 0) | (chip & 16 ? CORE_PIN17_BITMASK : 0);
		__disable_irq();
		GPIO6_DR = (GPIO6_DR & ~da_mask) | bits;   // DA4..DA0 in one store, /DEN still high
		__enable_irq();
		delayNanoseconds(100);
		digitalWriteFast(pin::den, LOW);           // SCK is high: exactly one /CSk falls
		delayNanoseconds(100);
		SPI.transfer16(word);                      // ends with SCK high
		digitalWriteFast(pin::den, HIGH);
		SPI.endTransaction();
	}
	void dac_load() override
	{
		delayNanoseconds(100);
		digitalWriteFast(pin::ld, LOW);
		delayNanoseconds(200);
		digitalWriteFast(pin::ld, HIGH);
	}
	void codecs_start() override;
	void codecs_stop() override;
	void irq_off() override { __disable_irq(); }
	void irq_on() override { __enable_irq(); }

	void codec_write(uint8_t cs, uint8_t reg, uint8_t value)
	{
		SPI.beginTransaction(codec_spi);
		digitalWriteFast(cs, LOW);
		SPI.transfer(0x9E);   // CS42448 chip address 1001111 + write (VERIFY)
		SPI.transfer(reg);
		SPI.transfer(value);
		digitalWriteFast(cs, HIGH);
		SPI.endTransaction();
	}
};

// ---- clocks and 96 kHz TDM output on SAI1 (two data lines: codec A on pin 7, codec B on pin 32) ----
namespace tdm {
constexpr unsigned half_frames = 32;   // frames per DMA half (0.33 ms)
DMAMEM __attribute__((aligned(32))) static int16_t buffer[2 * half_frames * 8 * 2];   // frame: 8 slots x (A, B)
static DMAChannel dma(false);
static uint8_t slot_voice[2][8];

static void fill(int16_t *dest)
{
	int16_t lanes[half_frames][fw::voices];
	g_logic->render(&lanes[0][0], half_frames);
	for (unsigned f = 0; f < half_frames; ++f)
		for (unsigned s = 0; s < 8; ++s)
		{
			*dest++ = lanes[f][slot_voice[0][s]];
			*dest++ = lanes[f][slot_voice[1][s]];
		}
}

static void isr()
{
	uint32_t const saddr = uint32_t(dma.TCD->SADDR);
	dma.clearInterrupt();
	int16_t *dest = saddr < uint32_t(buffer) + sizeof(buffer) / 2 ? buffer + sizeof(buffer) / 4 : buffer;
	fill(dest);
	arm_dcache_flush_delete(dest, sizeof(buffer) / 2);
}

// PLL4 (audio PLL) = 24 MHz x 32.768 = 786.432 MHz.  SAI1 root /16 = 49.152 MHz feeds the bit clocks (MCLK2);
// SAI2 root /32 = 24.576 MHz goes out on pin 23 as the codecs' MCLK (MCLK1).  DAC side: BCLK 24.576 MHz, 256 bits per
// frame -> 96 kHz (double speed, MCLK/LRCK = 256).  ADC side: BCLK 12.288 MHz -> 48 kHz (single speed, 512).
static void clocks()
{
	CCM_CCGR5 |= CCM_CCGR5_SAI1(CCM_CCGR_ON) | CCM_CCGR5_SAI2(CCM_CCGR_ON);
	CCM_ANALOG_PLL_AUDIO = CCM_ANALOG_PLL_AUDIO_BYPASS | CCM_ANALOG_PLL_AUDIO_ENABLE | CCM_ANALOG_PLL_AUDIO_POST_DIV_SELECT(2) |
		CCM_ANALOG_PLL_AUDIO_DIV_SELECT(32);
	CCM_ANALOG_PLL_AUDIO_NUM = 7680;
	CCM_ANALOG_PLL_AUDIO_DENOM = 10000;
	CCM_ANALOG_PLL_AUDIO &= ~CCM_ANALOG_PLL_AUDIO_POWERDOWN;
	while (!(CCM_ANALOG_PLL_AUDIO & CCM_ANALOG_PLL_AUDIO_LOCK)) {}
	CCM_ANALOG_MISC2 &= ~(CCM_ANALOG_MISC2_DIV_MSB | CCM_ANALOG_MISC2_DIV_LSB);
	CCM_ANALOG_PLL_AUDIO &= ~CCM_ANALOG_PLL_AUDIO_BYPASS;

	CCM_CSCMR1 = (CCM_CSCMR1 & ~(CCM_CSCMR1_SAI1_CLK_SEL_MASK | CCM_CSCMR1_SAI2_CLK_SEL_MASK)) |
		CCM_CSCMR1_SAI1_CLK_SEL(2) | CCM_CSCMR1_SAI2_CLK_SEL(2);   // both from PLL4
	CCM_CS1CDR = (CCM_CS1CDR & ~(CCM_CS1CDR_SAI1_CLK_PRED_MASK | CCM_CS1CDR_SAI1_CLK_PODF_MASK)) |
		CCM_CS1CDR_SAI1_CLK_PRED(3) | CCM_CS1CDR_SAI1_CLK_PODF(3);   // /4 /4 = /16
	CCM_CS2CDR = (CCM_CS2CDR & ~(CCM_CS2CDR_SAI2_CLK_PRED_MASK | CCM_CS2CDR_SAI2_CLK_PODF_MASK)) |
		CCM_CS2CDR_SAI2_CLK_PRED(7) | CCM_CS2CDR_SAI2_CLK_PODF(3);   // /8 /4 = /32
	IOMUXC_GPR_GPR1 = (IOMUXC_GPR_GPR1 & ~(IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL_MASK | IOMUXC_GPR_GPR1_SAI1_MCLK2_SEL_MASK)) |
		IOMUXC_GPR_GPR1_SAI1_MCLK_DIR | IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL(1) | IOMUXC_GPR_GPR1_SAI1_MCLK2_SEL(0);
	CORE_PIN23_CONFIG = 3;   // MCLK
}

static void start()
{
	for (unsigned v = 0; v < fw::voices; ++v) slot_voice[v / 8][fw::tdm_slot[v]] = uint8_t(v);
	memset(buffer, 0, sizeof(buffer));
	arm_dcache_flush_delete(buffer, sizeof(buffer));
	// transmitter: asynchronous master, its own bit clock and frame sync on pins 36/37 (Plan B), 8 x 32-bit slots,
	// 16-bit data left-justified (MSB first), frame sync one bit early, 2 data lines
	I2S1_TCSR = 0;
	I2S1_TMR = 0;
	I2S1_TCR1 = I2S_TCR1_RFW(4);
	I2S1_TCR2 = I2S_TCR2_SYNC(0) | I2S_TCR2_BCP | I2S_TCR2_MSEL(2) | I2S_TCR2_BCD | I2S_TCR2_DIV(0);
	I2S1_TCR3 = I2S_TCR3_TCE_2CH;
	I2S1_TCR4 = I2S_TCR4_FRSZ(7) | I2S_TCR4_SYWD(0) | I2S_TCR4_MF | I2S_TCR4_FSE | I2S_TCR4_FSD;
	I2S1_TCR5 = I2S_TCR5_WNW(31) | I2S_TCR5_W0W(31) | I2S_TCR5_FBT(31);
	// receiver: ADC clocks only for now (sampling input comes later), 48 kHz
	I2S1_RCSR = 0;
	I2S1_RMR = 0;
	I2S1_RCR1 = I2S_RCR1_RFW(4);
	I2S1_RCR2 = I2S_RCR2_SYNC(0) | I2S_RCR2_BCP | I2S_RCR2_MSEL(2) | I2S_RCR2_BCD | I2S_RCR2_DIV(1);
	I2S1_RCR3 = 0;
	I2S1_RCR4 = I2S_RCR4_FRSZ(7) | I2S_RCR4_SYWD(0) | I2S_RCR4_MF | I2S_RCR4_FSE | I2S_RCR4_FSD;
	I2S1_RCR5 = I2S_RCR5_WNW(31) | I2S_RCR5_W0W(31) | I2S_RCR5_FBT(31);
	CORE_PIN36_CONFIG = 3;   // TX_BCLK  (DAC BCLK via U56)
	CORE_PIN37_CONFIG = 3;   // TX_SYNC  (DAC LRCK via U56)
	CORE_PIN21_CONFIG = 3;   // RX_BCLK  (ADC BCLK)
	CORE_PIN20_CONFIG = 3;   // RX_SYNC  (ADC LRCK)
	CORE_PIN7_CONFIG = 3;    // TX_DATA0 -> codec A SDIN
	CORE_PIN32_CONFIG = 3;   // TX_DATA1 -> codec B SDIN

	// DMA: per slot, codec A's word into TDR0 and codec B's into TDR1 (upper halves: 16 bits left-justified)
	dma.begin(true);
	dma.TCD->SADDR = buffer;
	dma.TCD->SOFF = 2;
	dma.TCD->ATTR = DMA_TCD_ATTR_SSIZE(1) | DMA_TCD_ATTR_DSIZE(1);
	dma.TCD->NBYTES_MLOFFYES = DMA_TCD_NBYTES_DMLOE | DMA_TCD_NBYTES_MLOFFYES_MLOFF(-8) | DMA_TCD_NBYTES_MLOFFYES_NBYTES(4);
	dma.TCD->SLAST = -int32_t(sizeof(buffer));
	dma.TCD->DADDR = (void *)(uint32_t(&I2S1_TDR0) + 2);
	dma.TCD->DOFF = 4;
	dma.TCD->CITER_ELINKNO = sizeof(buffer) / 4;
	dma.TCD->DLASTSGA = -8;
	dma.TCD->BITER_ELINKNO = sizeof(buffer) / 4;
	dma.TCD->CSR = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;
	dma.triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_TX);
	dma.attachInterrupt(isr);
	dma.enable();
	I2S1_RCSR = I2S_RCSR_RE | I2S_RCSR_BCE;
	I2S1_TCSR = I2S_TCSR_TE | I2S_TCSR_BCE | I2S_TCSR_FRDE;
}

static void stop()
{
	I2S1_TCSR = 0;
	I2S1_RCSR = 0;
	dma.disable();
}
} // namespace tdm

void teensy_hal::codecs_start()
{
	tdm::start();                          // clocks running before the codecs leave reset
	delay(1);
	digitalWriteFast(pin::codec_rst, HIGH);
	delay(1);
	for (uint8_t cs : { pin::cs_codec_a, pin::cs_codec_b })
	{
		// CS42448 recommended power-up (datasheet 4.9): power down, configure, DACs muted, power up
		codec_write(cs, 0x02, 0xFF);       // Power Control: PDN
		codec_write(cs, 0x03, 0xF4);       // Functional Mode: slave, speed modes auto-detected (VERIFY MFREQ)
		codec_write(cs, 0x04, 0x76);       // Interface Formats: DAC and ADC TDM
		codec_write(cs, 0x05, 0x1C);       // ADC Control: single-ended inputs
		codec_write(cs, 0x06, 0x63);       // Transition Control: soft ramp
		codec_write(cs, 0x07, 0xFF);       // DAC Channel Mute: all muted
		codec_write(cs, 0x02, 0x00);       // Power Control: up
	}
	delay(25);                             // ~2000 LRCK cycles at 96 kHz
	for (uint8_t cs : { pin::cs_codec_a, pin::cs_codec_b }) codec_write(cs, 0x07, 0x00);   // DACs unmuted (relay still open)
}

void teensy_hal::codecs_stop()
{
	digitalWriteFast(pin::codec_rst, LOW);
	tdm::stop();
}

// ---- USB link, MIDI IN, pedal and footswitches ----
static teensy_hal g_hal;
static fw::board_logic g_board(g_hal);

void setup()
{
	// safe state first: relay open, buses gated, DACs cleared, codecs in reset
	for (uint8_t p : { pin::rly, pin::bus_en, pin::clr, pin::codec_rst }) { digitalWriteFast(p, LOW); pinMode(p, OUTPUT); }
	for (uint8_t p : { pin::ld, pin::den, pin::cs_codec_a, pin::cs_codec_b }) { digitalWriteFast(p, HIGH); pinMode(p, OUTPUT); }
	for (uint8_t p : { pin::da0, pin::da1, pin::da2, pin::da3, pin::da4 }) { digitalWriteFast(p, LOW); pinMode(p, OUTPUT); }
	pinMode(pin::rail_ok, INPUT);
	pinMode(pin::fs1, INPUT);
	pinMode(pin::fs2, INPUT);
	analogReadResolution(12);
	g_logic = &g_board;
	tdm::clocks();
	Serial.begin(0);                       // USB serial (speed is the USB bus's)
	Serial7.begin(31250);                  // MIDI IN on pin 28
}

void loop()
{
	static std::vector<uint8_t> pkt;
	static uint8_t buf[2048];
	static uint32_t last_status = 0, last_inputs = 0;
	static uint16_t pedal = 0xffff;
	static uint8_t switches = 0xff;

	while (int n = Serial.available())
	{
		n = Serial.readBytes(reinterpret_cast<char *>(buf), min(n, int(sizeof buf)));
		g_board.receive(buf, size_t(n));
	}
	g_board.poll();

	uint32_t const now = millis();
	if (now != last_status)
	{
		last_status = now;
		eiii::board::encode_status(pkt, g_board.status());
		if (Serial.availableForWrite() >= int(pkt.size())) Serial.write(pkt.data(), pkt.size());
	}
	if (int n = Serial7.available())
	{
		uint8_t midi[64];
		n = Serial7.readBytes(reinterpret_cast<char *>(midi), min(n, int(sizeof midi)));
		eiii::board::encode_midi(pkt, midi, size_t(n));
		Serial.write(pkt.data(), pkt.size());
	}
	if (now - last_inputs >= 10)
	{
		last_inputs = now;
		uint16_t const p = uint16_t(analogRead(pin::pedal));
		uint8_t const s = uint8_t((digitalReadFast(pin::fs1) ? 0 : 1) | (digitalReadFast(pin::fs2) ? 0 : 2));   // closed = low (VERIFY)
		if (s != switches || (p > pedal ? p - pedal : pedal - p) > 8)
		{
			pedal = p; switches = s;
			eiii::board::encode_inputs(pkt, pedal, switches);
			Serial.write(pkt.data(), pkt.size());
		}
	}
}
