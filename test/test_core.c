/*
 * PC tests: run the bridge core (core.c) with the frames and timings One-KVM's
 * LCUS serial relay driver produces (src/atx/serial_relay.rs, executor.rs timing).
 *
 * Usage:
 *     gcc -std=c11 -Wall -Wextra -O2 -o hosttest test/test_core.c core.c
 *     ./hosttest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core.h"

static char s_sent[16][CORE_CMD_MAX];
static int s_count = 0;
static int s_failures = 0;

#define CHECK(x_cond) do { \
		if (!(x_cond)) { \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x_cond); \
			++s_failures; \
		} \
	} while (0)

static void _send(const char *cmd, void *ctx) {
	(void)ctx;
	if (s_count < 16) {
		snprintf(s_sent[s_count], CORE_CMD_MAX, "%s", cmd);
	}
	++s_count;
}

static const core_config_t CFG = {.power_channel = 1, .reset_channel = 2, .long_after_ms = 1500, .long_press_ms = 6500};

static void reset(void) {
	s_count = 0;
	memset(s_sent, 0, sizeof(s_sent));
	core_init(&CFG, _send, NULL);
}

// One-KVM's SerialRelayBackend::build_command()
static void relay(uint8_t channel, int on, uint64_t now) {
	const uint8_t state = (uint8_t)(on ? 1 : 0);
	const uint8_t frame[4] = {0xA0, channel, state, (uint8_t)(0xA0 + channel + state)};
	core_feed(frame, 4, now);
}

// Advance time in 10 ms steps the way main.c's poll loop does
static void run_until(uint64_t from, uint64_t to) {
	for (uint64_t t = from; t <= to; t += 10) {
		core_tick(t);
	}
}

static void test_init_frames_do_nothing(void) {
	reset();
	relay(1, 0, 0); // One-KVM turns each channel off when it opens the port
	relay(2, 0, 0);
	run_until(0, 10000);
	CHECK(s_count == 0);
}

static void test_short_press(void) {
	reset();
	relay(1, 1, 1000);
	run_until(1000, 1500);
	CHECK(s_count == 0); // Nothing until the release: it may still become a long press
	relay(1, 0, 1500);   // One-KVM SHORT_PRESS: 500 ms
	CHECK(s_count == 1 && strcmp(s_sent[0], "POWER_SW") == 0);
	run_until(1500, 10000);
	CHECK(s_count == 1);
}

static void test_long_press(void) {
	reset();
	relay(1, 1, 1000);
	run_until(1000, 2490);
	CHECK(s_count == 0);
	run_until(2490, 2500); // 1500 ms held: long press starts now, not at the release
	CHECK(s_count == 1 && strcmp(s_sent[0], "POWER_SW_FORCE 6500") == 0);
	run_until(2500, 6000);
	relay(1, 0, 6000);     // One-KVM LONG_PRESS: 5000 ms
	CHECK(s_count == 1);   // No extra short press on release
}

static void test_reset_press(void) {
	reset();
	relay(2, 1, 1000);
	CHECK(s_count == 1 && strcmp(s_sent[0], "POWER_RESET") == 0);
	relay(2, 1, 1100); // Repeated "on" is not a new press
	relay(2, 0, 1500);
	CHECK(s_count == 1);
	relay(2, 1, 3000);
	CHECK(s_count == 2 && strcmp(s_sent[1], "POWER_RESET") == 0);
}

static void test_bad_and_split_frames(void) {
	reset();
	const uint8_t bad[4] = {0xA0, 0x01, 0x01, 0x00};
	core_feed(bad, 4, 0);
	CHECK(s_count == 0 && core_bad_frames() == 1);

	// Noise, then a press split across two reads
	const uint8_t part1[] = {0x00, 0xFF, 0xA0, 0x02};
	const uint8_t part2[] = {0x01, 0xA3};
	core_feed(part1, sizeof(part1), 100);
	CHECK(s_count == 0);
	core_feed(part2, sizeof(part2), 101);
	CHECK(s_count == 1 && strcmp(s_sent[0], "POWER_RESET") == 0);

	const uint8_t bad_state[4] = {0xA0, 0x01, 0x02, 0xA3};
	core_feed(bad_state, 4, 200);
	CHECK(core_bad_frames() == 2 && s_count == 1);
}

static void test_other_channels_ignored(void) {
	reset();
	relay(3, 1, 0);
	relay(3, 0, 500);
	relay(8, 1, 600);
	run_until(600, 5000);
	CHECK(s_count == 0);
}

static void test_next_timeout(void) {
	reset();
	CHECK(core_next_timeout(0) == -1);
	relay(1, 1, 1000);
	CHECK(core_next_timeout(1000) == 1500);
	CHECK(core_next_timeout(2000) == 500);
	CHECK(core_next_timeout(3000) == 0);
	core_tick(3000);
	CHECK(core_next_timeout(3000) == -1); // Long press already sent
}

static void test_both_buttons_back_to_back(void) {
	reset();
	// One-KVM shares one port for both channels: frames arrive back to back
	const uint8_t frames[] = {0xA0, 0x01, 0x01, 0xA2, 0xA0, 0x02, 0x01, 0xA3};
	core_feed(frames, sizeof(frames), 0);
	CHECK(s_count == 1 && strcmp(s_sent[0], "POWER_RESET") == 0);
	relay(1, 0, 500);
	CHECK(s_count == 2 && strcmp(s_sent[1], "POWER_SW") == 0);
}

int main(void) {
	test_init_frames_do_nothing();
	test_short_press();
	test_long_press();
	test_reset_press();
	test_bad_and_split_frames();
	test_other_channels_ignored();
	test_next_timeout();
	test_both_buttons_back_to_back();
	if (s_failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", s_failures);
		return EXIT_FAILURE;
	}
	printf("All bridge core tests passed\n");
	return EXIT_SUCCESS;
}
