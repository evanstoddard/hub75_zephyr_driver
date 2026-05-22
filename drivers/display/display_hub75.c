/*
 * Copyright (C) Evan Stoddard
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file display_hub75.c
 * @author Evan Stoddard
 * @brief Display controller driver for HUB75 RGB LED Panel
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <zephyr/device.h>

#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/logging/log.h>

/*****************************************************************************
 * Definitions
 *****************************************************************************/

LOG_MODULE_REGISTER(hub75, CONFIG_DISPLAY_LOG_LEVEL);

#define DT_DRV_COMPAT zephyr_hub75

#define HUB75_BYTES_PER_PIXEL (3U)

/*****************************************************************************
 * Typedefs, Structs, Enums
 *****************************************************************************/

struct hub75_config_t {
  uint16_t width;
  uint16_t height;

  const struct device *counter_dev;

  struct gpio_dt_spec pin_r1;
  struct gpio_dt_spec pin_r2;
  struct gpio_dt_spec pin_g1;
  struct gpio_dt_spec pin_g2;
  struct gpio_dt_spec pin_b1;
  struct gpio_dt_spec pin_b2;

  struct gpio_dt_spec pin_a;
  struct gpio_dt_spec pin_b;
  struct gpio_dt_spec pin_c;
  struct gpio_dt_spec pin_d;
  struct gpio_dt_spec pin_e;

  struct gpio_dt_spec pin_clk;
  struct gpio_dt_spec pin_latch;
  struct gpio_dt_spec pin_n_en;
};

struct hub75_data_t {
  const struct device *dev;

  uint8_t *framebuffer;

  bool blanking;

  uint16_t current_row;
  uint8_t bcm_mask;
};

/*****************************************************************************
 * variables
 *****************************************************************************/

/*****************************************************************************
 * Private Functions
 *****************************************************************************/

/**
 * @brief Shift out row's bit-plane for current frame
 *
 * @param config Pointer to device's config struct
 * @param data Pointer to device's data struct
 */
static void prv_shift_data(const struct hub75_config_t *config,
                           struct hub75_data_t *data) {
  size_t top_row_offset =
      config->width * data->current_row * HUB75_BYTES_PER_PIXEL;
  size_t bot_row_offset =
      top_row_offset +
      (config->width * (config->height / 2) * HUB75_BYTES_PER_PIXEL);

  uint8_t bcm_mask = data->bcm_mask;

  gpio_pin_set_dt(&config->pin_n_en, 1);

  gpio_pin_set_dt(&config->pin_a, (data->current_row >> 0) & 0x1);
  gpio_pin_set_dt(&config->pin_b, (data->current_row >> 1) & 0x1);
  gpio_pin_set_dt(&config->pin_c, (data->current_row >> 2) & 0x1);
  gpio_pin_set_dt(&config->pin_d, (data->current_row >> 3) & 0x1);
  gpio_pin_set_dt(&config->pin_e, (data->current_row >> 4) & 0x1);

  for (size_t i = 0; i < config->width; i++) {
    size_t col_offset = i * HUB75_BYTES_PER_PIXEL;
    uint8_t r1 =
        (bool)(data->framebuffer[col_offset + top_row_offset + 0] & bcm_mask);
    uint8_t g1 =
        (bool)(data->framebuffer[col_offset + top_row_offset + 1] & bcm_mask);
    uint8_t b1 =
        (bool)(data->framebuffer[col_offset + top_row_offset + 2] & bcm_mask);

    uint8_t r2 =
        (bool)(data->framebuffer[col_offset + bot_row_offset + 0] & bcm_mask);
    uint8_t g2 =
        (bool)(data->framebuffer[col_offset + bot_row_offset + 1] & bcm_mask);
    uint8_t b2 =
        (bool)(data->framebuffer[col_offset + bot_row_offset + 2] & bcm_mask);

    // Set data lines
    gpio_pin_set_dt(&config->pin_r1, r1);
    gpio_pin_set_dt(&config->pin_r2, r2);
    gpio_pin_set_dt(&config->pin_g1, g1);
    gpio_pin_set_dt(&config->pin_g2, g2);
    gpio_pin_set_dt(&config->pin_b1, b1);
    gpio_pin_set_dt(&config->pin_b2, b2);

    // Toggle clock pin
    gpio_pin_set_dt(&config->pin_clk, 1);
    gpio_pin_set_dt(&config->pin_clk, 0);
  }

  // Latch row
  gpio_pin_set_dt(&config->pin_latch, 1);
  gpio_pin_set_dt(&config->pin_latch, 0);
  gpio_pin_set_dt(&config->pin_n_en, data->blanking);
}

