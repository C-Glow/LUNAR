#ifndef CMSIS_OS2_SHIM_H
#define CMSIS_OS2_SHIM_H
#include <stdint.h>
#include <stddef.h>

typedef void* osMutexId_t;
typedef int32_t osStatus_t;

#define osOK 0
#define osWaitForever 0xFFFFFFFFu
#define osMutexRecursive 1u

typedef struct {
    const char* name;
    uint32_t attr_bits;
    void* cb_mem;
    uint32_t cb_size;
} osMutexAttr_t;

osMutexId_t osMutexNew(const osMutexAttr_t* attr);
osStatus_t osMutexAcquire(osMutexId_t m, uint32_t t);
osStatus_t osMutexRelease(osMutexId_t m);

#endif
