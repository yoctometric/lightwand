#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h"

#include <stdio.h>

// lights up a full 30 pixel (1m) neopixel strip as one color
static inline void put_30_pixels(uint32_t pixel_grb) {
    /*
    pio_sm_put_blocking will always put out 32 bits, but we want to send 24 bit chunks in a
    continuous stream. Therefore, some slightly more complicated looping behaviour is required
    */

    /*
    int i;
    for (i=0; i<30; i++) {
        pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
    }
    sleep_us(50);
    */

    
    int color_channel = 0; // 0: green, 1: red, 2: blue
    int transmitted_bytes = 0;
    int i;
    while(1) {
        uint32_t msg = 0;
        for(i=3; i>=0; i--) {
            // get the next color channel value
            uint8_t color_val = ((pixel_grb >> ((2-color_channel) * 8u)) & 0xff);

            // push the next color channel value onto the transmission message
            msg |= color_val << (i * 8u); 
            color_channel = (color_channel + 1) % 3;
            transmitted_bytes++;
        }

        // transmit the next 32-bit chunk of control data
        pio_sm_put_blocking(pio0, 0, msg);

        if (transmitted_bytes >= 90) {
            // 90 bytes of color data, for 30 pixels, have been transmitted
            break;
        }
    }

    // wait for the tx fifo to drain
    while (!pio_sm_is_tx_fifo_empty(pio0, 0))
        sleep_us(1);

    /*
    The ws2812 datasheet indicates that 50us must be waited between each set of transmissions. However, I found that for
    my use case 200us were needed. A possible explanation is that when pio_sm_is_tx_fifo_empty() returns false, the last 32-bit
    message is still being transmitted over the data line. Whatever the case, this works, and we don't need the wand to flicker
    particularly fast anyway 
    */
    sleep_us(200);    
}

// taken from hutscape.github.io. Converts r, g, b, to urgb
static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return  ((uint32_t)(b) << 0)    |
            ((uint32_t)(r) << 8)    |
            ((uint32_t)(g) << 16);
}

// sets up the ws2812 controller
void setup_ws2812();
