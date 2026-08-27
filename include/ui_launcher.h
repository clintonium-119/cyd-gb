#pragma once
#include "settings.h"

int launcher_ingame_menu();   // 0=resume 1=save 2=load 3=quit 5=settings
void launcher_settings_menu(settings_t* s);  // palette, frameskip, brightness
