// EIIIX / ESI-32 -> EIII bank conversion (see eiii_bank_import.h and tools/eiii_bank_import.py, which this follows
// line by line; the Python tool carries the format notes).
#include "runtime/eiii_bank_import.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <vector>

namespace eiii::bank_import
{
namespace
{
constexpr unsigned block = 512;
constexpr uint32_t ram_base = 0x1A000, heap_eiii = 0x1A6FE, heap_x = 0x1CB26, ram_limit = 0x28000;
constexpr uint32_t snd_base = 0x400000, snd_top = 0xBFD800;
constexpr int version = 1;                       // bump when converted discs change, so cached copies are redone

using bytes = std::vector<uint8_t>;
uint32_t u32(uint8_t const *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t u16(uint8_t const *p) { return uint16_t(p[0] | p[1] << 8); }
void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> 8 * i); }
void put16(uint8_t *p, unsigned v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }

enum class kind { none, eiii, eiiix, esi };
kind kind_of(uint8_t const *head)
{
	if (!memcmp(head, "EMULATOR THREE ", 15)) return kind::eiii;
	if (!memcmp(head, "EMULATOR 3X    ", 15)) return kind::eiiix;
	if (!memcmp(head, "EMU SI-32 v3   ", 15)) return kind::esi;
	return kind::none;
}

struct volume
{
	FILE *f = nullptr;
	uint32_t dir_lba = 0, dir_n = 0, data_lba = 0, csize = 0;
	bytes fat, head0;
	std::vector<bytes> entries;                  // 32-byte bank entries (type 0x81), directory order
	std::string name;
	~volume() { if (f) fclose(f); }
	bool blocks(uint64_t lba, uint32_t n, uint8_t *out)
	{
		return fseeko(f, off_t(lba * block), SEEK_SET) == 0 && fread(out, 1, size_t(n) * block, f) == size_t(n) * block;
	}
	bool open(std::string const &path, std::string &error)
	{
		if (!(f = fopen(path.c_str(), "rb"))) { error = "cannot open " + path; return false; }
		head0.resize(block);
		if (!blocks(0, 1, head0.data()) || memcmp(head0.data(), "EMU3", 4)) return false;
		uint8_t const *vh = head0.data();
		uint32_t const vol_lba = u32(vh + 0x08), fat_lba = u32(vh + 0x18), fat_n = u32(vh + 0x1C);
		dir_lba = u32(vh + 0x10); dir_n = u32(vh + 0x14); data_lba = u32(vh + 0x20);
		if (vh[0x28] < 1 || vh[0x28] > 8 || fat_n > 256 || dir_n > 4096) { error = "unusual EMU3 volume header"; return false; }
		csize = 0x80u << (vh[0x28] - 1);
		fat.resize(size_t(fat_n) * block);
		bytes d(size_t(dir_n) * block), v(block);
		if (!blocks(fat_lba, fat_n, fat.data()) || !blocks(dir_lba, dir_n, d.data())) { error = "cannot read the disc directory"; return false; }
		if (blocks(vol_lba, 1, v.data()) && v[0] >= 0x20 && v[0] < 0x7F)
		{
			name.assign(reinterpret_cast<char const *>(v.data()), 16);
			while (!name.empty() && (name.back() == ' ' || name.back() == 0)) name.pop_back();
		}
		for (size_t i = 0; i + 32 <= d.size(); i += 32)
			if (d[i + 0x1A] == 0x81 && d[i] >= 0x20) entries.emplace_back(d.begin() + i, d.begin() + i + 32);
		return true;
	}
	std::vector<uint16_t> chain(bytes const &e)
	{
		std::vector<uint16_t> c;
		for (uint16_t x = u16(&e[0x12]); x && x != 0x7FFF && c.size() < 0x10000 && 2u * x + 1 < fat.size(); x = u16(&fat[2u * x]))
			c.push_back(x);
		return c;
	}
	bool read(bytes const &e, bytes &out, size_t limit = SIZE_MAX)
	{
		auto const c = chain(e);
		size_t const cb = size_t(csize) * block;
		out.assign(std::min(limit, c.size() * cb), 0);
		for (size_t k = 0; k < c.size() && k * cb < out.size(); ++k)
		{
			size_t const n = std::min(cb, out.size() - k * cb);
			if (fseeko(f, off_t((uint64_t(data_lba) + (c[k] - 1) * uint64_t(csize)) * block), SEEK_SET) || fread(&out[k * cb], 1, n, f) != n) return false;
		}
		return true;
	}
};

struct bank
{
	kind k = kind::none;
	bytes file, ram, snd;
	std::map<int, bytes> presets;                // preset number -> record
	std::map<int, bytes> samples;                // sample number -> header + data
	uint32_t r32(uint32_t a) const { return a >= ram_base && a + 4 <= ram_base + ram.size() ? u32(&ram[a - ram_base]) : 0; }

