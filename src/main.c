/*
Main C file for the light wand project
*/

#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/multicore.h"

#include "neopixels.h"
#include "ADXL343.h"
#include "alphabet.h"

// misc defines
#define BUTTON_DEBOUNCE_TIME_US     1000
#define RAND_UPDATE_TIME_US         100000

// gpio pin defines
#define LED_PIN         25
#define BUTTON_PIN      28
#define ADX_SDA_PIN     16
#define ADX_SCL_PIN     17

// defines relating to wand position
#define ACCEL_MAX_MSS               30
#define DIR_HYSTERESIS_MASK   0x00ffffff

// defines relating to text display
#define PIXEL_CHAR_COLOR        urgbw_u32(0, 0, 255, 128)
#define PIXEL_BG_COLOR          urgbw_u32(0, 0, 0, 0)   
#define PIXEL_REST_COLOR        urgbw_u32(0, 0, 0, 0)

// choose the message to display on the wand
// the message will be centered in the columns, and scaled as well
#define MESSAGE_LEN             3
#define MESSAGE_CHAR_SCALE      2   // 2 to double the width of the characters
#define N_DISPLAY_COLUMNS       200
const uint32_t *message[MESSAGE_LEN] = {CHAR_E, CHAR_C, CHAR_E};

// function prototypes
void core1_main(void);
void core1_sio_irq(void);
void signal_dirchange(uint64_t swing_time, uint64_t dir_hist);
int build_columns(const uint32_t **message, int m_len, uint32_t *columns, int n_cols, int scale);
void gpio_callback(uint gpio, uint32_t events);

// display state
enum State {
    POV,
    RANDOM
};
// volatile global state so that GPIO interrupts can change the state
volatile enum State current_state;

// Core 0 main handles wand position calculations
int main() {
    int i, err;

    stdio_init_all();

    // Delay with some LED blinking on startup
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    for (int i = 0; i < 5; i++) {
        sleep_ms(50);
        gpio_put(LED_PIN, 1);
        sleep_ms(50);
        gpio_put(LED_PIN, 0);
    }

    printf("Starting...\n");
    // initialize the accelerometer
    adxl343 accelerometer;
    err = adxl343_setup(&accelerometer, i2c0, ADX_SDA_PIN, ADX_SCL_PIN, ADXL343_DEFAULT_ADDRESS);
    if (err < 0) {
        printf("ADXL343 Setup failed... error %d\n", err);
    }
    printf("Accelerometer setup complete...\n");  

    // initialize the LED strip
    setup_ws2812();
    put_15_pixels_on_rgbw(urgbw_u32(0, 255, 0, 128));
    printf("LED's lit green\n");

    // set up the pushbutton
    gpio_init(BUTTON_PIN);
    gpio_set_irq_enabled_with_callback(BUTTON_PIN, GPIO_IRQ_EDGE_RISE, true, &gpio_callback);
    gpio_pull_down(BUTTON_PIN);

    // Launch the second core
    multicore_launch_core1(core1_main);

    // variables relating to wand position
    int16_t ax_raw;
    float accel_mss = 0;
    float prev_accel_mss = 0;
    float jerk_msss = 0;
    uint64_t prev_dir_change_time = 0;
    uint64_t prev_swing_time_length = 0;
    uint64_t prev_frame_time = 0;
    uint64_t hidden_dir_hist = 0;  // lsb is current hidden direction
    uint64_t diplay_dir_hist = 0;  // lsb is current display direction.

    // variables relating to rand mode
    uint64_t prev_rand_update_us = 0;

    while(1) {   
        uint64_t now = time_us_64();

        // update raw adx reading
        adxl343_getx(&accelerometer, &ax_raw);

        // Logic for POV effect
        if (current_state == POV)
        {
            gpio_put(LED_PIN, 0);

            // convert to acceleration in meters/s^2
            accel_mss = (float)ax_raw * ADXL3XXVAL_TO_MSS;

            // calculate the jerk of the wand in meters/s^3
            jerk_msss = (accel_mss - prev_accel_mss) / ((float)(now - prev_frame_time) / 1000000.0f);
            
            // caculate the direction of the wand based on jerk
            if (jerk_msss < 0) {
                // if jerk is negative, wand is moving 'left' (0)
                hidden_dir_hist = (hidden_dir_hist << 1) | 0;
            }
            else if (jerk_msss > 0) {
                // if jerk is positive, wand is moving 'right' (1)
                hidden_dir_hist = (hidden_dir_hist << 1) | 1;
            }
            else {
                // otherwise, assume wand is continuing in the same direction
                i = hidden_dir_hist & 1;
                hidden_dir_hist = (hidden_dir_hist << 1) | i;
            }

            // update the displayed direction based on the hidden direction, using hysteresis.
            if (((hidden_dir_hist & DIR_HYSTERESIS_MASK) == 0) && ((diplay_dir_hist & 1) == 1)) {
                // all previous hidden directions in DIR_HYSTERESIS_MASK were '0' - set display direction to 0
                diplay_dir_hist = (diplay_dir_hist << 1) | 0;

                // the direction of the wand has changed - update the amount of time the swing that just ended took
                prev_swing_time_length = now - prev_dir_change_time;
                prev_dir_change_time = now;

                signal_dirchange(prev_swing_time_length, diplay_dir_hist);
            }
            else if (((hidden_dir_hist & DIR_HYSTERESIS_MASK) == DIR_HYSTERESIS_MASK) && ((diplay_dir_hist & 1) == 0)) {
                // all previous hidden directions in DIR_HYSTERESIS_MASK were '1' - set display direction to 1
                diplay_dir_hist = (diplay_dir_hist << 1) | 1;

                // the direction of the wand has changed - update the amount of time the swing that just ended took
                prev_swing_time_length = now - prev_dir_change_time;
                prev_dir_change_time = now;

                signal_dirchange(prev_swing_time_length, diplay_dir_hist);
            }
            else {
                // If there is no cause to change the display direction, just set it to the previous direction
                i = diplay_dir_hist & 1;
                diplay_dir_hist = (diplay_dir_hist << 1) | i;
            }

            prev_accel_mss = accel_mss;
            prev_frame_time = now;
        }
        // Logic for random display
        else if (current_state == RANDOM) {
            gpio_put(LED_PIN, 1);
            if (prev_rand_update_us + RAND_UPDATE_TIME_US < now) {
                put_15_random_rgbw(abs(ax_raw));
                prev_rand_update_us = time_us_64();
            }
        } 
    } 

    return 1;
}


