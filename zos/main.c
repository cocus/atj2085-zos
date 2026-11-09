/* zos entry: started by the homebrew launcher, then owns the machine until reboot */
#include <stdint.h>
#include "hal/hal.h"
#include "apps/apps.h"

uint16_t zos_main(void)
{
    hal_init();
    home();
    hal_exit();
    return 0;          /* start.s reboots (watchdog) into the stock firmware */
}
