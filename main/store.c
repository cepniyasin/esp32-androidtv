#include "store.h"

#include "nvs.h"

#define NS "atv"
#define KEY_PAIRED "paired"

bool store_get_paired(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    nvs_get_u8(h, KEY_PAIRED, &v);
    nvs_close(h);
    return v != 0;
}

esp_err_t store_set_paired(bool paired)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KEY_PAIRED, paired ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
