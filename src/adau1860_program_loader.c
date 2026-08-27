#include "adau1860_program_loader.h"
#include "adau1860_control.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(adau1860_program_loader, LOG_LEVEL_INF);

/* SigmaStudio+ "V1" firmware blob format, confirmed against the Linux
 * kernel's sigmadsp.c reference parser (sigma_firmware_header,
 * sigma_action, sigma_action_len(), sigma_action_size(), and the
 * SIGMA_ACTION_* enum) -- not guessed at. Quoting the layout here since
 * it's load-bearing for the byte offsets below:
 *
 *   struct sigma_firmware_header {          (12 bytes total)
 *       unsigned char magic[7];             "ADISIGM"
 *       uint8_t version;
 *       uint32_t crc;                       little-endian; not verified here
 *   };
 *
 *   struct sigma_action {                   (6-byte fixed part + payload)
 *       uint8_t instr;
 *       uint8_t len_hi;
 *       uint16_t len;                       little-endian
 *       uint16_t addr;                      big-endian
 *       uint8_t payload[];
 *   };
 *
 * action_len = (len_hi << 16) | len  -- this counts addr(2) + payload, so
 * payload_len = action_len - 2. Actions are packed back-to-back, but the
 * *next* action's offset is computed from the payload length rounded up
 * to a 2-byte boundary (sigma_action_size() in the reference parser) --
 * skip that rounding and offsets silently drift on any odd-length payload.
 */
#define SIGMA_MAGIC "ADISIGM"
#define SIGMA_MAGIC_LEN 7
#define SIGMA_HEADER_LEN (SIGMA_MAGIC_LEN + 1 + 4)
#define SIGMA_ACTION_FIXED_LEN 6

enum {
	SIGMA_ACTION_WRITEXBYTES = 0,
	SIGMA_ACTION_WRITESINGLE = 1,
	SIGMA_ACTION_WRITESAFELOAD = 2,
	SIGMA_ACTION_END = 3,
};

int adau1860_program_load(const uint8_t *blob, size_t len)
{
	size_t pos;

	if (len < SIGMA_HEADER_LEN || memcmp(blob, SIGMA_MAGIC, SIGMA_MAGIC_LEN) != 0) {
		LOG_ERR("Not a SigmaStudio+ V1 export (bad magic or too short)");
		return -EINVAL;
	}
	LOG_INF("Loading SigmaStudio+ program: version %u, %u bytes total",
		blob[SIGMA_MAGIC_LEN], (unsigned int)len);
	/* blob[8..11] is the header's CRC (little-endian); not verified here --
	 * this parser trusts the file, it doesn't re-derive its checksum.
	 */

	pos = SIGMA_HEADER_LEN;
	while (pos + SIGMA_ACTION_FIXED_LEN <= len) {
		uint8_t instr = blob[pos];

		if (instr == SIGMA_ACTION_END) {
			LOG_INF("Program load complete (%u bytes consumed)",
				(unsigned int)(pos + 1));
			return 0;
		}

		uint8_t len_hi = blob[pos + 1];
		uint32_t action_len =
			((uint32_t)len_hi << 16) | sys_get_le16(&blob[pos + 2]);
		uint16_t addr = sys_get_be16(&blob[pos + 4]);
		uint32_t payload_len;
		uint32_t aligned_payload_len;
		const uint8_t *payload;
		int err;

		if (action_len < 2) {
			LOG_ERR("Malformed action at offset %u: len %u < 2 (addr size)",
				(unsigned int)pos, action_len);
			return -EINVAL;
		}
		payload_len = action_len - 2;
		aligned_payload_len = ROUND_UP(payload_len, 2);
		payload = &blob[pos + SIGMA_ACTION_FIXED_LEN];

		if (pos + SIGMA_ACTION_FIXED_LEN + payload_len > len) {
			LOG_ERR("Truncated action at offset %u: needs %u payload bytes, "
				"only %u remain",
				(unsigned int)pos, payload_len,
				(unsigned int)(len - pos - SIGMA_ACTION_FIXED_LEN));
			return -EINVAL;
		}

		switch (instr) {
		case SIGMA_ACTION_WRITEXBYTES:
		case SIGMA_ACTION_WRITESINGLE:
			err = adau1860_i2c_write_reg(addr, payload, payload_len);
			if (err) {
				LOG_ERR("Write failed at offset %u, reg 0x%04x (err %d)",
					(unsigned int)pos, addr, err);
				return err;
			}
			break;
		case SIGMA_ACTION_WRITESAFELOAD:
			/* NOT dispatched as adau1860_i2c_write_reg(addr, ...). Safeload
			 * exists specifically so multi-byte parameter updates land
			 * atomically (no audible glitch from the DSP reading a
			 * half-updated coefficient set mid-write) -- that requires
			 * writing to the chip's dedicated safeload staging registers
			 * and then a commit, not writing straight to `addr`. Those
			 * staging register addresses are chip-specific and are NOT
			 * part of this action's own bytes, so there's nothing correct
			 * to do here yet. Silently treating this as a plain write
			 * would compile and probably even produce plausible-looking
			 * audio, while quietly reintroducing the exact glitch
			 * safeload exists to prevent -- worse than refusing outright.
			 */
			LOG_WRN("Skipping WRITESAFELOAD at offset %u, reg 0x%04x, %u "
				"bytes -- real safeload staging registers for this chip "
				"aren't known yet (TODO(hw-bringup))",
				(unsigned int)pos, addr, payload_len);
			break;
		default:
			LOG_ERR("Unknown action type %u at offset %u", instr,
				(unsigned int)pos);
			return -EINVAL;
		}

		pos += SIGMA_ACTION_FIXED_LEN + aligned_payload_len;
	}

	LOG_ERR("Blob ended (offset %u/%u) without a SIGMA_ACTION_END", (unsigned int)pos,
		(unsigned int)len);
	return -EINVAL;
}
