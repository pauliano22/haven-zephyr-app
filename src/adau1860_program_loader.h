/* Parses a SigmaStudio+-exported firmware blob and streams its write
 * actions to the ADAU1860 over I2C. See docs/sigmastudio-build-spec.md for
 * what that export needs to contain and docs/... for how to get one.
 */
#ifndef HAVEN_ADAU1860_PROGRAM_LOADER_H_
#define HAVEN_ADAU1860_PROGRAM_LOADER_H_

#include <stddef.h>
#include <stdint.h>

/* Parse and execute every write action in a SigmaStudio+ "V1" export
 * (magic "ADISIGM", sequential {instr, len, addr, payload} actions --
 * confirmed against the Linux kernel's sigmadsp.c reference parser, not
 * guessed at). Returns 0 if every action executed successfully and the
 * blob ended in a well-formed SIGMA_ACTION_END, a negative errno otherwise
 * (malformed blob, I2C failure, or truncated data).
 *
 * NOTE: SIGMA_ACTION_WRITESAFELOAD actions are deliberately NOT dispatched
 * as plain register writes -- see the .c file's comment on why a plain
 * write there would silently skip the glitch-free atomic-update mechanism
 * safeload exists for. They're currently logged and skipped, not applied.
 * Real safeload support needs the ADAU1860's specific staging-register
 * addresses, which aren't part of the exported blob itself.
 */
int adau1860_program_load(const uint8_t *blob, size_t len);

#endif /* HAVEN_ADAU1860_PROGRAM_LOADER_H_ */
