// Analog voice board firmware logic (firmware/teensy41/src/board_logic) against the board's contracts, with a
// recording stand-in for the hardware: power-up order (mute, rails, buses, /CLR release, prime every DAC8420 before the
// first LD, four rounds), CV words reaching the right chip / channel / code only when their frame plays, the stream
// buffering and underruns, unmuting only once safe, and muting first on a rail fault.
#include "../firmware/teensy41/src/board_logic.h"
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); ++fails; } printf("\n"); } while (0)

namespace {
struct op { std::string what; unsigned a = 0, b = 0; };

struct recorder : fw::hal
{
	uint32_t now = 0;
	bool rails = false, relay = false, bus = false, clear = true, codecs = false;
	std::vector<op> ops;
	uint32_t millis() override { return now; }
	bool rail_ok() override { return rails; }
	void set_relay(bool on) override { relay = on; ops.push_back({ on ? "relay on" : "relay off" }); }
	void set_bus(bool e) override { bus = e; ops.push_back({ e ? "bus on" : "bus off" }); }
	void set_clear(bool a) override { clear = a; ops.push_back({ a ? "clear" : "release" }); }
	void dac_word(unsigned chip, uint16_t w) override { ops.push_back({ "word", chip, w }); }
	void dac_load() override { ops.push_back({ "load" }); }
	void codecs_start() override { codecs = true; ops.push_back({ "codecs on" }); }
	void codecs_stop() override { codecs = false; ops.push_back({ "codecs off" }); }
	void irq_off() override {}
	void irq_on() override {}
	size_t find(std::string const &w, size_t from = 0) const { for (size_t i = from; i < ops.size(); ++i) if (ops[i].what == w) return i; return ops.size(); }
	size_t count(std::string const &w) const { size_t n = 0; for (auto const &o : ops) n += o.what == w; return n; }
};

void send(fw::board_logic &b, std::vector<uint8_t> const &pkt) { b.receive(pkt.data(), pkt.size()); }
}

