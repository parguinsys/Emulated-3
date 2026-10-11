#include "board_io.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

board_port::~board_port() { close(); }

bool board_port::open(std::string const &path, std::string &error)
{
	m_fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (m_fd < 0) { error = path + ": " + strerror(errno); return false; }
	termios tio;
	if (tcgetattr(m_fd, &tio) == 0) { cfmakeraw(&tio); tcsetattr(m_fd, TCSANOW, &tio); }   // a tty: raw bytes
	auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!m_have_status && !m_failed && std::chrono::steady_clock::now() < deadline) read_some(10);
	if (!m_have_status) { error = path + ": the board does not answer (no status within 3 s)"; close(); return false; }
	control(eiii::board::START);
	return true;
}

void board_port::close()
{
	if (m_fd < 0) return;
	if (!m_failed) control(eiii::board::MUTE);
	::close(m_fd);
	m_fd = -1;
}

bool board_port::read_some(int timeout_ms)
{
	pollfd p{ m_fd, POLLIN, 0 };
	if (::poll(&p, 1, timeout_ms) <= 0) return false;
	if (p.revents & (POLLERR | POLLHUP)) { m_failed = true; return false; }
	uint8_t buf[4096];
	ssize_t const n = ::read(m_fd, buf, sizeof buf);
	if (n <= 0) { if (n == 0 || (errno != EAGAIN && errno != EINTR)) m_failed = true; return false; }
	m_rd.push(buf, size_t(n), *this);
	return true;
}

void board_port::write_all(uint8_t const *data, size_t n)
{
	while (n && !m_failed)
	{
		ssize_t const w = ::write(m_fd, data, n);
		if (w > 0) { data += w; n -= size_t(w); continue; }
		if (w < 0 && errno != EAGAIN && errno != EINTR) { m_failed = true; break; }
		pollfd p{ m_fd, POLLOUT, 0 };
		::poll(&p, 1, 10);
	}
}

void board_port::send(uint8_t const *data, size_t n)
{
	if (m_fd < 0 || m_failed) return;
	unsigned const frames = n > 10 && data[2] == eiii::board::BLOCK ? data[10] : 0;
	// credit: frames in flight (sent, not yet played) must fit the board's buffer
	auto const t0 = std::chrono::steady_clock::now();
	while (!m_failed && uint32_t(m_sent + frames) - m_st.played > m_st.capacity) read_some(1);
	m_wait += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	write_all(data, n);
	m_sent += frames;
}

void board_port::control(eiii::board::control_command c)
{
	eiii::board::encode_control(m_pkt, c);
	write_all(m_pkt.data(), m_pkt.size());
}

void board_port::poll(std::vector<uint8_t> &midi)
{
	if (m_fd < 0) return;
	while (read_some(0)) {}
	midi.insert(midi.end(), m_midi.begin(), m_midi.end());
	m_midi.clear();
}
