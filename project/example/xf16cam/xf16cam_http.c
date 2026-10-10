#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "compiler.h"
#include "kernel/os/os.h"
#include "driver/chip/hal_flash.h"
#include "driver/chip/hal_prcm.h"
#include "driver/chip/hal_wdg.h"
#include "driver/chip/system_chip.h"
#include "lwip/sockets.h"
#include "net/wlan/wlan.h"
#include "net/wlan/wlan_ext_req.h"
#include "ota/ota.h"
#include "sys/sram_heap.h"
#include "common/framework/net_ctrl.h"
#include "common/framework/sysinfo.h"

#include "xf16cam_config.h"
#include "xf16cam_audio.h"
#include "xf16cam_board.h"
#include "xf16cam_http.h"
#include "xf16cam_log.h"
#include "xf16cam_media.h"
#include "xf16cam_mqtt.h"
#include "xf16cam_net.h"
#include "xf16cam_power.h"
#include "xf16cam_sensor.h"
#include "xf16cam_storage.h"
#include "xf16cam_ptz.h"
#include "xf16cam_version.h"
#include "xf16cam_xip.h"

#define XF16CAM_HTTP_PORT         (80)
#define XF16CAM_HTTP_REQUEST_SIZE (2048)
#define XF16CAM_HTTP_SCAN_MAX     (12)
#define XF16CAM_HTTP_STACK_SIZE   (3 * 1024)
#define XF16CAM_OTA_MAX_SIZE      (372 * 1024)
#define XF16CAM_HTTP_TIMEOUT_MS   (15000)
#define XF16CAM_HTTP_HEADER_MS    (5000)
#define XF16CAM_OTA_QUIESCE_MS    (10000U)
#define XF16CAM_OTA_UPLOAD_MS     (180000U)

enum {
	XF16CAM_HTTP_KEEP_RUNNING = 0,
	XF16CAM_HTTP_COLD_REBOOT,
	XF16CAM_HTTP_OTA_REBOOT,
	XF16CAM_HTTP_HIBERNATE,
	XF16CAM_HTTP_DETACH_CLIENT,
};

static OS_Thread_t g_http_thread;
/* Requests and scan results are consumed by the same HTTP task and never
 * coexist. Sharing their storage returns 912 bytes to the SRAM heap. */
static union {
	char request[XF16CAM_HTTP_REQUEST_SIZE];
	wlan_sta_ap_t scan_results[XF16CAM_HTTP_SCAN_MAX];
} g_http_workspace;
_Static_assert(sizeof(g_http_workspace.scan_results) <=
	       sizeof(g_http_workspace.request),
	       "HTTP scan results exceed the shared workspace");
#define g_request      g_http_workspace.request
#define g_scan_results g_http_workspace.scan_results
static uint32_t g_flash_jedec;
static uint32_t g_flash_size;

static void xf16cam_http_message(int fd, const char *status, const char *message);

__xip_text
__attribute__((noinline))
const char *xf16cam_http_boot_reason(void)
{
	switch (SysGetStartupState()) {
	case SYS_POWERON:            return "Power-on";
	case SYS_WATCHDOG_CHIP_RST: return "Watchdog (chip)";
	case SYS_WATCHDOG_CPU_RST:  return "Watchdog (CPU)";
	case SYS_SLEEP:              return "Sleep wake";
	case SYS_STANDBY:            return "Standby wake";
	case SYS_HIBERNATION:        return "Hibernation wake";
	case SYS_REBOOT:             return "Controlled reboot";
	case SYS_CPU_RST:            return "Controlled CPU reset";
	case SYS_NVIC_RST:           return "NVIC CPU reset";
	default:                     return "Unknown";
	}
}

__xip_text
__attribute__((noinline))
int xf16cam_http_chip_temperature(void)
{
	wlan_ext_temp_volt_get_t temperature;
	int32_t value;

	if (g_wlan_netif == NULL ||
	    wlan_ext_request(g_wlan_netif, WLAN_EXT_CMD_GET_TEMP_VOLT,
	                     (int)&temperature) != 0)
		return INT_MIN;
	value = temperature.Temperature;
	return value >= 0 ? (int)((value * 10 + 8) / 16) :
	                    -(int)((-value * 10 + 8) / 16);
}

__attribute__((noinline))
static void xf16cam_http_flash_info(uint32_t *jedec, uint32_t *size)
{
	struct FlashDev *device = getFlashDev(0);
	struct FlashChip *chip = device != NULL ? getFlashChip(device) : NULL;
	uint32_t capacity;

	*jedec = 0;
	*size = 0;
	if (chip == NULL || HAL_Flash_Open(0, 1000) != HAL_OK)
		return;
	device->drv->open(chip);
	chip->jedecID(chip, jedec);
	(device->drv->close)(chip);
	HAL_Flash_Close(0);

	/* The third JEDEC byte is the binary capacity exponent for SPI NOR. */
	capacity = (*jedec >> 16) & 0xff;
	if (capacity >= 16 && capacity < 32)
		*size = 1UL << capacity;
}

__xip_rodata static const char g_page_head[] =
	"<!doctype html><html><head><meta charset=utf-8>"
	"<meta name=viewport content='width=device-width,initial-scale=1'>"
	"<link rel=\"icon\" type=\"image/svg+xml\" href=\"data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0idXRmLTgiPz4KPHN2ZyBmaWxsPSIjMDAwMDAwIiB3aWR0aD0iMzJweCIgaGVpZ2h0PSIzMnB4IiB2aWV3Qm94PSIwIDAgMjQgMjQiIGlkPSJjY3R2LWNhbWVyYSIgZGF0YS1uYW1lPSJMaW5lIENvbG9yIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIGNsYXNzPSJpY29uIGxpbmUtY29sb3IiPjxwYXRoIGlkPSJwcmltYXJ5IiBkPSJNMTYuMTcsMTMuM0ExNS45MiwxNS45MiwwLDAsMSwxOCwyMUg2YTE1LjkyLDE1LjkyLDAsMCwxLDEuODMtNy43LDYsNiwwLDAsMCw4LjM0LDBaIiBzdHlsZT0iZmlsbDogbm9uZTsgc3Ryb2tlOiByZ2IoMCwgMCwgMCk7IHN0cm9rZS1saW5lY2FwOiByb3VuZDsgc3Ryb2tlLWxpbmVqb2luOiByb3VuZDsgc3Ryb2tlLXdpZHRoOiAyOyI+PC9wYXRoPjxjaXJjbGUgaWQ9InByaW1hcnktMiIgZGF0YS1uYW1lPSJwcmltYXJ5IiBjeD0iMTIiIGN5PSI5IiByPSI2IiBzdHlsZT0iZmlsbDogbm9uZTsgc3Ryb2tlOiByZ2IoMCwgMCwgMCk7IHN0cm9rZS1saW5lY2FwOiByb3VuZDsgc3Ryb2tlLWxpbmVqb2luOiByb3VuZDsgc3Ryb2tlLXdpZHRoOiAyOyI+PC9jaXJjbGU+PHBhdGggaWQ9InNlY29uZGFyeSIgZD0iTTE0LDlhMiwyLDAsMSwxLTItMkEyLDIsMCwwLDEsMTQsOVpNNCwyMUgyMCIgc3R5bGU9ImZpbGw6IG5vbmU7IHN0cm9rZTogcmdiKDQ0LCAxNjksIDE4OCk7IHN0cm9rZS1saW5lY2FwOiByb3VuZDsgc3Ryb2tlLWxpbmVqb2luOiByb3VuZDsgc3Ryb2tlLXdpZHRoOiAyOyI+PC9wYXRoPjwvc3ZnPg==\">"
	"<title>XF16Cam</title><style>"
	":root{--ink:#18212b;--muted:#647281;--line:#dbe2e8;--brand:#176b5b;--bg:#edf2f4}"
	"*{box-sizing:border-box}body{font:15px system-ui;margin:0;background:var(--bg);color:var(--ink)}"
	"header,main{width:min(1040px,calc(100% - 28px));margin:auto}header{display:flex;align-items:center;"
	"justify-content:space-between;padding:18px 0 12px}h1{font-size:1.45rem;margin:0}h2{font-size:1.05rem;margin:0 0 14px}"
	"p{line-height:1.45}.meta{color:var(--muted);font-size:.85rem}.pill{padding:5px 9px;"
	"border-radius:999px;background:#dcece8;color:#125648;font-weight:650}.viewer,.card{background:#fff;border:1px solid var(--line);"
	"border-radius:12px;box-shadow:0 2px 8px #18212b12}.viewer{padding:14px;margin-bottom:14px}.viewerTop{display:flex;"
	"justify-content:space-between;align-items:center;gap:10px;margin-bottom:10px}.screen{display:grid;place-items:center;"
	"min-height:240px;background:#111820;border-radius:8px;overflow:hidden;color:#aeb9c2}.screen img{display:block;width:100%;"
	"max-width:720px;max-height:70vh;object-fit:contain}.empty{text-align:center;padding:30px}.tabs{display:flex;gap:5px;overflow:auto;padding:5px;background:#dfe6ea;"
	"border-radius:10px;margin:0 0 14px;position:sticky;top:0;z-index:2}.tabs button{flex:1;min-width:max-content;border:0;"
	"background:transparent}.tabs button.on{background:#fff;color:var(--brand);box-shadow:0 1px 4px #0002}.panel{display:none}.panel.on{display:grid;"
	"grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}.card{padding:16px}.wide{grid-column:1/-1}.grid{display:grid;"
	"grid-template-columns:max-content 1fr;gap:7px 14px}.grid b{color:#43515e}form{margin:8px 0}label{display:block;margin:8px 0}"
	"input,button{font:inherit;min-height:44px;padding:9px 11px;margin:4px 0;border:1px solid #b9c5cd;border-radius:7px}input{width:100%;background:#fff;font-size:16px}"
	"button{background:#f7f9fa}button.primary{background:var(--brand);border-color:var(--brand);color:#fff}"
	"small{color:var(--muted)}a{color:#096b99}.rtsp{overflow-wrap:anywhere}.controls{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px;margin-top:10px}.controls>div{min-width:0}.led{display:inline-block;margin:8px 7px 8px 0}.nets{display:grid;gap:7px;margin:10px 0}.net{width:100%;display:flex;gap:10px;"
	"align-items:center;justify-content:space-between;text-align:left;margin:0;background:#fff}.net span:first-child{min-width:0;overflow-wrap:anywhere}.net span:last-child{flex:none;white-space:nowrap}.net.sel{border-color:var(--brand);"
	"box-shadow:0 0 0 2px #176b5b33;background:#f3faf8}.net.empty{justify-content:center;color:var(--muted)}@media(max-width:680px){header{align-items:flex-start}.panel.on{grid-template-columns:1fr}"
	".wide{grid-column:auto}.screen{min-height:180px}.grid{grid-template-columns:1fr}.controls{grid-template-columns:1fr}.grid b{margin-top:5px}}</style></head><body>";

