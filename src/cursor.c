/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#include "cursor.h"
#include "config.h"
#include "event.h"
#include "main.h"

#include <SDL.h>

#include <string.h>
#include <assert.h>

#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

struct cursor_resource{
    SDL_Cursor  *cursor;     
    SDL_Surface *surface;
    const char  *path;
    size_t       hot_x, hot_y;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct cursor_resource s_cursors[] = {
    [CURSOR_POINTER] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/pointer.bmp",
        .hot_x   = 0,
        .hot_y   = 0
    },
    [CURSOR_SCROLL_TOP] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_top.bmp",
        .hot_x   = 16,
        .hot_y   = 0
    },
    [CURSOR_SCROLL_TOP_RIGHT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_top_right.bmp",
        .hot_x   = 31,
        .hot_y   = 0
    },
    [CURSOR_SCROLL_RIGHT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_right.bmp",
        .hot_x   = 31,
        .hot_y   = 16 
    },
    [CURSOR_SCROLL_BOT_RIGHT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_bot_right.bmp",
        .hot_x   = 31,
        .hot_y   = 31
    },
    [CURSOR_SCROLL_BOT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_bot.bmp",
        .hot_x   = 16,
        .hot_y   = 31 
    },
    [CURSOR_SCROLL_BOT_LEFT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_bot_left.bmp",
        .hot_x   = 0,
        .hot_y   = 31
    },
    [CURSOR_SCROLL_LEFT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_left.bmp",
        .hot_x   = 0,
        .hot_y   = 16
    },
    [CURSOR_SCROLL_TOP_LEFT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_top_left.bmp",
        .hot_x   = 0,
        .hot_y   = 0
    },
    [CURSOR_TARGET] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/target.bmp",
        .hot_x   = 24,
        .hot_y   = 24 
    },
};

static enum cursortype s_rts_pointer = CURSOR_POINTER;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void cursor_rts_set_active(int mouse_x, int mouse_y)
{
    int width, height;
    Engine_WinDrawableSize(&width, &height);

    bool top = (mouse_y == 0);
    bool bot = (mouse_y == height - 1);
    bool left  = (mouse_x == 0);
    bool right = (mouse_x == width - 1);

    /* Check the corners first, then edges */
    if(top && left) {
        Cursor_SetActive(CURSOR_SCROLL_TOP_LEFT); 
    }else if(top && right) {
        Cursor_SetActive(CURSOR_SCROLL_TOP_RIGHT); 
    }else if(bot && left) {
        Cursor_SetActive(CURSOR_SCROLL_BOT_LEFT); 
    }else if(bot && right) {
        Cursor_SetActive(CURSOR_SCROLL_BOT_RIGHT); 
    }else if(top) {
        Cursor_SetActive(CURSOR_SCROLL_TOP); 
    }else if(bot) {
        Cursor_SetActive(CURSOR_SCROLL_BOT); 
    }else if(left) {
        Cursor_SetActive(CURSOR_SCROLL_LEFT); 
    }else if(right) {
        Cursor_SetActive(CURSOR_SCROLL_RIGHT); 
    }else {
        Cursor_SetActive(s_rts_pointer); 
    }
}

static void cursor_on_mousemove(void *unused1, void *unused2)
{
    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    
    cursor_rts_set_active(mouse_x, mouse_y); 
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void Cursor_SetRTSMode(bool on)
{
    if(on) {
        E_Global_Register(SDL_MOUSEMOTION, cursor_on_mousemove, NULL);
    }else {
        E_Global_Unregister(SDL_MOUSEMOTION, cursor_on_mousemove);
    }
}

bool Cursor_InitAll(const char *basedir)
{
    for(int i = 0; i < ARR_SIZE(s_cursors); i++) {
    
        struct cursor_resource *curr = &s_cursors[i];

        char path[512];
        strcpy(path, basedir);
        strcat(path, curr->path);

        curr->surface = SDL_LoadBMP(path);
        if(!curr->surface)
            goto fail;

        curr->cursor = SDL_CreateColorCursor(curr->surface, curr->hot_x, curr->hot_y);
        if(!curr->cursor)
            goto fail;
    }

    return true;

fail:
    Cursor_FreeAll();
    return false;
}

void Cursor_FreeAll(void)
{
    for(int i = 0; i < ARR_SIZE(s_cursors); i++) {
    
        struct cursor_resource *curr = &s_cursors[i];
        
        if(curr->surface) SDL_FreeSurface(curr->surface);
        if(curr->cursor)  SDL_FreeCursor(curr->cursor);
    }
}

void Cursor_SetActive(enum cursortype type)
{
    assert(type >= 0 && type < ARR_SIZE(s_cursors));
    SDL_SetCursor(s_cursors[type].cursor);
}

void Cursor_SetRTSPointer(enum cursortype type)
{
    s_rts_pointer = type;

    int x, y;
    SDL_GetMouseState(&x, &y);
    cursor_rts_set_active(x, y);
}

