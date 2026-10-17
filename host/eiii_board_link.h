// Link to the analog voice board (EIII / Wave Rev A): the emulator sends the 16 raw voice lanes and the 80 CV words over
// USB to the board's Teensy 4.1, which clocks them into the codecs and DAC8420s ahead of the real CEM3387 voice cells.
// The Teensy is the clock master; the host paces itself by the board's buffer status.  Board-side contracts: the KiCad
// project's docs (dac_write_sequence.md, codec_channel_map.csv, dac_cv_map.csv).
//
// Packet: E3 B0 | type u8 | 0 | payload length u16 | payload.  All integers little-endian.
//   host -> board
//     BLOCK   u32 first frame (low 32 bits of the 96 kHz frame count) | u8 frames | u8 CV events |
//             events: u8 frame offset, u8 voice << 4 | parameter, u16 word | frames x 16 x s16 lane values (voice 0 first)
//             A CV event applies before the frame at its offset.  voice = digital voice 0..15 (the CPU writes channel
//             15 - voice), parameter 0..4 = Fc1, Fc2, Q, Gain, Pan, word = the CPU's CV word (12 bits in bits 13..2).
//     CONTROL u8 command (control_command)
//   board -> host
//     STATUS  u32 frames played | u16 buffer capacity in frames | u16 underruns | u8 flags (status_flag)
//     MIDI    bytes from the MIDI IN socket
//     INPUTS  u16 pedal (0..4095) | u8 footswitch bits (bit 0 FS1, bit 1 FS2)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace eiii::board {

constexpr double frame_rate = 96000.0;   // codec DAC rate (Plan B clocks)
constexpr unsigned voices = 16;
constexpr unsigned block_frames = 48;    // 0.5 ms per block
constexpr uint8_t sync0 = 0xE3, sync1 = 0xB0;
constexpr size_t header_bytes = 6;
constexpr size_t max_payload = 6 + 255 * 4 + 255 * voices * 2;

enum packet_type : uint8_t { BLOCK = 0x01, CONTROL = 0x02, STATUS = 0x81, MIDI = 0x82, INPUTS = 0x83 };
enum control_command : uint8_t { START = 1, MUTE = 2, UNMUTE = 3 };
enum status_flag : uint8_t { RAIL_OK = 1, MUTED = 2 };

struct cv_event { uint8_t offset, voice, param; uint16_t word; };

struct status
{
	uint32_t played = 0;
	uint16_t capacity = 0, underruns = 0;
	uint8_t flags = 0;
};

// Collects lane frames and CV events into BLOCK packets.
class block_writer
{
public:
	void reset(uint64_t first_frame) { m_frame0 = first_frame; m_frames = 0; m_cv.clear(); m_pcm.clear(); }
	// a CV word that takes effect from the next frame pushed
	void cv(unsigned voice, unsigned param, uint16_t word) { m_cv.push_back({ uint8_t(m_frames), uint8_t(voice), uint8_t(param), word }); }
	void frame(int16_t const pcm[voices]) { m_pcm.insert(m_pcm.end(), pcm, pcm + voices); ++m_frames; }
	// a block is due when it holds block_frames frames, or when its CV table is about to overflow
	bool full() const { return m_frames >= block_frames || m_cv.size() >= 255; }
	bool empty() const { return m_frames == 0 && m_cv.empty(); }
	unsigned frames() const { return m_frames; }
	// encodes the block into `out` (replacing its contents) and starts the next one
	void finish(std::vector<uint8_t> &out);

private:
	uint64_t m_frame0 = 0;
	unsigned m_frames = 0;
	std::vector<cv_event> m_cv;
	std::vector<int16_t> m_pcm;
};

void encode_control(std::vector<uint8_t> &out, control_command c);
void encode_status(std::vector<uint8_t> &out, status const &s);
void encode_midi(std::vector<uint8_t> &out, uint8_t const *bytes, size_t n);
void encode_inputs(std::vector<uint8_t> &out, uint16_t pedal, uint8_t switches);

// Splits a byte stream into packets (resynchronising on E3 B0 after garbage) and decodes them.
class reader
{
public:
	struct handler
	{
		virtual ~handler() = default;
		virtual void on_block(uint32_t /*first_frame*/, unsigned /*frames*/, cv_event const * /*cv*/, unsigned /*n_cv*/, int16_t const * /*pcm*/) {}
		virtual void on_control(control_command) {}
		virtual void on_status(status const &) {}
		virtual void on_midi(uint8_t const *, size_t) {}
		virtual void on_inputs(uint16_t /*pedal*/, uint8_t /*switches*/) {}
	};
	void push(uint8_t const *data, size_t n, handler &h);
	uint64_t dropped() const { return m_dropped; }   // bytes skipped while resynchronising or in malformed packets

private:
	std::vector<uint8_t> m_buf;
	std::vector<cv_event> m_cv;
	std::vector<int16_t> m_pcm;
	uint64_t m_dropped = 0;
};

} // namespace eiii::board
