#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

// Shared state between the TV-session task (writer) and HTTP handlers
// (readers / command submitters).

typedef enum {
    ATV_STATE_BOOT,         // starting up / connecting WiFi
    ATV_STATE_PAIRING,      // pairing exchange in progress
    ATV_STATE_WAIT_CODE,    // TV is showing the code; waiting for web form
    ATV_STATE_PAIR_FAILED,  // last attempt failed; retrying
    ATV_STATE_PAIRED,       // paired, control channel not up yet
    ATV_STATE_CONNECTED,    // control channel live
} atv_state_t;

// From remote_start.started. Tri-state so the UI can distinguish "not
// known yet" (just booted/reconnected) from a confirmed off.
typedef enum {
    ATV_POWER_UNKNOWN,
    ATV_POWER_ON,
    ATV_POWER_OFF,
} atv_power_t;

typedef struct {
    volatile bool paired;
    volatile bool connected;
    volatile atv_state_t state;
    volatile atv_power_t power;
    // From remote_set_volume_level. max == 0 means the device delegates
    // volume to the TV over CEC and the protocol cannot change it.
    volatile int vol_level;
    volatile int vol_max;
    volatile bool vol_muted;
} atv_status_t;

extern atv_status_t g_atv_status;

// Independent connection state for the Samsung TV WebSocket volume
// fallback (see samsung_tv.c). Unused/always-disconnected if
// CONFIG_ATV_SAMSUNG_TV_IP is empty.
typedef struct {
    volatile bool connected;
} samsung_status_t;
extern samsung_status_t g_samsung_status;

// Pairing code entered in the web UI: 6 hex chars + NUL.
#define PAIR_CODE_LEN 6
extern QueueHandle_t g_pair_code_queue;  // item: char[PAIR_CODE_LEN + 1]

// Result bits for a submitted code, so POST /api/pair can respond.
#define PAIR_EVENT_OK BIT0
#define PAIR_EVENT_FAIL BIT1
extern EventGroupHandle_t g_pair_events;

// Key commands for the control channel. direction is a RemoteDirection
// value: SHORT for taps, START_LONG/END_LONG for press-and-hold (volume
// keys only respond to the press/release pair on Chromecast).
typedef struct {
    int code;       // RemoteKeyCode
    int direction;  // RemoteDirection
} key_cmd_t;
extern QueueHandle_t g_key_queue;

// App deep links for RemoteAppLinkLaunchRequest (e.g. "netflix://home").
#define APP_LINK_MAX 128
extern QueueHandle_t g_app_queue;  // item: char[APP_LINK_MAX]

void app_state_init(void);

const char *atv_state_str(atv_state_t s);
const char *atv_power_str(atv_power_t p);
