#include "camera.h"
#include "gl_uniforms.h"
#include "render/public/render.h"

#include <SDL2/SDL.h>

#include <stdlib.h>
#include <stddef.h>
#include <string.h>

struct camera {
    float    speed;
    vec3_t   pos;
    vec3_t   front;
    vec3_t   up;

    uint32_t prev_frame_ts;
};

const unsigned g_sizeof_camera = sizeof(struct camera);


struct camera *camera_new(void)
{
    return calloc(1, sizeof(struct camera));
}

void camera_free(struct camera *cam)
{
    free(cam);
}

void camera_init_stack(struct camera *cam)
{
    memset(cam, 0, sizeof(struct camera));
}

void camera_set_pos(struct camera *cam, vec3_t pos)
{
    cam->pos = pos; 
}

void camera_set_front(struct camera *cam, vec3_t front)
{
    cam->front = front; 
}

void camera_set_up(struct camera *cam, vec3_t up)
{
    cam->up = up; 
}

void camera_set_speed(struct camera *cam, float speed)
{
    cam->speed = speed;
}

void camera_move_left_tick(struct camera *cam)
{
    uint32_t tdelta;
    vec3_t   vdelta, right;
    
    if(!cam->prev_frame_ts)
        cam->prev_frame_ts = SDL_GetTicks();

    uint32_t curr = SDL_GetTicks();
    tdelta = curr - cam->prev_frame_ts;

    PFM_vec3_cross(&cam->front, &cam->up, &right);
    PFM_vec3_normal(&right, &right);

    PFM_vec3_scale(&right, tdelta * cam->speed, &vdelta);
    PFM_vec3_add(&cam->pos, &vdelta, &cam->pos);
}

void camera_move_right_tick(struct camera *cam)
{
    uint32_t tdelta;
    vec3_t   vdelta, right;
    
    if(!cam->prev_frame_ts)
        cam->prev_frame_ts = SDL_GetTicks();

    uint32_t curr = SDL_GetTicks();
    tdelta = curr - cam->prev_frame_ts;

    PFM_vec3_cross(&cam->front, &cam->up, &right);
    PFM_vec3_normal(&right, &right);

    PFM_vec3_scale(&right, tdelta * cam->speed, &vdelta);
    PFM_vec3_sub(&cam->pos, &vdelta, &cam->pos);
}

void camera_move_front_tick(struct camera *cam)
{
    uint32_t tdelta;
    vec3_t   vdelta;
    
    if(!cam->prev_frame_ts)
        cam->prev_frame_ts = SDL_GetTicks();

    uint32_t curr = SDL_GetTicks();
    tdelta = curr - cam->prev_frame_ts;

    PFM_vec3_scale(&cam->front, tdelta * cam->speed, &vdelta);
    PFM_vec3_add(&cam->pos, &vdelta, &cam->pos);
}

void camera_move_back_tick(struct camera *cam)
{
    uint32_t tdelta;
    vec3_t   vdelta;
    
    if(!cam->prev_frame_ts)
        cam->prev_frame_ts = SDL_GetTicks();

    uint32_t curr = SDL_GetTicks();
    tdelta = curr - cam->prev_frame_ts;

    PFM_vec3_scale(&cam->front, tdelta * cam->speed, &vdelta);
    PFM_vec3_sub(&cam->pos, &vdelta, &cam->pos);
}

void camera_tick_finish(struct camera *cam)
{
    mat4x4_t view, proj;

    /* Set the view matrix for the vertex shader */
    vec3_t target;
    PFM_vec3_add(&cam->pos, &cam->front, &target);
    PFM_mat4x4_make_look_at(&cam->pos, &target, &cam->up, &view);

    R_GL_SetView(&view, "generic");
    
    /* Set the projection matrix for the vertex shader */
    GLint viewport[4]; 
    glGetIntegerv(GL_VIEWPORT, viewport);
    PFM_mat4x4_make_perspective(DEG_TO_RAD(45.0f), ((GLfloat)viewport[2])/viewport[3], 0.1f, 250.0f, &proj);

    R_GL_SetProj(&proj, "generic");

    /* Update our last timestamp */
    cam->prev_frame_ts = SDL_GetTicks();
}
