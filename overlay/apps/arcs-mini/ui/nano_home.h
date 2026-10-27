#ifndef NANO_HOME_H
#define NANO_HOME_H

#include "lvgl.h"
#include "nano_shell.h"

lv_obj_t *nano_home_create(lv_obj_t *parent);
void nano_home_next(void);
nano_screen_t nano_home_selected_screen(void);
void nano_home_show(void);

#endif /* NANO_HOME_H */
