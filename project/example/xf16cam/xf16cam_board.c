#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "kernel/os/os.h"
#include "driver/chip/hal_adc.h"
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_prcm.h"
#include "driver/chip/hal_rtc.h"
#include "driver/chip/hal_wdg.h"
#include "FreeRTOS.h"
#include "task.h"

#include "xf16cam_board.h"
#include "xf16cam_config.h"
#include "xf16cam_log.h"
#include "xf16cam_media.h"
#include "xf16cam_storage.h"
#include "xf16cam_sensor.h"
#include "xf16cam_ptz.h"

#ifdef NO_PTZ
#define XF16CAM_LED_PIN              GPIO_PIN_21
#define XF16CAM_MODE_BUTTON_PIN      GPIO_PIN_15
#define XF16CAM_RESET_BUTTON_PIN     GPIO_PIN_20
#define XF16CAM_GPIO_PORT            GPIO_PORT_A
#else
#define XF16CAM_LED_PIN              GPIO_PIN_20
#define XF16CAM_IR_LED_PIN           GPIO_PIN_22
#define XF16CAM_MODE_BUTTON_PIN      #error "PTZ version does not have a mode button"
#define XF16CAM_RESET_BUTTON_PIN     GPIO_PIN_19
#define XF16CAM_GPIO_PORT            GPIO_PORT_B

#endif

#define XF16CAM_BUTTON_POLL_MS       (50)
#define XF16CAM_BUTTON_DEBOUNCE_MS   (100)
#define XF16CAM_RESET_HOLD_MS        (3000)
/* Upstream runs this task on 1 KiB. Ours also drains the console mirror
 * (256-byte chunk), flushes it and unmounts the SD card on the watchdog
 * reboot path, none of which is in the measured high-water mark, so keep
 * the 2 KiB and read the System tab's spare-stack figure before trimming. */
#define XF16CAM_BOARD_STACK_SIZE     (2*1024)
#define XF16CAM_BOARD_WDG_STACK_SIZE (512)
#define XF16CAM_CAPTURE_STALL_MS     (30U * 1000U)
#ifndef NO_PTZ
#define XF16CAM_CDS_CHANNEL           ADC_CHANNEL_5
#define XF16CAM_CDS_SAMPLES           (10U)
#define XF16CAM_CDS_DARK_THRESHOLD    (1500U)
#define XF16CAM_CDS_CHECK_MS          (5000U)
#endif

static OS_Thread_t g_board_thread;
static OS_Thread_t g_board_wdg_thread;
static volatile int g_board_ready;
static volatile int g_board_sleeping;
static char g_crash_task_name[32];

#define CRASH_MAGIC_NUMBER  (0xAA)

static int xf16cam_button_pressed(GPIO_Pin pin)
{
	return HAL_GPIO_ReadPin(XF16CAM_GPIO_PORT, pin) == GPIO_PIN_LOW;
}

#ifdef NO_PTZ
int xf16cam_board_mode_button_pressed(void)
{
	return xf16cam_button_pressed(XF16CAM_MODE_BUTTON_PIN);
}
#endif

int xf16cam_board_reset_button_pressed(void)
{
	return xf16cam_button_pressed(XF16CAM_RESET_BUTTON_PIN);
}

#ifndef NO_PTZ
static int g_cds_adc_ready;

__xip_text
static int xf16cam_board_cds_adc_init(void)
{
	ADC_InitParam param;

	memset(&param, 0, sizeof(param));
	param.delay = 10;
	param.freq = 500000;
	param.vref_mode = 1;
	param.mode = ADC_CONTI_CONV;
	if (HAL_ADC_Init(&param) != HAL_OK)
		return -1;
	g_cds_adc_ready = 1;
	return 0;
}

