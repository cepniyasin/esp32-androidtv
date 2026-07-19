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

typedef struct {
    volatile bool paired;
    volatile bool connected;
    volatile atv_state_t state;
} atv_status_t;

extern atv_status_t g_atv_status;

// Pairing code entered in the web UI: 6 hex chars + NUL.
#define PAIR_CODE_LEN 6
extern QueueHandle_t g_pair_code_queue;  // item: char[PAIR_CODE_LEN + 1]

// Result bits for a submitted code, so POST /api/pair can respond.
#define PAIR_EVENT_OK BIT0
#define PAIR_EVENT_FAIL BIT1
extern EventGroupHandle_t g_pair_events;

// Key commands for the control channel: int RemoteKeyCode values.
extern QueueHandle_t g_key_queue;

void app_state_init(void);

const char *atv_state_str(atv_state_t s);
