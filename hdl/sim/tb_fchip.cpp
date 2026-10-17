// Verilator testbench for hdl/fchip: the HDL lane bank against eiii::fchip_bank (fchip/eiii_fchip.cpp), which matches
// MAME bit for bit (tests/fchip_equiv).
//
//   tb_fchip random [seed] [events]   random voice activity; every register word compared after each event and every
//                                     lane's output compared at each 96 kHz output frame (sample fetch port included)
//   tb_fchip TRACE SOUND.bin          replay a MAME F-Chip trace (EIII_FCHIP_TRACE, as tests/fchip_equiv) and check
//                                     every read and PCM value against MAME
#include "Vfchip_bank.h"
#include "verilated.h"
#include "eiii_fchip.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {

struct dut
{
	Vfchip_bank m;
	std::vector<uint16_t> const &ram;
	uint64_t cycles = 0;

	explicit dut(std::vector<uint16_t> const &r, size_t words) : ram(r)
	{
		m.mem_words = uint32_t(words);
		m.rst = 1; clock(); clock(); m.rst = 0;
	}
	void clock()
	{
		m.clk = 0; m.eval();
		m.clk = 1; m.eval();
		m.rf_we = m.ctrl_we = m.ld_we = m.frame_req = m.tick = 0;
		++cycles;
	}
	void advance(uint64_t n) { while (n--) { m.tick = 1; clock(); } }
	void rf_write(unsigned v, unsigned slot, uint32_t val) { m.rf_we = 1; m.rf_voice = v; m.rf_slot = slot; m.rf_wdata = val & 0x1ffffff; clock(); }
	void ctrl_write(unsigned v, uint8_t b) { m.ctrl_we = 1; m.ctrl_voice = v; m.ctrl_wdata = b; clock(); }
	uint32_t rf_read(unsigned v, unsigned slot) { m.rd_voice = v; m.rd_slot = slot; m.eval(); return m.rd_data; }
	void load(unsigned v, eiii::fchip_bank::voice_state const &s)
	{
		for (unsigned i = 0; i < 4; ++i) rf_write(v, i, s.rf[i]);
		m.ld_we = 1; m.ld_voice = v; m.ld_phase = s.phase; m.ld_rate = s.rate; m.ld_active = s.active;
		m.ld_ctrl_phase = s.ctrl_phase; m.ld_ctrl_hi = s.ctrl_hi & 15;
		clock();
	}
	// one output frame through the fetch port; the lanes are not ticked meanwhile
	void frame(int16_t out[16])
	{
		m.frame_req = 1; clock();
		bool pending = false; uint32_t addr = 0;
		for (int guard = 0; guard < 1000; ++guard)
		{
			if (pending) { m.mem_valid = 1; m.mem_rdata = ram[addr]; }
			pending = false;
			if (m.mem_req) { pending = true; addr = m.mem_addr; }
			bool const done = m.frame_done;
			if (done) break;
			clock();
			m.mem_valid = 0;
		}
		if (!m.frame_done) { fprintf(stderr, "frame fetch did not finish\n"); exit(1); }
		for (unsigned v = 0; v < 16; ++v) out[v] = int16_t(m.pcm[v]);
	}
};