__xip_text
static int xf16cam_http_send_all(int fd, const void *data, size_t length)
{
	const uint8_t *p = data;

	while (length > 0) {
		int sent = send(fd, p, length, 0);
		if (sent <= 0)
			return -1;
		p += sent;
		length -= sent;
	}
	return 0;
}

__xip_text
static void xf16cam_http_send_text(int fd, const char *text)
{
	xf16cam_http_send_all(fd, text, strlen(text));
}

/* Static markup and the status/body pair of an error page are the other two
 * kinds of literal in this file. Like XF16CAM_XIP_FORMAT they belong in XIP
 * flash, not the app slot -- see xf16cam_xip.h for why. Every string in this
 * file should go through one of the three macros.
 *
 * XF16CAM_HTTP_MESSAGE gives each of the ~45 call sites its own copy of its
 * status string rather than sharing one deduplicated "409 Conflict", trading
 * flash there is plenty of for the slot CI gates on. */
#define XF16CAM_HTTP_SEND_LITERAL(fd, literal) do { \
	__xip_rodata static const char XF16CAM_XIP_JOIN(g_http_text_, __LINE__)[] = literal; \
	xf16cam_http_send_all((fd), XF16CAM_XIP_JOIN(g_http_text_, __LINE__), \
	                      sizeof(XF16CAM_XIP_JOIN(g_http_text_, __LINE__)) - 1); \
} while (0)

#define XF16CAM_HTTP_MESSAGE(fd, status, message) do { \
	__xip_rodata static const char XF16CAM_XIP_JOIN(g_http_st_, __LINE__)[] = status; \
	__xip_rodata static const char XF16CAM_XIP_JOIN(g_http_ms_, __LINE__)[] = message; \
	xf16cam_http_message((fd), XF16CAM_XIP_JOIN(g_http_st_, __LINE__), \
	                     XF16CAM_XIP_JOIN(g_http_ms_, __LINE__)); \
} while (0)

__xip_text
static void xf16cam_http_runtime(int fd, char *dynamic, size_t size)
{
	uint32_t uptime = OS_TicksToMSecs(OS_GetTicks()) / 1000U;
	int temperature = xf16cam_http_chip_temperature();
	char temperature_text[20];
	int length;

	if (temperature == INT_MIN)
		XF16CAM_XIP_FORMAT(temperature_text, sizeof(temperature_text), "Unavailable");
	else
		XF16CAM_XIP_FORMAT(temperature_text, sizeof(temperature_text), "%s%d.%d &deg;C",
		         temperature < 0 ? "-" : "", abs(temperature) / 10,
		         abs(temperature) % 10);
	length = XF16CAM_XIP_FORMAT(dynamic, size,
	                  "<section class=card><h2>Runtime</h2><div class=grid>"
	                  "<b>Uptime</b><span>%lu days %02lu:%02lu:%02lu</span>"
	                  "<b>Boot reason</b><span>%s</span>"
	                  "<b>XF16 chip temperature</b><span>%s</span>"
	                  "</div></section>",
	                  (unsigned long)(uptime / 86400U),
	                  (unsigned long)((uptime / 3600U) % 24U),
	                  (unsigned long)((uptime / 60U) % 60U),
	                  (unsigned long)(uptime % 60U), xf16cam_http_boot_reason(),
	                  temperature_text);
	if (length > 0 && (size_t)length < size)
		xf16cam_http_send_all(fd, dynamic, (size_t)length);
}

__xip_text
static void xf16cam_http_begin(int fd, const char *status, const char *type)
{
	char header[192];
	int length = XF16CAM_XIP_FORMAT(header, sizeof(header),
	                      "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
	                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
	                      status, type);
	xf16cam_http_send_all(fd, header, length);
}

__xip_text
static void xf16cam_http_begin_length(int fd, const char *status, const char *type,
	                                  size_t body_length)
{
	char header[224];
	int length = XF16CAM_XIP_FORMAT(header, sizeof(header),
	                      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
	                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
	                      status, type, (unsigned long)body_length);
	xf16cam_http_send_all(fd, header, length);
}

__xip_text
static void xf16cam_http_send_escaped(int fd, const uint8_t *text, size_t length,
	                                  int json)
{
	size_t i;
	char encoded[8];

	for (i = 0; i < length; ++i) {
		const char *replacement = NULL;
		switch (text[i]) {
		case '&': replacement = json ? "&" : "&amp;"; break;
		case '<': replacement = json ? "<" : "&lt;"; break;
		case '>': replacement = json ? ">" : "&gt;"; break;
		case '"': replacement = json ? "\\\"" : "&quot;"; break;
		case '\\': replacement = json ? "\\\\" : "\\"; break;
		default: break;
		}
		if (replacement != NULL) {
			xf16cam_http_send_text(fd, replacement);
		} else if ((text[i] >= 0x20 && text[i] < 0x7f) || text[i] >= 0x80) {
			xf16cam_http_send_all(fd, &text[i], 1);
		} else if (json) {
			XF16CAM_XIP_FORMAT(encoded, sizeof(encoded), "\\u%04x", text[i]);
			xf16cam_http_send_text(fd, encoded);
		}
	}
}

#ifdef XF16CAM_MQTT
__xip_rodata static const char g_mqtt_status_off[] = "disabled";
__xip_rodata static const char g_mqtt_status_connecting[] = "connecting";
__xip_rodata static const char g_mqtt_status_connected[] = "connected";

/* IP literal today; the character set also admits a hostname for the day
 * DNS is linked back in. Keeps the value safe to embed in JSON unescaped. */
__xip_text
static int xf16cam_http_mqtt_host_valid(const char *host)
{
	for (; *host != '\0'; ++host) {
		if (!isalnum((unsigned char)*host) && *host != '.' && *host != '-' &&
		    *host != ':')
			return 0;
	}
	return 1;
}

/* Second column of the Network tab. Static markup goes out as literals and
 * only the numbers are formatted, to keep this frame off the HTTP stack. */
__xip_text
static void xf16cam_http_mqtt_card(int fd, const XF16CamConfig *config)
{
	const XF16CamMqttInfo *mqtt = xf16cam_mqtt_info();
	const char *status = !mqtt->enabled ? g_mqtt_status_off :
	                     mqtt->connected ? g_mqtt_status_connected :
	                                       g_mqtt_status_connecting;
	char dynamic[64];
	int length;

	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "<section class=card><h2>MQTT (Home Assistant)</h2>"
	                  "<p>Publishes the camera state, a JPEG every few seconds in browser-video mode, "
	                  "and Home Assistant device discovery. The broker is an IP address; blank turns MQTT off.</p>"
	                  "<form method=post action=/api/mqtt><label>Broker IP"
	                  "<input name=host maxlength=31 autocapitalize=none spellcheck=false placeholder=192.168.0.2 value=\"");
	xf16cam_http_send_escaped(fd, (const uint8_t *)config->u.mqtt.host,
	                          strlen(config->u.mqtt.host), 0);
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "\"></label><label>Port<input type=number name=port min=1 max=65535 value=");
	length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic), "%u",
	                            (unsigned int)xf16cam_config_mqtt_port(config));
	xf16cam_http_send_all(fd, dynamic, (size_t)length);
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "></label><label>Username"
	                  "<input name=user maxlength=31 autocapitalize=none spellcheck=false value=\"");
	xf16cam_http_send_escaped(fd, (const uint8_t *)config->u.mqtt.user,
	                          strlen(config->u.mqtt.user), 0);
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "\"></label><label>Password"
	                  "<input type=password name=pass maxlength=31 autocomplete=new-password>"
	                  "<small>Blank keeps the saved password for the same broker.</small></label>"
	                  "<label>Image interval (seconds)"
	                  "<input type=number name=interval min=1 max=255 value=");
	length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic), "%u",
	                            (unsigned int)xf16cam_config_mqtt_interval(config));
	xf16cam_http_send_all(fd, dynamic, (size_t)length);
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "></label><button class=primary type=submit>Save and reboot</button></form>"
	                  "<p>Status: ");
	xf16cam_http_send_text(fd, status);
	length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic),
	                            ", %lu images sent, %lu reconnects</p></section>",
	                            (unsigned long)mqtt->images,
	                            (unsigned long)mqtt->disconnects);
	xf16cam_http_send_all(fd, dynamic, (size_t)length);
}
#endif