__xip_text
static int xf16cam_board_cds_is_dark(void)
{
	uint16_t samples[XF16CAM_CDS_SAMPLES];
	uint32_t sample;
	uint32_t total = 0;
	unsigned int index;

	// The ADC is initialized once at startup; re-initializing it on every check
	// leaked heap on every cycle and drained it after a couple of hours.
	if (!g_cds_adc_ready && xf16cam_board_cds_adc_init() != 0)
		return -1;
	for (index = 0; index < XF16CAM_CDS_SAMPLES; ++index) {
		if (HAL_ADC_Conv_Polling(XF16CAM_CDS_CHANNEL, &sample, 100) != HAL_OK)
			return -1;
		samples[index] = (uint16_t)(sample & 0xfff);
	}
	for (index = 1; index < XF16CAM_CDS_SAMPLES; ++index) {
		uint16_t value = samples[index];
		unsigned int sorted = index;

		while (sorted > 0 && samples[sorted - 1] > value) {
			samples[sorted] = samples[sorted - 1];
			--sorted;
		}
		samples[sorted] = value;
	}
	for (index = 1; index < XF16CAM_CDS_SAMPLES - 1; ++index)
		total += samples[index];
	return total / (XF16CAM_CDS_SAMPLES - 2U) > XF16CAM_CDS_DARK_THRESHOLD;
}
#endif

static void xf16cam_board_reboot(void)
{
	if (xf16cam_update_begin() != 0) {
		printf("xf16cam reboot deferred: firmware update is active\n");
		return;
	}
	if (xf16cam_storage_unmount() != 0)
		printf("xf16cam board: SD eject failed before reboot\n");
	OS_MSleep(250);
	xf16cam_log_flush();	/* the reason printed above must leave the board */
	HAL_PRCM_SetCPUABootFlag(PRCM_CPUA_BOOT_FROM_COLD_RESET);
	HAL_WDG_Reboot();
}

