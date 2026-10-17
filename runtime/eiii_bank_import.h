// Emulator IIIX and ESI-32 banks on the Emulator III: discs holding them are converted, bank by bank, into an EIII
// disc image the original OS loads from (same as tools/eiii_bank_import.py; format notes there).
#pragma once
#include <atomic>
#include <string>

namespace eiii::bank_import
{
struct disc_info
{
	bool emu3 = false;                   // an EMU3 volume (EIII / EIIIX / ESI-32 CD or hard disk)
	int eiii = 0, eiiix = 0, esi = 0;    // banks of each kind
	std::string name;                    // volume name, if the disc has one
};

// what is on a disc image; false (and error) if it cannot be read
bool scan(std::string const &path, disc_info &info, std::string &error);
// true if the disc has EIIIX or ESI-32 banks, i.e. the EIII needs a converted copy
bool needs_conversion(std::string const &path);

// writes `dst` (via a temporary file): every bank of `src`, EIIIX / ESI-32 ones converted and split to fit the
// EIII's 8 MB, presets too large for it left out.  `summary`: one line per output bank and per left-out preset.
bool convert(std::string const &src, std::string const &dst, std::string &summary, std::string &error,
	std::atomic<bool> const *cancel = nullptr);

// the converted copy of `src` kept in `cache_dir` ("<disc> (EIII).iso"), and whether it is up to date with `src`
std::string cached_path(std::string const &src, std::string const &cache_dir);
bool cache_fresh(std::string const &src, std::string const &converted);
}