int main()
{
	recorder h;
	fw::board_logic b(h);

	// DAC map against kicad_sm/docs/dac_cv_map.csv rows
	CHECK(fw::dac_chip(0, 0) == 3 && fw::dac_channel(0) == 3 && fw::dac_chip(10, 0) == 1 && fw::dac_channel(10) == 1 &&
		fw::dac_chip(11, 4) == 17 && fw::dac_channel(11) == 0 && fw::dac_chip(4, 1) == 6, "DAC map: voice 0 Fc1 = chip 3 D, voice 10 Fc1 = chip 1 B, voice 11 Pan = chip 17 A");
	CHECK(fw::dac_data(2, 0x3994) == (2 << 14 | 0xE65), "DAC data word: address bits + CV word bits 13..2");

	// rails not yet valid: muted, gated, cleared, nothing written
	for (int i = 0; i < 50; ++i) { h.now++; b.poll(); }
	CHECK(!h.relay && !h.bus && h.clear && h.count("word") == 0, "waiting for RAIL_OK: muted, buses gated, /CLR held, no DAC writes");

	h.rails = true;
	h.now++; b.poll();
	size_t const bus_on = h.find("bus on"), release = h.find("release"), first_word = h.find("word"), first_load = h.find("load");
	CHECK(bus_on < release && release < first_word && first_word < first_load, "buses enabled, then /CLR released, then DAC words");
	{
		bool every_chip = true;
		std::vector<bool> seen(20);
		for (size_t i = first_word; i < first_load; ++i) seen[h.ops[i].a] = true;
		for (bool s : seen) every_chip = every_chip && s;
		CHECK(every_chip, "every chip has a word before the first LD (prime)");
	}
	CHECK(h.count("load") == 4 && h.count("word") == 80, "start-up: four rounds, 80 words (%zu loads, %zu words)", h.count("load"), h.count("word"));
	bool startup_ok = true;
	for (auto const &o : h.ops)
		if (o.what == "word")
		{
			unsigned const ch = o.b >> 14, p = o.a / 4, v = (3 - o.a % 4) * 4 + (3 - ch);
			startup_ok = startup_ok && fw::dac_chip(v, p) == o.a && (o.b & 0xfff) == ((fw::startup_word[p] >> 2) & 0xfff);
		}
	CHECK(startup_ok, "start-up codes: filters open, no resonance, gain closed, pan centre");

	// codecs after the settle time, still muted
	for (int i = 0; i < 29; ++i) { h.now++; b.poll(); }
	CHECK(!h.codecs, "codecs wait for the 30 ms settle");
	for (int i = 0; i < 30; ++i) { h.now++; b.poll(); }
	CHECK(h.codecs && b.current() == fw::board_logic::state::ready && !h.relay, "codecs started, board ready, still muted");

	// host stream: START, then blocks with lane values and a CV word
	std::vector<uint8_t> pkt;
	eiii::board::encode_control(pkt, eiii::board::START); send(b, pkt);
	eiii::board::block_writer w; w.reset(0);
	int16_t frame[16];
	uint32_t sent = 0;
	auto send_frames = [&](unsigned n, int cv_at = -1) {
		for (unsigned f = 0; f < n; ++f, ++sent)
		{
			if (int(sent) == cv_at) w.cv(5, 3, 0x2a28);                          // voice 5 gain
			for (unsigned v = 0; v < 16; ++v) frame[v] = int16_t(sent * 16 + v);
			w.frame(frame);
			if (w.full()) { w.finish(pkt); send(b, pkt); }
		}
	};
	send_frames(960, 900);
	h.now++; b.poll();
	CHECK(!h.relay, "stream buffered (960 frames) but not yet playing: muted");
	size_t const before = h.ops.size();
	int16_t out[32 * 16];
	b.render(out, 32);
	bool zeros = true; for (int16_t s : out) zeros = zeros && s == 0;
	CHECK(zeros && b.status().played == 0, "before playback starts the codecs get silence");
	send_frames(96);
	h.now++; b.poll();                                       // >= capacity / 2 buffered: playback starts
	bool order_ok = true;
	uint32_t played = 0;
	size_t pre900 = 0;                                       // ops recorded before frame 900 (the CV's) played
	for (int ms = 0; ms < 40; ++ms)
	{
		for (int k = 0; k < 3; ++k)
		{
			if (played <= 900) pre900 = h.ops.size();
			b.render(out, 32);
			for (unsigned f = 0; f < 32; ++f, ++played)
				for (unsigned v = 0; v < 16; ++v) order_ok = order_ok && out[f * 16 + v] == int16_t(played * 16 + v);
		}
		send_frames(96);
		h.now++; b.poll();
		if (ms == 5) CHECK(!h.relay, "playing, CVs still settling: muted");
	}
	CHECK(order_ok, "lane frames reach the codecs in order (%u frames)", played);
	CHECK(h.relay, "unmuted once playing for the settle time");
	{
		size_t gw = h.ops.size();
		for (size_t i = before; i < h.ops.size(); ++i)
			if (h.ops[i].what == "word" && h.ops[i].a == fw::dac_chip(5, 3) && h.ops[i].b == fw::dac_data(fw::dac_channel(5), 0x2a28)) { gw = i; break; }
		CHECK(gw < h.ops.size() && h.find("load", gw) < h.ops.size(), "CV word for voice 5 gain written to chip %u channel %c, then latched", fw::dac_chip(5, 3), "ABCD"[fw::dac_channel(5)]);
		CHECK(gw >= pre900, "written only after its frame played");
		size_t words = 0; for (size_t i = before; i < h.ops.size(); ++i) words += h.ops[i].what == "word";
		CHECK(words == 1, "only the changed channel is rewritten (%zu words)", words);
	}
	auto s = b.status();
	CHECK(s.played == played && s.capacity == fw::board_logic::capacity - 96 && s.flags == eiii::board::RAIL_OK && s.underruns == 0,
		"status: %u played, capacity %u, rails ok, unmuted, no underruns", s.played, s.capacity);

	// host stops sending: one underrun, silence
	for (int k = 0; k < 60; ++k) b.render(out, 32);
	zeros = true; for (int16_t x : out) zeros = zeros && x == 0;
	CHECK(zeros && b.status().underruns == 1, "buffer runs dry: silence, one underrun counted (%u)", b.status().underruns);

	// rail fault: mute first, then nothing written until the rails return; frame numbering continues
	size_t const fault_at = h.ops.size();
	h.rails = false;
	h.now++; b.poll();
	CHECK(fault_at < h.ops.size() && h.ops[fault_at].what == "relay off" && !h.bus && h.clear && !h.codecs && b.faults() == 1,
		"rail fault: relay off first, codecs stopped, buses gated, DACs cleared");
	size_t const words_at_fault = h.count("word");
	for (int i = 0; i < 20; ++i) { h.now++; b.poll(); }
	CHECK(h.count("word") == words_at_fault, "no DAC writes while the rails are out");
	h.rails = true;
	for (int i = 0; i < 80; ++i) { h.now++; b.poll(); }
	CHECK(h.count("word") == words_at_fault + 80 && b.current() == fw::board_logic::state::ready && b.status().played == sent,
		"rails back: primed again, ready, played count continues at the host's frame (%u)", b.status().played);

	eiii::board::encode_control(pkt, eiii::board::MUTE); send(b, pkt);
	CHECK(!h.relay && (b.status().flags & eiii::board::MUTED), "host MUTE: relay off");
	printf("%s\n", fails ? "FAILED" : "all board firmware tests passed");
	return fails ? 1 : 0;
}
