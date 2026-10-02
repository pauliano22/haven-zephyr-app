/* TAC5301-Q1 register map -- only the registers this driver actually uses.
 *
 * Every address, bit field, and reset value below is read directly from the
 * real datasheet (Texas Instruments SLASFD9A, Rev. A, "TAC5301-Q1 Automotive
 * Mono Audio Codec", fetched and read in full on haven-dev-board-kicad's
 * TAC5301_BENCH_EXPERIMENT.md pass) -- nothing here is inferred from a
 * register *name* alone the way an earlier draft of that doc briefly did
 * (see its commit history for the DSP_CFG0/1 bit[3:2] and biquad-channel-
 * allocation corrections found while writing this file).
 *
 * Addressing: the device's register space is paged. Register 0x00 on every
 * page is PAGE_CFG (datasheet section 7.1.2.1) -- write the target page
 * number there before touching any other register on that page. The I2C
 * wire format is ONE byte of register address followed immediately by the
 * data byte(s) in a single write (datasheet section 6.4.1, Figure "I2C
 * Device Address and Register ... Data Byte ... Stop") -- NOT the ADAU1860's
 * 4-byte address scheme; see tac5301_control.c's reg_write()/reg_read().
 *
 * 7-bit I2C target address is fixed: 0x50 (datasheet 6.4.1, "the 7-bit I2C
 * target address is fixed to 7'b1010000").
 */
#ifndef HAVEN_TAC5301_REGS_H_
#define HAVEN_TAC5301_REGS_H_

#define TAC5301_I2C_ADDR 0x50

/* ── Page 0 registers ──────────────────────────────────────────────────── */
#define TAC5301_REG_PAGE_CFG   0x00 /* Any page: select the active page. */
#define TAC5301_REG_SW_RESET   0x01 /* bit0, self-clearing (datasheet 6.4.2). */

#define TAC5301_REG_MIXER_CFG0 0x2C
#define TAC5301_MIXER_EN_DAC_ASI_MIXER   (1u << 7)
#define TAC5301_MIXER_EN_SIDE_CHAIN      (1u << 6)
#define TAC5301_MIXER_EN_ADC_CH_MIXER    (1u << 5)
#define TAC5301_MIXER_EN_LOOPBACK_MIXER  (1u << 4) /* ADC-to-DAC loopback. */

#define TAC5301_REG_ADC_CH1_CFG0 0x50
/* bits[7:6] ADC_CH1_INSRC: 0=differential, 1=single-ended analog input. */
#define TAC5301_ADC_INSRC_SINGLE_ENDED (1u << 6)
/* bits[3:2] ADC_CH1_CM_TOL (coupling): 0=AC-coupled (default), 2=DC-coupled. */
#define TAC5301_ADC_COUPLING_AC (0u << 2)

/* DSP_CFG0 (ADC side) / DSP_CFG1 (DAC side): filter-type select, HPF select,
 * and -- the detail an earlier draft of the bench doc missed -- how many
 * biquads per channel are active. Reset default is 2 biquads/channel, NOT
 * the 3 Haven's 5-band split needs; bits[3:2] must be set to 3b (0b11) or
 * the 3rd biquad slot silently has no effect on the signal path even though
 * coefficients can still be written to it.
 */
#define TAC5301_REG_DSP_CFG0 0x72 /* ADC side. Reset 0x00. */
#define TAC5301_REG_DSP_CFG1 0x73 /* DAC side. Reset 0x18 (HPF=01b, BQ=10b preloaded -- see datasheet Table 7-87). */
#define TAC5301_DSP_DECIM_FILT_ULTRA_LOW_LATENCY (2u << 6) /* bits[7:6] = 10b */
#define TAC5301_DSP_INTERP_FILT_ULTRA_LOW_LATENCY (2u << 6)
#define TAC5301_DSP_HPF_SEL_1HZ (1u << 4) /* bits[5:4] = 01b, the reset default -- kept, not changed. */
#define TAC5301_DSP_BQ_CFG_3_PER_CHANNEL (3u << 2) /* bits[3:2] = 11b */

#define TAC5301_REG_CH_EN 0x76 /* Reset 0xCC: IN_CH1/2_EN, OUT_CH1/2_EN all on already. */

/* DAC channel 1's two differential sub-channels (A/B) each have their own
 * digital volume register; both are written together to stay matched.
 * Value 0 = a dedicated "muted" code (not part of the dB scale below it);
 * 1d=-100dB, 201d=0dB, 255d=+27dB, 0.5dB/step (datasheet 7.1.1.73/.75).
 */
#define TAC5301_REG_DAC_CH1A_DVOL 0x67
#define TAC5301_REG_DAC_CH1B_DVOL 0x69
#define TAC5301_DVOL_MUTE_CODE 0
#define TAC5301_DVOL_MIN_CODE  1   /* -100 dB */
#define TAC5301_DVOL_UNITY_CODE 201 /* 0 dB */
#define TAC5301_DVOL_MAX_CODE  255  /* +27 dB */

