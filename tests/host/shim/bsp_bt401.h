#ifndef BSP_BT401_SHIM_H
#define BSP_BT401_SHIM_H
#include <stdint.h>
#include "bsp_rtc.h"
typedef struct bsp_bt401_s bsp_bt401_t;
bsp_status_t bsp_bt401_send(bsp_bt401_t* h, const uint8_t* b, uint16_t n);
#endif
