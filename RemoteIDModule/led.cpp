#include <Arduino.h>
#include "led.h"
#include "board_config.h"

Led led;

void Led::init(void)
{
    if (done_init) {
        return;
    }
    done_init = true;
#ifdef PIN_STATUS_LED
    pinMode(PIN_STATUS_LED, OUTPUT);
#ifdef STATUS_LED_HEARTBEAT
    digitalWrite(PIN_STATUS_LED, !STATUS_LED_ACTIVE);
    last_led_trig_ms = millis();
#endif
#endif
#ifdef WS2812_LED_PIN
    pinMode(WS2812_LED_PIN, OUTPUT);
    ledStrip.begin();
#endif
}

#ifdef STATUS_LED_INIT_DISPLAY_MS
void Led::start_init_display(void)
{
    init();
    digitalWrite(PIN_STATUS_LED, !STATUS_LED_ACTIVE);
    last_led_trig_ms = millis();
    init_display_active = true;
}
#endif

void Led::update(void)
{
    init();

    const uint32_t now_ms = millis();

#ifdef PIN_STATUS_LED
#ifdef STATUS_LED_HEARTBEAT
    bool init_display_override = false;
#ifdef STATUS_LED_INIT_DISPLAY_MS
    if (init_display_active) {
        const uint32_t init_display_elapsed_ms = now_ms - last_led_trig_ms;
        if (init_display_elapsed_ms < STATUS_LED_INIT_DISPLAY_MS) {
            const bool led_active =
                ((init_display_elapsed_ms / STATUS_LED_INIT_TOGGLE_MS) & 1U) != 0;
            digitalWrite(PIN_STATUS_LED,
                         led_active ? STATUS_LED_ACTIVE : !STATUS_LED_ACTIVE);
            init_display_override = true;
        } else {
            init_display_active = false;
            last_state = state;
            last_led_trig_ms = now_ms;
        }
    }
#endif

    if (!init_display_override) {
        if (state != last_state) {
            last_state = state;
            last_led_trig_ms = now_ms;
        }

        switch (state) {
        case LedState::INIT:
            if (now_ms - last_led_trig_ms >= STATUS_LED_INIT_TOGGLE_MS) {
                digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
                last_led_trig_ms = now_ms;
            }
            break;

        case LedState::ARM_OK: {
            uint32_t heartbeat_phase_ms = now_ms - last_led_trig_ms;
            if (heartbeat_phase_ms >= STATUS_LED_HEARTBEAT_PERIOD_MS) {
                heartbeat_phase_ms %= STATUS_LED_HEARTBEAT_PERIOD_MS;
                last_led_trig_ms = now_ms - heartbeat_phase_ms;
            }
            digitalWrite(PIN_STATUS_LED,
                         heartbeat_phase_ms < STATUS_LED_HEARTBEAT_ON_MS ?
                             STATUS_LED_ACTIVE : !STATUS_LED_ACTIVE);
            break;
        }

        case LedState::PFST_FAIL:
        case LedState::ARM_FAIL:
            digitalWrite(PIN_STATUS_LED, STATUS_LED_ACTIVE);
            break;
        }
    }
#else
    switch (state) {
    case LedState::ARM_OK: {
        digitalWrite(PIN_STATUS_LED, STATUS_LED_OK);
        last_led_trig_ms = now_ms;
        break;
    }

    default:
        if (now_ms - last_led_trig_ms > 100) {
            digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
            last_led_trig_ms = now_ms;
        }
        break;
    }
#endif
#endif

#ifdef WS2812_LED_PIN
    ledStrip.clear();

    switch (state) {
    case LedState::ARM_OK:
        ledStrip.setPixelColor(0, ledStrip.Color(0, 255, 0));
        ledStrip.setPixelColor(0, ledStrip.Color(1, 255, 0)); //for db210pro, set the second LED to have the same output (for now)
        break;

    default:
        ledStrip.setPixelColor(0, ledStrip.Color(255, 0, 0));
        ledStrip.setPixelColor(1, ledStrip.Color(255, 0, 0)); //for db210pro, set the second LED to have the same output (for now)
        break;
    }
    if (now_ms - last_led_strip_ms >= 200) {
        last_led_strip_ms = now_ms;
        ledStrip.show();
    }
#endif
}
