#include "econtainer_guest.h"

#include <stdint.h>

static volatile uint32_t spin_count;

int32_t econtainer_init(void)
{
    static const uint8_t marker = 'x';
    if (econtainer_log(&marker, 1) != 0) return -1;
    if (econtainer_timer_start(1000U, 0U) == 0) return -2;
    for (;;) ++spin_count;
}

int32_t econtainer_on_event(const uint8_t *bytes, uint32_t size_bytes)
{
    (void)bytes;
    (void)size_bytes;
    return 0;
}

int32_t econtainer_stop(void)
{
    return 0;
}
