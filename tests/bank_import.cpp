// Banks converted from EIIIX / ESI-32 discs (runtime/eiii_bank_import, as the plugin does on mount, or
// tools/eiii_bank_import.py) load and play through the original OS.
// EIII_IMPORT_BANKS entries may name the bank instead of its number when the disc is converted here.
// EIII_IMPORT_CD: an EIIIX / ESI-32 disc (converted here first, to EIII_IMPORT_OUT or /tmp) or an already converted
// image, mounted as D3.  EIII_IMPORT_BANKS: "NN:Preset name,NN:Preset name,..." -
// each bank number is loaded with Load Bank, its selected preset must be shown on the main screen (an empty name:
// any preset), and notes on all MIDI channels must sound.
#include "panel_rig.h"
#include "runtime/eiii_bank_import.h"
#include "runtime/eiii_runtime.h"
#include <sstream>

using namespace eiii_test;
static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); ++fails; } printf("\n"); fflush(stdout); } while (0)

static void show(rig &t, char const *what) { printf("  %s: [%s] [%s] [%s] [%s]\n", what, t.row(0).c_str(), t.row(1).c_str(), t.row(2).c_str(), t.row(3).c_str()); }

// Load Bank -> (drive selection) -> D3 -> bank number -> Enter; true once `preset` is on the main screen
static bool load(rig &t, std::string const &num, std::string const &preset)
{
	t.press("load_bank");
	if (!t.wait_row(0, "   LOAD BANK from", 3.0)) { show(t, "no Load Bank screen"); return false; }
	t.pump(0.3);
	t.wait([&] { return t.row(3).find("Wait") == std::string::npos; }, 10.0);
	t.pump(0.5);
	if (t.lcd().find("Select a Bank") != std::string::npos && t.row(1).compare(0, 3, "D3 ") != 0) t.press("cursor_up");
	if (t.lcd().find("Select a Bank") == std::string::npos)
	{
		if (t.lcd().find("Select a Drive") == std::string::npos) { t.press("enter"); t.wait_text("Select a Drive", 3.0); }
		t.press("keypad_3");
		t.wait_row(1, "D3 ", 3.0);
		t.press("enter");
		if (!t.wait_text("Select a Bank", 10.0)) { show(t, "no bank list"); return false; }
	}
	t.press(("keypad_" + num.substr(0, 1)).c_str());
	t.press(("keypad_" + num.substr(1, 1)).c_str());
	t.pump(0.3);
	show(t, ("bank " + num).c_str());
	if (t.row(2).compare(0, 4, "B" + num + " ") != 0) return false;
	t.press("enter");
	bool const ok = t.wait([&] { std::string const r0 = t.row(0); return r0.compare(0, 1, "P") == 0 && t.lcd().find("Loading") == std::string::npos && r0.find(preset) != std::string::npos; }, 90.0);
	t.pump(0.5);
	show(t, "after load");
	return ok;
}

int main()
{
	auto cfg = media_config();
	std::string const cd = env("EIII_IMPORT_CD"), banks = env("EIII_IMPORT_BANKS");
	if (cfg.rt.hd_path.empty() || cd.empty() || banks.empty()) { printf("SKIP: set EIII_ROM_DIR, EIII_HD, EIII_IMPORT_CD, EIII_IMPORT_BANKS\n"); return 77; }
	cfg.rt.cd_path = cd;
	std::string summary;                                  // "Bnn <bank name>" lines of the conversion
	if (eiii::bank_import::needs_conversion(cd))
	{
		std::string const out = env("EIII_IMPORT_OUT", "/tmp/eiii_bank_import_test.iso");
		std::string err;
		bool const ok = eiii::bank_import::convert(cd, out, summary, err);
		CHECK(ok, "converted %s -> %s %s", cd.c_str(), out.c_str(), err.c_str());
		printf("%s", summary.c_str());
		CHECK(eiii::bank_import::cache_fresh(cd, out) && !eiii::bank_import::needs_conversion(out), "converted copy is up to date and needs no conversion");
		cfg.rt.cd_path = out;
	}
	rig t(cfg);
	CHECK(t.wait_row(0, "P00 ", 25.0), "power-on main screen at %.2f s: [%s]", t.now(), t.row(0).c_str());
	t.pump(1.0);

	std::stringstream ss(banks);
	for (std::string item; std::getline(ss, item, ',');)
	{
		auto const colon = item.find(':');
		std::string num = item.substr(0, colon), preset = item.substr(colon + 1);
		if (num.size() != 2 || !isdigit((unsigned char)num[0]))         // a bank name: its number from the conversion
		{
			auto const at = summary.find(" " + num);
			num = at != std::string::npos && at >= 3 && summary[at - 3] == 'B' ? summary.substr(at - 2, 2) : "??";
		}
		bool const ok = load(t, num, preset);
		CHECK(ok, "Load Bank %s from the converted disc shows \"%s\"", num.c_str(), preset.c_str());
		if (!ok) continue;
		uint32_t used = 0, s1 = 0, rate = 0;
		t.e.inspect([&](eiii::runtime &rt) {
			used = rt.debug_read32(0x1A344); s1 = rt.debug_read32(0x1A1B4 + 4);
			for (int s = 1; s < 100 && !rate; ++s) { uint32_t const p = rt.debug_read32(0x1A1B4 + 4 * s); if (p >= 0x400000 && p < 0xC00000) rate = rt.debug_read32(p + 0x34); }
		});
		eiii::media_result r; t.wait([&] { return t.e.poll_media_result(r); }, 5.0);
		printf("  sample memory used to %06X, S01 at %06X, first sample %u Hz\n", used, s1, rate);
		double best = 0; int best_key = 0;
		for (int key : { 36, 48, 60, 72, 84 })
		{
			for (int ch = 0; ch < 16; ++ch) t.midi({ uint8_t(0x90 | ch), uint8_t(key), 100 });
			double const rms = t.rms_over(0.5);
			for (int ch = 0; ch < 16; ++ch) t.midi({ uint8_t(0x80 | ch), uint8_t(key), 64 });
			t.pump(1.0);
			printf("  key %d: rms %.4f\n", key, rms);
			if (rms > best) { best = rms; best_key = key; }
		}
		CHECK(best > 0.01, "bank %s sounds (loudest key %d, rms %.4f)", num.c_str(), best_key, best);
	}
	printf("%s\n", fails ? "FAILED" : "ALL PASSED");
	return fails ? 1 : 0;
}
