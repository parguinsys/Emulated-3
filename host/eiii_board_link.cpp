#include "eiii_board_link.h"
#include <cstring>

namespace eiii::board {

namespace {
void put8(std::vector<uint8_t> &o, unsigned v) { o.push_back(uint8_t(v)); }
void put16(std::vector<uint8_t> &o, unsigned v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t> &o, uint32_t v) { put16(o, v & 0xffff); put16(o, v >> 16); }
unsigned get16(uint8_t const *p) { return unsigned(p[0]) | unsigned(p[1]) << 8; }
uint32_t get32(uint8_t const *p) { return uint32_t(get16(p)) | uint32_t(get16(p + 2)) << 16; }

void header(std::vector<uint8_t> &o, packet_type t, size_t payload)
{
	o.clear();
	put8(o, sync0); put8(o, sync1); put8(o, t); put8(o, 0); put16(o, unsigned(payload));
}
}

void block_writer::finish(std::vector<uint8_t> &out)
{
	header(out, BLOCK, 6 + m_cv.size() * 4 + m_pcm.size() * 2);
	put32(out, uint32_t(m_frame0)); put8(out, m_frames); put8(out, unsigned(m_cv.size()));
	for (auto const &e : m_cv) { put8(out, e.offset); put8(out, unsigned(e.voice) << 4 | e.param); put16(out, e.word); }
	for (int16_t s : m_pcm) put16(out, uint16_t(s));
	reset(m_frame0 + m_frames);
}

void encode_control(std::vector<uint8_t> &out, control_command c) { header(out, CONTROL, 1); put8(out, c); }

void encode_status(std::vector<uint8_t> &out, status const &s)
{
	header(out, STATUS, 9);
	put32(out, s.played); put16(out, s.capacity); put16(out, s.underruns); put8(out, s.flags);
}

void encode_midi(std::vector<uint8_t> &out, uint8_t const *bytes, size_t n)
{
	header(out, MIDI, n);
	out.insert(out.end(), bytes, bytes + n);
}

void encode_inputs(std::vector<uint8_t> &out, uint16_t pedal, uint8_t switches) { header(out, INPUTS, 3); put16(out, pedal); put8(out, switches); }

void reader::push(uint8_t const *data, size_t n, handler &h)
{
	m_buf.insert(m_buf.end(), data, data + n);
	size_t at = 0;
	while (m_buf.size() - at >= header_bytes)
	{
		uint8_t const *p = m_buf.data() + at;
		if (p[0] != sync0 || p[1] != sync1) { ++at; ++m_dropped; continue; }
		size_t const len = get16(p + 4);
		if (len > max_payload) { ++at; ++m_dropped; continue; }
		if (m_buf.size() - at < header_bytes + len) break;   // wait for the rest
		uint8_t const *q = p + header_bytes;
		bool ok = true;
		switch (p[2])
		{
		case BLOCK:
		{
			if (len < 6) { ok = false; break; }
			unsigned const frames = q[4], ncv = q[5];
			if (len != 6 + ncv * 4 + frames * voices * 2) { ok = false; break; }
			m_cv.resize(ncv);
			for (unsigned k = 0; k < ncv; ++k)
			{
				uint8_t const *e = q + 6 + 4 * k;
				m_cv[k] = { e[0], uint8_t(e[1] >> 4), uint8_t(e[1] & 15), uint16_t(get16(e + 2)) };
			}
			m_pcm.resize(size_t(frames) * voices);
			uint8_t const *s = q + 6 + 4 * ncv;
			for (size_t k = 0; k < m_pcm.size(); ++k) m_pcm[k] = int16_t(get16(s + 2 * k));
			h.on_block(get32(q), frames, m_cv.data(), ncv, m_pcm.data());
			break;
		}
		case CONTROL: if (len == 1) h.on_control(control_command(q[0])); else ok = false; break;
		case STATUS:
			if (len == 9) { status s; s.played = get32(q); s.capacity = uint16_t(get16(q + 4)); s.underruns = uint16_t(get16(q + 6)); s.flags = q[8]; h.on_status(s); }
			else ok = false;
			break;
		case MIDI: h.on_midi(q, len); break;
		case INPUTS: if (len == 3) h.on_inputs(uint16_t(get16(q)), q[2]); else ok = false; break;
		default: ok = false;
		}
		if (!ok) { ++at; ++m_dropped; continue; }
		at += header_bytes + len;
	}
	m_buf.erase(m_buf.begin(), m_buf.begin() + ptrdiff_t(at));
}

} // namespace eiii::board
