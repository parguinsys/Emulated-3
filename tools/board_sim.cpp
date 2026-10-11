// Stand-in for the analog voice board's Teensy, for testing the board link without hardware (POSIX).
// Opens a pseudo-terminal that eiii-standalone --board uses like the Teensy's USB serial port, plays the received frames
// at 96 kHz on the wall clock with a fixed buffer (reporting status every millisecond as the firmware will), and runs
// them through the software CEM3387 model in place of the voice cells, written to a WAV file.
//   board_sim [--link PATH] [--wav FILE] [--buffer FRAMES] [--midi T:HEX[,T:HEX...]]
//     --link    symlink to the pty (default /tmp/eiii-board)
//     --midi    MIDI IN bytes sent to the host T seconds after the stream starts (e.g. 1.0:903C64,1.5:803C40)
// Exits when the host closes the link; prints frames, CV words and underruns.
#include "../host/eiii_board_link.h"
#include "../cem/eiii_cem3387.h"
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

using namespace eiii;
using clk = std::chrono::steady_clock;

static volatile std::sig_atomic_t g_stop = 0;

struct sim : board::reader::handler
{
	cem3387_bank cem;
	FILE *wav = nullptr;
	uint64_t wav_frames = 0, frames = 0, cv_words = 0, blocks = 0, gaps = 0;
	uint32_t next_first = 0;
	double accl = 0, accr = 0;
	unsigned k = 0;
	bool started = false, muted = true;
	sim() { cem.prepare(board::frame_rate); }
	void apply(board::cv_event const &e) { cem.cv_write(e.param, 15 - e.voice, e.word); ++cv_words; }
	void on_block(uint32_t first, unsigned n, board::cv_event const *cv, unsigned n_cv, int16_t const *pcm) override
	{
		if (blocks++ && first != next_first) ++gaps;
		next_first = first + n;
		unsigned e = 0;
		for (unsigned f = 0; f < n; ++f)
		{
			for (; e < n_cv && cv[e].offset == f; ++e) apply(cv[e]);
			double l, r;
			cem.process(pcm + f * board::voices, l, r);
			accl += l; accr += r;
			if (++k == 2)
			{
				if (wav)
				{
					int16_t s[2];
					for (int c = 0; c < 2; ++c) { long v = lround((c ? accr : accl) / 2 / 10.0 * 32767.0); s[c] = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
					fwrite(s, 2, 2, wav); ++wav_frames;
				}
				accl = accr = 0; k = 0;
			}
		}
		for (; e < n_cv; ++e) apply(cv[e]);
		frames += n;
	}
	void on_control(board::control_command c) override
	{
		if (c == board::START) started = true;
		if (c == board::MUTE) muted = true;
		if (c == board::UNMUTE) muted = false;
	}
};

static void finish_wav(FILE *f, uint64_t frames)
{
	uint32_t const bytes = uint32_t(frames * 4);
	fseek(f, 0, SEEK_SET);
	auto p32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); }; auto p16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
	fwrite("RIFF", 1, 4, f); p32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f); p32(16); p16(1); p16(2); p32(48000); p32(48000 * 4); p16(4); p16(16);
	fwrite("data", 1, 4, f); p32(bytes);
	fclose(f);
}

