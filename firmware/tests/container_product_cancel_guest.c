#include "econtainer_guest.h"

#include <stdint.h>

static volatile uint32_t spin_count;

int32_t econtainer_init(void)
{
#ifdef ECONTAINER_INIT_LOOP
    for (;;) ++spin_count;
#endif
    return 0;
}

int32_t econtainer_on_event(const uint8_t *bytes, uint32_t size_bytes)
{
    if (bytes == 0 || size_bytes == 0) return -1;
    if (size_bytes == 1 && bytes[0] == 'T')
        return econtainer_timer_start(10U, 0U) != 0 ? 0 : -2;
    if ((size_bytes == 1 && bytes[0] == 'E') ||
        (size_bytes == 16 && bytes[0] == 'E' && bytes[1] == 'C' &&
         bytes[2] == 'T' && bytes[3] == 1)) {
        for (;;) ++spin_count;
    }
    return 0;
}

int32_t econtainer_stop(void)
{
#ifdef ECONTAINER_STOP_LOOP
    for (;;) ++spin_count;
#endif
#ifdef ECONTAINER_STOP_FAIL
    return -1;
#else
    return 0;
#endif
}