__xip_text
__attribute__((noinline))
static void xf16cam_http_page(int fd)
{
	const XF16CamConfig *config = xf16cam_config_get();
	const XF16CamAudioInfo *audio = xf16cam_audio_info();
	const XF16CamMediaInfo *media = xf16cam_media_info();
	const XF16CamStorageInfo *storage = xf16cam_storage_info();
	const XF16CamPowerInfo *power = xf16cam_power_info();
	const struct sysinfo *sysinfo = sysinfo_get();
	int camera_available = xf16cam_sensor_available();
	char camera_detail[48];
	char camera_output[24];
	char dynamic[640];
	int length;
	/* Hoisted out of the Device-grid format arguments below: a #ifdef cannot
	 * appear inside a macro invocation, and XF16CAM_XIP_FORMAT is a macro.
	 * xf16cam_http_system_json() resolves the same split the same way. */
#ifdef NO_PTZ
	const char *mode_button = xf16cam_board_mode_button_pressed() ? "pressed" : "released";
#else
	const char *mode_button = "N/A";
#endif

	if (camera_available) {
		XF16CAM_XIP_FORMAT(camera_detail, sizeof(camera_detail), "%s &middot; %ux%u JPEG",
		         xf16cam_sensor_name(), (unsigned int)xf16cam_sensor_width(),
		         (unsigned int)xf16cam_sensor_height());
		XF16CAM_XIP_FORMAT(camera_output, sizeof(camera_output), "%ux%u",
		         (unsigned int)xf16cam_sensor_width(),
		         (unsigned int)xf16cam_sensor_height());
	} else {
		XF16CAM_XIP_FORMAT(camera_detail, sizeof(camera_detail), "No supported sensor detected");
		XF16CAM_XIP_FORMAT(camera_output, sizeof(camera_output), "Unavailable");
	}

	xf16cam_http_begin(fd, "200 OK", "text/html; charset=utf-8");
	xf16cam_http_send_text(fd, g_page_head);
	length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic),
	                  "<header><div><h1>XF16Cam</h1><div class=meta>Firmware %s</div></div>"
	                  "<div><span class=pill>%s</span> <span class=meta>%s</span></div></header><main>"
	                  "<section class=viewer><div class=viewerTop><div><b>Live camera</b>"
	                  "<div class=meta>%s</div></div><span class=pill>%s</span></div>"
	                  "<div class=screen>",
	                  XF16CAM_VERSION,
	                  xf16cam_net_mode() == XF16CAM_WIFI_STA ? "Station" : "Open AP",
	                  xf16cam_net_ip(), camera_detail,
	                  !camera_available ? "Offline" :
	                  config->media_mode == XF16CAM_MEDIA_WEB ? "Browser MJPEG" : "RTSP");
	xf16cam_http_send_all(fd, dynamic, length);
	if (config->media_mode == XF16CAM_MEDIA_WEB) {
		if (camera_available)
			XF16CAM_HTTP_SEND_LITERAL(fd,
			                  "<img id=video alt='Live camera'></div>");
		else
			XF16CAM_HTTP_SEND_LITERAL(fd,
				                  "<div class=empty><b>Camera unavailable</b><p>Connect a sensor and reboot.</p></div></div>");
	} else if (camera_available) {
		length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic),
		                  "<div class=rtsp><a href='rtsp://%s:8554/stream'>rtsp://%s:8554/stream</a>"
		                  "<br><small>Open this address in VLC or another RTSP player.</small></div></div>",
		                  xf16cam_net_ip(), xf16cam_net_ip());
		xf16cam_http_send_all(fd, dynamic, length);
	} else {
		XF16CAM_HTTP_SEND_LITERAL(fd,
		                  "<div class=empty><b>Camera unavailable</b><p>Connect a sensor to enable RTSP.</p></div></div>");
	}
	XF16CAM_HTTP_SEND_LITERAL(fd,"<div class=controls><div>");
	if ((config->media_mode == XF16CAM_MEDIA_WEB || !camera_available) && audio->available) {
		XF16CAM_HTTP_SEND_LITERAL(fd,
		                  "<button type=button id=listen onclick=toggleAudio()>Listen</button> "
		                  "<span id=audioState aria-live=polite>Audio stopped</span>"
		                  "<script>let a,r,n=0;function u(v){let x=(~v)&255,t=((x&15)<<3)+132;"
		                  "t<<=(x&112)>>4;return((x&128)?132-t:t-132)/32768}async function toggleAudio(){"
		                  "let b=document.querySelector('#listen'),s=document.querySelector('#audioState');"
		                  "if(a){let c=a,q=r;a=r=null;b.disabled=true;try{if(q)await q.cancel()}catch(e){}"
		                  "try{await c.close()}catch(e){}b.disabled=false;b.textContent='Listen';s.textContent='Audio stopped';return}"
		                  "let c;try{c=new AudioContext;a=c;await c.resume();let f=await fetch('/stream.pcmu');if(!f.ok||!f.body)throw 0;"
		                  "let p=f.body.getReader();if(a!==c){await p.cancel();return}r=p;b.textContent='Stop audio';s.textContent='Listening';"
		                  "n=c.currentTime+.15;while(a===c){let x=await p.read();if(x.done||a!==c)break;let q=c.createBuffer(1,x.value.length,8000),d=q.getChannelData(0);"
		                  "for(let i=0;i<d.length;i++)d[i]=u(x.value[i]);let o=a.createBufferSource();o.buffer=q;o.connect(a.destination);"
		                  "let t=Math.max(n,a.currentTime+.04);o.start(t);n=t+q.duration}"
		                  "if(a===c){a=r=null;b.textContent='Listen';s.textContent='Audio stopped';try{await c.close()}catch(e){}}}"
		                  "catch(e){if(a===c){a=r=null;b.textContent='Listen';s.textContent='Audio connection failed';"
		                  "try{if(c)await c.close()}catch(e){}}}}</script>");
	}
	XF16CAM_HTTP_SEND_LITERAL(fd,
						"<form method=post action=/api/media>"
						"<button name=mode value=web onclick='videoStop(1)'>Browser video</button> "
						"<button name=mode value=rtsp onclick='videoStop(1)'>RTSP</button></form>"
						"<small>Changing mode reboots.</small></div><div>");
	#ifndef NO_PTZ
	//Add LED control button
	XF16CAM_HTTP_SEND_LITERAL(fd,
					  "<script>async function submitLed(event,form){event.preventDefault();"
					  "let button=form.querySelector('button');button.disabled=true;"
					  "let data=new URLSearchParams();data.set(button.name,button.value);"
					  "try{let response=await fetch(form.action,{method:'POST',body:data});"
					  "if(!response.ok)throw 0;let status=await response.json(),on=!!status[button.name];"
					  "button.value=on?'false':'true';button.textContent=(button.name=='ir_led_on'?'Turn IR LED ':'Turn LED ')+(on?'off':'on');"
					  "}catch(e){}finally{button.disabled=false}return false}"
					  "async function submitPtz(event,form){event.preventDefault();let button=event.submitter;"
					  "if(!button)return false;button.disabled=true;let data=new URLSearchParams();"
					  "data.set(button.name,button.value);try{await fetch(form.action,{method:'POST',body:data})}"
					  "finally{button.disabled=false}return false}</script>"
					  );
	int led_on = xf16cam_board_get_led_on();
	length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic),
	                  "<form class=led method=post action=/api/led onsubmit='return submitLed(event,this)'>"
	                  "<button name=led_on value=%s>"
					  "Turn LED %s</button></form>"
					  , led_on ? "'false'" : "'true'"
					  , led_on ? "off" : "on"
	                  );
	xf16cam_http_send_all(fd, dynamic, length);
	//Add IR LED control button for PTZ version
	int ir_led_on = xf16cam_board_get_ir_led_on();
	length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic),
	                  "<form class=led method=post action=/api/ir_led onsubmit='return submitLed(event,this)'>"
	                  "<button name=ir_led_on value=%s>"
					  "Turn IR LED %s</button></form>"
					  , ir_led_on ? "'false'" : "'true'"
					  , ir_led_on ? "off" : "on"
	                  );
	xf16cam_http_send_all(fd, dynamic, length);
	//Add PTZ control buttons for PTZ version
	XF16CAM_HTTP_SEND_LITERAL(fd,
					  "<form method=post action=/api/ptz onsubmit='return submitPtz(event,this)'>"
					  "<div style='display:grid;grid-template-columns:repeat(3,44px);gap:4px;width:max-content'>"
					  "<span></span><button name=mode value=up>▲</button><span></span>"
					  "<button name=mode value=left>◀</button>"
					  "<button name=mode value=home>⦿</button>"
					  "<button name=mode value=right>▶</button>"
					  "<span></span><button name=mode value=down>▼</button><span></span></div>"
					  "</form>");
	#endif
	XF16CAM_HTTP_SEND_LITERAL(fd,"</div></div>");

	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "</section>"
	                  "<nav class=tabs><button type=button data-tab=live onclick=tab('live')>Live</button>"
	                  "<button type=button data-tab=network onclick=tab('network')>Network</button>"
	                  "<button type=button data-tab=storage onclick=tab('storage')>Storage</button>"
	                  "<button type=button data-tab=system onclick=tab('system')>System</button></nav>"
	                  "<div id=live class=panel><section class=card><h2>Camera</h2><div class=grid>"
	                  "<b>Sensor</b><span>");
	xf16cam_http_send_text(fd, xf16cam_sensor_name());
	length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic),
	                  "</span><b>JPEG output</b><span>%s</span><b>Stream mode</b><span>%s</span>"
	                  "<b>JPEG body buffer</b><span>%lu bytes</span><b>Frames captured</b><span>%lu</span>"
	                  "<b>Largest JPEG body</b><span>%lu bytes</span><b>Capture errors</b><span>%lu</span></div>",
	                  camera_output, !camera_available ? "Disabled (no sensor)" :
	                  config->media_mode == XF16CAM_MEDIA_WEB ? "Browser MJPEG" : "RTSP",
	                  (unsigned long)media->jpeg_capacity, (unsigned long)media->frames,
	                  (unsigned long)media->largest_jpeg,
	                  (unsigned long)media->capture_errors);
	xf16cam_http_send_all(fd, dynamic, length);
	if (xf16cam_sensor_supports_vga()) {
		if (xf16cam_sensor_width() == 640)
			XF16CAM_HTTP_SEND_LITERAL(fd,
			                  "<form method=post action=/api/resolution>"
			                  "<button name=resolution value=qvga>320x240</button> "
			                  "<button disabled>640x480 (active)</button></form>");
		else
			XF16CAM_HTTP_SEND_LITERAL(fd,
			                  "<form method=post action=/api/resolution>"
			                  "<button disabled>320x240 (active)</button> "
			                  "<button name=resolution value=vga>640x480</button></form>");
		XF16CAM_HTTP_SEND_LITERAL(fd,
		                  "<small>Changing resolution reboots. VGA uses more bandwidth and may reduce frame rate.</small>");
	}
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "</section><section class=card><h2>Audio</h2><div class=grid><b>Microphone</b><span>");
	length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic),
	                  "%s</span><b>Peak level</b><span><span id=mic>%u</span>/32768</span></div></section></div>",
	                  !audio->available ? "AMIC unavailable" :
	                  audio->active ? "AMIC active, PCMU/8000" : "AMIC ready (on demand)", audio->peak);
	xf16cam_http_send_all(fd, dynamic, length);

