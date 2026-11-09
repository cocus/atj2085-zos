#ifndef ZOS_APPS_H
#define ZOS_APPS_H
#include <stdint.h>
/* a built-in app: run() returns when the user leaves it */
typedef struct { const char *name; void (*run)(void); } app_t;
void home(void);
void app_modules(void);
#endif