	bool parse(bytes &&f)
	{
		file = std::move(f);
		if (file.size() < 0x4C || (k = kind_of(file.data())) == kind::none) return false;
		uint32_t const ram_n = u32(&file[0x30]), snd_n = u32(&file[0x34]);
		if (0x4Cull + ram_n + snd_n > file.size()) return false;
		ram.assign(file.begin() + 0x4C, file.begin() + 0x4C + ram_n);
		snd.assign(file.begin() + 0x4C + ram_n, file.begin() + 0x4C + ram_n + snd_n);
		uint32_t ptr[101];
		int nsmp; uint32_t stab;
		if (k == kind::eiii) { for (int i = 0; i < 101; ++i) ptr[i] = r32(0x1A020 + 4 * i); nsmp = 100; stab = 0x1A1B4; }
		else { for (int i = 0; i < 101; ++i) ptr[i] = heap_x + r32(0x1B77E + 4 * i); nsmp = 1000; stab = 0x1BB82; }
		for (int i = 0; i < 100; ++i)
		{
			uint32_t const a = ptr[i], n = ptr[i + 1] - ptr[i];
			if (ptr[i + 1] > ptr[i] && n >= 0x8E && a >= ram_base && a + n <= ram_base + ram.size())
				presets[i].assign(ram.begin() + (a - ram_base), ram.begin() + (a - ram_base + n));
		}
		std::vector<uint32_t> sp(nsmp);
		std::set<uint32_t> live;
		for (int i = 0; i < nsmp; ++i) { sp[i] = r32(stab + 4 * i); if (sp[i] >= snd_base && sp[i] < snd_base + snd.size()) live.insert(sp[i]); }
		for (int i = 0; i < nsmp; ++i)
		{
			if (!live.count(sp[i])) continue;
			auto nx = live.upper_bound(sp[i]);
			uint32_t const end = nx == live.end() ? uint32_t(snd_base + snd.size()) : *nx;
			samples[i].assign(snd.begin() + (sp[i] - snd_base), snd.begin() + (end - snd_base));
		}
		return true;
	}
	static std::vector<size_t> voice_records(bytes const &rec)
	{
		std::vector<size_t> v;
		for (size_t o = 0x8E + 4 * size_t(rec[0x35]); o + 0x30 <= rec.size(); o += 0x30) v.push_back(o);
		return v;
	}
	std::set<int> preset_samples(int i) const
	{
		std::set<int> s;
		auto const &rec = presets.at(i);
		for (size_t v : voice_records(rec)) if (samples.count(rec[v + 1])) s.insert(rec[v + 1]);
		return s;
	}
	int link(int i) const
	{
		int const l = presets.at(i)[0x31];
		return l > 0 && l <= 100 && presets.count(l - 1) && l - 1 != i ? l - 1 : -1;
	}
	std::vector<int> group(int i) const
	{
		std::vector<int> g;
		for (int j = i; j >= 0 && std::find(g.begin(), g.end(), j) == g.end(); j = link(j)) g.push_back(j);
		return g;
	}
	std::set<int> samples_of(std::vector<int> const &ps) const
	{
		std::set<int> s;
		for (int p : ps) { auto const q = preset_samples(p); s.insert(q.begin(), q.end()); }
		return s;
	}
	size_t sample_bytes(std::vector<int> const &ps) const
	{
		size_t n = 0;
		for (int s : samples_of(ps)) n += samples.at(s).size() + (samples.at(s).size() & 1);
		return n;
	}
	std::string preset_name(int i) const
	{
		std::string n(reinterpret_cast<char const *>(presets.at(i).data()), 16);
		while (!n.empty() && n.back() == ' ') n.pop_back();
		return n;
	}
};

// presets split into parts that fit the EIII (linked presets together); left-out presets reported in `notes`
std::vector<std::vector<int>> plan(bank const &b, std::string &notes, std::string const &bank_name)
{
	size_t const room = snd_top - snd_base;
	std::vector<std::vector<int>> parts;
	std::vector<int> cur;
	std::set<int> done;
	for (auto const &[p, rec] : b.presets)
	{
		if (done.count(p)) continue;
		auto const g = b.group(p);
		done.insert(g.begin(), g.end());
		if (b.sample_bytes(g) > room || b.samples_of(g).size() > 99)
		{
			notes += "  " + bank_name + ": left out preset " + std::to_string(p) + " " + b.preset_name(p) + " (needs more than 8 MB)\n";
			continue;
		}
		auto trial = cur;
		for (int q : g) if (std::find(trial.begin(), trial.end(), q) == trial.end()) trial.push_back(q);
		if (!cur.empty() && (b.sample_bytes(trial) > room - 0x100 || b.samples_of(trial).size() > 99))
		{
			parts.push_back(cur);
			trial = g;
		}
		cur = trial;
	}
	if (!cur.empty()) parts.push_back(cur);
	std::vector<std::vector<int>> out;
	for (auto &p : parts) if (b.sample_bytes(p)) out.push_back(std::move(p));
	return out;
}

bool eiii_bank_file(bank const &src, std::string const &name16, int number, std::vector<int> const &keep, bytes &out)
{
	std::set<int> const used = src.samples_of(keep);
	std::map<int, int> remap;
	bool const fits = used.empty() || *used.rbegin() < 100;
	int n = 0;
	for (int s : used) remap[s] = fits ? s : ++n;
	if (used.size() > 99) return false;
	bytes snd;
	uint32_t sptr[100] = {};
	for (int s : used)
	{
		sptr[remap[s]] = uint32_t(snd_base + snd.size());
		snd.insert(snd.end(), src.samples.at(s).begin(), src.samples.at(s).end());
		if (snd.size() & 1) snd.push_back(0);
	}
	if (snd_base + snd.size() > snd_top) return false;

	bytes heap;
	uint32_t pptr[101];
	for (int i = 0; i < 100; ++i)
	{
		pptr[i] = uint32_t(heap_eiii + heap.size());
		if (std::find(keep.begin(), keep.end(), i) == keep.end()) continue;
		bytes rec = src.presets.at(i);
		for (size_t v : bank::voice_records(rec)) if (remap.count(rec[v + 1])) rec[v + 1] = uint8_t(remap[rec[v + 1]]);
		heap.insert(heap.end(), rec.begin(), rec.end());
	}
	pptr[100] = uint32_t(heap_eiii + heap.size());
	bytes ram(heap_eiii - ram_base, 0);
	static uint8_t const chan_tables[0x30] = {
		0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0xFE, 0xFF, 0xFF, 0x28, 0, 2, 0, 5, 0, 0x55, 0x6E, 0, 0, 0, 0x80, 0xFF, 0xFF, 0, 0, 0, 0xFE,
		0xFF, 0xFF, 0x28, 0, 2, 0, 5, 0, 0x55, 0x6E, 0, 0, 0, 0x80, 0, 0 };
	memcpy(&ram[0x690], chan_tables, sizeof chan_tables);
	memcpy(&ram[0], name16.data(), 16);
	// start on the first preset that plays (ESI banks keep an empty "Untitled Preset" at 0)
	int first = *std::min_element(keep.begin(), keep.end()), playable = 100;
	for (int p : keep) if (p < playable && !src.preset_samples(p).empty()) playable = p;
	if (playable < 100) first = playable;
	put32(&ram[0x10], uint32_t(first)); put32(&ram[0x14], 1);
	memset(&ram[0x686], first, 16);              // MIDI channel -> preset
	for (int i = 0; i < 101; ++i) put32(&ram[0x20 + 4 * i], pptr[i]);
	for (int i = 0; i < 100; ++i) put32(&ram[0x1B4 + 4 * i], sptr[i]);
	put32(&ram[0x344], uint32_t(snd_base + snd.size())); put32(&ram[0x348], snd_top);
	ram.insert(ram.end(), heap.begin(), heap.end());
	ram.push_back(0);
	if (ram_base + ram.size() > ram_limit) return false;

	uint32_t const b0 = uint32_t((0x4C + ram.size() + block - 1) / block), b1 = uint32_t(snd.size() / block + 1);
	out.assign(0x4C, 0);
	memcpy(&out[0], "EMULATOR THREE ", 16);
	memcpy(&out[16], name16.data(), 16);
	uint32_t const h[] = { uint32_t(number), 1, 1, (b0 + b1 + 1599) / 1600, uint32_t(ram.size()), uint32_t(snd.size()), 0, b0, b1, 0, b0 + b1 };
	for (int i = 0; i < 11; ++i) put32(&out[0x20 + 4 * i], h[i]);
	out.insert(out.end(), ram.begin(), ram.end());
	out.insert(out.end(), snd.begin(), snd.end());
	return true;
}

std::string name16(std::string n)
{
	n.resize(16, ' ');
	return n;
}

// streams bank files into a new EMU3 volume (256 KB clusters, as E-mu's CDs)
struct writer
{
	static constexpr uint32_t csize = 0x200, fat_lba = 2, fat_n = 5, vol_lba = 7, vol_n = 2, dir_lba = 9, dir_n = 0x66, data_lba = dir_lba + dir_n;
	FILE *f = nullptr;
	std::vector<uint16_t> fat = std::vector<uint16_t>(fat_n * block / 2, 0);
	bytes dir = bytes(dir_n * block, 0);
	uint32_t next_c = 1;
	int files = 0;
	bool add(std::string const &name, int number, bytes const &data)
	{
		uint32_t const nb = uint32_t((data.size() + block - 1) / block), nc = (nb + csize - 1) / csize;
		if (next_c + nc >= fat.size() || files >= 7 * int(block) / 32) return false;
		for (uint32_t c = next_c; c < next_c + nc; ++c) fat[c] = uint16_t(c + 1);
		fat[next_c + nc - 1] = 0x7FFF;
		uint8_t *e = &dir[32 * files];
		memcpy(e, name.data(), 16);
		e[0x11] = uint8_t(number);
		put16(e + 0x12, next_c); put16(e + 0x14, nc); put16(e + 0x16, nb - (nc - 1) * csize); put16(e + 0x18, unsigned(data.size() - (nb - 1) * size_t(block)));
		e[0x1A] = 0x81;
		if (fseeko(f, off_t((uint64_t(data_lba) + (next_c - 1) * uint64_t(csize)) * block), SEEK_SET) || fwrite(data.data(), 1, data.size(), f) != data.size()) return false;
		next_c += nc; ++files;
		return true;
	}
	bool finish(std::string const &label)
	{
		uint32_t total = data_lba + (next_c - 1) * csize;
		total += (4 - total % 4) % 4;
		bytes vh(block, 0);
		memcpy(&vh[0], "EMU3", 4);
		uint32_t const w[] = { total, vol_lba, vol_n, dir_lba, dir_n, fat_lba, fat_n, data_lba, next_c - 1 };
		for (int i = 0; i < 9; ++i) put32(&vh[4 + 4 * i], w[i]);
		vh[0x28] = 3; vh[0x29] = 1; put32(&vh[0x2A], total + 0x0B); vh[0x32] = 1; vh[0x33] = 0x0D;
		unsigned sum = 0;
		for (unsigned i = 0; i < 255; ++i) sum += u16(&vh[2 * i]);
		put16(&vh[510], sum & 0xFFFF);
		uint32_t const used_dir = std::max(1u, uint32_t((files * 32 + block - 1) / block));
		bytes alloc(block, 0x80);
		put32(&alloc[0], dir_lba + used_dir);
		bytes vol(vol_n * block, 0);
		memcpy(&vol[0], name16(label).data(), 16);
		put16(&vol[16], 0x4000);
		for (uint32_t k = 0; k < 7; ++k) put16(&vol[18 + 2 * k], k < used_dir ? dir_lba + k : 0xFFFF);
		bytes fatb(fat.size() * 2);
		for (size_t i = 0; i < fat.size(); ++i) put16(&fatb[2 * i], fat[i]);
		auto put = [&](uint32_t lba, bytes const &b) { return fseeko(f, off_t(uint64_t(lba) * block), SEEK_SET) == 0 && fwrite(b.data(), 1, b.size(), f) == b.size(); };
		if (!put(0, vh) || !put(1, alloc) || !put(fat_lba, fatb) || !put(vol_lba, vol) || !put(dir_lba, dir)) return false;
		if (fflush(f) || fseeko(f, 0, SEEK_END)) return false;
		off_t const end = ftello(f);
		if (end < off_t(uint64_t(total) * block))
		{
			if (fseeko(f, off_t(uint64_t(total) * block - 1), SEEK_SET) || fputc(0, f) == EOF) return false;
		}
		return true;
	}
};

std::string stamp(std::string const &src)
{
	std::error_code ec;
	auto const size = std::filesystem::file_size(src, ec);
	auto const t = std::filesystem::last_write_time(src, ec).time_since_epoch().count();
	return "v" + std::to_string(version) + " " + std::to_string(size) + " " + std::to_string(t) + " " + src + "\n";
}
}