#ifdef XF16CAM_MQTT
	/* The MQTT card takes the second grid column, so the Wi-Fi card is half
	 * width here and keeps the full row on builds without MQTT. */
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "<div id=network class=panel><section class=card><h2>Wi-Fi setup</h2>");
#else
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "<div id=network class=panel><section class='card wide'><h2>Wi-Fi setup</h2>");
#endif
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "<p>Tap a scan result, or enter a hidden SSID.</p>"
	                  "<button type=button id=scanButton onclick=scan()>Scan networks</button> <span id=scan aria-live=polite></span>"
	                  "<div id=networks class=nets aria-live=polite></div>"
	                  "<form method=post action=/api/wifi><label>Network name (SSID)"
	                  "<input id=ssid name=ssid maxlength=32 required autocapitalize=none spellcheck=false oninput=clearNet() value=\"");
	xf16cam_http_send_escaped(fd, (const uint8_t *)config->ssid, strlen(config->ssid), 0);
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "\"></label>"
	                  "<label>Password<input type=password name=password maxlength=63 autocomplete=new-password>"
	                  "<small>Blank keeps the saved password for the current SSID, or joins a new open network.</small></label>"
	                  "<button class=primary type=submit>Save and reboot</button></form>"
	                  "<form method=post action=/api/ap><button type=submit>Return to open AP</button></form></section>");
#ifdef XF16CAM_MQTT
	xf16cam_http_mqtt_card(fd, config);
#endif
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "</div><div id=storage class=panel><section class='card wide'><h2>SD card</h2><div class=grid>"
	                  "<b>Status</b><span>");
	if (storage->mounted) {
		length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic),
		                  "Mounted</span><b>Capacity</b><span>%lu MiB</span>"
		                  "<b>Free space</b><span>%lu MiB</span></div>",
		                  (unsigned long)storage->total_mb, (unsigned long)storage->free_mb);
		xf16cam_http_send_all(fd, dynamic, length);
		XF16CAM_HTTP_SEND_LITERAL(fd,
		                  "<form method=post action=/api/sd/eject><button type=submit>Safely eject</button></form>");
	} else {
		XF16CAM_HTTP_SEND_LITERAL(fd, "Not checked</span></div>");
	}
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "<form method=post action=/api/sd/refresh><button type=submit>Check card</button></form>"
	                  "<form method=post action=/api/sd/format onsubmit=\"return confirm('Erase and format the SD card?')\">"
	                  "<button type=submit>Format FAT32</button></form>"
	                  "<small>Formatting permanently erases the card. Ejecting lets the shared PA23 camera/SD rail power down while idle.</small></section></div>"
	                  "<div id=system class=panel><section class=card><h2>Device</h2><div class=grid>");
	length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic),
	                  "<b>DHCP hostname</b><span>%s</span>"
	                  "<b>Network mode</b><span>%s</span><b>IP address</b><span>%s</span>"
	                  "<b>Wi-Fi MAC</b><span>%02X:%02X:%02X:%02X:%02X:%02X (eFuse)</span>"
	                  "<b>Available SRAM heap</b><span>%lu bytes</span><b>Flash JEDEC ID</b><span>%02lX %02lX %02lX</span>"
	                  "<b>Flash capacity</b><span>%lu KiB</span><b>Mode button</b><span>PA15 (%s)</span>"
	                  "<b>Setup button</b><span>PA20 (%s)</span>",
	                  xf16cam_net_hostname(),
	                  xf16cam_net_mode() == XF16CAM_WIFI_STA ? "Station" : "Open AP", xf16cam_net_ip(),
	                  sysinfo->mac_addr[0], sysinfo->mac_addr[1], sysinfo->mac_addr[2],
	                  sysinfo->mac_addr[3], sysinfo->mac_addr[4], sysinfo->mac_addr[5],
	                  (unsigned long)sram_free_heap_size(),
	                  (unsigned long)(g_flash_jedec & 0xff),
	                  (unsigned long)((g_flash_jedec >> 8) & 0xff),
	                  (unsigned long)((g_flash_jedec >> 16) & 0xff),
	                  (unsigned long)(g_flash_size / 1024), mode_button,
	                  xf16cam_board_reset_button_pressed() ? "pressed" : "released");
	if (length > 0 && (size_t)length < sizeof(dynamic))
		xf16cam_http_send_all(fd, dynamic, (size_t)length);
	length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic),
	                  "<b>HTTP stack spare</b><span>%lu bytes</span>"
	                  "<b>Audio stack spare</b><span>%lu bytes</span>"
	                  "<b>Board stack spare</b><span>%lu bytes</span>"
	                  "<b>Crash task name</b><span>%s</span>",
	                  (unsigned long)OS_ThreadGetStackMinFreeSize(&g_http_thread),
	                  (unsigned long)xf16cam_audio_stack_min_free(),
	                  (unsigned long)xf16cam_board_stack_min_free(),
					  xf16cam_board_get_crash_task_name());
	xf16cam_http_send_all(fd, dynamic, length);
#ifdef XF16CAM_MQTT
	length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic),
	                  "<b>MQTT stack spare</b><span>%lu bytes</span>",
	                  (unsigned long)xf16cam_mqtt_stack_min_free());
	xf16cam_http_send_all(fd, dynamic, length);
#endif
	XF16CAM_HTTP_SEND_LITERAL(fd, "</div></section>");
	xf16cam_http_runtime(fd, dynamic, sizeof(dynamic));

	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "<section class=card><h2>XF16 pin map</h2><div class=grid>"
	                  "<b>Camera CSI</b><span>PA0-PA11</span>"
	                  "<b>Camera control</b><span>PA14</span>"
					  #ifdef NO_PTZ
	                  "<b>Status LED</b><span>PA21</span>"
					  #else
					  "<b>Illumination LED</b><span>PB20</span>"
					  "<b>IR LED</b><span>PA22</span>"
					  #endif
	                  "<b>Battery sense</b><span>PA16 / ADC6</span>"
	                  "<b>Camera / SD power rail</b><span>PA23</span>"
	                  "<b>SD card</b><span>PB16 CMD, PB17 D0, PB18 CLK</span>"
	                  "<b>Console</b><span>PB0 TX, PB1 RX</span>"
	                  "<b>SPI flash</b><span>PB2-PB7</span>"
					  #ifdef NO_PTZ
	                  "<b>Mode button</b><span>PA15; short press switches Web/RTSP</span>"
	                  "<b>Setup button</b><span>PA20; hold 3 seconds to restore AP</span>"
					  #else
	                  "<b>PTZ control</b><span>PB2-PB7</span>"
	                  "<b>Setup button</b><span>PB19; hold 3 seconds to restore AP</span>"
					  #endif
	                  "</div></section><section class=card><h2>Power</h2><div class=grid>"
	                  "<b>Battery input</b><span id=battery>");
	if (power->valid) {
		length = XF16CAM_XIP_FORMAT(dynamic, sizeof(dynamic), "%u mV (raw %u, approx.)",
		                  power->millivolts, power->raw);
		xf16cam_http_send_all(fd, dynamic, length);
	} else {
		XF16CAM_HTTP_SEND_LITERAL(fd, "Not measured");
	}
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "</span><b>Charging</b><span>Unknown</span>"
	                  "</div><button type=button onclick=measurePower()>Measure voltage</button>");
	#ifdef NO_PTZ
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "<form method=post action=/api/hibernate onsubmit=\"return confirm('Hibernate? Press PA20 to wake.')\">"
	                  "<button type=submit>Hibernate</button></form>");
	#endif
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "<form method=post action=/api/reboot onsubmit=\"return confirm('Reboot now?')\">"
	                  "<button type=submit>Reboot</button></form>"
	                  "<small>Voltage is approximate; no auto cutoff. Rebooting drops streams; settings are kept.</small></section>");
	XF16CAM_HTTP_SEND_LITERAL(fd,
	                  "<section class='card wide'><h2>Firmware update</h2><p>Choose an OTA image and keep power connected.</p>"
	                  "<input id=ota type=file accept=.img><button id=otaInstall class=primary type=button onclick=update()>Install update</button> <span id=up aria-live=polite></span></section></div>"
	                  "<script>let D=document,$=s=>D.querySelector(s),A=s=>D.querySelectorAll(s),vt,vh=0;"
	                  "function videoStop(h){if(h)vh=1;clearTimeout(vt);let v=$('#video');if(v)v.removeAttribute('src')}"
	                  "function videoStart(){let v=$('#video');if(!v||vh||D.hidden)return;clearTimeout(vt);v.src='/stream.mjpeg'}"
	                  "function videoRetry(){clearTimeout(vt);if(!vh&&!D.hidden)vt=setTimeout(videoStart,1500)}"
	                  "function tab(id){A('.panel').forEach(e=>e.classList.toggle('on',e.id==id));"
	                  "A('.tabs button').forEach(e=>{let on=e.dataset.tab==id;e.classList.toggle('on',on);e.setAttribute('aria-selected',on)})}"
	                  "function clearNet(){A('.net').forEach(e=>e.classList.remove('sel'))}"
	                  "function pickNet(b){$('#ssid').value=b.dataset.ssid;clearNet();b.classList.add('sel');"
	                  "$('input[name=password]').focus()}"
	                  "async function scan(){let s=$('#scan'),b=$('#scanButton'),d=$('#networks');"
	                  "s.textContent='Scanning...';b.disabled=true;d.innerHTML='';try{let r=await fetch('/api/scan');if(!r.ok)throw 0;let a=await r.json();"
	                  "a.sort((x,y)=>y.rssi-x.rssi);a.forEach(n=>{let o=D.createElement('button'),x=D.createElement('span'),m=D.createElement('span');"
	                  "o.type='button';o.className='net';o.dataset.ssid=n.ssid;o.onclick=()=>pickNet(o);x.textContent=n.ssid||'(hidden network)';"
	                  "m.textContent=n.rssi+' dBm · '+(n.secure?'Secured':'Open');o.append(x,m);d.append(o)});"
	                  "if(!a.length)d.innerHTML='<div class=\"net empty\">No networks found</div>';s.textContent=a.length+' found'}"
	                  "catch(e){s.textContent='Scan failed';d.innerHTML='<div class=\"net empty\">Try scanning again</div>'}finally{b.disabled=false}}"
	                  "async function measurePower(){let b=$('#battery');b.textContent='Measuring...';try{let r=await fetch('/api/power',{method:'POST'});"
	                  "if(!r.ok)throw 0;let p=await r.json();b.textContent=p.millivolts+' mV (raw '+p.raw+', approx.)'}catch(e){b.textContent='Measurement failed'}}"
	                  "async function update(){let f=$('#ota').files[0],s=$('#up'),b=$('#otaInstall');if(b.disabled)return;"
	                  "if(!f){s.textContent='Choose a file';return}if(!confirm('Install '+f.name+' and reboot?'))return;"
	                  "b.disabled=true;videoStop(1);s.textContent='Uploading...';let ok=false;try{let r=await fetch('/api/ota',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f}),t=await r.text();"
	                  "ok=r.ok;s.textContent=ok?t:'Update failed (HTTP '+r.status+')'}catch(e){s.textContent='Connection closed; check for reboot'}finally{b.disabled=false;if(!ok){vh=0;videoStart()}}}"
	                  "async function meter(){if(!D.hidden&&$('#live').classList.contains('on'))try{"
	                  "let a=await(await fetch('/api/audio')).json(),m=$('#mic');if(m)m.textContent=a.peak}catch(e){}setTimeout(meter,2000)}"
	                  "let v=$('#video');if(v){v.onerror=videoRetry;videoStart()}"
	                  "D.addEventListener('visibilitychange',()=>D.hidden?videoStop():videoStart());"
	                  "tab('live');meter()</script></main></body></html>");
}