#define TAC5301_REG_PWR_CFG 0x78
#define TAC5301_PWR_ADC_PDZ     (1u << 7)
#define TAC5301_PWR_DAC_PDZ     (1u << 6)
#define TAC5301_PWR_MICBIAS_PDZ (1u << 5)

/* ── Page 1 ────────────────────────────────────────────────────────────── */
#define TAC5301_PAGE_1 1
#define TAC5301_REG_MICBIAS_CFG 0x73 /* Page 1. Reset 0xA0 = MICBIAS_VAL 1010b = 7.5V. */
#define TAC5301_MICBIAS_VAL_SHIFT 4

/* ── Biquad coefficient pages ──────────────────────────────────────────────
 * "3 biquads per channel" mode allocates filters across FOUR channel slots,
 * interleaved -- NOT the first 3 filters in sequence (datasheet Table 6-17
 * for ADC / Table 6-41 for DAC). For a mono part, only channel 1 is used,
 * which gets filters 1, 5, and 9 -- filters 2/3/4/6/7/8/10/11/12 belong to
 * channels 2-4 and do nothing on this part. Table 6-18 (ADC) / 6-42 (DAC)
 * give the filter -> page/register mapping; the three channel-1 slots are
 * reproduced here directly so nothing has to be re-derived from the tables
 * at call sites.
 *
 * Each biquad occupies 20 registers = 5 coefficients (N0, N1, N2, D1, D2) x
 * 4 bytes each, big-endian within each 32-bit coefficient (BYT1 = bits
 * [31:24] first on the wire, per the register map, e.g. 7.2.1's
 * ADC_BQ1_N0_BYT1..BYT4). The transfer function (datasheet Equation 2/4) is
 * H(z) = (N0 + N1 z^-1 + N2 z^-2) / (1 + D1 z^-1 + D2 z^-2) -- note this is
 * the *unnegated* textbook form (D1 = a1, D2 = a2 directly), unlike the
 * ADAU1860's FastDSP convention which stores feedback taps negated. This is
 * inferred from the register naming (N.../D... matching the standard
 * biquad transfer function, not a FastDSP-style "feedback tap" name) and
 * from the reset coefficients only producing a stable all-pass at N0=unity/
 * everything else 0 under this convention -- not an explicit sign-convention
 * statement in the datasheet text. Verify with a real coefficient readback
 * or a bench sweep before trusting this for a safety-relevant filter.
 *
 * Coefficient encoding: reset N0 = 0x7FFFFFFF, which is two's-complement
 * Q1.31's closest representable value to +1.0 (2^31 - 1, since Q1.31 cannot
 * represent exactly +1.0) -- a reasonable inference from the reset value,
 * not an explicit "Q1.31" statement in this datasheet. See
 * TAC5301_BENCH_EXPERIMENT.md (haven-dev-board-kicad) for the same caveat.
 */
#define TAC5301_BIQUAD_COEFF_COUNT 5 /* N0, N1, N2, D1, D2 */
#define TAC5301_BIQUAD_REG_COUNT 20  /* 5 coeffs x 4 bytes */

#define TAC5301_PAGE_ADC_BQ_1_6  8
#define TAC5301_PAGE_ADC_BQ_7_12 9
#define TAC5301_PAGE_DAC_BQ_1_6  16
#define TAC5301_PAGE_DAC_BQ_7_12 17

/* Channel-1 biquad A/B/C: (page, first register). */
#define TAC5301_ADC_CH1_BQ_A_PAGE TAC5301_PAGE_ADC_BQ_1_6
#define TAC5301_ADC_CH1_BQ_A_REG  8   /* filter 1, P8_R8-R27 */
#define TAC5301_ADC_CH1_BQ_B_PAGE TAC5301_PAGE_ADC_BQ_1_6
#define TAC5301_ADC_CH1_BQ_B_REG  88  /* filter 5, P8_R88-R107 */
#define TAC5301_ADC_CH1_BQ_C_PAGE TAC5301_PAGE_ADC_BQ_7_12
#define TAC5301_ADC_CH1_BQ_C_REG  48  /* filter 9, P9_R48-R67 */

#define TAC5301_DAC_CH1_BQ_A_PAGE TAC5301_PAGE_DAC_BQ_1_6
#define TAC5301_DAC_CH1_BQ_A_REG  8   /* filter 1, P16_R8-R27 */
#define TAC5301_DAC_CH1_BQ_B_PAGE TAC5301_PAGE_DAC_BQ_1_6
#define TAC5301_DAC_CH1_BQ_B_REG  88  /* filter 5, P16_R88-R107 */
#define TAC5301_DAC_CH1_BQ_C_PAGE TAC5301_PAGE_DAC_BQ_7_12
#define TAC5301_DAC_CH1_BQ_C_REG  48  /* filter 9, P17_R48-R67 */

#endif /* HAVEN_TAC5301_REGS_H_ */
