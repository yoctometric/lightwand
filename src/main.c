#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/double.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "neopixels.h"
#include "ADXL343.h"
#include "alphabet.h"

// gpio pin defines
#define LED_PIN         25
#define ADX_SDA_PIN     16
#define ADX_SCL_PIN     17

// defines relating to the wand transform
#define ACCEL_MAX_MSS           30
#define DIRECTION_TIMEOUT_US    1000 * 500
#define JERK_AVERAGING_WINDOW_WIDTH     50

// defines relating to text display
#define COL_SHOW_TIME_US        1000 * 2

#define PIXEL_CHAR_COLOR        urgb_u32(90, 149, 207)
#define PIXEL_BG_COLOR          urgb_u32(0, 15, 0)   
#define PIXEL_REST_COLOR        urgb_u32(0, 0, 15)

// choose the message to display on the wand
#define MESSAGE_LEN             3
#define MESSAGE_LEN_COLUMNS     MESSAGE_LEN * CHAR_WIDTH
const uint32_t *message[MESSAGE_LEN] = {CHAR_E, CHAR_C, CHAR_E};

int main() {
    int i, err;

    stdio_init_all();

    // Delay with some LED blinking on startup
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    for (int i = 0; i < 5; i++) {
        sleep_ms(250);
        gpio_put(LED_PIN, 1);
        sleep_ms(250);
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
    put_15_pixels_on(urgb_u32(0x0f, 0xbf, 0x0f));
    printf("LED's lit green\n");

    // allocate memory for the jerk averaging window
    double *jerk_history = calloc(sizeof(double), JERK_AVERAGING_WINDOW_WIDTH);

    // variables relating to wand position
    int16_t az_raw;
    double accel_mss = 0;
    double average_jerk = 0;
    int dir = 0;    // what direction is the wand moving in
    uint64_t last_moved_time_us = 0;

    // variables for tracking changes across frames
    uint64_t prev_now = 0;
    double prev_accel_mss = 0;

    // variables relating to the text message
    uint64_t last_changed_col_time_us = 0;
    int message_index = -1;

    while(1) {   
        uint64_t now = time_us_64();

        // update raw adx reading
        adxl343_getz(&accelerometer, &az_raw);

        // convert to acceleration in meters/s^2
        accel_mss = (double)az_raw * ADXL3XXVAL_TO_MSS;

        // calculate the jerk of the wand based on the previous frame's acceleration and dt, and
        // move it onto the jerk history buffer. jerk is NOT calculated to be in m/s^3
        for (i = JERK_AVERAGING_WINDOW_WIDTH-1; i > 0; i--) {
            jerk_history[i] = jerk_history[i-1];
        }
        jerk_history[0] = (accel_mss - prev_accel_mss) / ((double)(now - prev_now));;

        // calculate the average jerk
        average_jerk = 0;
        for (i = 0; i < JERK_AVERAGING_WINDOW_WIDTH; i++) {
            average_jerk += jerk_history[i];
        }
        average_jerk = average_jerk / JERK_AVERAGING_WINDOW_WIDTH;

        // debug print mss
        printf("%.07f\t%.07f\t%d\n", jerk_history[0], average_jerk, dir);

        // update "direction" of stick when the acceleration changes suddenly
        if (average_jerk < 0) {
            last_moved_time_us = now;
            dir = -1;

            if (message_index == -1) // handle restarting from timeout
                message_index = MESSAGE_LEN_COLUMNS - 1;
        }
        else if (average_jerk > 0) {
            last_moved_time_us = now;
            dir =  1;
            
            if (message_index == -1) // handle restarting from timeout
                message_index = 0;
        }
        else if (last_moved_time_us < (now - DIRECTION_TIMEOUT_US)) {
            // if the wand has not 'changed direction' in a while, assume it has stopped moving and timeout
            dir = 0;
            message_index = -1;
            put_15_pixels_on(PIXEL_REST_COLOR);
        }

        // display the direction of the wand on the wand
        if (dir == -1)
            put_15_pixels_on(urgb_u32(255, 0, 0));
        else if (dir == 1)
            put_15_pixels_on(urgb_u32(0, 255, 0));

        /*
        // scroll through the columns of the characters of the message
        if ((last_changed_col_time_us + COL_SHOW_TIME_US < now) && message_index != -1) {
            message_index = message_index + dir;
            if (message_index > MESSAGE_LEN_COLUMNS - 1) message_index = 0;
            if (message_index < 0) message_index = MESSAGE_LEN_COLUMNS -1;
            last_changed_col_time_us = now;

            put_15_pixels(message[message_index/CHAR_WIDTH][message_index % CHAR_WIDTH], 
                PIXEL_CHAR_COLOR, PIXEL_BG_COLOR);
        }
        */

        prev_now = now;
       prev_accel_mss = accel_mss;    
    } 

    free(jerk_history);

    return 1;
}