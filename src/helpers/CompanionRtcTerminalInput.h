#pragma once

// Only the qualified Wireless Paper Full recipe moves its existing CPU-only
// input lines. They never back an SDK/DMA queue and retain their full sizes.
#ifndef COMPANION_RTC_TERMINAL_INPUT
#define COMPANION_RTC_TERMINAL_INPUT 0
#endif
#if COMPANION_RTC_TERMINAL_INPUT != 0 && COMPANION_RTC_TERMINAL_INPUT != 1
#error "RTC terminal input must be 0 or 1"
#endif

#if COMPANION_RTC_TERMINAL_INPUT
#if !defined(ESP32_PLATFORM) || !defined(COMPANION_RADIO_FULL) \
    || !defined(HELTEC_WIRELESS_PAPER) || !defined(ENABLE_USB_INTERFACE) \
    || !defined(WIFI_SSID) || !defined(ENABLE_OTA)
#error "RTC terminal input requires the qualified Wireless Paper Full recipe"
#endif
#if COMPANION_RTC_FAST_OFFLINE_FRAMES || COMPANION_RTC_SLOW_OFFLINE_FRAMES
#error "RTC terminal input cannot share its bank with RTC offline frames"
#endif
#include <sdkconfig.h>
#include <esp_attr.h>
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "RTC terminal input is qualified only on ESP32-S3"
#endif
#if !CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
#error "RTC terminal input requires byte-accessible RTC FAST memory"
#endif
#define MESH_COMPANION_TERMINAL_INPUT_ATTR alignas(4) RTC_FAST_ATTR
#else
#define MESH_COMPANION_TERMINAL_INPUT_ATTR
#endif
