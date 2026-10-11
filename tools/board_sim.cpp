// Stand-in for the analog voice board's Teensy, for testing the board link without hardware (POSIX).
// Listens on a Unix socket that eiii-standalone --board uses like the Teensy's USB serial port, plays the received frames
// at 96 kHz on the wall clock with a fixed buffer (reporting status every millisecond as the firmware will), and runs
// them through the software CEM3387 model in place of the voice cells, written to a WAV file.
//   board_sim [--link PATH] [--wav FILE] [--buffer FRAMES] [--midi T:HEX[,T:HEX...]]
//     --link    socket path (default /tmp/eiii-board)
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
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using namespace eiii;
using clk = std::chrono::steady_clock;

static volatile std::sig_atomic_t g_stop = 0;

// the voice cells: the CEM3387 model on its own thread, so modelling never delays reading the link or the clock
struct voice_cells
{
	struct block { std::vector<board::cv_event> cv; std::vector<int16_t> pcm; unsigned frames; };
	cem3387_bank cem;
	FILE *wav = nullptr;
	uint64_t wav_frames = 0;
	double accl = 0, accr = 0;
	unsigned k = 0;
	std::mutex mx;
	std::condition_variable cv_ready;
	std::deque<block> queue;
	bool done = false;
	std::thread worker;
	voice_cells() { cem.prepare(board::frame_rate); worker = std::thread([this] { run(); }); }
	void push(block &&b) { { std::lock_guard<std::mutex> l(mx); queue.push_back(std::move(b)); } cv_ready.notify_one(); }
	void finish() { { std::lock_guard<std::mutex> l(mx); done = true; } cv_ready.notify_one(); worker.join(); }
	void apply(board::cv_event const &e) { cem.cv_write(e.param, 15 - e.voice, e.word); }
	void run()
	{
		for (;;)
		{
			block b;
			{
				std::unique_lock<std::mutex> l(mx);
				cv_ready.wait(l, [&] { return done || !queue.empty(); });
				if (queue.empty()) return;
				b = std::move(queue.front()); queue.pop_front();
			}
			unsigned e = 0;
			for (unsigned f = 0; f < b.frames; ++f)
			{
				for (; e < b.cv.size() && b.cv[e].offset == f; ++e) apply(b.cv[e]);
				double l, r;
				cem.process(b.pcm.data() + f * board::voices, l, r);
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
			for (; e < b.cv.size(); ++e) apply(b.cv[e]);
		}
	}
};

struct sim : board::reader::handler
{
	voice_cells cells;
	uint64_t frames = 0, cv_words = 0, blocks = 0, gaps = 0;
	uint32_t next_first = 0;
	bool started = false, muted = true;
	void on_block(uint32_t first, unsigned n, board::cv_event const *cv, unsigned n_cv, int16_t const *pcm) override
	{
		if (blocks++ && first != next_first) ++gaps;
		next_first = first + n;
		cells.push({ std::vector<board::cv_event>(cv, cv + n_cv), std::vector<int16_t>(pcm, pcm + n * board::voices), n });
		cv_words += n_cv;
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

	// a Unix socket rather than a pty: the tty layer hands data on through a kernel work queue, which on a busy machine
	// held it back for tens of milliseconds and made the simulated board underrun
	int const ls = socket(AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (link.size() >= sizeof addr.sun_path) { fprintf(stderr, "board_sim: link path too long\n"); return 1; }
	strcpy(addr.sun_path, link.c_str());
	unlink(link.c_str());
	if (ls < 0 || bind(ls, (sockaddr *)&addr, sizeof addr) || listen(ls, 1)) { perror(link.c_str()); return 1; }
	fprintf(stderr, "board_sim: waiting on %s, buffer %u frames (%.1f ms)\n", link.c_str(), capacity, capacity / 96.0);
	int const fd = accept(ls, nullptr, nullptr);
	if (fd < 0) { perror("board_sim: accept"); unlink(link.c_str()); return 1; }
	fcntl(fd, F_SETFL, O_NONBLOCK);
	std::signal(SIGINT, [](int) { g_stop = 1; });
	std::signal(SIGTERM, [](int) { g_stop = 1; });
	std::signal(SIGPIPE, SIG_IGN);   // the host closing the link ends the run, not the process

	sim s;
	if (!wavfile.empty()) { s.cells.wav = fopen(wavfile.c_str(), "wb"); uint8_t hdr[44] = {}; fwrite(hdr, 1, 44, s.cells.wav); }
	board::reader rd;
	board::status st;
	st.capacity = uint16_t(capacity);
	st.flags = board::RAIL_OK;
	std::vector<uint8_t> pkt;
	uint64_t played = 0, max_queue = 0;
	size_t next_midi = 0;
	bool connected = false, running = false, hangup = false;
	clk::time_point t0;
	auto tick = clk::now();
	std::vector<double> under_at;             // stream time of the first underruns
	double max_silence = 0;                   // longest time without data while playing
	clk::time_point last_data = clk::now();
	auto receive = [&]() {
		for (;;)
		{
			uint8_t buf[65536];
			ssize_t const n = read(fd, buf, sizeof buf);
			if (n == 0) hangup = true;
			if (n <= 0) break;
			rd.push(buf, size_t(n), s); connected = true;
			auto const now = clk::now();
			if (running) max_silence = std::max(max_silence, std::chrono::duration<double>(now - last_data).count());
			last_data = now;
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
			if (due > s.frames) { if (!(s.started && s.muted) && (played < s.frames || due - s.frames > board::block_frames)) { ++st.underruns; if (under_at.size() < 12) under_at.push_back(double(s.frames) / board::frame_rate); } played = s.frames; t0 = clk::now() - std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(played / board::frame_rate)); }
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
		if (hangup) break;
		// keep reading until the next tick (the socket buffer holds only a few milliseconds of lanes)
		while (!g_stop && !hangup)
		{
			auto const left = tick - clk::now();
			if (left <= clk::duration::zero()) break;
			timespec ts{ 0, long(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count()) };
			pollfd p{ fd, POLLIN, 0 };
			if (ppoll(&p, 1, &ts, nullptr) > 0) receive();
		}
	}
	unlink(link.c_str());
	s.cells.finish();
	if (!under_at.empty())
	{
		fprintf(stderr, "board_sim: first underruns at stream time (s):");
		for (double t : under_at) fprintf(stderr, " %.3f", t);
		fprintf(stderr, "\n");
	}
	fprintf(stderr, "board_sim: longest wait for data while playing %.1f ms\n", max_silence * 1e3);
	if (s.cells.wav) finish_wav(s.cells.wav, s.cells.wav_frames);
	fprintf(stderr, "board_sim: %llu frames (%.3f s) in %llu blocks, %llu CV words, %u underruns, %llu frame gaps, deepest queue %llu frames, %llu bad bytes%s%s\n",
		(unsigned long long)s.frames, s.frames / board::frame_rate, (unsigned long long)s.blocks, (unsigned long long)s.cv_words, st.underruns,
		(unsigned long long)s.gaps, (unsigned long long)max_queue, (unsigned long long)rd.dropped(), wavfile.empty() ? "" : ", wrote ", wavfile.c_str());
	return s.gaps || rd.dropped() ? 1 : 0;
}
