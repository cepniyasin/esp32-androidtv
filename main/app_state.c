#include "app_state.h"

atv_status_t g_atv_status;
samsung_status_t g_samsung_status;
QueueHandle_t g_pair_code_queue;
EventGroupHandle_t g_pair_events;
QueueHandle_t g_key_queue;
QueueHandle_t g_app_queue;

void app_state_init(void)
{
    g_pair_code_queue = xQueueCreate(1, PAIR_CODE_LEN + 1);
    g_pair_events = xEventGroupCreate();
    g_key_queue = xQueueCreate(8, sizeof(key_cmd_t));
    g_app_queue = xQueueCreate(2, APP_LINK_MAX);
    g_atv_status.state = ATV_STATE_BOOT;
}

const char *atv_state_str(atv_state_t s)
{
    switch (s) {
    case ATV_STATE_BOOT: return "boot";
    case ATV_STATE_PAIRING: return "pairing";
    case ATV_STATE_WAIT_CODE: return "wait_code";
    case ATV_STATE_PAIR_FAILED: return "pair_failed";
    case ATV_STATE_PAIRED: return "paired";
    case ATV_STATE_CONNECTED: return "connected";
    }
    return "unknown";
}
