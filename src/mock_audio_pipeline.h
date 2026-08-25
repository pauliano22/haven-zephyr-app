/* Software-in-the-loop mock audio pipeline — bench simulation standing in
 * for the real ADAU1860 signal path until the DSP board arrives (see
 * adau1860_control.c's TODOs). Runs on the system workqueue, generates a
 * synthetic test-tone buffer on a timer, and processes it through a biquad
 * whose coefficients (frequency range) and output gain (volume) track
 * whatever's currently set via the Haven Audio Control Service
 * (gatt_audio_service.c) in real time.
 */
#ifndef HAVEN_MOCK_AUDIO_PIPELINE_H_
#define HAVEN_MOCK_AUDIO_PIPELINE_H_

/* Registers with gatt_audio_service's callbacks and starts the periodic
 * pipeline tick. Call once at boot, after gatt_audio_service_init().
 */
int mock_audio_pipeline_init(void);

#endif /* HAVEN_MOCK_AUDIO_PIPELINE_H_ */