static void xf16cam_board_task(void *arg)
{
	unsigned int mode_held_ms = 0;
	unsigned int reset_held_ms = 0;
	unsigned int blink_ms = 0;
	#ifndef NO_PTZ
	unsigned int cds_elapsed_ms = 0;
	int lamp_restored = 0;
	#endif
	int mode_handled = 0;
	int reset_handled = 0;
	int led = 0;

	(void)arg;
	while (1) {
		#ifdef NO_PTZ
		int mode_pressed = xf16cam_board_mode_button_pressed();
		#else
		if(g_ptz_ready && g_last_ptz_time != 0 &&
			OS_TicksToMSecs(OS_GetTicks()) - g_last_ptz_time > PTZ_IDLE_TIMEOUT_MS) {
			g_last_ptz_time = 0;
			xf16cam_ptz_power_down();
		}
		int mode_pressed = 0;
		#endif
		int reset_pressed = xf16cam_board_reset_button_pressed();

		xf16cam_log_poll();	/* one UDP datagram of console output, if any */

		// if (OS_TicksToMSecs(OS_GetTicks()) > 2U * 60U * 60U * 1000U) {
		// 	printf("xf16cam board: rebooting after 6 hours uptime\n");
		// 	xf16cam_board_reboot();
		// }

		// Fallback recovery if capture stalls before the 2-hour mark.
		// Only sessions that hold the camera count: last_frame_ms is
		// re-stamped when the camera is acquired, so a client that is
		// connected but not yet playing cannot trip this.
		if (xf16cam_media_capturing() > 0) {
			uint32_t now = OS_TicksToMSecs(OS_GetTicks());
			uint32_t last = xf16cam_media_info()->last_frame_ms;

			if (last == 0)
				last = now;
			if (now - last >= XF16CAM_CAPTURE_STALL_MS) {
				printf("xf16cam board: no camera frames for %lus with clients connected; rebooting\n",
				       (unsigned long)((now - last) / 1000U));
				xf16cam_board_reboot();
			}
		}

		if (g_board_sleeping) {
			if (led) {
				led = 0;
				HAL_GPIO_WritePin(XF16CAM_GPIO_PORT, XF16CAM_LED_PIN, GPIO_PIN_LOW);
			}
			OS_MSleep(XF16CAM_BUTTON_POLL_MS);
			continue;
		}
		if (!g_board_ready) {
			blink_ms += XF16CAM_BUTTON_POLL_MS;
			if (blink_ms >= 250) {
				blink_ms = 0;
				led = !led;
				HAL_GPIO_WritePin(XF16CAM_GPIO_PORT, XF16CAM_LED_PIN,
				                  led ? GPIO_PIN_HIGH : GPIO_PIN_LOW);
			}
		}

		if (!g_board_ready) {
			mode_held_ms = 0;
			reset_held_ms = 0;
			OS_MSleep(XF16CAM_BUTTON_POLL_MS);
			continue;
		}
		#ifndef NO_PTZ
		if (!lamp_restored) {
			lamp_restored = 1;
			led = 0;
			/* The boot blink ends here. On PTZ this LED is the illumination
			 * lamp rather than a status light, so restore what the user last
			 * chose instead of forcing it off. Keyed on its own flag, not on
			 * led: the blink can leave the lamp already dark, and the restore
			 * still has to run. */
			xf16cam_board_set_led(xf16cam_config_get()->led_on != 0);
		}
		cds_elapsed_ms += XF16CAM_BUTTON_POLL_MS;
		if (cds_elapsed_ms >= XF16CAM_CDS_CHECK_MS) {
			int dark = xf16cam_board_cds_is_dark() > 0;
			cds_elapsed_ms = 0;
			if (dark != xf16cam_board_get_ir_led_on())
				xf16cam_board_set_ir_led(dark);
		}
		#else
		if (!led) {
			led = 1;
			// non-PTZ version: LED is status LED, so turn it on when ready
			HAL_GPIO_WritePin(XF16CAM_GPIO_PORT, XF16CAM_LED_PIN, GPIO_PIN_HIGH);
		}
		#endif

		if (mode_pressed) {
			mode_held_ms += XF16CAM_BUTTON_POLL_MS;
		} else {
			if (!mode_handled && mode_held_ms >= XF16CAM_BUTTON_DEBOUNCE_MS) {
				XF16CamMediaMode next = xf16cam_config_get()->media_mode == XF16CAM_MEDIA_WEB ?
				                          XF16CAM_MEDIA_RTSP : XF16CAM_MEDIA_WEB;
				printf("xf16cam PA15: switching media mode to %s\n",
				       next == XF16CAM_MEDIA_WEB ? "WEB" : "RTSP");
				if (xf16cam_config_save_media(next) == 0)
					xf16cam_board_reboot();
			}
			mode_held_ms = 0;
			mode_handled = 0;
		}

		if (reset_pressed) {
			reset_held_ms += XF16CAM_BUTTON_POLL_MS;
			if (!reset_handled && reset_held_ms >= XF16CAM_RESET_HOLD_MS) {
				reset_handled = 1;
				printf("xf16cam PA20: restoring setup AP\n");
				if (xf16cam_config_save_ap() == 0)
					xf16cam_board_reboot();
			}
		} else {
			reset_held_ms = 0;
			reset_handled = 0;
		}

		OS_MSleep(XF16CAM_BUTTON_POLL_MS);
	}
}


// Pass the port prefix and numeric pin as adjacent printf arguments.
#define GPIO_PORT_TEXT(port) \
	(((port) == GPIO_PORT_A) ? "PA" : \
	 ((port) == GPIO_PORT_B) ? "PB" : "P?")
#define GPIO_TXT(port, pin) GPIO_PORT_TEXT(port), (unsigned int)(pin)

