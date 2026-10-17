// Analog board link (host/eiii_board_link): the lanes and CV words sent to the board carry everything the voice cells
// need.  The same stimulus drives a software backend (CEM3387 V1 model at 96 kHz) and a board-mode backend whose packets
// are decoded and fed to the same model, as a board with ideal voice cells would; the two outputs must be identical.
// Also: packet round trips, resynchronisation after garbage, CV bursts larger than one block's table.
#include "../host/eiii_backend.h"
#include "../host/eiii_board_link.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace eiii;
static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); ++fails; } printf("\n"); } while (0)

namespace {
struct lcg { uint32_t s; uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; } };   // low LCG bits are poor

// the board side: decodes the packets and runs them through the CEM3387 model as the voice cells would
struct model_board : board_sink, board::reader::handler
{
	board::reader rd;
	cem3387_bank cem;
	std::vector<float> out;
	double accl = 0, accr = 0;
	unsigned k = 0, blocks = 0, cv_events = 0;
	uint64_t next_frame = 0;
	bool frames_contiguous = true;
	model_board() { cem.prepare(board::frame_rate); }
	void send(uint8_t const *data, size_t n) override
	{
		// deliver in uneven pieces, as a USB serial port does
		size_t const piece = 1 + (blocks * 37) % 97;
		for (size_t at = 0; at < n; at += piece) rd.push(data + at, std::min(piece, n - at), *this);
	}
	void apply(board::cv_event const &e) { cem.cv_write(e.param, 15 - e.voice, e.word); ++cv_events; }
	void on_block(uint32_t first, unsigned frames, board::cv_event const *cv, unsigned n_cv, int16_t const *pcm) override
	{
		++blocks;
		if (first != uint32_t(next_frame)) frames_contiguous = false;
		next_frame += frames;
		unsigned e = 0;
		for (unsigned f = 0; f < frames; ++f)
		{
			for (; e < n_cv && cv[e].offset == f; ++e) apply(cv[e]);
			double l, r;
			cem.process(pcm + f * board::voices, l, r);
			accl += l; accr += r;
			if (++k == 2) { out.push_back(float(accl / 2 / 10.0)); out.push_back(float(accr / 2 / 10.0)); accl = accr = 0; k = 0; }
		}
		for (; e < n_cv; ++e) apply(cv[e]);
	}
};

// firmware-like use of the voice hardware: lanes looping through sample RAM at different rates, CV updates every 2 ms
template <class F> void stimulus(F &&each, double seconds)
{
	lcg rng{4711};
	uint64_t const end = uint64_t(seconds * 20e6), tick = 40000;   // 500 Hz in 20 MHz clocks
	uint64_t t = 1000;
	for (unsigned v = 0; v < 16; ++v)
	{
		unsigned const c = 15 - v;
		each([=](output_backend &b) { b.cv_write(t, 0, c, 0x3ffc); b.cv_write(t, 1, c, 0x3ffc); b.cv_write(t, 2, c, 0x3ffc); b.cv_write(t, 3, c, 0); b.cv_write(t, 4, c, 0x2000); });
	}
	unsigned env[16] = {};
	bool on[16] = {};
	for (t = tick; t < end; t += tick)
	{
		for (unsigned v = 0; v < 16; ++v)
		{
			unsigned const c = 15 - v;
			uint64_t const tv = t + rng.next() % 3000;
			if (rng.next() % 200 == 0)
			{
				on[v] = !on[v]; env[v] = 0;
				if (on[v])
				{
					uint32_t const DEC = 1u << 23, LOOP = 1u << 24;
					uint32_t const start = (rng.next() % 0x3000) & ~1u, rate = 0x0a4 + rng.next() % 0x300;
					each([=](output_backend &b) {
						b.ctrl_write(tv, v, uint8_t(rate >> 8)); b.ctrl_write(tv + 2, v, uint8_t(rate));
						b.rf_write(tv + 4, v, 2, start); b.rf_write(tv + 6, v, 0, 0x7ff | DEC | LOOP);
						b.rf_write(tv + 8, v, 3, start + 0x100); b.rf_write(tv + 10, v, 1, 0x3ff | DEC | LOOP);
					});
				}
			}
			if (on[v])
			{
				env[v] = std::min(env[v] + 1, 400u);
				uint16_t const g = uint16_t((env[v] < 50 ? env[v] * 300 : 15000 - (env[v] - 50) * 20) & 0x3ffc);
				uint16_t const f = uint16_t((0x2800 + (env[v] * 13) % 0x1000) & 0x3ffc);
				each([=](output_backend &b) { b.cv_write(tv + 20, 3, c, g); b.cv_write(tv + 24, 1, c, f); b.cv_write(tv + 28, 2, c, uint16_t(0x3994 - (v * 0x100))); });
			}
		}
		each([=](output_backend &b) { b.render_until(t); });
	}
}

