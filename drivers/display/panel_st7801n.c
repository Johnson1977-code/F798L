/*
 * Sitronix ST7801N AMOLED panel driver
 * nRF54L15 sQSPI (MSPI shim), NCS v3.4.0 / Zephyr 4.4
 */

#define DT_DRV_COMPAT sitronix_st7801n

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(st7801n, CONFIG_DISPLAY_LOG_LEVEL);

/* ---------------- 私有结构 ---------------- */

struct st7801n_config {
    uint16_t width;
    uint16_t height;
    const struct gpio_dt_spec reset;
    const struct gpio_dt_spec te;
};

struct st7801n_data {
    const struct device *mspi;
    struct mspi_dev_id dev_id;
    bool inited;
};

/* ---------------- 底层：DCS 命令发送 ---------------- */

/* DCS 无参: mipi.write 0x05 <cmd> */
static int st7801n_dcs0(const struct device *dev, uint8_t prefix, uint8_t cmd)
{
    struct st7801n_data *d = dev->data;

    uint8_t tx[1] = { cmd };

    struct mspi_xfer_packet pkt = {
        .dir       = MSPI_TX,
        .cb_mask   = MSPI_BUS_NO_CB,
        .cmd       = prefix,
        .address   = 0,
        .num_bytes = sizeof(tx),
        .data_buf  = tx,
    };

    struct mspi_xfer xfer = {
        .async       = false,
        .xfer_mode   = MSPI_PIO,
        .cmd_length  = 1,
        .addr_length = 0,
        .hold_ce     = false,
        .packets     = &pkt,
        .num_packet  = 1,
        .timeout     = 1000,
    };

    int ret = mspi_transceive(d->mspi, &d->dev_id, &xfer);
    if (ret) {
        LOG_ERR("dcs0 prefix=0x%02x cmd=0x%02x ret=%d", prefix, cmd, ret);
    }
    return ret;
}

/* DCS 有参: mipi.write 0x29 <cmd> <data...> */
static int st7801n_dcs(const struct device *dev, uint8_t prefix,
                       uint8_t cmd, const uint8_t *data, size_t len)
{
    struct st7801n_data *d = dev->data;

    if (len > 31) {
        return -EINVAL;
    }

    uint8_t tx[32];
    tx[0] = cmd;
    memcpy(&tx[1], data, len);

    struct mspi_xfer_packet pkt = {
        .dir       = MSPI_TX,
        .cb_mask   = MSPI_BUS_NO_CB,
        .cmd       = prefix,
        .address   = 0,
        .num_bytes = 1 + len,
        .data_buf  = tx,
    };

    struct mspi_xfer xfer = {
        .async       = false,
        .xfer_mode   = MSPI_PIO,
        .cmd_length  = 1,
        .addr_length = 0,
        .hold_ce     = true,
        .packets     = &pkt,
        .num_packet  = 1,
        .timeout     = 1000,
    };

    int ret = mspi_transceive(d->mspi, &d->dev_id, &xfer);
    if (ret) {
        LOG_ERR("dcs prefix=0x%02x cmd=0x%02x len=%d ret=%d",
                prefix, cmd, (int)len, ret);
    }
    return ret;
}

static inline int st7801n_cmd0(const struct device *dev, uint8_t cmd)
{
    return st7801n_dcs0(dev, 0x05, cmd);
}

static inline int st7801n_cmd(const struct device *dev, uint8_t cmd,
                              const uint8_t *data, size_t len)
{
    return st7801n_dcs(dev, 0x29, cmd, data, len);
}

/* ---------------- 硬件复位 ---------------- */

static void st7801n_hw_reset(const struct device *dev)
{
    const struct st7801n_config *cfg = dev->config;

    if (cfg->reset.port == NULL) {
        return;
    }

    gpio_pin_set_dt(&cfg->reset, 0);
    k_msleep(20);
    gpio_pin_set_dt(&cfg->reset, 1);
    k_msleep(120);
}

/* ---------------- 像素写（只发 0x2C + 数据） ---------------- */

static int st7801n_write_pixels(const struct device *dev,
                                const uint8_t *rgb565, size_t len)
{
    struct st7801n_data *d = dev->data;

    struct mspi_xfer_packet pkt = {
        .dir       = MSPI_TX,
        .cb_mask   = MSPI_BUS_NO_CB,
        .cmd       = 0x2C,
        .address   = 0,
        .num_bytes = len,
        .data_buf  = (uint8_t *)rgb565,
    };

    struct mspi_xfer xfer = {
        .async       = false,
        .xfer_mode   = MSPI_PIO,
        .cmd_length  = 1,
        .addr_length = 0,
        .hold_ce     = true,
        .packets     = &pkt,
        .num_packet  = 1,
        .timeout     = 2000,
    };

    int ret = mspi_transceive(d->mspi, &d->dev_id, &xfer);
    if (ret) {
        LOG_ERR("write_pixels len=%u ret=%d", (unsigned)len, ret);
    }
    return ret;
}

/* ---------------- panel 初始化序列 ---------------- */

