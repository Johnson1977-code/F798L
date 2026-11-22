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
#include <string.h>

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

/*
 * 所有 DCS 数据统一走这里：
 *   SPI opcode = 0x02（单线写）
 *   data_buf   = DCS 前缀 + DCS 命令 + 参数
 */
static int st7801n_dcs_raw(const struct device *dev,
			   const uint8_t *bytes, size_t len)
{
	struct st7801n_data *d = dev->data;

	struct mspi_xfer_packet pkt = {
		.dir       = MSPI_TX,
		.cb_mask   = MSPI_BUS_NO_CB,
		.cmd       = 0x02,	/* QSPI 单线写指令 */
		.address   = 0,
		.num_bytes = len,
		.data_buf  = (uint8_t *)bytes,
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
		LOG_ERR("dcs_raw len=%u ret=%d", (unsigned)len, ret);
	}
	return ret;
}

/* DCS 无参：data = {0x05, cmd} */
static int st7801n_dcs0(const struct device *dev, uint8_t prefix, uint8_t cmd)
{
	uint8_t b[2] = { prefix, cmd };
	return st7801n_dcs_raw(dev, b, 2);
}

/* DCS 带参：data = {0x29, cmd, data...} */
static int st7801n_dcs(const struct device *dev, uint8_t prefix,
		       uint8_t cmd, const uint8_t *data, size_t len)
{
	uint8_t b[32];
	if (len > 29) {
		return -EINVAL;
	}
	b[0] = prefix;
	b[1] = cmd;
	memcpy(&b[2], data, len);
	return st7801n_dcs_raw(dev, b, 2 + len);
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

/* ---------------- 像素写 ---------------- */

static int st7801n_write_pixels(const struct device *dev,
				const uint8_t *rgb565, size_t len)
{
	struct st7801n_data *d = dev->data;

	/*
	 * 先发 0x2C（Memory Write）命令，再发像素数据。
	 * 拼成一条 SPI 事务：opcode=0x02, data={0x2C, pixel...}
	 */
	uint8_t *tx = k_malloc(1 + len);
	if (!tx) {
		return -ENOMEM;
	}

	tx[0] = 0x2C;			/* Memory Write DCS 命令 */
	memcpy(&tx[1], rgb565, len);

	struct mspi_xfer_packet pkt = {
		.dir       = MSPI_TX,
		.cb_mask   = MSPI_BUS_NO_CB,
		.cmd       = 0x02,
		.address   = 0,
		.num_bytes = 1 + len,
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
		.timeout     = 2000,
	};

	int ret = mspi_transceive(d->mspi, &d->dev_id, &xfer);
	k_free(tx);

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
	ret = st7801n_cmd0(dev, 0x11);
	if (ret) return ret;
	k_msleep(120);

	/* Write CTRL Display: 0x53 0x20 */
	p = 0x20;
	ret = st7801n_cmd(dev, 0x53, &p, 1);
	if (ret) return ret;

	/* Write Display Brightness: 0x51 0xFF */
	p = 0xFF;
	ret = st7801n_cmd(dev, 0x51, &p, 1);
	if (ret) return ret;

	/* COLMOD: RGB565 */
	p = 0x55;
	ret = st7801n_cmd(dev, 0x3A, &p, 1);
	if (ret) return ret;

	/* TE off */
	p = 0x00;
	ret = st7801n_cmd(dev, 0x35, &p, 1);
	if (ret) return ret;

	/* Display ON */
	ret = st7801n_cmd0(dev, 0x29);
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

	/* CASET: 列地址 */
	uint8_t ca[4] = { xs >> 8, xs & 0xFF, xe >> 8, xe & 0xFF };
	ret = st7801n_cmd(dev, 0x2A, ca, 4);
	if (ret) return ret;

	/* RASET: 行地址 */
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

#define SQSPI_NODE DT_NODELABEL(sqspi)

static int st7801n_init(const struct device *dev)
{
	struct st7801n_data *d = dev->data;
	const struct st7801n_config *cfg = dev->config;
	int ret;

	printk(">>> st7801n_init\n");

	/* 1. DT 拿 sQSPI device */
	d->mspi = DEVICE_DT_GET(SQSPI_NODE);
	if (!device_is_ready(d->mspi)) {
		printk("mspi NOT ready %p\n", d->mspi);
		return -ENODEV;
	}
	printk("mspi=%p ready\n", d->mspi);

	/* 2. reset gpio */
	if (cfg->reset.port) {
		gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE);
	}

    /* 3. dev_id + ce *//* 3. dev_id - ce 清零，让 shim 用硬件 CS（pinctrl 已配 CS0） */
        d->dev_id.dev_idx = 0;
        memset(&d->dev_id.ce, 0, sizeof(d->dev_id.ce));

	/* 4. 必须先 dev_config */
	struct mspi_dev_cfg dc = {
		.freq        = 4000000,
		.io_mode     = MSPI_IO_MODE_SINGLE,
		.data_rate   = MSPI_DATA_RATE_SINGLE,
		.cpp         = MSPI_CPP_MODE_0,
		.endian      = MSPI_XFER_BIG_ENDIAN,
		.ce_polarity = MSPI_CE_ACTIVE_LOW,
		.cmd_length  = 1,
		.addr_length = 0,
	};

	ret = mspi_dev_config(d->mspi, &d->dev_id,
			      MSPI_DEVICE_CONFIG_FREQUENCY |
			      MSPI_DEVICE_CONFIG_IO_MODE |
			      MSPI_DEVICE_CONFIG_DATA_RATE |
			      MSPI_DEVICE_CONFIG_CPP |
			      MSPI_DEVICE_CONFIG_ENDIAN |
			      MSPI_DEVICE_CONFIG_CE_POL |
			      MSPI_DEVICE_CONFIG_CMD_LEN |
			      MSPI_DEVICE_CONFIG_ADDR_LEN,
			      &dc);
	if (ret) {
		printk("mspi_dev_config failed %d\n", ret);
		return ret;
	}

	d->inited = false;
	printk("st7801n probe done, dev_cfg ok\n");
	return 0;
}

/* ---------------- DT 实例化 ---------------- */

#define ST7801N_DEFINE(inst)							\
	static struct st7801n_data st7801n_data_##inst;				\
	static const struct st7801n_config st7801n_cfg_##inst = {			\
		.width  = DT_INST_PROP(inst, width),					\
		.height = DT_INST_PROP(inst, height),					\
		.reset  = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),		\
		.te     = GPIO_DT_SPEC_INST_GET_OR(inst, te_gpios, {0}),		\
	};										\
	DEVICE_DT_INST_DEFINE(inst, st7801n_init, NULL,					\
			      &st7801n_data_##inst, &st7801n_cfg_##inst,		\
			      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,		\
			      &st7801n_api);

DT_INST_FOREACH_STATUS_OKAY(ST7801N_DEFINE)