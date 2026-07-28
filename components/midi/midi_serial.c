/*
 * osynth — serial (UART/DIN) MIDI input (SYNTH_ENABLE_SERIAL_MIDI).
 *
 * Classic MIDI byte stream: 31250 baud 8N1, RX only, UART1, pin from
 * Kconfig (OSYNTH_SERIAL_MIDI_RX_GPIO). A small control-plane task
 * reassembles channel-voice messages — running status honored, real-time
 * bytes routed to the seqarp clock (S12; they may interleave anywhere and
 * never disturb the parser state), sysex skipped — and feeds them to the
 * router. Electrical side (optocoupler) in docs/HARDWARE.md.
 */
#include "midi_serial.h"

#include "synth_config.h"

#if SYNTH_ENABLE_SERIAL_MIDI

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "midi.h"

static const char* TAG = "midi_ser";

#define SERIAL_MIDI_UART       UART_NUM_1
#define SERIAL_MIDI_BAUD       31250
#define SERIAL_MIDI_RX_GPIO    CONFIG_OSYNTH_SERIAL_MIDI_RX_GPIO
#define SERIAL_MIDI_TASK_PRIO  10 /* control plane, core 0 */
#define SERIAL_MIDI_TASK_STACK 3072

/* Byte-stream parser state — only the serial task touches it. */
static uint8_t s_status; /* running status, 0 = none */
static uint8_t s_data[2];
static uint8_t s_need;
static uint8_t s_have;

static void parse_byte(uint8_t b) {
    if (b >= 0xF8) {       /* real-time: clock/start/stop -> seqarp (S12) */
        midi_route_realtime(b);
        return;
    }
    if (b >= 0xF0) {       /* system common/sysex cancels running status;
                            * subsequent sysex payload drops as stray data */
        s_status = 0;
        return;
    }
    if (b & 0x80) { /* channel status byte */
        s_status = b;
        s_have = 0;
        const uint8_t kind = b & 0xF0;
        s_need = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
        return;
    }
    if (s_status == 0) return; /* stray data byte */
    s_data[s_have++] = b;
    if (s_have == s_need) {
        midi_route_channel_message(s_status, s_data[0],
                                   (s_need == 2) ? s_data[1] : 0);
        s_have = 0; /* message done; running status stays armed */
    }
}

static void serial_midi_task(void* arg) {
    (void)arg;
    uint8_t buf[64];
    for (;;) {
        const int n = uart_read_bytes(SERIAL_MIDI_UART, buf, sizeof(buf),
                                      pdMS_TO_TICKS(100));
        for (int i = 0; i < n; ++i) {
            parse_byte(buf[i]);
        }
    }
}

esp_err_t midi_serial_start(void) {
    const uart_config_t conf = {
        .baud_rate = SERIAL_MIDI_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(SERIAL_MIDI_UART, &conf);
    if (err != ESP_OK) return err;
    err = uart_set_pin(SERIAL_MIDI_UART, UART_PIN_NO_CHANGE,
                       SERIAL_MIDI_RX_GPIO, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(SERIAL_MIDI_UART, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;

    BaseType_t ok = xTaskCreatePinnedToCore(serial_midi_task, "midi_ser",
                                            SERIAL_MIDI_TASK_STACK, NULL,
                                            SERIAL_MIDI_TASK_PRIO, NULL, 0);
    if (ok != pdPASS) {
        uart_driver_delete(SERIAL_MIDI_UART);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "up: UART%d, RX GPIO%d, %d baud", (int)SERIAL_MIDI_UART,
             (int)SERIAL_MIDI_RX_GPIO, (int)SERIAL_MIDI_BAUD);
    return ESP_OK;
}

#else /* !SYNTH_ENABLE_SERIAL_MIDI */

esp_err_t midi_serial_start(void) { return ESP_OK; }

#endif /* SYNTH_ENABLE_SERIAL_MIDI */
