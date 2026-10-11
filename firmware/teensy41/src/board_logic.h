// Analog voice board firmware (Teensy 4.1 on EIII / Wave Rev A): the hardware-independent part, built for the Teensy and
// for the host unit test (tests/board_fw.cpp).  It takes the emulator's packets (host/eiii_board_link.h), buffers the
// lane frames for the codec DMA, applies each CV word when its frame is played, writes the DAC8420s in the order the
// board's contract requires (dac_write_sequence.md), and runs the power-up / mute sequence (mute_operation.md).
#pragma once
#include "../../../host/eiii_board_link.h"
#include <cstddef>
#include <cstdint>

namespace fw {

// the board, as the logic sees it; main.cpp implements it on the Teensy, the unit test with a recorder
struct hal
{
	virtual ~hal() = default;
	virtual uint32_t millis() = 0;
	virtual bool rail_ok() = 0;                          // RAIL_OK_T (pin 38): rails and references valid
	virtual void set_relay(bool on) = 0;                  // RLY (pin 34): true = outputs live
	virtual void set_bus(bool enabled) = 0;               // BUS_EN (pin 35); false also parks LD high, /DEN high
	virtual void set_clear(bool asserted) = 0;            // /CLR (pin 31): true = held low (DACs at zero scale)
	virtual void dac_word(unsigned chip, uint16_t word) = 0;   // one addressed 16-bit transfer to chip 0..19
	virtual void dac_load() = 0;                          // LD pulse: every chip latches its pending word
	virtual void codecs_start() = 0;                      // clocks, reset release, register setup, DMA running
	virtual void codecs_stop() = 0;                       // DMA stopped, codecs held in reset
	virtual void irq_off() = 0;                           // around state shared with the audio interrupt
	virtual void irq_on() = 0;
};

// DAC8420 map (kicad_sm/docs/dac_cv_map.csv): parameter p of voice v is on chip 4p + 3 - v/4, channel D, C, B, A for
// v % 4 = 0..3; 12-bit code = CV word bits 13..2 (both full scales are 4.96 V).  Data word: A1 A0 x x D11..D0.
constexpr unsigned dac_chips = 20, params = 5, voices = 16;
constexpr unsigned dac_chip(unsigned voice, unsigned param) { return 4 * param + 3 - voice / 4; }
constexpr unsigned dac_channel(unsigned voice) { return 3 - voice % 4; }   // 0 = A .. 3 = D
constexpr uint16_t dac_data(unsigned channel, uint16_t cv_word) { return uint16_t(channel << 14 | ((cv_word >> 2) & 0xfff)); }
// start-up words (the EIII OS's idle state): filters open, no resonance (Q is inverted), gain closed, pan centre
constexpr uint16_t startup_word[params] = { 0x3ffc, 0x3ffc, 0x3ffc, 0x0000, 0x2000 };

// codec slots (codec_channel_map.csv): voices 0-7 on codec A, 8-15 on codec B, AOUT n in TDM slot n-1.  The map is
// marked VERIFY (which AOUT is on the jack tip); a swap is a change to this table only.
constexpr uint8_t tdm_slot[voices] = { 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7 };

class board_logic : private eiii::board::reader::handler
{
public:
	static constexpr unsigned capacity = 2048;            // frames (21 ms at 96 kHz)
	static constexpr uint32_t settle_ms = 30;             // after the CVs are written, before codecs / unmute

	enum class state : uint8_t { wait_rails, settle, codecs, ready, fault };

	explicit board_logic(hal &h) : m_hal(h) {}
	// bytes from the host's USB serial port
	void receive(uint8_t const *data, size_t n) { m_rd.push(data, n, *this); }
	// main loop: power sequence and DAC writes; call often (at least every millisecond)
	void poll();
	// audio interrupt: fills `frames` frames of 16 lane values (voice order) for the codecs; zeros on underrun
	void render(int16_t *out, unsigned frames);

	eiii::board::status status() const;
	state current() const { return m_state; }
	bool muted() const { return !m_relay; }
	uint32_t faults() const { return m_faults; }
	uint32_t overflows() const { return m_overflows; }

private:
	void on_block(uint32_t first, unsigned frames, eiii::board::cv_event const *cv, unsigned n_cv, int16_t const *pcm) override;
	void on_control(eiii::board::control_command c) override;
	void enter_fault();
	void write_dacs(bool all);
	void reset_stream();

	hal &m_hal;
	eiii::board::reader m_rd;
	state m_state = state::wait_rails;
	uint32_t m_since = 0, m_faults = 0, m_overflows = 0;
	bool m_relay = false, m_host_started = false, m_playing = false;

	// lane frames: written by the USB side (main loop), read by the audio interrupt
	int16_t m_ring[capacity][voices] = {};
	volatile uint32_t m_written = 0, m_played = 0;
	volatile uint16_t m_underruns = 0;
	bool m_in_underrun = false;

	// CV words waiting for their frame (frame number = m_written-relative absolute count)
	struct pending_cv { uint32_t frame; uint8_t voice, param; uint16_t word; };
	static constexpr unsigned cv_capacity = 1024;
	pending_cv m_cv[cv_capacity];
	volatile uint32_t m_cv_head = 0, m_cv_tail = 0;

	// DAC targets (set by the interrupt when a CV's frame plays) and what the DACs hold
	volatile uint16_t m_target[voices][params];
	volatile uint32_t m_dirty[params] = {};               // bit v: voice v changed
	uint16_t m_written_word[voices][params];
};

} // namespace fw
