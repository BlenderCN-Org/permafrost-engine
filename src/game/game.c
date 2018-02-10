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
 */

#include "public/game.h"
#include "gamestate.h"
#include "../render/public/render.h"
#include "../anim/public/anim.h"
#include "../map/public/map.h"
#include "../entity.h"
#include "../camera.h"
#include "../cam_control.h"
#include "../asset_load.h"

#include <assert.h> 


#define CAM_HEIGHT          175.0f
#define CAM_TILT_UP_DEGREES 25.0f

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct gamestate s_gs;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void g_center_camera(void)
{
    Camera_SetPos(s_gs.camera, (vec3_t){ 0.0f, CAM_HEIGHT, 0.0f }); 
}

static void g_reset(void)
{
    assert(s_gs.camera);
    assert(s_gs.cam_ctx);

    while(s_gs.active->size > 0) {
    
        struct entity *ent;

        kl_shift(entity, s_gs.active, &ent);
        AL_EntityFree(ent);
    }

    if(s_gs.map) AL_MapFree(s_gs.map);
    s_gs.map = NULL;

    g_center_camera();
}

static bool g_init_camera(void) 
{
    s_gs.camera = Camera_New();
    if(!s_gs.camera) {
        return false;
    }

    Camera_SetPitchAndYaw(s_gs.camera, -(90.0f - CAM_TILT_UP_DEGREES), 90.0f + 45.0f);
    Camera_SetSpeed(s_gs.camera, 0.15f);
    Camera_SetSens (s_gs.camera, 0.05f);

    CamControl_RTS_Install(s_gs.camera);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Init(void)
{
    s_gs.active = kl_init(entity);

    if(!s_gs.active)
        return false;

    if(g_init_camera())
        return false; 

    g_reset();

    return true;
}

bool G_NewGameWithMap(const char *dir, const char *pfmap)
{
    g_reset();

    s_gs.map = AL_MapFromPFMap(dir, pfmap);
    if(!s_gs.map)
        return false;

    M_CenterAtOrigin(s_gs.map);
    M_RestrictRTSCamToMap(s_gs.map, s_gs.camera);

    return true;
}

void G_Shutdown(void)
{
    g_reset();

    CamControl_UninstallActive();
    Camera_Free(s_gs.camera);

    assert(s_gs.active);
    kl_destroy(entity, s_gs.active);
}

void G_Render(void)
{
    assert(s_gs.active);

    if(s_gs.map) M_RenderEntireMap(s_gs.map);

    kliter_t(entity) *p;
    for (p = kl_begin(s_gs.active); p != kl_end(s_gs.active); p = kl_next(p)) {
    
        struct entity *curr = kl_val(p);

        /* TODO: Currently, we perform animation right before rendering due to 'A_Update' setting
         * some uniforms for the shader. Investigate if it's better to perform the animation for all
         * entities at once and just set the uniform right before rendering.  */
        if(curr->animated) A_Update(curr);

        mat4x4_t model;
        Entity_ModelMatrix(curr, &model);
        R_GL_Draw(curr->render_private, &model);
    }
}

void G_Update(void)
{

}

bool G_AddEntity(struct entity *ent)
{
    *kl_pushp(entity, s_gs.active) = ent;
}

bool G_RemoveEntity(struct entity *ent)
{
    return (0 == kl_remove_first(entity, s_gs.active, &ent));
}

