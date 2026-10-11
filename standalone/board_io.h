// Host side of the analog board link (POSIX): the Teensy's USB serial port (/dev/ttyACM0) or the simulator's pty.
// Sends the backend's BLOCK packets, waiting while the board's buffer is full (the board's clock paces the emulation),
// and collects the board's status, MIDI IN bytes and pedal / footswitch state.
#pragma once
#include "../host/eiii_backend.h"
#include "../host/eiii_board_link.h"
#include <chrono>
#include <string>
#include <vector>

class board_port : public eiii::board_sink, private eiii::board::reader::handler
{
public:
	~board_port();
	// opens the port and waits for the board's first status (it reports every millisecond)
	bool open(std::string const &path, std::string &error);
	void close();
	bool is_open() const { return m_fd >= 0; }
	void send(uint8_t const *data, size_t n) override;
	void control(eiii::board::control_command c);
	// reads whatever the board has sent; MIDI IN bytes are appended to `midi`
	void poll(std::vector<uint8_t> &midi);

	eiii::board::status const &status() const { return m_st; }
	uint64_t frames_sent() const { return m_sent; }
	double seconds_waiting() const { return m_wait; }   // blocked on a full board buffer (idle time)
	bool failed() const { return m_failed; }
	double longest_write() const { return m_max_write; }    // longest time blocked writing one packet
	double longest_gap() const { return m_max_gap; }        // longest time between two packets (excluding credit waits)

private:
	bool read_some(int timeout_ms);
	void write_all(uint8_t const *data, size_t n);
	void on_status(eiii::board::status const &s) override { m_st = s; m_have_status = true; }
	void on_midi(uint8_t const *b, size_t n) override { m_midi.insert(m_midi.end(), b, b + n); }

	int m_fd = -1;
	eiii::board::reader m_rd;
	eiii::board::status m_st;
	bool m_have_status = false, m_failed = false;
	uint64_t m_sent = 0;
	double m_wait = 0, m_max_write = 0, m_max_gap = 0;
	std::chrono::steady_clock::time_point m_last{};
	std::vector<uint8_t> m_midi, m_pkt;
};
