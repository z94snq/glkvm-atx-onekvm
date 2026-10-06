/*
 * glatx-bridge: GL.iNet GL-ATXPC power board for One-KVM, with no One-KVM changes.
 *
 * One-KVM (ATX driver "Serial", LCUS relay) -> fake serial port (pty) -> this program
 * this program -> GL-ATXPC (USB 1209:c550, ASCII line protocol) -> target's front panel
 *
 * Runs inside the One-KVM container, started before One-KVM (see README), so the
 * fake port exists when One-KVM opens it and both always restart together.
 *
 * Environment (all optional):
 *   GLATX_LINK           fake port path (default /dev/ttyACM-glatx; One-KVM only lists
 *                        /dev/ttyACM* and /dev/ttyUSB* names)
 *   GLATX_DEVICE         board serial device (default: auto-detect USB 1209:c550)
 *   GLATX_POWER_CHANNEL  One-KVM power channel (default 1)
 *   GLATX_RESET_CHANNEL  One-KVM reset channel (default 2)
 *   GLATX_LONG_AFTER_MS  power held this long counts as a long press (default 1500)
 *   GLATX_LONG_PRESS_MS  POWER_SW_FORCE duration (default 6500)
 *   GLATX_TIMEOUT_MS     wait for the board's "RCV:" echo (default 1000)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "core.h"

#define USB_VID "1209"
#define USB_PID "c550"

static const char *s_link;
static const char *s_device;    // NULL: auto-detect per command (survives re-plugging)
static int s_timeout_ms;
static volatile sig_atomic_t s_stop = 0;


static void _log(const char *fmt, ...) {
	char ts[32];
	const time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, "%s [glatx] ", ts);
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

static uint64_t _now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static long _env_long(const char *name, long def, long min, long max) {
	const char *value = getenv(name);
	if (value == NULL || *value == '\0') {
		return def;
	}
	char *end;
	const long v = strtol(value, &end, 10);
	if (*end != '\0' || v < min || v > max) {
		_log("Ignoring invalid %s=%s, using %ld", name, value, def);
		return def;
	}
	return v;
}


// ===== GL-ATXPC board =====
static bool _read_attr(const char *dir, const char *name, char *buf, size_t size) {
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return false;
	}
	const bool ok = (fgets(buf, (int)size, f) != NULL);
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

// Find /dev/ttyACMn of the first USB 1209:c550 device; false if none
static bool _find_board(char *out, size_t size) {
	DIR *dir = opendir("/sys/class/tty");
	if (dir == NULL) {
		return false;
	}
	bool found = false;
	struct dirent *ent;
	while (!found && (ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, "ttyACM", 6) != 0) {
			continue;
		}
		char usb[PATH_MAX], real[PATH_MAX], vid[8], pid[8];
		snprintf(usb, sizeof(usb), "/sys/class/tty/%s/device/..", ent->d_name);
		if (realpath(usb, real) == NULL) {
			continue;
		}
		if (_read_attr(real, "idVendor", vid, sizeof(vid)) && _read_attr(real, "idProduct", pid, sizeof(pid))
			&& strcmp(vid, USB_VID) == 0 && strcmp(pid, USB_PID) == 0) {
			snprintf(out, size, "/dev/%s", ent->d_name);
			found = true;
		}
	}
	closedir(dir);
	return found;
}

static bool _board_device(char *out, size_t size) {
	if (s_device != NULL) {
		snprintf(out, size, "%s", s_device);
		return true;
	}
	return _find_board(out, size);
}

// Read one line (without CR/LF) before the deadline; false on timeout or error
static bool _read_line(int fd, char *buf, size_t size, uint64_t deadline) {
	size_t len = 0;
	while (true) {
		const uint64_t now = _now_ms();
		if (now >= deadline) {
			return false;
		}
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		if (poll(&pfd, 1, (int)(deadline - now)) <= 0) {
			return false;
		}
		char ch;
		const ssize_t n = read(fd, &ch, 1);
		if (n <= 0) {
			return false;
		}
		if (ch == '\n') {
			if (len > 0 && buf[len - 1] == '\r') {
				--len;
			}
			buf[len] = '\0';
			return true;
		}
		if (len + 1 < size) {
			buf[len++] = ch;
		}
	}
}

// Send one command and wait for its "RCV: <TOKEN>" echo. Opens the port per command,
// like the reference kvmd driver. If payload is not NULL, also reads the next line.
static bool _board_request(const char *cmd, char *payload, size_t payload_size) {
	char device[PATH_MAX];
	if (!_board_device(device, sizeof(device))) {
		_log("Can't send %s: board not found (USB %s:%s)", cmd, USB_VID, USB_PID);
		return false;
	}
	const int fd = open(device, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		_log("Can't send %s: %s: %s", cmd, device, strerror(errno));
		return false;
	}
	struct termios tio;
	if (tcgetattr(fd, &tio) == 0) {
		cfmakeraw(&tio);
		tcsetattr(fd, TCSANOW, &tio);
	}
	tcflush(fd, TCIFLUSH);

	bool ok = false;
	char line[128], ack[64];
	const size_t token_len = strcspn(cmd, " ");
	snprintf(ack, sizeof(ack), "RCV: %.*s", (int)token_len, cmd);
	dprintf(fd, "%s\n", cmd);
	const uint64_t deadline = _now_ms() + (uint64_t)s_timeout_ms;
	while (_read_line(fd, line, sizeof(line), deadline)) {
		if (strcmp(line, ack) == 0) {
			ok = (payload == NULL || _read_line(fd, payload, payload_size, deadline));
			break;
		}
	}
	close(fd);
	if (!ok) {
		_log("Can't send %s: no %s from %s", cmd, (payload == NULL ? "echo" : "reply"), device);
	}
	return ok;
}

static void _send(const char *cmd, void *ctx) {
	(void)ctx;
	if (_board_request(cmd, NULL, 0)) {
		_log("Sent %s", cmd);
	}
}


// ===== Fake LCUS relay port =====
// Returns the pty master. Also keeps a slave fd open (out_slave) so reads on the
// master don't fail while One-KVM has the port closed (e.g. during a settings reload).
static int _open_pty(int *out_slave) {
	const int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
		_log("Can't create pty: %s", strerror(errno));
		exit(1);
	}
	const char *name = ptsname(master);
	const int slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (slave < 0) {
		_log("Can't open %s: %s", name, strerror(errno));
		exit(1);
	}
	struct termios tio;
	if (tcgetattr(slave, &tio) == 0) {
		cfmakeraw(&tio); // No echo or line editing; One-KVM sends raw bytes
		tcsetattr(slave, TCSANOW, &tio);
	}
	// Replace the link atomically, so a stale link from an earlier run never wins
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s.tmp", s_link);
	unlink(tmp);
	if (symlink(name, tmp) != 0 || rename(tmp, s_link) != 0) {
		_log("Can't link %s -> %s: %s", s_link, name, strerror(errno));
		exit(1);
	}
	_log("Fake LCUS relay port: %s -> %s", s_link, name);
	*out_slave = slave;
	return master;
}

static void _on_signal(int sig) {
	(void)sig;
	s_stop = 1;
}

int main(void) {
	s_link = getenv("GLATX_LINK");
	if (s_link == NULL || *s_link == '\0') {
		s_link = "/dev/ttyACM-glatx";
	}
	s_device = getenv("GLATX_DEVICE");
	if (s_device != NULL && *s_device == '\0') {
		s_device = NULL;
	}
	s_timeout_ms = (int)_env_long("GLATX_TIMEOUT_MS", 1000, 100, 10000);
	const core_config_t cfg = {
		.power_channel = (uint8_t)_env_long("GLATX_POWER_CHANNEL", 1, 1, 8),
		.reset_channel = (uint8_t)_env_long("GLATX_RESET_CHANNEL", 2, 1, 8),
		.long_after_ms = (uint32_t)_env_long("GLATX_LONG_AFTER_MS", 1500, 600, 4900),
		.long_press_ms = (uint32_t)_env_long("GLATX_LONG_PRESS_MS", 6500, 1000, 30000),
	};
	if (cfg.power_channel == cfg.reset_channel) {
		_log("GLATX_POWER_CHANNEL and GLATX_RESET_CHANNEL must differ (both %u)", cfg.power_channel);
		return 1;
	}
	core_init(&cfg, _send, NULL);

	struct sigaction sa = {.sa_handler = _on_signal};
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	int slave;
	const int master = _open_pty(&slave);
	_log("One-KVM ATX settings: driver Serial, device %s, power channel %u, reset channel %u",
		s_link, cfg.power_channel, cfg.reset_channel);

	char device[PATH_MAX], sn[64];
	if (!_board_device(device, sizeof(device))) {
		_log("Board not found yet (USB %s:%s); looking again on every button press", USB_VID, USB_PID);
	} else if (_board_request("GET_SN", sn, sizeof(sn))) { // Read-only: presses nothing
		_log("Board %s, serial number %s", device, sn);
	}

	while (!s_stop) {
		struct pollfd pfd = {.fd = master, .events = POLLIN};
		const int rc = poll(&pfd, 1, core_next_timeout(_now_ms()));
		if (rc < 0 && errno != EINTR) {
			_log("poll: %s", strerror(errno));
			break;
		}
		if (rc > 0 && (pfd.revents & POLLIN)) {
			uint8_t buf[64];
			const ssize_t n = read(master, buf, sizeof(buf));
			if (n > 0) {
				core_feed(buf, (size_t)n, _now_ms());
			}
		}
		core_tick(_now_ms());
	}

	unlink(s_link);
	close(slave);
	close(master);
	_log("Stopped");
	return 0;
}