int run_random(unsigned seed, unsigned events)
{
	std::mt19937 rng(seed);
	auto rnd = [&](uint32_t n) { return uint32_t(rng() % n); };
	size_t const words = 0x380000;                       // smaller than the address space: out-of-range reads give 0
	std::vector<uint16_t> ram(0x400000);
	for (auto &w : ram) w = uint16_t(rng());
	eiii::fchip_bank ref(ram.data(), words);
	dut d(ram, words);

	uint64_t mism = 0, compared = 0, frames = 0, ticks = 0, skipped = 0;
	uint64_t since_frame = 0;
	auto check_regs = [&](unsigned e) {
		for (unsigned v = 0; v < 16; ++v)
			for (unsigned s = 0; s < 4; ++s)
			{
				uint32_t const a = ref.rf_read(v, s), b = d.rf_read(v, s);
				++compared;
				if (a != b && mism++ < 10) fprintf(stderr, "event %u: v%u slot%u model=%07X hdl=%07X\n", e, v, s, a, b);
			}
	};
	auto check_frame = [&](unsigned e) {
		int16_t out[16];
		d.frame(out);
		++frames;
		for (unsigned v = 0; v < 16; ++v)
			if (out[v] != ref.pcm(v) && mism++ < 10) fprintf(stderr, "event %u frame: v%u model=%d hdl=%d\n", e, v, ref.pcm(v), out[v]);
	};
	// a register word: random flags; counts near an end so turnarounds, loops and one-shot ends happen often
	auto word = [&](bool near_end) {
		uint32_t flags = rnd(8) << 22;
		uint32_t data;
		if (near_end) { uint32_t const k = rnd(rnd(4) ? 300 : 20000); data = (flags & (1u << 23)) ? k : 0x3fffff - k; }
		else data = rnd(4) ? rnd(words + 0x20000) : rnd(0x400000);
		return flags | data;
	};

	for (unsigned e = 0; e < events; ++e)
	{
		// time to the next event: often none, sometimes long; output frames every 208 ticks (96 kHz) on the way
		uint64_t gap = rnd(4) == 0 ? 0 : rnd(3) == 0 ? rnd(20000) : rnd(400);
		while (gap)
		{
			uint64_t const n = std::min<uint64_t>(gap, 208 - since_frame);
			ref.advance(n); d.advance(n);
			gap -= n; ticks += n; since_frame += n;
			if (since_frame == 208) { since_frame = 0; check_frame(e); }
		}
		unsigned const v = rnd(16);
		switch (rnd(6))
		{
		case 0: case 1:                                  // start a voice: loop words, address, then the count
		{
			uint32_t const is = word(true), ia = word(false), a = word(false), s = word(true);
			ref.rf_write(v, 1, is); d.rf_write(v, 1, is);
			ref.rf_write(v, 3, ia); d.rf_write(v, 3, ia);
			ref.rf_write(v, 2, a);  d.rf_write(v, 2, a);
			ref.rf_write(v, 0, s);  d.rf_write(v, 0, s);
			break;
		}
		case 2:                                          // one register word
		{
			unsigned const s = rnd(4);
			uint32_t const w = word(s == 0 || s == 1);
			// the second documented difference: restarting an ended lane keeps a chunk-dependent divide count
			if (s == 0 && !ref.state(v).active) { ++skipped; break; }
			ref.rf_write(v, s, w); d.rf_write(v, s, w);
			break;
		}
		case 3: case 4:                                  // rate (divisor) change, both bytes
		{
			uint16_t n = uint16_t(rnd(3) == 0 ? 1 + rnd(16) : 1 + rnd(4095));
			if (rnd(10) == 0) n = 0;
			auto const &st = ref.state(v);
			// the documented difference (fchip_lane.sv): skip jumps the HDL takes one step at a time
			uint8_t const hi = uint8_t(rnd(16) << 4 | n >> 8), lo = uint8_t(n);
			unsigned const realign = unsigned(st.ctrl_hi & 15) << 8 | hi;   // the divisor the realigning byte completes
			if ((n && st.phase + 1 >= 2u * n) || (st.ctrl_phase && realign && st.phase + 1 >= 2u * realign)) { ++skipped; break; }
			if (st.ctrl_phase) { ref.ctrl_write(v, hi); d.ctrl_write(v, hi); }  // realign the byte pair
			ref.ctrl_write(v, hi); d.ctrl_write(v, hi);
			ref.ctrl_write(v, lo); d.ctrl_write(v, lo);
			break;
		}
		case 5:                                          // a lone control byte (the pair realigns later)
		{
			uint8_t const b = uint8_t(rng());
			auto const &st = ref.state(v);
			if (st.ctrl_phase && b && st.phase + 1 >= 2u * (unsigned(st.ctrl_hi & 15) << 8 | b)) { ++skipped; break; }
			ref.ctrl_write(v, b); d.ctrl_write(v, b);
			break;
		}
		}
		check_regs(e);
	}
	printf("random seed %u: %u events, %llu ticks, %llu register words and %llu frames compared, %llu events skipped (documented differences), "
		"mismatches=%llu\n", seed, events, (unsigned long long)ticks, (unsigned long long)compared,
		(unsigned long long)frames, (unsigned long long)skipped, (unsigned long long)mism);
	return mism ? 1 : 0;
}