/**
 * @brief Counter top callback to kick off transfer of row's bit-plane
 */
static void prv_refresh(const struct device *counter_dev, uint8_t chan_id,
                        uint32_t ticks, void *user_data) {
  ARG_UNUSED(chan_id);
  ARG_UNUSED(ticks);
  struct hub75_data_t *data = user_data;
  const struct hub75_config_t *config = data->dev->config;

  prv_shift_data(config, data);

  uint8_t displayed_mask = data->bcm_mask;

  data->bcm_mask = data->bcm_mask << 1;
  if (data->bcm_mask == 0) {
    data->bcm_mask = 1;
    data->current_row++;
    if (data->current_row == config->height / 2) {
      data->current_row = 0;
    }
  }

  struct counter_alarm_cfg alarm_cfg = {
      .callback = prv_refresh,
      .ticks = counter_us_to_ticks(counter_dev, displayed_mask),
      .user_data = data,
      .flags = 0,
  };

  counter_set_channel_alarm(counter_dev, 0, &alarm_cfg);
}

/**
 * @brief Initialize GPIO pins connected to panel
 *
 * @param dev Pointer to driver instance
 * @retval -ENODEV GPIO pin devices are not ready
 * @retval 0 Success
 */
static int prv_initialize_gpio(const struct device *dev) {
  int ret = 0;

  const struct hub75_config_t *config = dev->config;

  if (!gpio_is_ready_dt(&config->pin_r1) ||
      !gpio_is_ready_dt(&config->pin_r2) ||
      !gpio_is_ready_dt(&config->pin_g1) ||
      !gpio_is_ready_dt(&config->pin_g2) ||
      !gpio_is_ready_dt(&config->pin_b1) ||
      !gpio_is_ready_dt(&config->pin_b2) || !gpio_is_ready_dt(&config->pin_a) ||
      !gpio_is_ready_dt(&config->pin_b) || !gpio_is_ready_dt(&config->pin_c) ||
      !gpio_is_ready_dt(&config->pin_d) || !gpio_is_ready_dt(&config->pin_e) ||
      !gpio_is_ready_dt(&config->pin_clk) ||
      !gpio_is_ready_dt(&config->pin_latch) ||
      !gpio_is_ready_dt(&config->pin_n_en)) {
    LOG_ERR("GPIO devices are not ready.");
    return -ENODEV;
  }

  // Set data pins to inactive
  ret = gpio_pin_configure_dt(&config->pin_r1, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_configure_dt(&config->pin_r2, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_configure_dt(&config->pin_g1, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_configure_dt(&config->pin_g2, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_configure_dt(&config->pin_b1, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_configure_dt(&config->pin_b2, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  // Set address lines to inactive
  ret = gpio_pin_configure_dt(&config->pin_a, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_configure_dt(&config->pin_b, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_configure_dt(&config->pin_c, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_configure_dt(&config->pin_d, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_configure_dt(&config->pin_e, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  // Set control pins to inactive
  ret = gpio_pin_configure_dt(&config->pin_clk, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_configure_dt(&config->pin_latch, GPIO_OUTPUT_INACTIVE);
  if (ret < 0) {
    return ret;
  }

  ret = gpio_pin_configure_dt(&config->pin_n_en, GPIO_OUTPUT_INACTIVE);

  return ret;
}

/*****************************************************************************
 * Functions
 *****************************************************************************/

/**
 * @brief Initialize HUB75 Driver
 *
 * @param dev Pointer to device instance
 * @return Returns 0 on success
 */
static int hub75_init(const struct device *dev) {
  struct hub75_data_t *data = dev->data;

  data->dev = dev;

  int ret = prv_initialize_gpio(dev);
  if (ret < 0) {
    LOG_ERR("Failed to initialize GPIO: %d", ret);
    return ret;
  }

  const struct hub75_config_t *config = dev->config;

  if (!device_is_ready(config->counter_dev)) {
    LOG_ERR("Counter device not ready.");
    return -ENODEV;
  }

  struct counter_alarm_cfg alarm_cfg = {
      .callback = prv_refresh,
      .user_data = data,
      .flags = 0,
      .ticks = counter_us_to_ticks(config->counter_dev, 1),
  };

  counter_start(config->counter_dev);
  counter_set_channel_alarm(config->counter_dev, 0, &alarm_cfg);

  memset(data->framebuffer, 0,
         config->width * config->height * HUB75_BYTES_PER_PIXEL);

  return 0;
}

/*****************************************************************************
 * Bindings
 *****************************************************************************/

/**
 * @brief Set blanking on (turns off display)
 *
 * @param dev Pointer to device instance
 * @return Returns 0 on success
 */
static int hub75_blanking_on(const struct device *dev) {
  struct hub75_data_t *data = dev->data;
  data->blanking = true;

  return 0;
}

/**
 * @brief Set blanking off (turns on display)
 *
 * @param dev Pointer to device instance
 * @return Returns 0 on success
 */
static int hub75_blanking_off(const struct device *dev) {
  struct hub75_data_t *data = dev->data;
  data->blanking = false;

  return 0;
}

/**
 * @brief Write to frame buffer for display.
 *
 * @param dev Pointer to device instance
 * @param x X offset of incoming buffer (currently ignored)
 * @param y Y offset of incoming buffer (currently ignored)
 * @param desc Pointer to buffer descriptor (mostly ignored except for size)
 * @param buf Pointer to incoming buffer
 * @return Currently always returns 0
 */
static int hub75_write(const struct device *dev, const uint16_t x,
                       const uint16_t y,
                       const struct display_buffer_descriptor *desc,
                       const void *buf) {
  // FIXME: Missing bounds checks
  struct hub75_data_t *data = dev->data;
  const struct hub75_config_t *config = dev->config;
  size_t fb_size = config->width * config->height * HUB75_BYTES_PER_PIXEL;

  memcpy(data->framebuffer, buf, MIN(desc->buf_size, fb_size));

  return 0;
}

/**
 * @brief Not implemented
 *
 * @param dev Unused
 * @param x Unused
 * @param y Unused
 * @param desc Unused
 * @param buf Unused
 * @return Returns -ENOTSUP
 */
static int hub75_read(const struct device *dev, const uint16_t x,
                      const uint16_t y,
                      const struct display_buffer_descriptor *desc, void *buf) {
  return -ENOTSUP;
}

/**
 * @brief Get pointer to framebuffer written to display
 *
 * @param dev Pointer to device instance
 * @return Pointer of raw framebuffer
 */
static void *hub75_get_framebuffer(const struct device *dev) {
  return ((struct hub75_data_t *)dev->data)->framebuffer;
}

/**
 * @brief Not implemented
 *
 * @param dev Unused
 * @param brightness Unused
 * @return Returns -ENOTSUP
 */
static int hub75_set_brightness(const struct device *dev, uint8_t brightness) {
  return -ENOTSUP;
}

/**
 * @brief Not implemented
 *
 * @param dev Unused
 * @param constrast Unused
 * @return Returns -ENOTSUP
 */
static int hub75_set_contrast(const struct device *dev, uint8_t constrast) {
  return -ENOTSUP;
}

/**
 * @brief Not implemented
 *
 * @param dev Unused
 * @param capabilities Unused
 */
static void hub75_get_capabilities(const struct device *dev,
                                   struct display_capabilities *capabilities) {
  return;
}

/**
 * @brief Not implemented.  Currently only supports 24-bit RGB pixel format
 * (8-bit R, 8-bit G, 8-bit B)
 *
 * @param dev Unused
 * @param pixel_format Unused
 * @return Returns -ENOTSUP
 */
static int
hub75_set_pixel_format(const struct device *dev,
                       const enum display_pixel_format pixel_format) {
  return -ENOTSUP;
}

/**
 * @brief Not implemented
 *
 * @param dev Unused
 * @param orientation Unused
 * @return Unused
 */
static int hub75_set_orientation(const struct device *dev,
                                 const enum display_orientation orientation) {
  return -ENOTSUP;
}

static DEVICE_API(display, hub75_api) = {
    .blanking_on = hub75_blanking_on,
    .blanking_off = hub75_blanking_off,
    .write = hub75_write,
    .read = hub75_read,
    .get_framebuffer = hub75_get_framebuffer,
    .set_brightness = hub75_set_brightness,
    .set_contrast = hub75_set_contrast,
    .get_capabilities = hub75_get_capabilities,
    .set_pixel_format = hub75_set_pixel_format,
    .set_orientation = hub75_set_orientation,
};

#define HUB75_INIT(inst)                                                       \
                                                                               \
  static uint8_t hub75_framebuffer_##inst[(DT_INST_PROP(inst, width) *         \
                                           DT_INST_PROP(inst, height) *        \
                                           HUB75_BYTES_PER_PIXEL)];            \
                                                                               \
  static struct hub75_data_t hub75_data_##inst = {                             \
      .framebuffer = hub75_framebuffer_##inst,                                 \
      .bcm_mask = 1,                                                           \
      .current_row = 0};                                                       \
                                                                               \
  static const struct hub75_config_t hub75_config_##inst = {                   \
      .width = DT_INST_PROP(inst, width),                                      \
      .height = DT_INST_PROP(inst, height),                                    \
      .counter_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, counter)),            \
      .pin_r1 = GPIO_DT_SPEC_INST_GET(inst, pin_r1_gpios),                     \
      .pin_r2 = GPIO_DT_SPEC_INST_GET(inst, pin_r2_gpios),                     \
      .pin_g1 = GPIO_DT_SPEC_INST_GET(inst, pin_g1_gpios),                     \
      .pin_g2 = GPIO_DT_SPEC_INST_GET(inst, pin_g2_gpios),                     \
      .pin_b1 = GPIO_DT_SPEC_INST_GET(inst, pin_b1_gpios),                     \
      .pin_b2 = GPIO_DT_SPEC_INST_GET(inst, pin_b2_gpios),                     \
      .pin_a = GPIO_DT_SPEC_INST_GET(inst, pin_a_gpios),                       \
      .pin_b = GPIO_DT_SPEC_INST_GET(inst, pin_b_gpios),                       \
      .pin_c = GPIO_DT_SPEC_INST_GET(inst, pin_c_gpios),                       \
      .pin_d = GPIO_DT_SPEC_INST_GET(inst, pin_d_gpios),                       \
      .pin_e = GPIO_DT_SPEC_INST_GET(inst, pin_e_gpios),                       \
      .pin_clk = GPIO_DT_SPEC_INST_GET(inst, pin_clk_gpios),                   \
      .pin_latch = GPIO_DT_SPEC_INST_GET(inst, pin_latch_gpios),               \
      .pin_n_en = GPIO_DT_SPEC_INST_GET(inst, pin_n_en_gpios),                 \
  };                                                                           \
                                                                               \
  DEVICE_DT_INST_DEFINE(inst, hub75_init, NULL, &hub75_data_##inst,            \
                        &hub75_config_##inst, POST_KERNEL,                     \
                        CONFIG_DISPLAY_INIT_PRIORITY, &hub75_api);

DT_INST_FOREACH_STATUS_OKAY(HUB75_INIT)