int main(int argc, char **argv)
{
	std::string link = "/tmp/eiii-board", wavfile;
	unsigned capacity = 1024;
	std::vector<std::pair<double, std::vector<uint8_t>>> midi;
	for (int i = 1; i < argc; ++i)
	{
		std::string const a = argv[i];
		auto val = [&]() -> std::string { if (i + 1 >= argc) { fprintf(stderr, "board_sim: %s needs a value\n", a.c_str()); exit(2); } return argv[++i]; };
		if (a == "--link") link = val();
		else if (a == "--wav") wavfile = val();
		else if (a == "--buffer") capacity = unsigned(atoi(val().c_str()));
		else if (a == "--midi")
		{
			std::string const list = val();
			for (size_t p = 0; p < list.size();)
			{
				size_t const comma = std::min(list.find(',', p), list.size()), colon = list.find(':', p);
				std::vector<uint8_t> bytes;
				for (size_t h = colon + 1; h + 2 <= comma; h += 2) bytes.push_back(uint8_t(strtoul(list.substr(h, 2).c_str(), nullptr, 16)));
				midi.emplace_back(atof(list.substr(p, colon - p).c_str()), bytes);
				p = comma + 1;
			}
		}
		else { fprintf(stderr, "usage: board_sim [--link PATH] [--wav FILE] [--buffer FRAMES] [--midi T:HEX,...]\n"); return 2; }
	}
	capacity = std::min(capacity, 65535u);

	int const fd = posix_openpt(O_RDWR | O_NOCTTY);
	if (fd < 0 || grantpt(fd) || unlockpt(fd)) { perror("board_sim: pty"); return 1; }
	char const *slave = ptsname(fd);
	int keep = open(slave, O_RDWR | O_NOCTTY);   // held open so the pty survives until the host connects
	termios tio; tcgetattr(keep, &tio); cfmakeraw(&tio); tcsetattr(keep, TCSANOW, &tio);
	fcntl(fd, F_SETFL, O_NONBLOCK);
	unlink(link.c_str());
	if (symlink(slave, link.c_str())) { perror(link.c_str()); return 1; }
	fprintf(stderr, "board_sim: link %s -> %s, buffer %u frames (%.1f ms)\n", link.c_str(), slave, capacity, capacity / 96.0);
	std::signal(SIGINT, [](int) { g_stop = 1; });
	std::signal(SIGTERM, [](int) { g_stop = 1; });

	sim s;
	if (!wavfile.empty()) { s.wav = fopen(wavfile.c_str(), "wb"); uint8_t hdr[44] = {}; fwrite(hdr, 1, 44, s.wav); }
	board::reader rd;
	board::status st;
	st.capacity = uint16_t(capacity);
	st.flags = board::RAIL_OK;
	std::vector<uint8_t> pkt;
	uint64_t played = 0, max_queue = 0;
	size_t next_midi = 0;
	bool connected = false, running = false;
	clk::time_point t0;
	auto tick = clk::now();
	auto receive = [&]() {
		for (;;)
		{
			uint8_t buf[65536];
			ssize_t const n = read(fd, buf, sizeof buf);
			if (n <= 0) break;
			rd.push(buf, size_t(n), s); connected = true;
		}
	};
	while (!g_stop)
	{
		tick += std::chrono::milliseconds(1);
		receive();
		// the codec clock: once the buffer is half full, play 96 frames per millisecond; an empty buffer when frames are
		// due is an underrun
		if (!running && s.frames >= capacity / 2) { running = true; t0 = clk::now(); }
		if (running)
		{
			uint64_t const due = uint64_t(std::chrono::duration<double>(clk::now() - t0).count() * board::frame_rate);
			if (due > s.frames) { if (played < s.frames || due - s.frames > board::block_frames) ++st.underruns; played = s.frames; t0 = clk::now() - std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(played / board::frame_rate)); }
			else played = due;
			max_queue = std::max(max_queue, s.frames - played);
			double const t = double(played) / board::frame_rate;
			for (; next_midi < midi.size() && midi[next_midi].first <= t; ++next_midi)
			{
				board::encode_midi(pkt, midi[next_midi].second.data(), midi[next_midi].second.size());
				if (write(fd, pkt.data(), pkt.size()) < 0) {}
			}
		}
		st.played = uint32_t(played);
		board::encode_status(pkt, st);
		if (write(fd, pkt.data(), pkt.size()) < 0) {}
		// host gone: the slave side hangs up once the host closes and we drop our own handle
		if (connected && keep >= 0) { close(keep); keep = -1; }
		pollfd p{ fd, POLLIN, 0 };
		if (connected && poll(&p, 1, 0) > 0 && (p.revents & POLLHUP)) break;
		// keep reading until the next tick: a pty holds only a few kB, so reading once per millisecond would throttle the
		// host below the 3 MB/s the lanes need
		while (!g_stop)
		{
			auto const left = tick - clk::now();
			if (left <= clk::duration::zero()) break;
			timespec ts{ 0, long(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count()) };
			pollfd p{ fd, POLLIN, 0 };
			if (ppoll(&p, 1, &ts, nullptr) > 0) { if (p.revents & POLLHUP) break; receive(); }
		}
	}
	unlink(link.c_str());
	if (s.wav) finish_wav(s.wav, s.wav_frames);
	fprintf(stderr, "board_sim: %llu frames (%.3f s) in %llu blocks, %llu CV words, %u underruns, %llu frame gaps, deepest queue %llu frames, %llu bad bytes%s%s\n",
		(unsigned long long)s.frames, s.frames / board::frame_rate, (unsigned long long)s.blocks, (unsigned long long)s.cv_words, st.underruns,
		(unsigned long long)s.gaps, (unsigned long long)max_queue, (unsigned long long)rd.dropped(), wavfile.empty() ? "" : ", wrote ", wavfile.c_str());
	return s.gaps || rd.dropped() ? 1 : 0;
}
