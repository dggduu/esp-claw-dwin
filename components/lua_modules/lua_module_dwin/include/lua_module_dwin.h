
#pragma once

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_dwin(lua_State *L);
esp_err_t lua_module_dwin_register(void);

#ifdef __cplusplus
}
#endif