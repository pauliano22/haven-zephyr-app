/* Host-test fake for <zephyr/sys/byteorder.h> -- real little-endian pack/
 * unpack semantics (not a no-op), since gatt_audio_service.c's wire format
 * correctness for FreqRange depends on these actually doing what they say.
 */
#ifndef FAKE_ZEPHYR_SYS_BYTEORDER_H_
#define FAKE_ZEPHYR_SYS_BYTEORDER_H_

#include <stdint.h>

static inline void sys_put_le16(uint16_t val, uint8_t dst[2])
{
	dst[0] = (uint8_t)(val & 0xff);
	dst[1] = (uint8_t)((val >> 8) & 0xff);
}

static inline uint16_t sys_get_le16(const uint8_t src[2])
{
	return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

#endif /* FAKE_ZEPHYR_SYS_BYTEORDER_H_ */
