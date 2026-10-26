#pragma once

// ENV_PIN_SDA / ENV_PIN_SCL select a dedicated environmental-sensor bus.
// GPIO 0 is valid on supported targets, while negative values are the common
// disconnected-pin sentinel. Keep every consumer on the same validity rule.
#if defined(ENV_PIN_SDA) && defined(ENV_PIN_SCL) \
    && (ENV_PIN_SDA >= 0) && (ENV_PIN_SCL >= 0)
#if defined(HELTEC_V4_DISPLAY_UNIFIED) && HELTEC_V4_DISPLAY_UNIFIED \
    && defined(PIN_BOARD_SDA) && defined(PIN_BOARD_SCL) \
    && ENV_PIN_SDA == PIN_BOARD_SDA && ENV_PIN_SCL == PIN_BOARD_SCL
// Unified V4 reserves controller 1 for the display/touch probe. Expansion
// sensors share the primary board bus, not a second controller on those pins.
#define ENV_HAS_SECONDARY_I2C 0
#else
#define ENV_HAS_SECONDARY_I2C 1
#endif
#else
#define ENV_HAS_SECONDARY_I2C 0
#endif