int xf16cam_board_init(void)
{
	GPIO_InitParam input = {
		.mode = GPIOx_Pn_F0_INPUT,
		.driving = GPIO_DRIVING_LEVEL_1,
		.pull = GPIO_PULL_UP,
	};
	GPIO_InitParam output = {
		.mode = GPIOx_Pn_F1_OUTPUT,
		.driving = GPIO_DRIVING_LEVEL_1,
		.pull = GPIO_PULL_NONE,
	};

	#ifdef NO_PTZ
	HAL_GPIO_Init(XF16CAM_GPIO_PORT, XF16CAM_MODE_BUTTON_PIN, &input);
	#else
	xf16cam_ptz_init();
	HAL_GPIO_Init(GPIO_PORT_A, XF16CAM_IR_LED_PIN, &output);
	HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_IR_LED_PIN, GPIO_PIN_LOW);
	#endif
	HAL_GPIO_Init(XF16CAM_GPIO_PORT, XF16CAM_RESET_BUTTON_PIN, &input);
	HAL_GPIO_Init(XF16CAM_GPIO_PORT, XF16CAM_LED_PIN, &output);
	HAL_GPIO_WritePin(XF16CAM_GPIO_PORT, XF16CAM_LED_PIN, GPIO_PIN_LOW);
	printf("xf16cam board: PA15 mode=%s PA20 reset=%s %s%u status LED\n",
		#ifdef NO_PTZ
	       xf16cam_board_mode_button_pressed() ? "pressed" : "released",
		#else
	       "N/A",
		#endif
		xf16cam_board_reset_button_pressed() ? "pressed" : "released",
		GPIO_TXT(XF16CAM_GPIO_PORT, XF16CAM_LED_PIN)
	);

	if (OS_ThreadCreate(&g_board_thread, "xf16cam-board", xf16cam_board_task,
	                    NULL, OS_THREAD_PRIO_APP, XF16CAM_BOARD_STACK_SIZE) != OS_OK) {
		printf("xf16cam board thread create failed\n");
		return -1;
	}

	xf16cam_board_init_hardware_watchdog();
	if (OS_ThreadCreate(&g_board_wdg_thread, "xf16cam-wdg", xf16cam_board_wdg_task,
		 				NULL, OS_PRIORITY_IDLE, XF16CAM_BOARD_WDG_STACK_SIZE) != OS_OK) {
		printf("xf16cam board watchdog thread create failed\n");
		return -1;
	}

	return 0;
}

void xf16cam_board_set_ready(void)
{
	g_board_ready = 1;
}

void xf16cam_board_prepare_sleep(void)
{
	g_board_sleeping = 1;
	g_board_ready = 0;
	HAL_GPIO_WritePin(XF16CAM_GPIO_PORT, XF16CAM_LED_PIN, GPIO_PIN_LOW);
	#ifndef NO_PTZ
	if (g_ptz_ready) {
		xf16cam_ptz_power_down();
	}
	#endif
}

/* Every manual lamp toggle comes through here, so this is where the choice
 * persists. The boot restore passes the stored value back and writes nothing. */
__xip_text
void xf16cam_board_set_led(int on)
{
	HAL_GPIO_WritePin(XF16CAM_GPIO_PORT, XF16CAM_LED_PIN,
	                  on ? GPIO_PIN_HIGH : GPIO_PIN_LOW);
	xf16cam_config_save_led(on);
}

int xf16cam_board_get_led_on(void)
{
	return HAL_GPIO_ReadPin(XF16CAM_GPIO_PORT, XF16CAM_LED_PIN) == GPIO_PIN_HIGH;
}

//IR LED control functions for PTZ version
__xip_text
void xf16cam_board_set_ir_led(int on)
{
	#ifdef XF16CAM_IR_LED_PIN
	HAL_GPIO_WritePin(GPIO_PORT_A, XF16CAM_IR_LED_PIN,
	                  on ? GPIO_PIN_HIGH : GPIO_PIN_LOW);
	xf16cam_sensor_switch_cam_sensor_mode(on);
	#endif
}

__xip_text
int xf16cam_board_get_ir_led_on(void)
{
	#ifdef XF16CAM_IR_LED_PIN
	return HAL_GPIO_ReadPin(GPIO_PORT_A, XF16CAM_IR_LED_PIN) == GPIO_PIN_HIGH;
	#else
	return 0;
	#endif
}

__xip_text
uint32_t xf16cam_board_stack_min_free(void)
{
	return OS_ThreadGetStackMinFreeSize(&g_board_thread);
}

// Stack overflow hook for FreeRTOS tasks, method name must be exactly vApplicationStackOverflowHook
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
  printf("Stack overflow in task %s\n", pcTaskName);

  __disable_irq(); // Disable interrupts to prevent further damage

  uint32_t name_address = (uint32_t)pcTaskName;

  uint8_t byte0 = (name_address >> 24) & 0xFF;
  uint8_t byte1 = (name_address >> 16) & 0xFF;
  uint8_t byte2 = (name_address >> 8) & 0xFF;
  uint8_t byte3 = name_address & 0xFF;

  HAL_RTC_SetYYMMDD(byte0, byte1, byte2, byte3);

  HAL_RTC_SetDDHHMMSS((RTC_WeekDay)CRASH_MAGIC_NUMBER, 0, 0, 0);

  // Reboot the system to recover from stack overflow
  HAL_WDG_Reboot();
  while (1) {
    __NOP(); // Do nothing, just wait for the system to reboot
  }
}