__xip_text
static void xf16cam_http_led_json(int fd)
{
	int led_on = xf16cam_board_get_led_on();
	char body[64];
	int length = XF16CAM_XIP_FORMAT(body, sizeof(body),
	                      "{\"led_on\":%s}",
	                      led_on ? "true" : "false");

	xf16cam_http_begin_length(fd, "200 OK", "application/json", length);
	xf16cam_http_send_all(fd, body, length);
}

__xip_text
static void xf16cam_http_ir_led_json(int fd)
{
	int ir_led_on = xf16cam_board_get_ir_led_on();
	char body[64];
	int length = XF16CAM_XIP_FORMAT(body, sizeof(body),
	                      "{\"ir_led_on\":%s}",
	                      ir_led_on ? "true" : "false");

	xf16cam_http_begin_length(fd, "200 OK", "application/json", length);
	xf16cam_http_send_all(fd, body, length);
}

__xip_text
static void xf16cam_http_audio_json(int fd)
{
	const XF16CamAudioInfo *audio = xf16cam_audio_info();
	char body[128];
	int length = XF16CAM_XIP_FORMAT(body, sizeof(body),
	                      "{\"available\":%s,\"active\":%s,\"peak\":%u,\"mean\":%u,\"packets\":%lu,\"read_errors\":%lu}",
	                      audio->available ? "true" : "false", audio->active ? "true" : "false", audio->peak, audio->mean,
	                      (unsigned long)audio->packets, (unsigned long)audio->read_errors);

	xf16cam_http_begin_length(fd, "200 OK", "application/json", length);
	xf16cam_http_send_all(fd, body, length);
}

__xip_text
static void xf16cam_http_power_json(int fd)
{
	const XF16CamPowerInfo *power;
	char body[80];
	int length;

	if (xf16cam_power_measure() != 0) {
		XF16CAM_HTTP_MESSAGE(fd, "503 Service Unavailable", "Battery read failed.");
		return;
	}
	power = xf16cam_power_info();
	length = XF16CAM_XIP_FORMAT(body, sizeof(body),
	                  "{\"raw\":%u,\"millivolts\":%u,\"calibrated\":false}",
	                  power->raw, power->millivolts);
	xf16cam_http_begin_length(fd, "200 OK", "application/json", length);
	xf16cam_http_send_all(fd, body, length);
}

/* Streamed like the scan endpoint: one small buffer reused for every chunk
 * keeps this off the 4 KiB HTTP stack. "Connection: close" delimits the body,
 * so the missing Content-Length is deliberate. */
__xip_text
static void xf16cam_http_system_chunk(int fd, char *body, size_t size, int length)
{
	if (length > 0 && (size_t)length < size)
		xf16cam_http_send_all(fd, body, length);
}

__xip_text
static void xf16cam_http_system_json(int fd)
{
	const XF16CamConfig *config = xf16cam_config_get();
	const XF16CamAudioInfo *audio = xf16cam_audio_info();
	const XF16CamMediaInfo *media = xf16cam_media_info();
	const XF16CamStorageInfo *storage = xf16cam_storage_info();
	const XF16CamPowerInfo *power = xf16cam_power_info();
	const struct sysinfo *sysinfo = sysinfo_get();
	int temperature = xf16cam_http_chip_temperature();
	uint32_t uptime = OS_TicksToMSecs(OS_GetTicks()) / 1000U;
	/* Worst-case chunk is the sensor/media one at ~210 bytes with maximal
	 * counters, so 256 keeps real headroom. A chunk that overflows is
	 * dropped whole, which would silently return valid but incomplete JSON. */
	char body[256];
	char temperature_text[16];
	const char *mode_button;

#ifdef NO_PTZ
	mode_button = xf16cam_board_mode_button_pressed() ? "true" : "false";
#else
	mode_button = "null";	/* PTZ boards have no mode button */
#endif
	if (temperature == INT_MIN)
		XF16CAM_XIP_FORMAT(temperature_text, sizeof(temperature_text), "null");
	else
		XF16CAM_XIP_FORMAT(temperature_text, sizeof(temperature_text), "%d", temperature);

	xf16cam_http_begin(fd, "200 OK", "application/json");

	/* Flash identity comes from the values cached at startup. Never call
	 * xf16cam_http_flash_info() here: it disables flash while it runs. */
	xf16cam_http_system_chunk(fd, body, sizeof(body),
	                  XF16CAM_XIP_FORMAT(body, sizeof(body),
	                  "{\"ver\":\"" XF16CAM_VERSION "\",\"mode\":\"%s\",\"ip\":\"%s\","
	                  "\"host\":\"%s\","
	                  "\"mac\":\"%02X%02X%02X%02X%02X%02X\",\"up\":%lu,"
	                  "\"boot\":\"%s\",\"temp\":%s,\"heap\":%lu,",
	                  xf16cam_net_mode() == XF16CAM_WIFI_STA ? "sta" : "ap",
	                  xf16cam_net_ip(), xf16cam_net_hostname(),
	                  sysinfo->mac_addr[0], sysinfo->mac_addr[1],
	                  sysinfo->mac_addr[2], sysinfo->mac_addr[3],
	                  sysinfo->mac_addr[4], sysinfo->mac_addr[5],
	                  (unsigned long)uptime, xf16cam_http_boot_reason(),
	                  temperature_text,
	                  (unsigned long)sram_free_heap_size()));

	xf16cam_http_system_chunk(fd, body, sizeof(body),
	                  XF16CAM_XIP_FORMAT(body, sizeof(body),
	                  "\"flash\":{\"id\":\"%02lX%02lX%02lX\",\"kib\":%lu},"
	                  "\"stack\":{\"http\":%lu,\"audio\":%lu,\"board\":%lu},"
	                  "\"btn\":{\"mode\":%s,\"setup\":%s},"
	                  "\"bat\":{\"valid\":%s,\"raw\":%u,\"mv\":%u},",
	                  (unsigned long)(g_flash_jedec & 0xff),
	                  (unsigned long)((g_flash_jedec >> 8) & 0xff),
	                  (unsigned long)((g_flash_jedec >> 16) & 0xff),
	                  (unsigned long)(g_flash_size / 1024),
	                  (unsigned long)OS_ThreadGetStackMinFreeSize(&g_http_thread),
	                  (unsigned long)xf16cam_audio_stack_min_free(),
	                  (unsigned long)xf16cam_board_stack_min_free(),
	                  mode_button,
	                  xf16cam_board_reset_button_pressed() ? "true" : "false",
	                  power->valid ? "true" : "false",
	                  power->raw, power->millivolts));

	xf16cam_http_system_chunk(fd, body, sizeof(body),
	                  XF16CAM_XIP_FORMAT(body, sizeof(body),
	                  "\"cam\":{\"name\":\"%s\",\"ok\":%s,\"w\":%u,\"h\":%u,\"vga\":%s},"
	                  "\"media\":{\"mode\":\"%s\",\"frames\":%lu,\"largest\":%lu,"
	                  "\"errors\":%lu,\"cap\":%lu,\"last_ms\":%lu,\"clients\":%lu},",
	                  xf16cam_sensor_name(),
	                  xf16cam_sensor_available() ? "true" : "false",
	                  (unsigned int)xf16cam_sensor_width(),
	                  (unsigned int)xf16cam_sensor_height(),
	                  xf16cam_sensor_supports_vga() ? "true" : "false",
	                  config->media_mode == XF16CAM_MEDIA_WEB ? "web" : "rtsp",
	                  (unsigned long)media->frames,
	                  (unsigned long)media->largest_jpeg,
	                  (unsigned long)media->capture_errors,
	                  (unsigned long)media->jpeg_capacity,
	                  (unsigned long)media->last_frame_ms,
	                  (unsigned long)xf16cam_media_active_clients()));

#ifdef XF16CAM_MQTT
	{
		const XF16CamMqttInfo *mqtt = xf16cam_mqtt_info();

		/* The host is validated to [0-9A-Za-z.:-] on save, so it needs no
		 * JSON escaping; the password is never reported. */
		xf16cam_http_system_chunk(fd, body, sizeof(body),
		                  XF16CAM_XIP_FORMAT(body, sizeof(body),
		                  "\"mqtt\":{\"on\":%s,\"connected\":%s,\"host\":\"%s\",\"port\":%u,"
		                  "\"interval\":%u,\"connects\":%lu,\"disconnects\":%lu,"
		                  "\"images\":%lu,\"states\":%lu,\"commands\":%lu,\"dropped\":%lu,"
		                  "\"stack\":%lu},",
		                  mqtt->enabled ? "true" : "false",
		                  mqtt->connected ? "true" : "false",
		                  config->u.mqtt.host,
		                  (unsigned int)xf16cam_config_mqtt_port(config),
		                  (unsigned int)xf16cam_config_mqtt_interval(config),
		                  (unsigned long)mqtt->connects,
		                  (unsigned long)mqtt->disconnects,
		                  (unsigned long)mqtt->images,
		                  (unsigned long)mqtt->states,
		                  (unsigned long)mqtt->commands,
		                  (unsigned long)mqtt->dropped,
		                  (unsigned long)xf16cam_mqtt_stack_min_free()));
	}
#endif
	xf16cam_http_system_chunk(fd, body, sizeof(body),
	                  XF16CAM_XIP_FORMAT(body, sizeof(body),
	                  "\"audio\":{\"ok\":%s,\"active\":%s,\"peak\":%u,\"mean\":%u,"
	                  "\"packets\":%lu,\"errors\":%lu},"
	                  "\"sd\":{\"mounted\":%s,\"total\":%lu,\"free\":%lu}}",
	                  audio->available ? "true" : "false",
	                  audio->active ? "true" : "false",
	                  audio->peak, audio->mean,
	                  (unsigned long)audio->packets,
	                  (unsigned long)audio->read_errors,
	                  storage->mounted ? "true" : "false",
	                  (unsigned long)storage->total_mb,
	                  (unsigned long)storage->free_mb));
}

