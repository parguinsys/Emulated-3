// latency_bench: how late a note sounds, and whether the audio holds up, at a given host block size.
//   latency_bench PLUGIN.clap [--block N] [--seconds S] [--chord K] [--every S]
// One headless instance (no GUI), activated with --block as both the block size and the largest block, driven by a
// paced audio thread like a DAW's.  The main thread runs the plugin's timers, so autoload_bank / autoload_preset
// (eiii.conf or EIII_AUTOLOAD_*) load a bank.  Every --every seconds a K-note chord on channel 14 is held for 1 s.
// For each chord that sounds, the note-to-sound delay is the time from the note-on to the first output sample above
// the level before it; it includes the reported latency and the emulated OS's own response time.  The plugin's
// EIII_RACK_REPORT line gives the engine's underruns and load.  Media and the latency margin come from the plugin's
// own configuration (EIII_LATENCY_MARGIN overrides).
#include "clap/clap.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct timer_rec { uint32_t period_ms; std::chrono::steady_clock::time_point next; };
std::map<clap_id, timer_rec> g_timers;
std::vector<int> g_fds;
clap_id g_next_timer = 1;

bool h_register_timer(clap_host_t const *, uint32_t ms, clap_id *id)
{
	*id = g_next_timer++;
	g_timers[*id] = { std::max<uint32_t>(ms, 1), std::chrono::steady_clock::now() };
	return true;
}
bool h_unregister_timer(clap_host_t const *, clap_id id) { return g_timers.erase(id) > 0; }
clap_host_timer_support_t const k_timer = { h_register_timer, h_unregister_timer };
bool h_register_fd(clap_host_t const *, int fd, clap_posix_fd_flags_t) { g_fds.push_back(fd); return true; }
bool h_modify_fd(clap_host_t const *, int, clap_posix_fd_flags_t) { return true; }
bool h_unregister_fd(clap_host_t const *, int fd) { g_fds.erase(std::remove(g_fds.begin(), g_fds.end(), fd), g_fds.end()); return true; }
clap_host_posix_fd_support_t const k_fd = { h_register_fd, h_modify_fd, h_unregister_fd };
void h_latency_changed(clap_host_t const *) { }
clap_host_latency_t const k_latency = { h_latency_changed };
void h_mark_dirty(clap_host_t const *) { }
clap_host_state_t const k_state = { h_mark_dirty };
void const *h_get_extension(clap_host_t const *, char const *id)
{
	if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &k_timer;
	if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &k_fd;
	if (!strcmp(id, CLAP_EXT_LATENCY)) return &k_latency;
	if (!strcmp(id, CLAP_EXT_STATE)) return &k_state;
	return nullptr;
}
void h_noop(clap_host_t const *) { }

struct event_list { std::vector<clap_event_midi_t> ev; };
uint32_t ev_size(clap_input_events_t const *l) { return uint32_t(static_cast<event_list *>(l->ctx)->ev.size()); }
clap_event_header_t const *ev_get(clap_input_events_t const *l, uint32_t i) { return &static_cast<event_list *>(l->ctx)->ev[i].header; }
bool out_try_push(clap_output_events_t const *, clap_event_header_t const *) { return true; }

