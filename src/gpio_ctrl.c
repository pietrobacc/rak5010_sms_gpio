#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "gpio_ctrl.h"

LOG_MODULE_REGISTER(gpio_ctrl, CONFIG_LOG_DEFAULT_LEVEL);

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static const struct gpio_dt_spec bg95_pwrkey =
    GPIO_DT_SPEC_GET(DT_NODELABEL(modem), mdm_power_gpios);

static const struct device *mcp23017_dev =
    DEVICE_DT_GET(DT_NODELABEL(mcp23017));

int gpio_ctrl_init(void)
{
    int ret;

    /* --- LED --- */
    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("LED GPIO device non pronto");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) return ret;

    /* --- BG95 PWRKEY --- */
    if (!gpio_is_ready_dt(&bg95_pwrkey)) {
        LOG_ERR("PWRKEY GPIO device non pronto");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&bg95_pwrkey, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) return ret;

    /* --- MCP23017 --- */
    if (!device_is_ready(mcp23017_dev)) {
        LOG_WRN("MCP23017 non pronto - GPIO espansi non disponibili");
        return 0;
    }

    /* GPA0-GPA7 (pin 0-7): output */
    for (int i = 0; i < EXP_OUT_COUNT; i++) {
        ret = gpio_pin_configure(mcp23017_dev, i, GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            LOG_WRN("Errore config EXP_OUT%d: %d", i, ret);
            return 0;
        }
    }

    /* GPB0-GPB7 (pin 8-15): input con pull-up */
    for (int i = 0; i < EXP_IN_COUNT; i++) {
        ret = gpio_pin_configure(mcp23017_dev, 8 + i, GPIO_INPUT | GPIO_PULL_UP);
        if (ret < 0) {
            LOG_WRN("Errore config EXP_IN%d: %d", i, ret);
            return 0;
        }
    }

    LOG_INF("GPIO inizializzati:");
    LOG_INF("  LED:P0.12  PWRKEY:P0.02");
    LOG_INF("  MCP23017@0x20: GPA0-7=output  GPB0-7=input");
    return 0;
}

void gpio_ctrl_led_set(bool on)
{
    gpio_pin_set_dt(&led, on ? 1 : 0);
}

int gpio_ctrl_exp_out_set(exp_out_id_t id, bool value)
{
    if (id >= EXP_OUT_COUNT) {
        LOG_ERR("ID exp_out non valido: %d", id);
        return -EINVAL;
    }
    int ret = gpio_pin_set(mcp23017_dev, id, value ? 1 : 0);
    if (ret < 0) {
        LOG_ERR("Errore set EXP_OUT%d: %d", id, ret);
    } else {
        LOG_INF("EXP_OUT%d -> %s", id, value ? "ON" : "OFF");
    }
    return ret;
}

int gpio_ctrl_exp_out_get(exp_out_id_t id)
{
    if (id >= EXP_OUT_COUNT) return -EINVAL;
    return gpio_pin_get(mcp23017_dev, id);
}

int gpio_ctrl_exp_in_get(exp_in_id_t id)
{
    if (id >= EXP_IN_COUNT) {
        LOG_ERR("ID exp_in non valido: %d", id);
        return -EINVAL;
    }
    return gpio_pin_get(mcp23017_dev, 8 + id);
}

void gpio_ctrl_bg95_power_on(void)
{
    LOG_INF("BG95: avvio sequenza accensione");
    gpio_pin_set_dt(&bg95_pwrkey, 1);
    k_msleep(600);
    gpio_pin_set_dt(&bg95_pwrkey, 0);
    k_msleep(5000);
    LOG_INF("BG95: acceso");
}

void gpio_ctrl_bg95_power_off(void)
{
    LOG_INF("BG95: avvio sequenza spegnimento");
    gpio_pin_set_dt(&bg95_pwrkey, 1);
    k_msleep(700);
    gpio_pin_set_dt(&bg95_pwrkey, 0);
    k_msleep(3000);
    LOG_INF("BG95: spento");
}

void gpio_ctrl_bg95_reset(void)
{
    LOG_INF("BG95: reset via power cycle");
    gpio_ctrl_bg95_power_off();
    k_msleep(1000);
    gpio_ctrl_bg95_power_on();
}

bool gpio_ctrl_mcp_is_ready(void)
{
    return device_is_ready(mcp23017_dev);
}