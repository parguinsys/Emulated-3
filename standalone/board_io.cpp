#include "board_io.h"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

board_port::~board_port() { close(); }

bool board_port::open(std::string const &path, std::string &error)
{
	struct stat sb;
	if (::stat(path.c_str(), &sb) == 0 && S_ISSOCK(sb.st_mode))   // tools/board_sim
	{
		std::signal(SIGPIPE, SIG_IGN);   // a closed simulator is reported as a lost link
		sockaddr_un addr{};
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
		m_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (m_fd >= 0 && connect(m_fd, (sockaddr *)&addr, sizeof addr)) { ::close(m_fd); m_fd = -1; }
		if (m_fd >= 0) fcntl(m_fd, F_SETFL, O_NONBLOCK);
	}
	else m_fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
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
	using clk = std::chrono::steady_clock;
	auto const t0 = clk::now();
	if (m_sent) m_max_gap = std::max(m_max_gap, std::chrono::duration<double>(t0 - m_last).count());
	while (!m_failed && uint32_t(m_sent + frames) - m_st.played > m_st.capacity) read_some(1);
	auto const t1 = clk::now();
	m_wait += std::chrono::duration<double>(t1 - t0).count();
	write_all(data, n);
	m_last = clk::now();
	m_max_write = std::max(m_max_write, std::chrono::duration<double>(m_last - t1).count());
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
