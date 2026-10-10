#ifndef XF16CAM_BOARD_H
#define XF16CAM_BOARD_H

#include <stdint.h>

int xf16cam_board_init(void);
void xf16cam_board_set_ready(void);
void xf16cam_board_prepare_sleep(void);
#ifdef NO_PTZ
int xf16cam_board_mode_button_pressed(void);
#endif
/* Compiled on both variants; no-op and always 0 without an IR LED pin. */
void xf16cam_board_set_ir_led(int on);
int xf16cam_board_get_ir_led_on(void);
void xf16cam_board_set_led(int on);
int xf16cam_board_get_led_on(void);

int xf16cam_board_reset_button_pressed(void);
/* The ADC is shared by the CDS light check (PTZ) and the battery divider.
 * acquire() locks it and initialises it on first use; release() unlocks it
 * (and on NO_PTZ, with no permanent user, deinitialises it). Every
 * acquire() that returned 0 must be paired with a release(). */
int xf16cam_board_adc_acquire(void);
void xf16cam_board_adc_release(void);
uint32_t xf16cam_board_stack_min_free(void);
/* Cold reboot behind the update lock: ejects the SD card and flushes the
 * console mirror first. Returns (without rebooting) while an update holds
 * the lock. */
void xf16cam_board_reboot(void);

void xf16cam_board_init_hardware_watchdog(void);
void xf16cam_board_wdg_task(void *pvParameters);
void xf16cam_board_check_previous_crash(void);
const char *xf16cam_board_get_crash_task_name(void);
#endif
