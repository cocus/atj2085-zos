/* SNAKE as a module: linked at 0x4000 against the core API table */
#include <stdint.h>
void app_snake(void);
uint16_t mod_main(void) { app_snake(); return 0; }