// core1_main handles displaying on the LED strip
void core1_main(void)
{
    uint32_t fifo_val;
    uint64_t col_display_time, prev_swing_length, render_time;
    int dir, i;

    printf("Launched core1\n");

    // set up the columns
    uint32_t *columns = (uint32_t *)malloc(N_DISPLAY_COLUMNS * sizeof(uint32_t));
    i = build_columns(message, MESSAGE_LEN, columns, N_DISPLAY_COLUMNS, MESSAGE_CHAR_SCALE);
    if (i < 0) {
        printf("Failed to build the columns\n");
        return;
    }

    while (1) {
        // wait for something to appear on the fifo
        while (!multicore_fifo_rvalid())
            tight_loop_contents();

        // now that something is on the fifo, pop it all off
        while (multicore_fifo_rvalid())
            fifo_val = multicore_fifo_pop_blocking();

        // extract the prev swing length from the fifo value
        prev_swing_length = (uint64_t)(fifo_val & ~(1 << 31));

        // extract the current direction from the fifo value
        dir = (int)(fifo_val >> 31);

        // calculate the amount of time in us each column should be displayed
        col_display_time = prev_swing_length / N_DISPLAY_COLUMNS;

        // now, loop for the duration of the swing, in the proper direction 
        for (i = 0; i < N_DISPLAY_COLUMNS; i++) {
            // If something new shows up on the FIFO, finish the swing
            if (multicore_fifo_rvalid()) {
                break;
            }

            // index in the proper direction
            if (dir == 0)
                render_time = put_15_pixels_rgbw(columns[N_DISPLAY_COLUMNS-i-1], PIXEL_CHAR_COLOR, PIXEL_BG_COLOR);
            else
                render_time = put_15_pixels_rgbw(columns[i], PIXEL_CHAR_COLOR, PIXEL_BG_COLOR);

            // sleep the remaining amount of column time
            sleep_us(col_display_time - render_time);
        }

        // printf("finished %d/%d\n", i, N_DISPLAY_COLUMNS);
    }

    free(columns);
    return;
}


// Sends a message to Core 1 that starts displaying the next cycle
void signal_dirchange(uint64_t swing_time, uint64_t dir_hist)
{
    // printf("%f\t\n", swing_time / 1000000.0f);

    // tell core1 to start displaying a new swing. Send 31 bits of prev_swing_time_length,
    // with the MSB of the transferred 32 bit value being the direction of the wand.
    // 31 bits of prev_swing_time_length corresponds to 2^31 * 10^-6 = 2.1k seconds maximum swing time
    // In other words, as long as the user completes their swing in under half an hour, there should be no issues.
    multicore_fifo_push_blocking((uint32_t)(swing_time | (dir_hist << 31)));
}


/*
Converts the message (an array of arrays representing characters) into a 1-D array of column data
The message is centered in the columns, and scaled by 'scale'
Returns -1 on failure
*/
int build_columns(const uint32_t **message, int m_len, uint32_t *columns, int n_cols, int scale)
{
    int i, j;
    int m_col_width = m_len * CHAR_WIDTH;

    // check that the message will fit
    if (n_cols < m_col_width * scale) 
        return -1;

    j = 0;
    for (i = 0; i < n_cols; i++) {
        columns[i] = 0;

        // only write the characters to the columns when near the center
        if ((i >= n_cols/2 - ((m_col_width / 2) * scale)) && (i <= n_cols/2 + ((m_col_width / 2) * scale))) {
            columns[i] = message[(j / scale) / CHAR_WIDTH][(j / scale) % CHAR_WIDTH]; 
            j++;
        }
    }

    return 0;
}

void gpio_callback(uint gpio, uint32_t events) {
    static uint64_t last_pressed_us = 0;
    if(gpio==BUTTON_PIN && (time_us_64() - last_pressed_us) > BUTTON_DEBOUNCE_TIME_US) {
        current_state = (current_state + 1) % (RANDOM + 1);
        last_pressed_us = time_us_64();
    }
}