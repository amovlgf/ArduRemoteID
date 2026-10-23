#pragma once
#include <cstddef>
using esp_err_t = int;
using nvs_handle = unsigned;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NVS_NOT_FOUND=1, NVS_READWRITE=1;
esp_err_t nvs_open(const char *, int, nvs_handle *);
void nvs_close(nvs_handle);
esp_err_t nvs_get_blob(nvs_handle, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle, const char *, const void *, size_t);
esp_err_t nvs_erase_key(nvs_handle, const char *);
esp_err_t nvs_commit(nvs_handle);
