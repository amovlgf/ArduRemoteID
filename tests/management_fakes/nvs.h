#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t=int;
using nvs_handle=unsigned;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NVS_NOT_FOUND=1,ESP_ERR_NVS_TYPE_MISMATCH=2,NVS_READONLY=0,NVS_READWRITE=1;
esp_err_t nvs_open(const char *,int,nvs_handle *);
void nvs_close(nvs_handle);
esp_err_t nvs_commit(nvs_handle);
esp_err_t nvs_erase_all(nvs_handle);
esp_err_t nvs_erase_key(nvs_handle,const char *);
esp_err_t nvs_get_u8(nvs_handle,const char *,uint8_t *);
esp_err_t nvs_get_i8(nvs_handle,const char *,int8_t *);
esp_err_t nvs_get_u32(nvs_handle,const char *,uint32_t *);
esp_err_t nvs_get_str(nvs_handle,const char *,char *,size_t *);
esp_err_t nvs_get_blob(nvs_handle,const char *,void *,size_t *);
esp_err_t nvs_set_u8(nvs_handle,const char *,uint8_t);
esp_err_t nvs_set_i8(nvs_handle,const char *,int8_t);
esp_err_t nvs_set_u32(nvs_handle,const char *,uint32_t);
esp_err_t nvs_set_str(nvs_handle,const char *,const char *);
esp_err_t nvs_set_blob(nvs_handle,const char *,const void *,size_t);
