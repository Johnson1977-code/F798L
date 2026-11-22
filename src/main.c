#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>

static uint8_t framebuf[466 * 2] __aligned(4);

int main(void)
{
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

    printk("boot\n");

    if (!device_is_ready(disp)) {
        printk("display not ready\n");
        return 0;
    }
    
    display_blanking_off(disp);
    printk("blanking_off done\n");

    for (int i = 0; i < 466; i++) {
        ((uint16_t *)framebuf)[i] = 0xF800;   /* 红 */
    }

    struct display_buffer_descriptor desc = {
        .width = 466, .height = 1, .pitch = 466, .buf_size = 466 * 2,
    };

    int r = display_write(disp, 0, 233, &desc, framebuf);
    printk("write ret=%d\n", r);
    return 0;
}