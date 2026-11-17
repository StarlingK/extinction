#ifndef UI_TILES_H
#define UI_TILES_H

#include <string.h>
#include "gamedata.h"

static unsigned ui_tile_texture(const char *cls)
{
    return gd_tile_texture(cls);
}

static unsigned ui_preview_texture(const char *cls, float rotation_deg, float distance)
{
    return gd_preview_texture(cls, rotation_deg, distance);
}

#endif