#ifdef XF16CAM_NETLOG
/* The console ring as text. Streamed like /api/system, so no Content-Length;
 * "Connection: close" delimits the body. */
__xip_text
static void xf16cam_http_log(int fd)
{
	char chunk[256];
	uint32_t cursor = 0;
	int count;

	xf16cam_http_begin(fd, "200 OK", "text/plain; charset=utf-8");
	while ((count = xf16cam_log_read(&cursor, chunk, sizeof(chunk))) > 0) {
		if (xf16cam_http_send_all(fd, chunk, (size_t)count) != 0)
			break;
	}
}
#endif

__xip_text
static int xf16cam_http_scan(void)
{
	wlan_sta_scan_results_t results;
	int count = 0;

	if (xf16cam_net_mode() == XF16CAM_WIFI_AP) {
		wlan_ap_scan_bss_max_count(XF16CAM_HTTP_SCAN_MAX);
		if (wlan_ap_scan_once() != 0)
			return -1;
	} else {
		wlan_sta_bss_max_count(XF16CAM_HTTP_SCAN_MAX);
		if (wlan_sta_scan_once() != 0)
			return -1;
	}
	OS_MSleep(1500);
	results.ap = g_scan_results;
	results.size = XF16CAM_HTTP_SCAN_MAX;
	if (xf16cam_net_mode() == XF16CAM_WIFI_AP) {
		if (wlan_ap_scan_result(&results) != 0)
			return -1;
	} else if (wlan_sta_scan_result(&results) != 0) {
		return -1;
	}
	count = results.num;
	return count > XF16CAM_HTTP_SCAN_MAX ? XF16CAM_HTTP_SCAN_MAX : count;
}

__xip_text
static void xf16cam_http_scan_json(int fd)
{
	int count = xf16cam_http_scan();
	int i;
	char item[80];

	if (count < 0) {
		xf16cam_http_begin(fd, "503 Service Unavailable", "application/json");
		XF16CAM_HTTP_SEND_LITERAL(fd, "{\"error\":\"scan failed\"}");
		return;
	}
	xf16cam_http_begin(fd, "200 OK", "application/json");
	XF16CAM_HTTP_SEND_LITERAL(fd, "[");
	for (i = 0; i < count; ++i) {
		wlan_sta_ap_t *ap = &g_scan_results[i];
		if (i > 0)
			XF16CAM_HTTP_SEND_LITERAL(fd, ",");
		XF16CAM_HTTP_SEND_LITERAL(fd, "{\"ssid\":\"");
		xf16cam_http_send_escaped(fd, ap->ssid.ssid, ap->ssid.ssid_len, 1);
		XF16CAM_XIP_FORMAT(item, sizeof(item), "\",\"rssi\":%d,\"secure\":%s}",
		         ap->level, (ap->wpa_key_mgmt || ap->rsn_key_mgmt) ? "true" : "false");
		xf16cam_http_send_text(fd, item);
	}
	XF16CAM_HTTP_SEND_LITERAL(fd, "]");
}

