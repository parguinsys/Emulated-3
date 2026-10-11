#include "board_logic.h"
#include <atomic>

namespace fw {

using namespace eiii;

void board_logic::reset_stream()
{
	m_hal.irq_off();
	m_playing = false;
	m_written = m_played = 0;
	m_cv_head = m_cv_tail = 0;
	m_in_underrun = false;
	m_hal.irq_on();
}

void board_logic::on_control(board::control_command c)
{
	switch (c)
	{
	case board::START:
		// a new stream: silence first, then play once it has been buffered (poll)
		m_hal.set_relay(false); m_relay = false;
		reset_stream();
		m_host_started = true;
		break;
	case board::MUTE:
		m_hal.set_relay(false); m_relay = false;
		m_host_started = false;
		reset_stream();
		break;
	case board::UNMUTE: break;   // the firmware unmutes by itself once the board is safe and the stream is running
	}
}

void board_logic::on_block(uint32_t, unsigned frames, board::cv_event const *cv, unsigned n_cv, int16_t const *pcm)
{
	if (!m_host_started) return;
	uint32_t const w = m_written;
	if (w - m_played + frames > capacity) { ++m_overflows; return; }   // the host ignored the credit
	for (unsigned e = 0; e < n_cv; ++e)
	{
		uint32_t const head = m_cv_head;
		if (head - m_cv_tail >= cv_capacity) { ++m_overflows; continue; }
		m_cv[head % cv_capacity] = { w + cv[e].offset, cv[e].voice, cv[e].param, cv[e].word };
		std::atomic_signal_fence(std::memory_order_release);
		m_cv_head = head + 1;
	}
	for (unsigned f = 0; f < frames; ++f)
		for (unsigned v = 0; v < voices; ++v) m_ring[(w + f) % capacity][v] = pcm[f * voices + v];
	std::atomic_signal_fence(std::memory_order_release);
	m_written = w + frames;
}

void board_logic::render(int16_t *out, unsigned frames)
{
	for (unsigned f = 0; f < frames; ++f, out += voices)
	{
		uint32_t const p = m_played;
		if (!m_playing || p == m_written)
		{
			for (unsigned v = 0; v < voices; ++v) out[v] = 0;
			if (m_playing && !m_in_underrun) { m_in_underrun = true; ++m_underruns; }
			continue;
		}
		m_in_underrun = false;
		// CV words take effect with their frame
		while (m_cv_tail != m_cv_head && int32_t(m_cv[m_cv_tail % cv_capacity].frame - p) <= 0)
		{
			pending_cv const &c = m_cv[m_cv_tail % cv_capacity];
			if (c.voice < voices && c.param < params) { m_target[c.voice][c.param] = c.word; m_dirty[c.param] |= 1u << c.voice; }
			m_cv_tail = m_cv_tail + 1;
		}
		for (unsigned v = 0; v < voices; ++v) out[v] = m_ring[p % capacity][v];
		m_played = p + 1;
	}
}

// One pass over the DACs: a round per channel letter (A..D).  In a round every chip with something to write gets one
// addressed word, then one LD latches them all; a chip not written re-latches its previous word, which still equals
// that channel's output.  `all`: every chip in every round (the prime after /CLR release, so no chip latches an
// undefined input register on the first shared LD).
void board_logic::write_dacs(bool all)
{
	uint16_t target[voices][params];
	uint32_t dirty[params];
	m_hal.irq_off();
	for (unsigned p = 0; p < params; ++p) { dirty[p] = all ? 0xffff : m_dirty[p]; m_dirty[p] = 0; }
	for (unsigned v = 0; v < voices; ++v) for (unsigned p = 0; p < params; ++p) target[v][p] = m_target[v][p];
	m_hal.irq_on();
	for (unsigned ch = 0; ch < 4; ++ch)
	{
		bool wrote = false;
		for (unsigned chip = 0; chip < dac_chips; ++chip)
		{
			unsigned const p = chip / 4, v = (3 - chip % 4) * 4 + (3 - ch);
			if (!(dirty[p] >> v & 1)) continue;
			m_hal.dac_word(chip, dac_data(ch, target[v][p]));
			m_written_word[v][p] = target[v][p];
			wrote = true;
		}
		if (wrote) m_hal.dac_load();
	}
}

void board_logic::enter_fault()
{
	m_hal.set_relay(false); m_relay = false;   // mute first
	// drop what is buffered but keep counting in the host's frame numbers, so its credit stays right
	m_hal.irq_off(); m_playing = false; m_played = m_written; m_cv_tail = m_cv_head; m_hal.irq_on();
	m_hal.codecs_stop();
	m_hal.set_bus(false);
	m_hal.set_clear(true);
	++m_faults;
	m_state = state::wait_rails;
}

void board_logic::poll()
{
	uint32_t const now = m_hal.millis();
	if (m_state != state::wait_rails && !m_hal.rail_ok()) { enter_fault(); return; }
	switch (m_state)
	{
	case state::wait_rails:
	case state::fault:
		if (!m_hal.rail_ok()) break;
		// rails and references valid: buses on, /CLR released, then every channel loaded with its start-up word
		m_hal.set_bus(true);
		m_hal.set_clear(false);
		m_hal.irq_off();
		for (unsigned v = 0; v < voices; ++v) for (unsigned p = 0; p < params; ++p) m_target[v][p] = startup_word[p];
		m_hal.irq_on();
		write_dacs(true);
		m_state = state::settle; m_since = now;
		break;
	case state::settle:
		if (now - m_since < settle_ms) break;
		m_hal.codecs_start();
		m_state = state::codecs; m_since = now;
		break;
	case state::codecs:
		// CS42448: ready ~2000 LRCK cycles after power-up (21 ms at 96 kHz), DAC unmute ~90 cycles later
		if (now - m_since < 25) break;
		m_state = state::ready;
		break;
	case state::ready:
		write_dacs(false);
		if (m_host_started && !m_playing && m_written - m_played >= capacity / 2)
		{
			m_hal.irq_off(); m_playing = true; m_hal.irq_on();
			m_since = now;
		}
		// unmute once the stream has played long enough for its CV words to reach the DACs and settle
		if (m_playing && !m_relay && now - m_since >= settle_ms) { m_hal.set_relay(true); m_relay = true; }
		break;
	}
}

board::status board_logic::status() const
{
	board::status s;
	s.played = m_played;
	s.capacity = uint16_t(capacity - 2 * board::block_frames);   // room for the blocks already on their way
	s.underruns = m_underruns;
	s.flags = uint8_t((m_hal.rail_ok() ? board::RAIL_OK : 0) | (m_relay ? 0 : board::MUTED));
	return s;
}

} // namespace fw
