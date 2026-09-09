#pragma once

#include <stdint.h>
#include "board_config.h"

#ifdef WS2812_LED_PIN
#include <Adafruit_NeoPixel.h>
#endif

class Led {
public:
    enum class LedState {
        INIT=0,
        PFST_FAIL,
        ARM_FAIL,
        ARM_OK
    };

    void set_state(LedState _state) {
        state = _state;
    }
    void update(void);
#ifdef STATUS_LED_INIT_DISPLAY_MS
    void start_init_display(void);
#endif

private:
    void init(void);
    bool done_init;
    uint32_t last_led_trig_ms;
    LedState state;

#ifdef STATUS_LED_HEARTBEAT
    LedState last_state = LedState::INIT;
#ifdef STATUS_LED_INIT_DISPLAY_MS
    bool init_display_active = false;
#endif
#endif

#ifdef WS2812_LED_PIN
    uint32_t last_led_strip_ms;
    Adafruit_NeoPixel ledStrip{2, WS2812_LED_PIN, NEO_GRB + NEO_KHZ800}; //the BlueMark db210pro boards has two LEDs, therefore we need to use 2.
#endif
};

extern Led led;