__xip_text
static int xf16cam_hex(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

__xip_text
static int xf16cam_form_value(const char *body, const char *key,
	                          char *out, size_t out_size)
{
	size_t key_len = strlen(key);
	const char *p = body;
	size_t used = 0;

	while (*p != '\0') {
		if ((p == body || p[-1] == '&') && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
			p += key_len + 1;
			while (*p != '\0' && *p != '&') {
				char value = *p++;
				if (value == '+') {
					value = ' ';
				} else if (value == '%' && p[0] != '\0' && p[1] != '\0' &&
				           isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
					value = (char)((xf16cam_hex(p[0]) << 4) | xf16cam_hex(p[1]));
					p += 2;
				}
				if (used + 1 >= out_size)
					return -1;
				out[used++] = value;
			}
			out[used] = '\0';
			return 0;
		}
		p = strchr(p, '&');
		if (p == NULL)
			break;
		++p;
	}
	return -1;
}

__xip_text
static void xf16cam_http_message(int fd, const char *status, const char *message)
{
	__xip_rodata static const char prefix[] = "<section><h1>XF16Cam</h1><p>";
	__xip_rodata static const char suffix[] = "</p><a href='/'>Return</a></section></body></html>";
	size_t length = sizeof(g_page_head) - 1 + sizeof(prefix) - 1 + strlen(message) + sizeof(suffix) - 1;

	xf16cam_http_begin_length(fd, status, "text/html; charset=utf-8", length);
	xf16cam_http_send_all(fd, g_page_head, sizeof(g_page_head) - 1);
	xf16cam_http_send_all(fd, prefix, sizeof(prefix) - 1);
	xf16cam_http_send_text(fd, message);
	xf16cam_http_send_all(fd, suffix, sizeof(suffix) - 1);
}

__xip_text
static int xf16cam_http_content_length(char *request, char *header_end, int *length)
{
	char *line = strstr(request, "\r\n");
	int found = 0;

	*length = 0;

	while (line != NULL && line < header_end) {
		char *end;
		char *value;
		unsigned int parsed = 0;

		line += 2;
		if (line >= header_end)
			break;
		end = strstr(line, "\r\n");
		if (end == NULL || end > header_end)
			return -1;
		if (end - line < (int)sizeof("Content-Length:") - 1 ||
		    strncasecmp(line, "Content-Length", sizeof("Content-Length") - 1) != 0 ||
		    line[sizeof("Content-Length") - 1] != ':') {
			line = end;
			continue;
		}
		if (found)
			return -1;
		value = line + sizeof("Content-Length");
		while (value < end && (*value == ' ' || *value == '\t'))
			++value;
		if (value == end || *value < '0' || *value > '9')
			return -1;
		while (value < end && *value >= '0' && *value <= '9') {
			unsigned int digit = (unsigned int)(*value++ - '0');
			if (parsed > ((unsigned int)INT_MAX - digit) / 10U)
				return -1;
			parsed = parsed * 10U + digit;
		}
		while (value < end && (*value == ' ' || *value == '\t'))
			++value;
		if (value != end)
			return -1;
		*length = (int)parsed;
		found = 1;
		line = end;
	}
	return 0;
}

/* Apply both a total deadline and the normal per-read stall timeout. */
static int xf16cam_http_recv_deadline(int fd, void *buffer, int length,
				      uint32_t start, uint32_t total_ms)
{
	uint32_t elapsed = OS_TicksToMSecs(OS_GetTicks()) - start;
	int timeout;

	if (elapsed >= total_ms)
		return -1;
	timeout = (int)(total_ms - elapsed);
	if (timeout > XF16CAM_HTTP_TIMEOUT_MS)
		timeout = XF16CAM_HTTP_TIMEOUT_MS;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	return recv(fd, buffer, length, 0);
}

__attribute__((noinline))
static int xf16cam_http_ota(int fd, char *body, int body_length, int content_length)
{
	int written = 0;
	uint32_t upload_start;

	if (content_length <= 0 || content_length > XF16CAM_OTA_MAX_SIZE) {
		XF16CAM_HTTP_MESSAGE(fd, "413 Payload Too Large", "Invalid OTA image size.");
		return XF16CAM_HTTP_KEEP_RUNNING;
	}
	if (xf16cam_update_begin() != 0) {
		XF16CAM_HTTP_MESSAGE(fd, "409 Conflict", "Another firmware or settings update is active.");
		return XF16CAM_HTTP_KEEP_RUNNING;
	}
	if (xf16cam_media_quiesce_for_update(XF16CAM_OTA_QUIESCE_MS) != 0) {
		xf16cam_update_end();
		XF16CAM_HTTP_MESSAGE(fd, "503 Service Unavailable",
		                     "Active media could not stop safely. Close stream clients and try again.");
		return XF16CAM_HTTP_KEEP_RUNNING;
	}
	HAL_WDG_Stop();
	if (ota_push_init() != OTA_STATUS_OK || ota_push_start() != OTA_STATUS_OK)
		goto fail;
	upload_start = OS_TicksToMSecs(OS_GetTicks());
	if (body_length > content_length)
		body_length = content_length;
	if (body_length > 0 && ota_push_data((uint8_t *)body, body_length) != OTA_STATUS_OK)
		goto fail;
	written = body_length;
	while (written < content_length) {
		int wanted = content_length - written;
		int count;
		if (wanted > (int)sizeof(g_request))
			wanted = sizeof(g_request);
		count = xf16cam_http_recv_deadline(fd, g_request, wanted, upload_start,
		                                      XF16CAM_OTA_UPLOAD_MS);
		if (count <= 0 || ota_push_data((uint8_t *)g_request, count) != OTA_STATUS_OK)
			goto fail;
		written += count;
	}
	if (ota_push_finish() != OTA_STATUS_OK)
		goto fail;
	{
		static const char success[] = "Update verified. Rebooting...";
		xf16cam_http_begin_length(fd, "200 OK", "text/plain; charset=utf-8", sizeof(success) - 1);
		xf16cam_http_send_all(fd, success, sizeof(success) - 1);
	}
	return XF16CAM_HTTP_OTA_REBOOT;

fail:
	ota_push_stop();
	xf16cam_update_end();
	XF16CAM_HTTP_MESSAGE(fd, "400 Bad Request", "OTA verification failed; the current firmware is unchanged.");
	return XF16CAM_HTTP_KEEP_RUNNING;
}

__xip_text
static int xf16cam_http_handle(int fd)
{
	char method[8];
	char path[96];
	char *header_end;
	char *body;
	int received = 0;
	int content_length = 0;
	uint32_t request_start = OS_TicksToMSecs(OS_GetTicks());
	char ssid[XF16CAM_SSID_MAX_LEN + 1];
	char psk[XF16CAM_PSK_MAX_LEN + 1];
	char mode[8];
#ifdef XF16CAM_MQTT
	char user[XF16CAM_MQTT_USER_MAX_LEN + 1];
#endif

	memset(g_request, 0, sizeof(g_request));
	header_end = NULL;
	while (received < (int)sizeof(g_request) - 1) {
		int count = xf16cam_http_recv_deadline(fd, g_request + received,
		                                         sizeof(g_request) - 1 - received,
		                                         request_start, XF16CAM_HTTP_HEADER_MS);
		if (count <= 0)
			return 0;
		received += count;
		g_request[received] = '\0';
		header_end = strstr(g_request, "\r\n\r\n");
		if (header_end != NULL)
			break;
	}
	if (sscanf(g_request, "%7s %95s", method, path) != 2) {
		XF16CAM_HTTP_MESSAGE(fd, "400 Bad Request", "Malformed request.");
		return 0;
	}
	header_end = strstr(g_request, "\r\n\r\n");
	if (header_end == NULL) {
		XF16CAM_HTTP_MESSAGE(fd, "431 Request Header Fields Too Large", "Request headers are too large.");
		return XF16CAM_HTTP_KEEP_RUNNING;
	}
	body = header_end ? header_end + 4 : g_request + received;
	if (xf16cam_http_content_length(g_request, header_end, &content_length) != 0) {
		XF16CAM_HTTP_MESSAGE(fd, "400 Bad Request", "Invalid Content-Length header.");
		return XF16CAM_HTTP_KEEP_RUNNING;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ota") == 0)
		return xf16cam_http_ota(fd, body, received - (body - g_request), content_length);

	if (content_length > (int)(sizeof(g_request) - 1 - (body - g_request))) {
		XF16CAM_HTTP_MESSAGE(fd, "413 Payload Too Large", "Request body is too large.");
		return XF16CAM_HTTP_KEEP_RUNNING;
	}
	while (received - (body - g_request) < content_length) {
		int count = xf16cam_http_recv_deadline(fd, g_request + received,
		                                         sizeof(g_request) - 1 - received,
		                                         request_start, XF16CAM_HTTP_TIMEOUT_MS);
		if (count <= 0)
			return XF16CAM_HTTP_KEEP_RUNNING;
		received += count;
		g_request[received] = '\0';
	}
	body[content_length] = '\0';

	if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
		xf16cam_http_page(fd);
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/stream.mjpeg") == 0) {
		if (!xf16cam_sensor_available())
			XF16CAM_HTTP_MESSAGE(fd, "503 Service Unavailable", "No supported camera sensor is available.");
		else if (xf16cam_config_get()->media_mode != XF16CAM_MEDIA_WEB)
			XF16CAM_HTTP_MESSAGE(fd, "409 Conflict", "Browser video mode is not active.");
		else if (xf16cam_mjpeg_start(fd) != 0)
			XF16CAM_HTTP_MESSAGE(fd, "503 Service Unavailable", "Browser video is at the 3-client limit.");
		else
			return XF16CAM_HTTP_DETACH_CLIENT;
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/stream.pcmu") == 0) {
		if (xf16cam_config_get()->media_mode != XF16CAM_MEDIA_WEB &&
		    xf16cam_sensor_available())
			XF16CAM_HTTP_MESSAGE(fd, "409 Conflict", "Browser video mode is not active.");
		else if (xf16cam_audio_http_start(fd) != 0)
			XF16CAM_HTTP_MESSAGE(fd, "503 Service Unavailable", "Browser audio is at the 3-client limit.");
		else
			return XF16CAM_HTTP_DETACH_CLIENT;
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/scan") == 0) {
		xf16cam_http_scan_json(fd);
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/audio") == 0) {
		xf16cam_http_audio_json(fd);
#ifdef XF16CAM_NETLOG
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/log") == 0) {
		xf16cam_http_log(fd);
#endif
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/led") == 0) {
		xf16cam_http_led_json(fd);
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/ir_led") == 0) {
		xf16cam_http_ir_led_json(fd);
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/system") == 0) {
		xf16cam_http_system_json(fd);
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/power") == 0) {
		xf16cam_http_power_json(fd);
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wifi") == 0) {
		int invalid = xf16cam_form_value(body, "ssid", ssid, sizeof(ssid)) != 0 ||
		              xf16cam_form_value(body, "password", psk, sizeof(psk)) != 0;

		if (!invalid) {
			const XF16CamConfig *current = xf16cam_config_get();

			/* A blank password for the currently saved secured SSID means
			 * "unchanged". A different SSID with a blank password remains a
			 * deliberate request to join an open network. */
			if (psk[0] == '\0' && current->wifi_mode == XF16CAM_WIFI_STA &&
			    current->psk[0] != '\0' && strcmp(ssid, current->ssid) == 0)
				memcpy(psk, current->psk, sizeof(psk));
			invalid = xf16cam_config_save_sta(ssid, psk) != 0;
		}
		if (invalid) {
			XF16CAM_HTTP_MESSAGE(fd, "400 Bad Request",
			                     "Invalid SSID or password. WPA passwords must contain 8 to 63 characters.");
			return 0;
		}
		if (xf16cam_update_begin() != 0) {
			XF16CAM_HTTP_MESSAGE(fd, "409 Conflict",
			                     "Settings were saved, but reboot was deferred by another update.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		XF16CAM_HTTP_MESSAGE(fd, "200 OK", "Settings saved. Rebooting into station mode...");
		return XF16CAM_HTTP_COLD_REBOOT;
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ap") == 0) {
		if (xf16cam_config_save_ap() != 0) {
			XF16CAM_HTTP_MESSAGE(fd, "500 Internal Server Error", "Could not save open AP mode.");
			return 0;
		}
		if (xf16cam_update_begin() != 0) {
			XF16CAM_HTTP_MESSAGE(fd, "409 Conflict",
			                     "Open AP was saved, but reboot was deferred by another update.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		XF16CAM_HTTP_MESSAGE(fd, "200 OK", "Open AP restored. Rebooting...");
		return XF16CAM_HTTP_COLD_REBOOT;
#ifdef XF16CAM_MQTT
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/mqtt") == 0) {
		/* host and pass reuse the Wi-Fi buffers: the HTTP stack is small. */
		unsigned long port = 0;
		unsigned long interval = 0;
		int invalid = xf16cam_form_value(body, "host", ssid, sizeof(ssid)) != 0 ||
		              xf16cam_form_value(body, "user", user, sizeof(user)) != 0 ||
		              xf16cam_form_value(body, "pass", psk, sizeof(psk)) != 0 ||
		              !xf16cam_http_mqtt_host_valid(ssid);

		if (!invalid && xf16cam_form_value(body, "port", mode, sizeof(mode)) == 0 &&
		    mode[0] != '\0') {
			port = strtoul(mode, NULL, 10);
			invalid = port == 0 || port > 65535UL;
		}
		if (!invalid && xf16cam_form_value(body, "interval", mode, sizeof(mode)) == 0 &&
		    mode[0] != '\0') {
			interval = strtoul(mode, NULL, 10);
			invalid = interval == 0 || interval > 255UL;
		}
		if (!invalid) {
			const XF16CamConfig *current = xf16cam_config_get();

			/* A blank password for the saved broker means "unchanged",
			 * as with the Wi-Fi form. */
			if (psk[0] == '\0' && ssid[0] != '\0' &&
			    strcmp(ssid, current->u.mqtt.host) == 0)
				memcpy(psk, current->u.mqtt.pass, sizeof(current->u.mqtt.pass));
			invalid = xf16cam_config_save_mqtt(ssid, (uint16_t)port, user, psk,
			                                   (uint8_t)interval) != 0;
		}
		if (invalid) {
			XF16CAM_HTTP_MESSAGE(fd, "400 Bad Request",
			                     "Invalid MQTT settings. The broker is an IP address; name and password hold up to 31 characters.");
			return 0;
		}
		if (xf16cam_update_begin() != 0) {
			XF16CAM_HTTP_MESSAGE(fd, "409 Conflict",
			                     "MQTT settings were saved, but reboot was deferred by another update.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		XF16CAM_HTTP_MESSAGE(fd, "200 OK", "MQTT settings saved. Rebooting...");
		return XF16CAM_HTTP_COLD_REBOOT;
#endif
	} else if(strcmp(method, "POST") == 0 && strcmp(path, "/api/led") == 0) {
		int led_on = xf16cam_form_value(body, "led_on", mode, sizeof(mode)) == 0 &&
		             strcmp(mode, "true") == 0;
		xf16cam_board_set_led(led_on);
		xf16cam_http_led_json(fd);
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ir_led") == 0) {
		int ir_led_on = xf16cam_form_value(body, "ir_led_on", mode, sizeof(mode)) == 0 &&
		                strcmp(mode, "true") == 0;
		xf16cam_board_set_ir_led(ir_led_on);
		xf16cam_http_ir_led_json(fd);
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ptz") == 0) {
		if (xf16cam_form_value(body, "mode", mode, sizeof(mode)) != 0 ||
		    (strcmp(mode, "left") != 0 && strcmp(mode, "right") != 0 &&
		     strcmp(mode, "up") != 0 && strcmp(mode, "down") != 0 &&
		     strcmp(mode, "home") != 0)) {
			XF16CAM_HTTP_MESSAGE(fd, "400 Bad Request", "Invalid PTZ direction.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		#ifdef NO_PTZ
		XF16CAM_HTTP_MESSAGE(fd, "501 Not Implemented", "PTZ is not supported on this board.");
		return XF16CAM_HTTP_KEEP_RUNNING;
		#else
		if (xf16cam_form_value(body, "mode", mode, sizeof(mode)) == 0){
			if (strcmp(mode, "left") == 0) {
				ptz_move_left();
			} else if (strcmp(mode, "right") == 0) {
				ptz_move_right();
			} else if (strcmp(mode, "up") == 0) {
				ptz_move_up();
			} else if (strcmp(mode, "down") == 0) {
				ptz_move_down();
			} else if (strcmp(mode, "home") == 0) {
				ptz_move_home();
			} else {
				XF16CAM_HTTP_MESSAGE(fd, "400 Bad Request", "Invalid PTZ direction.");
				return XF16CAM_HTTP_KEEP_RUNNING;
			}
			XF16CAM_HTTP_MESSAGE(fd, "200 OK", "PTZ command sent.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		#endif
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/media") == 0) {
			if (xf16cam_form_value(body, "mode", mode, sizeof(mode)) != 0 ||
				xf16cam_config_save_media(strcmp(mode, "web") == 0 ? XF16CAM_MEDIA_WEB :
										strcmp(mode, "rtsp") == 0 ? XF16CAM_MEDIA_RTSP : 0) != 0) {
				XF16CAM_HTTP_MESSAGE(fd, "400 Bad Request", "Invalid camera mode.");
				return XF16CAM_HTTP_KEEP_RUNNING;
			}
			if (xf16cam_update_begin() != 0) {
				XF16CAM_HTTP_MESSAGE(fd, "409 Conflict",
									"Camera mode was saved, but reboot was deferred by another update.");
				return XF16CAM_HTTP_KEEP_RUNNING;
			}
			XF16CAM_HTTP_MESSAGE(fd, "200 OK", "Camera mode saved. Rebooting...");
			return XF16CAM_HTTP_COLD_REBOOT;
		} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/resolution") == 0) {
		XF16CamResolution resolution;

		if (xf16cam_form_value(body, "resolution", mode, sizeof(mode)) != 0 ||
		    (strcmp(mode, "qvga") != 0 && strcmp(mode, "vga") != 0)) {
			XF16CAM_HTTP_MESSAGE(fd, "400 Bad Request", "Invalid camera resolution.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		resolution = strcmp(mode, "vga") == 0 ? XF16CAM_RESOLUTION_VGA :
		                                              XF16CAM_RESOLUTION_QVGA;
		if (resolution == XF16CAM_RESOLUTION_VGA && !xf16cam_sensor_supports_vga()) {
			XF16CAM_HTTP_MESSAGE(fd, "409 Conflict", "The detected sensor does not support VGA output.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		if (xf16cam_config_save_resolution(resolution) != 0) {
			XF16CAM_HTTP_MESSAGE(fd, "500 Internal Server Error", "Could not save camera resolution.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		if (xf16cam_update_begin() != 0) {
			XF16CAM_HTTP_MESSAGE(fd, "409 Conflict",
			                     "Camera resolution was saved, but reboot was deferred by another update.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		XF16CAM_HTTP_MESSAGE(fd, "200 OK", "Camera resolution saved. Rebooting...");
		return XF16CAM_HTTP_COLD_REBOOT;
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/sd/refresh") == 0) {
		if (xf16cam_storage_refresh() != 0)
			XF16CAM_HTTP_MESSAGE(fd, "503 Service Unavailable", "No readable FAT SD card was found.");
		else
			XF16CAM_HTTP_MESSAGE(fd, "200 OK", "SD card mounted. Return to the main page for capacity details.");
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/sd/eject") == 0) {
		if (xf16cam_storage_unmount() != 0)
			XF16CAM_HTTP_MESSAGE(fd, "500 Internal Server Error", "SD card eject failed.");
		else
			XF16CAM_HTTP_MESSAGE(fd, "200 OK", "SD card safely ejected.");
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/sd/format") == 0) {
		if (xf16cam_storage_format() != 0)
			XF16CAM_HTTP_MESSAGE(fd, "500 Internal Server Error", "SD card formatting failed.");
		else
			XF16CAM_HTTP_MESSAGE(fd, "200 OK", "SD card formatted as FAT32.");
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/reboot") == 0) {
		if (xf16cam_update_begin() != 0) {
			XF16CAM_HTTP_MESSAGE(fd, "409 Conflict", "Device is busy.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		XF16CAM_HTTP_MESSAGE(fd, "200 OK", "Rebooting...");
		return XF16CAM_HTTP_COLD_REBOOT;
	#ifdef NO_PTZ
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/hibernate") == 0) {
		if (xf16cam_update_begin() != 0) {
			XF16CAM_HTTP_MESSAGE(fd, "409 Conflict", "Device is busy.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		if (xf16cam_media_quiesce_for_update(XF16CAM_OTA_QUIESCE_MS) != 0) {
			xf16cam_update_end();
			XF16CAM_HTTP_MESSAGE(fd, "503 Service Unavailable",
			                     "Active media could not stop safely. Close stream clients and try again.");
			return XF16CAM_HTTP_KEEP_RUNNING;
		}
		XF16CAM_HTTP_MESSAGE(fd, "200 OK", "Hibernating; press PA20 to wake.");
		return XF16CAM_HTTP_HIBERNATE;
	#endif
	} else {
		XF16CAM_HTTP_MESSAGE(fd, "404 Not Found", "Page not found.");
	}
	return 0;
}

static void xf16cam_http_task(void *arg)
{
	int server = (int)(intptr_t)arg;

	printf("xf16cam HTTP ready: http://%s/\n", xf16cam_net_ip());
	while (1) {
		int client = accept(server, NULL, NULL);
		int action;
		if (client < 0)
			continue;
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &((int){XF16CAM_HTTP_TIMEOUT_MS}), sizeof(int));
		setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &((int){XF16CAM_HTTP_TIMEOUT_MS}), sizeof(int));
		action = xf16cam_http_handle(client);
		if (action == XF16CAM_HTTP_DETACH_CLIENT)
			continue;
		closesocket(client);
		if (action != XF16CAM_HTTP_KEEP_RUNNING) {
			OS_MSleep(750);
			/* All HTTP-triggered restarts pass through this task. Unmount here,
			 * after the response has reached the client, so configuration, media
			 * mode and OTA restarts cannot leave a mounted card dirty. */
			if (xf16cam_storage_unmount() != 0)
				printf("xf16cam HTTP: SD eject failed before restart\n");
			xf16cam_log_flush();
			if (action == XF16CAM_HTTP_OTA_REBOOT)
				ota_reboot();
			/* PTZ builds have no hibernate route or button. */
			#ifdef NO_PTZ
			if (action == XF16CAM_HTTP_HIBERNATE) {
				xf16cam_power_hibernate();
				xf16cam_update_end();
				continue;
			}
			#endif
			HAL_PRCM_SetCPUABootFlag(PRCM_CPUA_BOOT_FROM_COLD_RESET);
			HAL_WDG_Reboot();
		}
	}

	closesocket(server);
	printf("xf16cam HTTP server failed\n");
	OS_ThreadDelete(&g_http_thread);
}

__xip_text
int xf16cam_http_start(void)
{
	struct sockaddr_in address;
	int option = 1;
	int server;

	/* Query the immutable identity once while executing from SRAM. The page
	 * renderer lives in XIP and must never disable the flash containing it. */
	xf16cam_http_flash_info(&g_flash_jedec, &g_flash_size);
	server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (server < 0)
		goto fail;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(XF16CAM_HTTP_PORT);
	address.sin_addr.s_addr = INADDR_ANY;
	if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(server, XF16CAM_MAX_PARALLEL_CLIENTS) != 0)
		goto fail_close;
	if (OS_ThreadCreate(&g_http_thread, "xf16cam-http", xf16cam_http_task,
	                    (void *)(intptr_t)server, OS_THREAD_PRIO_APP,
	                    XF16CAM_HTTP_STACK_SIZE) != OS_OK)
		goto fail_close;
	return 0;

fail_close:
	closesocket(server);
fail:
	printf("xf16cam HTTP start failed\n");
	return -1;
}