double percentile(std::vector<double> v, double p)
{
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
}

} // namespace

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (argc < 2) { fprintf(stderr, "usage: %s PLUGIN.clap [--block N] [--seconds S] [--chord K] [--every S]\n", argv[0]); return 2; }
	uint32_t block = 256; double seconds = 90, every = 3; int chord = 8;
	double const rate = 48000;
	for (int i = 2; i < argc; ++i)
	{
		std::string const a = argv[i];
		if (a == "--block" && i + 1 < argc) block = uint32_t(atoi(argv[++i]));
		else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
		else if (a == "--chord" && i + 1 < argc) chord = std::clamp(atoi(argv[++i]), 1, 16);
		else if (a == "--every" && i + 1 < argc) every = std::max(1.5, atof(argv[++i]));
	}
	char report[] = "/tmp/eiii_latency_bench_XXXXXX";
	int const rfd = mkstemp(report);
	if (rfd < 0) { perror("mkstemp"); return 1; }
	close(rfd);
	setenv("EIII_RACK_REPORT", report, 1);

	void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
	auto const *entry = static_cast<clap_plugin_entry_t const *>(dlsym(lib, "clap_entry"));
	if (!entry || !entry->init(argv[1])) { fprintf(stderr, "no clap_entry\n"); return 1; }
	auto const *fac = static_cast<clap_plugin_factory_t const *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	clap_plugin_descriptor_t const *desc = fac->get_plugin_descriptor(fac, 0);
	clap_host_t host{ CLAP_VERSION_INIT, nullptr, "eiii latency_bench", "eiii-core", "", "1.0", h_get_extension, h_noop, h_noop, h_noop };
	clap_plugin_t const *p = fac->create_plugin(fac, &host, desc->id);
	if (!p || !p->init(p)) { fprintf(stderr, "create/init failed\n"); return 1; }
	auto const *tim = static_cast<clap_plugin_timer_support_t const *>(p->get_extension(p, CLAP_EXT_TIMER_SUPPORT));
	auto const *fdx = static_cast<clap_plugin_posix_fd_support_t const *>(p->get_extension(p, CLAP_EXT_POSIX_FD_SUPPORT));
	auto const *lat = static_cast<clap_plugin_latency_t const *>(p->get_extension(p, CLAP_EXT_LATENCY));
	if (!p->activate(p, rate, 1, block)) { fprintf(stderr, "activate failed\n"); return 1; }
	unsigned const latency = lat ? lat->get(p) : 0;
	printf("block %u frames (%.2f ms), reported latency %u frames (%.2f ms), chord of %d every %.1f s, %.0f s\n",
		block, block * 1e3 / rate, latency, latency * 1e3 / rate, chord, every, seconds);

	std::atomic<bool> run{true};
	std::vector<double> delays;
	int64_t first_sound = -1;                               // frame of the first chord that sounded
	uint64_t late_blocks = 0, blocks = 0, chords = 0;
	double worst_process = 0;
	std::thread audio([&] {
		sched_param sp{}; sp.sched_priority = 20;
		bool const rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
		printf("audio thread: %s\n", rt ? "SCHED_FIFO 20" : "normal priority (no realtime permission)");
		p->start_processing(p);
		std::vector<float> l(block), r(block), il(block, 0.f), ir(block, 0.f);
		float *chans[2] = { l.data(), r.data() }, *ichans[2] = { il.data(), ir.data() };
		clap_audio_buffer_t out{ chans, nullptr, 2, 0, 0 }, inb{ ichans, nullptr, 2, 0, 0 };
		event_list el; clap_input_events_t in{ &el, ev_size, ev_get };
		clap_output_events_t oe{ nullptr, out_try_push };
		clap_process_t pr{}; pr.frames_count = block; pr.audio_outputs = &out; pr.audio_outputs_count = 1;
		pr.audio_inputs = &inb; pr.audio_inputs_count = 1; pr.in_events = &in; pr.out_events = &oe;
		uint64_t const period = uint64_t(every * rate), hold = uint64_t(rate);
		uint64_t const window = uint64_t(0.5 * rate);       // onset search after a note-on
		int64_t note_on_at = -1; float pre_peak = 0;          // pre_peak: level in the 20 ms before the note-on
		std::vector<float> recent; size_t const pre_n = size_t(0.02 * rate);
		auto next = std::chrono::steady_clock::now();
		for (uint64_t blk = 0; run; ++blk)
		{
			el.ev.clear();
			uint64_t const f0 = blk * block;
			for (uint32_t k = 0; k < block; ++k)
			{
				uint64_t const f = f0 + k;
				if (f < period || (f % period != 0 && f % period != hold)) continue;
				bool const on = f % period == 0;
				for (int n = 0; n < chord; ++n)
				{
					clap_event_midi_t e{}; e.header = { sizeof e, k, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
					e.data[0] = on ? 0x9d : 0x8d; e.data[1] = uint8_t(48 + 3 * n); e.data[2] = on ? 100 : 64;
					el.ev.push_back(e);
				}
				if (on)
				{
					++chords; note_on_at = int64_t(f);
					pre_peak = 0; for (float v : recent) pre_peak = std::max(pre_peak, v);
				}
			}
			auto const t0 = std::chrono::steady_clock::now();
			p->process(p, &pr);
			double const dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			worst_process = std::max(worst_process, dt);
			if (dt > 0.5 * block / rate) ++late_blocks;
			for (uint32_t k = 0; k < block; ++k)
			{
				float const v = std::max(std::fabs(l[k]), std::fabs(r[k]));
				int64_t const f = int64_t(f0 + k);
				if (note_on_at >= 0 && f >= note_on_at)
				{
					if (v > std::max(1e-3f, 4.f * pre_peak)) { delays.push_back(double(f - note_on_at) * 1e3 / rate); if (first_sound < 0) first_sound = f; note_on_at = -1; }
					else if (uint64_t(f - note_on_at) > window) note_on_at = -1;
				}
				recent.push_back(v);
			}
			if (recent.size() > pre_n) recent.erase(recent.begin(), recent.end() - pre_n);
			++blocks;
			pr.steady_time = int64_t(f0 + block);
			next += std::chrono::nanoseconds(int64_t(block * 1e9 / rate));
			std::this_thread::sleep_until(next);
		}
		p->stop_processing(p);
	});

	auto const t_end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	while (std::chrono::steady_clock::now() < t_end)
	{
		std::vector<pollfd> pf; for (int fd : g_fds) pf.push_back({ fd, POLLIN, 0 });
		poll(pf.data(), nfds_t(pf.size()), 5);
		if (fdx) for (auto &x : pf) if (x.revents & POLLIN) fdx->on_fd(p, x.fd, CLAP_POSIX_FD_READ);
		auto const now = std::chrono::steady_clock::now();
		if (tim) for (auto &[id, t] : g_timers) if (now >= t.next) { t.next = now + std::chrono::milliseconds(t.period_ms); tim->on_timer(p, id); }
	}
	run = false;
	audio.join();
	p->deactivate(p);
	p->destroy(p);
	entry->deinit();
	dlclose(lib);

	printf("process(): worst %.3f ms, %llu of %llu blocks over half the block time\n", worst_process * 1e3, (unsigned long long)late_blocks, (unsigned long long)blocks);
	if (delays.empty()) printf("note-to-sound: no chord sounded (%llu played): no bank loaded?\n", (unsigned long long)chords);
	else printf("note-to-sound: %zu of %llu chords, min %.1f ms, median %.1f ms, max %.1f ms (reported latency %.1f ms of it); first sound at frame %lld\n",
		delays.size(), (unsigned long long)chords, percentile(delays, 0), percentile(delays, 0.5), percentile(delays, 1), latency * 1e3 / rate, (long long)first_sound);
	if (FILE *f = fopen(report, "r")) { char b[1024]; while (fgets(b, sizeof b, f)) printf("engine: %s", b); fclose(f); }
	unlink(report);
	return 0;
}