const char *xf16cam_board_get_crash_task_name(void)
{
    return g_crash_task_name;
}

void xf16cam_board_check_previous_crash(void) {
  uint8_t b0, b1, b2, b3;
  RTC_WeekDay wday;
  uint8_t h, m, s;

  HAL_RTC_GetYYMMDD(&b0, &b1, &b2, &b3);
  HAL_RTC_GetDDHHMMSS(&wday, &h, &m, &s);

  if ((uint8_t)wday == CRASH_MAGIC_NUMBER) {
    uint32_t recovered_address = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) |
                                 ((uint32_t)b2 << 8) | (uint32_t)b3;

    if (recovered_address >= 0x10000000 && recovered_address < 0x50000000) {
      char *full_task_name = (char *)recovered_address;
      memcpy(g_crash_task_name, full_task_name, sizeof(g_crash_task_name));
      g_crash_task_name[11] = '\0'; // Ensure null termination
      printf("STACK OVERFLOW in task: %s\n", full_task_name);
    } else {
      sprintf(g_crash_task_name, "INVALID");
      printf(
          "STACK OVERFLOW occurred, but the task name address is invalid.\n");
    }
    // Clear the crash information from the RTC
    HAL_RTC_SetYYMMDD(0, 26, 1, 1);
    HAL_RTC_SetDDHHMMSS(RTC_WDAY_MONDAY, 0, 0, 0);
  } else {
    sprintf(g_crash_task_name, "NONE"); // No previous crash detected
  }
}

void xf16cam_board_init_hardware_watchdog(void)
{
    WDG_InitParam param;

    param.hw.event = WDG_EVT_RESET;
    param.hw.timeout = WDG_TIMEOUT_5SEC;
	param.hw.resetCycle = WDG_DEFAULT_RESET_CYCLE;

    HAL_WDG_Init(&param);
    HAL_WDG_Start();
}

void xf16cam_board_wdg_task(void *pvParameters)
{
    (void)pvParameters;

    while (1) {
        HAL_WDG_Feed();
        OS_MSleep(2000);
    }
}

/* Callback triggered by the GCC compiler if a stack canary check fails */
void __attribute__((noreturn)) __stack_chk_fail(void) {
  uint32_t *sp;
  uint32_t lr;

  // 1. Get the current Link Register (return address of the caller)
  lr = (uint32_t)__builtin_return_address(0);

  // 2. Get the current Stack Pointer
  __asm__ volatile("mov %0, sp" : "=r"(sp));

  printf("\r\n============================================\r\n");
  printf("!!! CRITICAL: Stack Smashing Detected !!!\r\n");
  printf("Triggered near address: 0x%08X\r\n", lr);
  printf("Current Stack Pointer (SP): %p\r\n", (void *)sp);
  printf("============================================\r\n");

  // 3. Raw Hex Dump of the Stack Memory
  printf("Stack Dump (Top 64 words):\r\n");
  for (int i = 0; i < 64; i++) {
    if (i % 4 == 0) {
      printf("\r\n0x%08X: ", (uint32_t)(sp + i));
    }
    printf("0x%08X ", sp[i]);
  }
  printf("\r\n============================================\r\n");

  //Decode the address to find the corresponding source code line using addr2line:
  //arm-none-eabi-addr2line -e your_firmware.elf 0x<THE_LR_ADDRESS>

  // 4. Force a reboot using the hardware watchdog
  HAL_WDG_Reboot();
  while (1) {
    __NOP(); // Do nothing, just wait for the system to reboot
  }
}

/* Global canary variable required by the GCC compiler stack protector */
uintptr_t __stack_chk_guard = 0xDEADC0DE; // Change this to a random runtime value during boot if possible