bool scan(std::string const &path, disc_info &info, std::string &error)
{
	info = {};
	volume v;
	if (!v.open(path, error)) return error.empty();      // not an EMU3 volume: nothing to report
	info.emu3 = true;
	info.name = v.name;
	bytes head;
	for (auto const &e : v.entries)
	{
		if (!v.read(e, head, 0x4C) || head.size() < 0x4C) continue;
		switch (kind_of(head.data()))
		{
		case kind::eiii: ++info.eiii; break;
		case kind::eiiix: ++info.eiiix; break;
		case kind::esi: ++info.esi; break;
		default: break;
		}
	}
	return true;
}

bool needs_conversion(std::string const &path)
{
	disc_info i;
	std::string e;
	return scan(path, i, e) && i.emu3 && i.eiiix + i.esi > 0;
}

bool convert(std::string const &src, std::string const &dst, std::string &summary, std::string &error, std::atomic<bool> const *cancel)
{
	volume v;
	if (!v.open(src, error)) { if (error.empty()) error = src + " is not an Emulator III / ESI disc"; return false; }
	std::string const tmp = dst + ".part";
	writer w;
	if (!(w.f = fopen(tmp.c_str(), "wb"))) { error = "cannot write " + tmp; return false; }
	bool ok = true;
	int number = 0;
	summary.clear();
	for (auto const &e : v.entries)
	{
		if (cancel && *cancel) { ok = false; error = "cancelled"; break; }
		bytes data;
		if (!v.read(e, data)) { ok = false; error = "cannot read " + src; break; }
		bank b;
		if (!b.parse(std::move(data))) continue;
		std::string const bname(reinterpret_cast<char const *>(e.data()), 16);
		std::string trimmed = bname;
		while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
		std::vector<std::pair<std::string, bytes>> outs;
		if (b.k == kind::eiii)
		{
			// already an EIII bank: copied as it is, renumbered
			bytes f(b.file.begin(), b.file.begin() + std::min(b.file.size(), size_t(0x4C) + u32(&b.file[0x30]) + u32(&b.file[0x34]) + u32(&b.file[0x38])));
			outs.emplace_back(bname, std::move(f));
		}
		else
		{
			auto const parts = plan(b, summary, trimmed);
			for (size_t k = 0; k < parts.size(); ++k)
			{
				std::string n = bname;
				if (parts.size() > 1) { n = trimmed.substr(0, 14); n.resize(14, ' '); n += ' '; n += char('A' + k % 26); }
				bytes f;
				if (eiii_bank_file(b, name16(n), number + int(outs.size()), parts[k], f)) outs.emplace_back(name16(n), std::move(f));
			}
		}
		for (auto &[n, f] : outs)
		{
			if (number > 99) { summary += "  disc has more than 100 banks after conversion: the rest are left out\n"; break; }
			put32(&f[0x20], uint32_t(number));
			if (!w.add(n, number, f)) { ok = false; error = "cannot write " + tmp; break; }
			char line[64];
			snprintf(line, sizeof line, "B%02d %.16s\n", number, n.c_str());
			summary += line;
			++number;
		}
		if (!ok || number > 99) break;
	}
	if (ok && !number) { ok = false; error = "no banks on " + src; }
	ok = ok && w.finish(v.name.empty() ? std::filesystem::path(src).stem().string() : v.name);
	ok = fclose(w.f) == 0 && ok;
	w.f = nullptr;
	std::error_code ec;
	if (!ok) { std::filesystem::remove(tmp, ec); if (error.empty()) error = "cannot write " + tmp; return false; }
	std::filesystem::rename(tmp, dst, ec);
	if (ec) { error = "cannot write " + dst + ": " + ec.message(); return false; }
	std::ofstream(dst + ".source") << stamp(src);
	return true;
}

std::string cached_path(std::string const &src, std::string const &cache_dir)
{
	std::string const stem = std::filesystem::path(src).stem().string();
	return (std::filesystem::path(cache_dir) / (stem + " (EIII).iso")).string();
}

bool cache_fresh(std::string const &src, std::string const &converted)
{
	std::ifstream in(converted + ".source");
	std::string line;
	std::error_code ec;
	return std::getline(in, line) && line + "\n" == stamp(src) && std::filesystem::exists(converted, ec);
}
}
