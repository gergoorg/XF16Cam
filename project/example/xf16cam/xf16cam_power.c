#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "compiler.h"
#include "driver/chip/hal_adc.h"
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_prcm.h"
#include "driver/chip/hal_wdg.h"
#include "driver/chip/hal_wakeup.h"
#include "pm/pm.h"

#include "xf16cam_board.h"
#include "xf16cam_log.h"
#include "xf16cam_power.h"
#include "xf16cam_storage.h"

#define XF16CAM_BATTERY_CHANNEL       ADC_CHANNEL_6
#define XF16CAM_BATTERY_SAMPLES       (11U)
#define XF16CAM_BATTERY_FULL_SCALE_MV (4250U)
#define XF16CAM_WAKE_IO_PA20          (6U)

static XF16CamPowerInfo g_power;

int xf16cam_power_measure(void)
{
	uint16_t samples[XF16CAM_BATTERY_SAMPLES];
	uint32_t sample;
	unsigned int i;

	/* The ADC profile (2.5 V reference, the factory A9 settings) lives in
	 * xf16cam_board.c, which shares the converter with the CDS check. */
	if (xf16cam_board_adc_acquire() != 0)
		goto fail;
	for (i = 0; i < XF16CAM_BATTERY_SAMPLES; ++i) {
		if (HAL_ADC_Conv_Polling(XF16CAM_BATTERY_CHANNEL, &sample, 100) != HAL_OK) {
			xf16cam_board_adc_release();
			goto fail;
		}
		samples[i] = (uint16_t)(sample & 0xfff);
	}
	xf16cam_board_adc_release();
	/* Wi-Fi and the analogue microphone can occasionally disturb one ADC
	 * conversion. An in-place insertion sort gives a robust median without
	 * adding heap use or a generic sorting-library dependency. */
	for (i = 1; i < XF16CAM_BATTERY_SAMPLES; ++i) {
		uint16_t value = samples[i];
		unsigned int j = i;

		while (j > 0 && samples[j - 1] > value) {
			samples[j] = samples[j - 1];
			--j;
		}
		samples[j] = value;
	}
	g_power.raw = samples[XF16CAM_BATTERY_SAMPLES / 2U];
	/* Factory divider is approximately 1.7:1: raw * 2500/4096 * 17/10. */
	g_power.millivolts = (uint16_t)(((uint32_t)g_power.raw *
	                                XF16CAM_BATTERY_FULL_SCALE_MV + 2048U) / 4096U);
	g_power.valid = 1;
	printf("xf16cam battery: raw=%u approximate=%u mV (uncalibrated)\n",
	       g_power.raw, g_power.millivolts);
	return 0;

fail:
	g_power.valid = 0;
	printf("xf16cam battery: ADC read failed\n");
	return -1;
}

const XF16CamPowerInfo *xf16cam_power_info(void)
{
	return &g_power;
}

/* NO_PTZ only: PTZ boards have no wake button, so there would be no way back. */
#ifdef NO_PTZ
void xf16cam_power_hibernate(void)
{
	GPIO_InitParam input = {
		.mode = GPIOx_Pn_F0_INPUT,
		.driving = GPIO_DRIVING_LEVEL_1,
		.pull = GPIO_PULL_UP,
	};

	if (xf16cam_storage_unmount() != 0)
		printf("xf16cam power: SD eject failed before hibernation\n");
	xf16cam_board_prepare_sleep();
	HAL_GPIO_Init(GPIO_PORT_A, GPIO_PIN_20, &input);
	HAL_PRCM_SetWakeupDebClk0(0);
	HAL_PRCM_SetWakeupIOxDebSrc(XF16CAM_WAKE_IO_PA20, 0);
	HAL_PRCM_SetWakeupIOxDebounce(XF16CAM_WAKE_IO_PA20, 1);
	HAL_Wakeup_SetIO(XF16CAM_WAKE_IO_PA20, WKUPIO_WK_MODE_FALLING_EDGE,
	                 GPIO_PULL_UP);
	printf("xf16cam power: entering hibernation; press PA20 to wake\n");
	if (pm_enter_mode(PM_MODE_HIBERNATION) != 0)
		printf("xf16cam power: hibernation failed; rebooting\n");
	else
		printf("xf16cam power: hibernation returned unexpectedly; rebooting\n");
	/* A successful XR872 hibernation never returns and wakes through a cold
	 * boot. Recover the same way if platform PM rejects or exits the request;
	 * media and board services have already been quiesced by this point. */
	xf16cam_log_flush();
	HAL_PRCM_SetCPUABootFlag(PRCM_CPUA_BOOT_FROM_COLD_RESET);
	HAL_WDG_Reboot();
}
#endif /* NO_PTZ */