int run_trace(char const *trace, char const *sound)
{
	FILE *s = fopen(sound, "rb");
	if (!s) { perror(sound); return 2; }
	std::vector<uint16_t> ram(0x400000);
	size_t const words = fread(ram.data(), 2, ram.size(), s);
	fclose(s);
	dut d(ram, words);
	FILE *t = fopen(trace, "r");
	if (!t) { perror(trace); return 2; }
	char line[512];
	unsigned long long now = 0, lines = 0, reads = 0, writes = 0, ctrl = 0, ticks = 0, pcmvals = 0, mism = 0;
	bool init = false;
	while (fgets(line, sizeof line, t))
	{
		++lines;
		unsigned long long tt = 0;
		char const k = line[0];
		if (k == 'S')
		{
			unsigned v, act, cph, chi, rate, rf[4]; unsigned long long ph;
			sscanf(line + 2, "%llu %u %x %x %x %x %llu %u %u %u %u", &tt, &v, &rf[0], &rf[1], &rf[2], &rf[3], &ph, &rate, &act, &cph, &chi);
			eiii::fchip_bank::voice_state st;
			for (int i = 0; i < 4; ++i) st.rf[i] = rf[i];
			st.phase = uint32_t(ph); st.rate = uint16_t(rate); st.active = act; st.ctrl_phase = uint8_t(cph); st.ctrl_hi = uint8_t(chi);
			d.load(v, st);
			now = tt; init = true;
			continue;
		}
		if (!init) continue;
		sscanf(line + 2, "%llu", &tt);
		if (tt < now) { fprintf(stderr, "time went backwards at line %llu\n", lines); return 1; }
		d.advance(tt - now);
		now = tt;
		if (k == 'W') { unsigned v, sl, val; sscanf(line + 2, "%*u %u %u %x", &v, &sl, &val); d.rf_write(v, sl, val); ++writes; }
		else if (k == 'C') { unsigned v, b; sscanf(line + 2, "%*u %u %x", &v, &b); d.ctrl_write(v, uint8_t(b)); ++ctrl; }
		else if (k == 'R')
		{
			unsigned v, sl, val; sscanf(line + 2, "%*u %u %u %x", &v, &sl, &val); ++reads;
			uint32_t const got = d.rf_read(v, sl);
			if (got != val && mism++ < 10) fprintf(stderr, "READ mismatch line %llu v%u slot%u mame=%08X hdl=%08X\n", lines, v, sl, val, got);
		}
		else if (k == 'P')
		{
			++ticks;
			int16_t out[16];
			d.frame(out);
			char *p = line + 2; strtoull(p, &p, 10);
			for (unsigned v = 0; v < 16; ++v)
			{
				long const m = strtol(p, &p, 10); ++pcmvals;
				if (m != out[v] && mism++ < 10) fprintf(stderr, "PCM mismatch line %llu v%u mame=%ld hdl=%d\n", lines, v, m, out[v]);
			}
		}
	}
	fclose(t);
	printf("%s: %llu writes, %llu ctrl bytes, %llu reads, %llu audio ticks (%llu pcm values), %llu cycles, mismatches=%llu\n",
		trace, writes, ctrl, reads, ticks, pcmvals, (unsigned long long)d.cycles, mism);
	return mism ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
	Verilated::commandArgs(argc, argv);
	if (argc >= 2 && !strcmp(argv[1], "random"))
		return run_random(argc > 2 ? unsigned(atoi(argv[2])) : 1, argc > 3 ? unsigned(atoi(argv[3])) : 20000);
	if (argc >= 3) return run_trace(argv[1], argv[2]);
	fprintf(stderr, "usage: %s random [seed] [events] | %s TRACE SOUND.bin\n", argv[0], argv[0]);
	return 2;
}
