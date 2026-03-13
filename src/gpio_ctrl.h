#ifndef GPIO_CTRL_H
#define GPIO_CTRL_H

#include <stdint.h>
#include <stdbool.h>

/* Output espansi MCP23017 - GPA0-GPA7 */
typedef enum {
    EXP_OUT_0 = 0,   /**< GPA0 - ex OUT1 */
    EXP_OUT_1,       /**< GPA1 - ex OUT2 */
    EXP_OUT_2,
    EXP_OUT_3,
    EXP_OUT_4,
    EXP_OUT_5,
    EXP_OUT_6,
    EXP_OUT_7,
    EXP_OUT_COUNT
} exp_out_id_t;

/* Input espansi MCP23017 - GPB0-GPB7 */
typedef enum {
    EXP_IN_0 = 0,
    EXP_IN_1,
    EXP_IN_2,
    EXP_IN_3,
    EXP_IN_4,
    EXP_IN_5,
    EXP_IN_6,
    EXP_IN_7,
    EXP_IN_COUNT
} exp_in_id_t;

int  gpio_ctrl_init(void);
void gpio_ctrl_led_set(bool on);
int  gpio_ctrl_exp_out_set(exp_out_id_t id, bool value);
int  gpio_ctrl_exp_out_get(exp_out_id_t id);
int  gpio_ctrl_exp_in_get(exp_in_id_t id);
void gpio_ctrl_bg95_power_on(void);
void gpio_ctrl_bg95_power_off(void);
void gpio_ctrl_bg95_reset(void);
bool gpio_ctrl_mcp_is_ready(void);

#endif /* GPIO_CTRL_H */