static int st7801n_panel_init(const struct device *dev)
{
    int ret;
    uint8_t p;

    printk("INIT START\n");
    st7801n_hw_reset(dev);

    /* Sleep Out */
    ret = st7801n_dcs0(dev, 0x05, 0x11);
    if (ret) return ret;
    k_msleep(120);

    /* 0x53 0x20 */
    p = 0x20;
    ret = st7801n_dcs(dev, 0x29, 0x53, &p, 1);
    if (ret) return ret;

    /* 0x51 0xFF */
    p = 0xFF;
    ret = st7801n_dcs(dev, 0x29, 0x51, &p, 1);
    if (ret) return ret;

    /* COLMOD: RGB565 */
    p = 0x55;
    ret = st7801n_dcs(dev, 0x29, 0x3A, &p, 1);
    if (ret) return ret;

    /* TE off */
    p = 0x00;
    ret = st7801n_dcs(dev, 0x29, 0x35, &p, 1);
    if (ret) return ret;

    /* Display ON */
    ret = st7801n_dcs0(dev, 0x05, 0x29);
    if (ret) return ret;
    k_msleep(20);

    printk("INIT END\n");
    return 0;
}

/* ---------------- Display Driver API ---------------- */

static int st7801n_blanking_off(const struct device *dev)
{
    struct st7801n_data *d = dev->data;
    int ret;

    if (!d->inited) {
        ret = st7801n_panel_init(dev);
        if (ret) {
            LOG_ERR("panel init failed %d", ret);
            return ret;
        }
        d->inited = true;
        LOG_INF("panel init done");
    }

    return st7801n_cmd0(dev, 0x29);
}

static int st7801n_blanking_on(const struct device *dev)
{
    return st7801n_cmd0(dev, 0x28);
}

static int st7801n_write(const struct device *dev,
                         const uint16_t x, const uint16_t y,
                         const struct display_buffer_descriptor *desc,
                         const void *buf)
{
    int ret;

    uint16_t xs = x;
    uint16_t xe = x + desc->width  - 1;
    uint16_t ys = y;
    uint16_t ye = y + desc->height - 1;

    uint8_t ca[4] = { xs >> 8, xs & 0xFF, xe >> 8, xe & 0xFF };
    ret = st7801n_cmd(dev, 0x2A, ca, 4);
    if (ret) return ret;

    uint8_t ra[4] = { ys >> 8, ys & 0xFF, ye >> 8, ye & 0xFF };
    ret = st7801n_cmd(dev, 0x2B, ra, 4);
    if (ret) return ret;

    size_t total = (size_t)desc->width * desc->height * 2U;

    if (((uintptr_t)buf & 0x3U) != 0U) {
        LOG_ERR("pixel buf not 4-byte aligned: %p", buf);
        return -EINVAL;
    }

    printk(">>> write x=%d y=%d w=%d h=%d total=%u\n",
           x, y, desc->width, desc->height, (unsigned)total);

    ret = st7801n_write_pixels(dev, buf, total);
    if (ret) {
        LOG_ERR("pixel write failed: %d", ret);
    }
    return ret;
}

static int st7801n_set_pixel_format(const struct device *dev,
                                    const enum display_pixel_format fmt)
{
    if (fmt != PIXEL_FORMAT_RGB_565) {
        return -ENOTSUP;
    }

    uint8_t colmod = 0x55;
    return st7801n_cmd(dev, 0x3A, &colmod, 1);
}

static void st7801n_get_capabilities(const struct device *dev,
                                     struct display_capabilities *caps)
{
    const struct st7801n_config *cfg = dev->config;

    caps->x_resolution            = cfg->width;
    caps->y_resolution            = cfg->height;
    caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
    caps->current_pixel_format    = PIXEL_FORMAT_RGB_565;
    caps->screen_info             = 0;
    caps->current_orientation     = DISPLAY_ORIENTATION_NORMAL;
}

static const struct display_driver_api st7801n_api = {
    .blanking_off     = st7801n_blanking_off,
    .blanking_on      = st7801n_blanking_on,
    .write            = st7801n_write,
    .set_pixel_format = st7801n_set_pixel_format,
    .get_capabilities = st7801n_get_capabilities,
};

/* ---------------- init ---------------- */

static int st7801n_init(const struct device *dev)
{
    struct st7801n_data *d = dev->data;
    const struct st7801n_config *cfg = dev->config;

    printk(">>> st7801n_init\n");

    /* 按名字逐个试 */
    d->mspi = device_get_binding("sqspi");
    if (d->mspi == NULL) {
        d->mspi = device_get_binding("SQSPI");
    }
    if (d->mspi == NULL) {
        d->mspi = device_get_binding("MSPI_0");
    }
    if (d->mspi == NULL) {
        d->mspi = device_get_binding("mspi@3b40");
    }

    printk("mspi=%p\n", d->mspi);

    if (d->mspi != NULL) {
        printk("mspi ready=%d\n", device_is_ready(d->mspi));
    } else {
        printk("mspi NOT found by any name!\n");
    }

    /* reset gpio 配好 */
    if (cfg->reset.port) {
        gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE);
    }

    d->dev_id.dev_idx = 0;
    d->inited = false;

    printk("st7801n probe done\n");
    return 0;
}

/* ---------------- DT 实例化 ---------------- */

#define ST7801N_DEFINE(inst)                                                \
    static struct st7801n_data st7801n_data_##inst;                         \
    static const struct st7801n_config st7801n_cfg_##inst = {               \
        .width  = DT_INST_PROP(inst, width),                                \
        .height = DT_INST_PROP(inst, height),                               \
        .reset  = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),         \
        .te     = GPIO_DT_SPEC_INST_GET_OR(inst, te_gpios, {0}),            \
    };                                                                      \
    DEVICE_DT_INST_DEFINE(inst, st7801n_init, NULL,                         \
                          &st7801n_data_##inst, &st7801n_cfg_##inst,        \
                          POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,         \
                          &st7801n_api);

DT_INST_FOREACH_STATUS_OKAY(ST7801N_DEFINE)