struct capture : board::reader::handler
{
	board::status st; std::vector<uint8_t> midi; uint16_t pedal = 0; uint8_t sw = 0; int controls = 0, blocks = 0;
	void on_status(board::status const &s) override { st = s; }
	void on_midi(uint8_t const *b, size_t n) override { midi.insert(midi.end(), b, b + n); }
	void on_inputs(uint16_t p, uint8_t s) override { pedal = p; sw = s; }
	void on_control(board::control_command c) override { controls += c; }
	void on_block(uint32_t, unsigned, board::cv_event const *, unsigned, int16_t const *) override { ++blocks; }
};
}

int main()
{
	std::vector<uint16_t> ram(0x10000);
	for (size_t i = 0; i < ram.size(); ++i) ram[i] = uint16_t(int(12000 * std::sin(i * 0.031) + 3000 * std::sin(i * 0.17)));

	software_backend sw(ram.data(), ram.size(), 48000.0), hw(ram.data(), ram.size(), 48000.0);
	model_board board;
	hw.use_board(&board);
	// the board starts from the CV words written so far (all zero here, as after the DACs' CLR); same for the model
	for (unsigned c = 0; c < 16; ++c) for (unsigned bk = 0; bk < 5; ++bk) sw.cv_write(0, bk, c, 0);
	stimulus([&](auto const &op) { op(sw); op(hw); }, 3.0);
	auto const &a = sw.audio(), &b = board.out;
	size_t const n = std::min(a.size(), b.size());
	double peak = 0, diff = 0;
	for (size_t i = 0; i < n; ++i) { peak = std::max(peak, double(std::fabs(a[i]))); diff = std::max(diff, double(std::fabs(a[i] - b[i]))); }
	CHECK(peak > 0.01, "stimulus sounds (peak %.3f, %zu samples)", peak, a.size());
	CHECK(b.size() + 2 * board::block_frames >= a.size() && b.size() <= a.size(), "board received all but the last open block (%zu of %zu samples)", b.size(), a.size());
	CHECK(diff == 0.0, "board path through the same voice model is identical to the software backend (max difference %g)", diff);
	CHECK(hw.audio().size() == a.size(), "board mode keeps the host's output clock (%zu silent samples)", hw.audio().size());
	CHECK(board.frames_contiguous && board.rd.dropped() == 0, "%u blocks, contiguous frame numbers, no dropped bytes", board.blocks);

	// a CV burst larger than one block's table is split over several packets
	{
		software_backend burst(ram.data(), ram.size(), 48000.0);
		model_board bb;
		burst.use_board(&bb);
		for (unsigned k = 0; k < 600; ++k) burst.cv_write(100, k % 5, k % 16, uint16_t(k * 4));
		burst.render_until(20000);   // ~96 frames
		CHECK(bb.cv_events == 80 + 600 && bb.next_frame >= 96, "600-word CV burst delivered (%u events incl. 80 initial, %llu frames)", bb.cv_events, (unsigned long long)bb.next_frame);
	}

	// board -> host packets, with garbage in between
	{
		std::vector<uint8_t> stream, pkt;
		board::status s; s.played = 0x12345678; s.capacity = 1024; s.underruns = 3; s.flags = board::RAIL_OK;
		stream = { 0x00, 0xE3, 0x17 };
		board::encode_status(pkt, s); stream.insert(stream.end(), pkt.begin(), pkt.end());
		uint8_t const m[] = { 0x90, 60, 100 };
		board::encode_midi(pkt, m, 3); stream.insert(stream.end(), pkt.begin(), pkt.end());
		stream.push_back(0xE3);
		board::encode_inputs(pkt, 2048, 2); stream.insert(stream.end(), pkt.begin(), pkt.end());
		board::encode_control(pkt, board::UNMUTE); stream.insert(stream.end(), pkt.begin(), pkt.end());
		capture c; board::reader rd;
		for (uint8_t byte : stream) rd.push(&byte, 1, c);
		CHECK(c.st.played == 0x12345678 && c.st.capacity == 1024 && c.st.underruns == 3 && c.st.flags == board::RAIL_OK, "status round trip");
		CHECK(c.midi == std::vector<uint8_t>(m, m + 3) && c.pedal == 2048 && c.sw == 2 && c.controls == board::UNMUTE, "MIDI, inputs and control round trip");
		CHECK(rd.dropped() == 4, "resynchronised after %llu garbage bytes", (unsigned long long)rd.dropped());
	}
	printf("%s\n", fails ? "FAILED" : "all board link tests passed");
	return fails ? 1 : 0;
}
