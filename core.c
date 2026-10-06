/*
 * Bridge core.
 *
 * One-KVM's LCUS serial relay driver (src/atx/serial_relay.rs) writes
 * [0xA0][channel][state][0xA0 + channel + state] and never reads anything.
 * It turns a channel off once at startup, and presses a button as "on", wait, "off":
 * 500 ms for a short power press, 5000 ms for a long one, 500 ms for reset.
 *
 * The GL-ATXPC can't hold a button for as long as it is told to; it has fixed
 * presses instead. So:
 *   power on -> off within long_after_ms   POWER_SW (board presses ~0.5 s)
 *   power still on after long_after_ms     POWER_SW_FORCE <long_press_ms>, at that moment
 *   reset off -> on                        POWER_RESET
 */
#include "core.h"

#include <stdio.h>
#include <string.h>


static core_config_t s_cfg;
static core_send_fn s_send;
static void *s_ctx;

static uint8_t s_frame[LCUS_FRAME_LEN];
static size_t s_index = 0;
static unsigned s_bad_frames = 0;

static bool s_power_on = false;
static uint64_t s_power_since = 0;
static bool s_long_sent = false;
static bool s_reset_on = false;


void core_init(const core_config_t *config, core_send_fn send, void *ctx) {
	s_cfg = *config;
	s_send = send;
	s_ctx = ctx;
	s_index = 0;
	s_bad_frames = 0;
	s_power_on = false;
	s_long_sent = false;
	s_reset_on = false;
}

unsigned core_bad_frames(void) {
	return s_bad_frames;
}

static void _power(bool on, uint64_t now_ms) {
	if (on && !s_power_on) {
		s_power_on = true;
		s_power_since = now_ms;
		s_long_sent = false;
	} else if (!on && s_power_on) {
		s_power_on = false;
		if (!s_long_sent) {
			s_send("POWER_SW", s_ctx);
		}
	}
}

static void _reset(bool on) {
	if (on && !s_reset_on) {
		s_send("POWER_RESET", s_ctx);
	}
	s_reset_on = on;
}

static void _frame(uint64_t now_ms) {
	const uint8_t channel = s_frame[1];
	const uint8_t state = s_frame[2];
	if ((uint8_t)(s_frame[0] + channel + state) != s_frame[3] || state > 1) {
		++s_bad_frames;
		return;
	}
	if (channel == s_cfg.power_channel) {
		_power(state == 1, now_ms);
	} else if (channel == s_cfg.reset_channel) {
		_reset(state == 1);
	}
	// Other channels: not wired to anything on this board
}

void core_feed(const uint8_t *data, size_t len, uint64_t now_ms) {
	for (size_t i = 0; i < len; ++i) {
		if (s_index == 0 && data[i] != LCUS_HEAD) {
			continue; // Resync on the next header byte
		}
		s_frame[s_index++] = data[i];
		if (s_index == LCUS_FRAME_LEN) {
			s_index = 0;
			_frame(now_ms);
		}
	}
	core_tick(now_ms);
}

void core_tick(uint64_t now_ms) {
	if (s_power_on && !s_long_sent && now_ms - s_power_since >= s_cfg.long_after_ms) {
		char cmd[CORE_CMD_MAX];
		snprintf(cmd, sizeof(cmd), "POWER_SW_FORCE %u", (unsigned)s_cfg.long_press_ms);
		s_long_sent = true;
		s_send(cmd, s_ctx);
	}
}

int core_next_timeout(uint64_t now_ms) {
	if (!s_power_on || s_long_sent) {
		return -1;
	}
	const uint64_t deadline = s_power_since + s_cfg.long_after_ms;
	return (now_ms >= deadline ? 0 : (int)(deadline - now_ms));
}
