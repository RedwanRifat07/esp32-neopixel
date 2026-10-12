#include <assert.h>
#include <stdio.h>
#include "../main/led_color.h"

int main(void)
{
    led_rgb_t c = led_color_scaled(0xFFB46E, 100);
    assert(c.r == 0xFF && c.g == 0xB4 && c.b == 0x6E);
    c = led_color_scaled(0xFFB46E, 25);            /* 255*.25=63.75->64, 180*.25=45, 110*.25=27.5->28 */
    assert(c.r == 64 && c.g == 45 && c.b == 28);
    c = led_color_scaled(0xFFFFFF, 0);
    assert(c.r == 0 && c.g == 0 && c.b == 0);
    c = led_color_scaled(0xFFFFFF, 250);           /* clamped to 100 */
    assert(c.r == 255 && c.g == 255 && c.b == 255);
    c = led_color_scaled(0x000000, 50);
    assert(c.r == 0 && c.g == 0 && c.b == 0);
    puts("led_color OK");
    return 0;
}
