/*
 * Bridge core: decodes One-KVM's LCUS serial relay frames and turns relay
 * pulses into GL-ATXPC board commands. No I/O, so it can be tested on a PC (test/).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LCUS_HEAD           0xA0
#define LCUS_FRAME_LEN      4       // [0xA0][channel][state][sum]
#define CORE_CMD_MAX        48      // Longest board command, incl. "POWER_SW_FORCE <ms>"

typedef struct {
	uint8_t power_channel;      // One-KVM power button channel (1-based)
	uint8_t reset_channel;      // One-KVM reset button channel (1-based)
	uint32_t long_after_ms;     // A power press held this long is a long press
	uint32_t long_press_ms;     // Duration sent with POWER_SW_FORCE
} core_config_t;

// Called with each board command to send ("POWER_SW", "POWER_SW_FORCE 6500", "POWER_RESET")
typedef void (*core_send_fn)(const char *cmd, void *ctx);

void core_init(const core_config_t *config, core_send_fn send, void *ctx);

// Feed bytes written by One-KVM to the relay, with the time they arrived
void core_feed(const uint8_t *data, size_t len, uint64_t now_ms);

// Call periodically: detects a power press that has become a long press
void core_tick(uint64_t now_ms);

// Milliseconds until core_tick() has something to do, or -1 for no deadline
int core_next_timeout(uint64_t now_ms);

// Frames rejected because of a bad checksum (for diagnostics)
unsigned core_bad_frames(void);
