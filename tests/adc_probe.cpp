// adc_probe: which ADC board configurations the OS uses on the SAMPLE screens (diagnostics, needs media).
// For each step: the LCD, and the ADC word reads per control-latch value during that step (with a 1 kHz tone at
// half scale on Sample In, then silence).  Used to find what the "5 Setup - VU Meter" screen reads.
#include "panel_rig.h"
#include "runtime/eiii_runtime.h"

using namespace eiii_test;

static void reads(rig &t, uint64_t l[256], uint64_t r[256])
{
	t.e.inspect([l, r](eiii::runtime &rt) { rt.debug_adc_reads(l, r); });
	eiii::media_result res;
	t.wait([&] { return t.e.poll_media_result(res); }, 5.0);
}

static void step(rig &t, char const *what, double secs)
{
	uint64_t l0[256], r0[256], l1[256], r1[256];
	reads(t, l0, r0);
	t.pump(secs);
	reads(t, l1, r1);
	printf("%s\n  LCD [%s] [%s] [%s] [%s]\n  reads:", what, t.row(0).c_str(), t.row(1).c_str(), t.row(2).c_str(), t.row(3).c_str());
	for (unsigned c = 0; c < 256; ++c)
		if (l1[c] != l0[c] || r1[c] != r0[c]) printf(" ctrl %02X L %llu R %llu;", c, (unsigned long long)(l1[c] - l0[c]), (unsigned long long)(r1[c] - r0[c]));
	printf("\n");
	fflush(stdout);
}

int main()
{
	auto cfg = media_config();
	if (cfg.rt.hd_path.empty()) { printf("SKIP: set EIII_ROM_DIR, EIII_HD\n"); return 77; }
	rig t(cfg);
	bool tone = false;
	t.input = [&](uint64_t f0, float *l, float *r, size_t n) {
		for (size_t k = 0; k < n; ++k) { float const v = tone ? float(0.5 * std::sin(2 * M_PI * 1000.0 * double(f0 + k) / 48000.0)) : 0.f; l[k] = v; r[k] = v; }
	};
	if (!t.wait_row(0, "P00 ", 25.0)) { printf("FAIL no main screen\n"); return 1; }
	t.pump(1.0);
	step(t, "main screen, silence", 1.0);
	t.press("sample"); t.wait_text("SAMPLE", 3.0);
	step(t, "SAMPLE module, silence", 1.0);
	tone = true;
	step(t, "SAMPLE module, tone", 1.0);
	t.press("keypad_5"); t.pump(0.5);
	step(t, "SAMPLE 5 (Setup - VU Meter), tone", 2.0);
	tone = false;
	step(t, "SAMPLE 5, silence", 1.0);
	tone = true;
	t.press("enter"); t.pump(0.5);
	step(t, "SAMPLE 5 then Enter, tone", 2.0);
	t.press("enter"); t.pump(0.5);
	step(t, "SAMPLE 5, Enter x2, tone", 2.0);
	return 0;
}
