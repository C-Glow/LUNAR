#ifndef BSP_RTC_SHIM_H
#define BSP_RTC_SHIM_H
#include <stdint.h>
typedef struct bsp_rtc_s bsp_rtc_t;
typedef enum { BSP_OK=0, BSP_ERROR=-1, BSP_ERR_PARAM=-2, BSP_ERR_BUSY=-3, BSP_ERR_TIMEOUT=-4, BSP_ERR_HW=-5, BSP_ERR_NOMEM=-6, BSP_ERR_NOTINIT=-7 } bsp_status_t;
bsp_status_t bsp_rtc_set_utc(bsp_rtc_t* h, uint32_t utc);
#endif
