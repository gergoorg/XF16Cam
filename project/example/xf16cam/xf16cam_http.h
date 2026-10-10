#ifndef XF16CAM_HTTP_H
#define XF16CAM_HTTP_H

int xf16cam_http_start(void);
/* XR872 die temperature in tenths of a degree Celsius, INT_MIN when the
 * Wi-Fi firmware cannot report it. */
int xf16cam_http_chip_temperature(void);
/* SDK-cached startup state as short text ("Power-on", "Controlled reboot"). */
const char *xf16cam_http_boot_reason(void);

#endif
