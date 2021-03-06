/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

#include "render_gl.h"
#include "render_private.h"
#include "mesh.h"
#include "vertex.h"
#include "shader.h"
#include "material.h"
#include "gl_assert.h"
#include "gl_uniforms.h"
#include "public/render.h"
#include "../map/public/tile.h"
#include "../map/public/map.h"
#include "../collision.h"
#include "../camera.h"
#include "../config.h"

#include <GL/glew.h>

#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
  

#define ARR_SIZE(a)                 (sizeof(a)/sizeof(a[0]))

#define MAG(x, y)                   sqrt(pow(x,2) + pow(y,2))

#define VEC3_EQUAL(a, b)            (0 == memcmp((a).raw, (b).raw, sizeof((a).raw)))

#define INDICES_MASK_8(a, b)        (uint8_t)( (((a) & 0xf) << 4) | ((b) & 0xf) )

#define INDICES_MASK_32(a, b, c, d) (uint32_t)( (((a) & 0xff) << 24) | (((b) & 0xff) << 16) | (((c) & 0xff) << 8) | (((d) & 0xff) << 0) )

#define SAME_INDICES_32(i)          (  (( (i) >> 0) & 0xffff) == (( (i) >> 16) & 0xfffff) \
                                    && (( (i) >> 0) & 0xff  ) == (( (i) >> 8 ) & 0xff   ) \
                                    && (( (i) >> 0) & 0xf   ) == (( (i) >> 4 ) & 0xf    ) )

/* We take the directions to be relative to a normal vector facing outwards
 * from the plane of the face. West is to the right, east is to the left,
 * north is top, south is bottom. */
struct face{
    struct vertex nw, ne, se, sw; 
};

struct tile_adj_info{
    const struct tile *tile;
    uint8_t middle_mask; 
    uint8_t top_left_mask;
    uint8_t top_right_mask;
    uint8_t bot_left_mask;
    uint8_t bot_right_mask;
    int top_center_idx;
    int bot_center_idx;
    int left_center_idx;
    int right_center_idx;
};

struct tri{
    struct vertex verts[3];
};

/* Each top face is made up of 8 triangles, in the following configuration:
 *   +------+------+
 *   |\     |     /|
 *   |  \   |   /  |
 *   |    \ | /    |
 *   +------+------+
 *   |    / | \    |
 *   |  /   |   \  |
 *   |/     |     \|
 *   +------+------+
 * Each face can be thought of as being made of of 4 "major" triangles,
 * each of which has its' own adjacency info as a flat attribute. The 4 major
 * triangles are the minimal configuration that is necessary for the blending
 * system to work.
 *   +------+------+
 *   |\           /|
 *   |  \   2   /  |
 *   |    \   /    |
 *   +  1  >+<  3  +
 *   |    /   \    |
 *   |  /   0   \  |
 *   |/           \|
 *   +------+------+
 * The "major" trinagles can be futher subdivided. The triangles they are divided 
 * into must inherit the flat adjacency attributes and interpolate their positions, 
 * uv coorinates, and normals. In our case, we futher subdivide each of the major
 * triangles into 2 triangles. This is to give an extra vertex on the midpoint 
 * of each edge. When smoothing the normals, this extra point having its' own 
 * normal is essential. Care must be taken to ensure the appropriate winding order
 * for each triangle for backface culling!
 */
union top_face_vbuff{
    struct vertex verts[VERTS_PER_TOP_FACE];
    struct tri tris[VERTS_PER_TOP_FACE/3];
    struct{
        /* Tri 0 */
        struct vertex se0; 
        struct vertex s0;
        struct vertex center0;
        /* Tri 1 */
        struct vertex center1;
        struct vertex s1;
        struct vertex sw0;
        /* Tri 2 */
        struct vertex sw1;
        struct vertex w0;
        struct vertex center2;
        /* Tri 3 */
        struct vertex center3;
        struct vertex w1;
        struct vertex nw0;
        /* Tri 4 */
        struct vertex nw1;
        struct vertex n0;
        struct vertex center4;
        /* Tri 5 */
        struct vertex center5;
        struct vertex n1;
        struct vertex ne0;
        /* Tri 6 */
        struct vertex ne1;
        struct vertex e0;
        struct vertex center6;
        /* Tri 7 */
        struct vertex center7;
        struct vertex e1;
        struct vertex se1;
    };
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void tile_top_normals(const struct tile *tile, vec3_t out_tri_normals[static 2], bool *out_tri_left)
{
    switch(tile->type) {
    case TILETYPE_FLAT: {
        out_tri_normals[0]  = (vec3_t) {0.0f, 1.0f, 0.0f};
        out_tri_normals[1]  = (vec3_t) {0.0f, 1.0f, 0.0f};

        *out_tri_left = true;
        break;
    }
    case TILETYPE_RAMP_SN: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, Z_COORDS_PER_TILE);

        out_tri_normals[0] = (vec3_t) {0.0f, sin(normal_angle), cos(normal_angle)};
        out_tri_normals[1] = (vec3_t) {0.0f, sin(normal_angle), cos(normal_angle)};

        *out_tri_left = true;
        break;
    }
    case TILETYPE_RAMP_NS: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, Z_COORDS_PER_TILE);
    
        out_tri_normals[0] = (vec3_t) {0.0f, sin(normal_angle), -cos(normal_angle)};
        out_tri_normals[1] = (vec3_t) {0.0f, sin(normal_angle), -cos(normal_angle)};

        *out_tri_left = true;
        break;
    }
    case TILETYPE_RAMP_EW: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, X_COORDS_PER_TILE);
    
        out_tri_normals[0] = (vec3_t) {-cos(normal_angle), sin(normal_angle), 0.0f};
        out_tri_normals[1] = (vec3_t) {-cos(normal_angle), sin(normal_angle), 0.0f};

        *out_tri_left = true;
        break;
    }
    case TILETYPE_RAMP_WE: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, X_COORDS_PER_TILE);
    
        out_tri_normals[0] = (vec3_t) {cos(normal_angle), sin(normal_angle), 0.0f};
        out_tri_normals[1] = (vec3_t) {cos(normal_angle), sin(normal_angle), 0.0f};

        *out_tri_left = true;
        break;
    }
    case TILETYPE_CORNER_CONCAVE_SW: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {0.0f, 1.0f, 0.0f};
        out_tri_normals[1] = (vec3_t) {cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       cos(normal_angle) * sin(M_PI/4.0f)};

        *out_tri_left = false;
        break; 
    }
    case TILETYPE_CORNER_CONVEX_SW: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       cos(normal_angle) * sin(M_PI/4.0f)};
        out_tri_normals[1] = (vec3_t) {0.0f, 1.0f, 0.0f};

        *out_tri_left = false;
        break; 
    }
    case TILETYPE_CORNER_CONCAVE_SE: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {0.0f, 1.0f, 0.0f};
        out_tri_normals[1] = (vec3_t) {-cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                        cos(normal_angle) * sin(M_PI/4.0f)};

        *out_tri_left = true;
        break; 
    }
    case TILETYPE_CORNER_CONVEX_SE: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {-cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                        cos(normal_angle) * sin(M_PI/4.0f)};
        out_tri_normals[1] = (vec3_t) {0.0f, 1.0f, 0.0f};

        *out_tri_left = true;
        break; 
    }
    case TILETYPE_CORNER_CONCAVE_NW: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) { cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       -cos(normal_angle) * sin(M_PI/4.0f)};
        out_tri_normals[1] = (vec3_t) {0.0f, 1.0f, 0.0f};

        *out_tri_left = true;
        break; 
    }
    case TILETYPE_CORNER_CONVEX_NW: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {0.0f, 1.0f, 0.0f};
        out_tri_normals[1] = (vec3_t) { cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       -cos(normal_angle) * sin(M_PI/4.0f)};

        *out_tri_left = true;
        break; 
    }
    case TILETYPE_CORNER_CONCAVE_NE: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {-cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       -cos(normal_angle) * sin(M_PI/4.0f)};
        out_tri_normals[1] = (vec3_t) {0.0f, 1.0f, 0.0f};

        *out_tri_left = false;
        break; 
    }
    case TILETYPE_CORNER_CONVEX_NE: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {0.0f, 1.0f, 0.0f};
        out_tri_normals[1] = (vec3_t) {-cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       -cos(normal_angle) * sin(M_PI/4.0f)};

        *out_tri_left = false;
        break; 
    }
    default: assert(0);
    }

    PFM_Vec3_Normal(out_tri_normals, out_tri_normals);
    PFM_Vec3_Normal(out_tri_normals + 1, out_tri_normals + 1);
}

static void tile_smooth_normals_corner(struct tile *adj_cw[static 4], struct vertex *inout)
{
    enum{
        ADJ_CW_IDX_TOP_LEFT  = 0,
        ADJ_CW_IDX_TOP_RIGHT = 1,
        ADJ_CW_IDX_BOT_RIGHT = 2,
        ADJ_CW_IDX_BOT_LEFT  = 3,
    };
    vec3_t norm_total = {0};

    for(int i = 0; i < 4; i++) {

        if(!adj_cw[i])
            continue;
    
        vec3_t normals[2];
        bool top_tri_left_aligned;
        tile_top_normals(adj_cw[i], normals, &top_tri_left_aligned);

        switch(i) {
        case ADJ_CW_IDX_TOP_LEFT: 
            PFM_Vec3_Add(&norm_total, normals + 1, &norm_total);
            PFM_Vec3_Add(&norm_total, normals + (top_tri_left_aligned ? 1 : 0), &norm_total);
            break;
        case ADJ_CW_IDX_TOP_RIGHT:
            PFM_Vec3_Add(&norm_total, normals + 1, &norm_total);
            PFM_Vec3_Add(&norm_total, normals + (top_tri_left_aligned ? 0 : 1), &norm_total);
            break;
        case ADJ_CW_IDX_BOT_RIGHT:
            PFM_Vec3_Add(&norm_total, normals + 0, &norm_total);
            PFM_Vec3_Add(&norm_total, normals + (top_tri_left_aligned ? 0 : 1), &norm_total);
            break;
        case ADJ_CW_IDX_BOT_LEFT:
            PFM_Vec3_Add(&norm_total, normals + 0, &norm_total);
            PFM_Vec3_Add(&norm_total, normals + (top_tri_left_aligned ? 1 : 0), &norm_total);
            break;
        default: assert(0);
        }
    }

    PFM_Vec3_Normal(&norm_total, &norm_total);
    inout->normal = norm_total;
}

static void tile_smooth_normals_edge(struct tile *adj_lrtb[static 4], struct vertex *inout)
{
    vec3_t norm_total = {0};
    assert((!!adj_lrtb[0] + !!adj_lrtb[1] + !!adj_lrtb[2] + !!adj_lrtb[3]) <= 2);

    for(int i = 0; i < 4; i++) {

        if(!adj_lrtb[i])
            continue;
    
        vec3_t normals[2];
        bool top_tri_left_aligned;
        tile_top_normals(adj_lrtb[i], normals, &top_tri_left_aligned);

        PFM_Vec3_Add(&norm_total, normals + 0, &norm_total);
        PFM_Vec3_Add(&norm_total, normals + 1, &norm_total);
    }

    assert(PFM_Vec3_Len(&norm_total) > 0);
    PFM_Vec3_Normal(&norm_total, &norm_total);
    inout->normal = norm_total;
}

static void tile_mat_indices(struct tile_adj_info *inout, bool *out_top_tri_left_aligned)
{
    assert(inout->tile);

    vec3_t top_tri_normals[2];
    bool   top_tri_left_aligned;
    tile_top_normals(inout->tile, top_tri_normals, out_top_tri_left_aligned);

    GLint tri_mats[2] = {
        fabs(top_tri_normals[0].y) < 1.0 && (inout->tile->ramp_height > 1) ? inout->tile->sides_mat_idx : inout->tile->top_mat_idx,
        fabs(top_tri_normals[1].y) < 1.0 && (inout->tile->ramp_height > 1) ? inout->tile->sides_mat_idx : inout->tile->top_mat_idx,
    };

    /*
     * CONFIG 1 (left-aligned)   CONFIG 2
     * (nw)      (ne)            (nw)      (ne)
     * +---------+               +---------+
     * |       / |               | \       |
     * |     /   |               |   \     |
     * |   /     |               |     \   |
     * | /       |               |       \ |
     * +---------+               +---------+
     * (sw)      (se)            (sw)      (se)
     */
    inout->middle_mask = INDICES_MASK_8(tri_mats[0], tri_mats[1]);
    inout->bot_center_idx = tri_mats[0];
    inout->top_center_idx = tri_mats[1];

    if(!(*out_top_tri_left_aligned)) {
        inout->top_left_mask     = INDICES_MASK_8(tri_mats[1], tri_mats[0]);
        inout->top_right_mask    = INDICES_MASK_8(tri_mats[1], tri_mats[1]);
        inout->bot_left_mask     = INDICES_MASK_8(tri_mats[0], tri_mats[0]);
        inout->bot_right_mask    = INDICES_MASK_8(tri_mats[0], tri_mats[1]);

        inout->left_center_idx  = tri_mats[0];
        inout->right_center_idx = tri_mats[1];
    }else {
        inout->top_left_mask     = INDICES_MASK_8(tri_mats[1], tri_mats[1]);
        inout->top_right_mask    = INDICES_MASK_8(tri_mats[0], tri_mats[1]);
        inout->bot_left_mask     = INDICES_MASK_8(tri_mats[1], tri_mats[0]);
        inout->bot_right_mask    = INDICES_MASK_8(tri_mats[0], tri_mats[0]);

        inout->left_center_idx  = tri_mats[1];
        inout->right_center_idx = tri_mats[0];
    }
}

/* When all the materials for the tile are the same, we don't have to perform 
 * blending in the shader. This aids performance. */
enum blend_mode optimal_blendmode(const struct vertex *vert)
{
    if(SAME_INDICES_32(vert->adjacent_mat_indices[0])
    && SAME_INDICES_32(vert->adjacent_mat_indices[1])
    && vert->adjacent_mat_indices[0] == vert->adjacent_mat_indices[1]
    && (vert->adjacent_mat_indices[0] & 0xf) == vert->material_idx) {
    
        return BLEND_MODE_NOBLEND;
    }else{
        return vert->blend_mode; 
    }
}

static bool arr_contains(int *array, size_t size, int elem)
{
    for(int i = 0; i < size; i++) {
        if(array[i] == elem) 
            return true;
    }
    return false;
}

static int arr_indexof(int *array, size_t size, int elem)
{
    for(int i = 0; i < size; i++) {
        if(array[i] == elem) 
            return i;
    }
    return -1;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_TileDrawSelected(const struct tile_desc *in, const void *chunk_rprivate, mat4x4_t *model, 
                           int tiles_per_chunk_x, int tiles_per_chunk_z)
{
    struct vertex vbuff[VERTS_PER_TILE];
    vec3_t red = (vec3_t){1.0f, 0.0f, 0.0f};
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;

    const struct render_private *priv = chunk_rprivate;
    size_t offset = (in->tile_r * tiles_per_chunk_x + in->tile_c) * VERTS_PER_TILE * sizeof(struct vertex);
    size_t length = VERTS_PER_TILE * sizeof(struct vertex);

    glBindBuffer(GL_ARRAY_BUFFER, priv->mesh.VBO);
    const struct vertex *vert_base = glMapBufferRange(GL_ARRAY_BUFFER, offset, length, GL_MAP_READ_BIT);
    assert(vert_base);
    memcpy(vbuff, vert_base, sizeof(vbuff));
    glUnmapBuffer(GL_ARRAY_BUFFER);

    /* Additionally, scale the tile selection mesh slightly around its' center. This is so that 
     * it is slightly larger than the actual tile underneath and can be rendered on top of it. */
    const float SCALE_FACTOR = 1.025f;
    mat4x4_t final_model;
    mat4x4_t scale, trans, trans_inv, tmp1, tmp2;
    PFM_Mat4x4_MakeScale(SCALE_FACTOR, SCALE_FACTOR, SCALE_FACTOR, &scale);

    vec3_t center = (vec3_t){
        ( 0.0f - (in->tile_c* X_COORDS_PER_TILE) - X_COORDS_PER_TILE/2.0f ), 
        (-1.0f * Y_COORDS_PER_TILE + Y_COORDS_PER_TILE/2.0f), 
        ( 0.0f + (in->tile_r* Z_COORDS_PER_TILE) + Z_COORDS_PER_TILE/2.0f),
    };
    PFM_Mat4x4_MakeTrans(-center.x, -center.y, -center.z, &trans);
    PFM_Mat4x4_MakeTrans( center.x,  center.y,  center.z, &trans_inv);

    PFM_Mat4x4_Mult4x4(&scale, &trans, &tmp1);
    PFM_Mat4x4_Mult4x4(&trans_inv, &tmp1, &tmp2);
    PFM_Mat4x4_Mult4x4(model, &tmp2, &final_model);

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void*)0);
    glEnableVertexAttribArray(0);

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);

    /* Attribute 2 - normal */
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, normal));
    glEnableVertexAttribArray(2);

    shader_prog = R_Shader_GetProgForName("mesh.static.tile-outline");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, final_model.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform3fv(loc, 1, red.raw);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, sizeof(vbuff), vbuff, GL_STATIC_DRAW);

    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, VERTS_PER_TILE);

cleanup:
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

void R_GL_TilePatchVertsBlend(void *chunk_rprivate, const struct map *map, struct tile_desc tile)
{
    const struct render_private *priv = chunk_rprivate;
    GLuint VBO = priv->mesh.VBO;

    struct map_resolution res;
    M_GetResolution(map, &res);

    struct tile *curr_tile      = NULL,
                *top_tile       = NULL,
                *bot_tile       = NULL,
                *left_tile      = NULL,
                *right_tile     = NULL,
                *top_right_tile = NULL,
                *bot_right_tile = NULL,
                *top_left_tile  = NULL,
                *bot_left_tile  = NULL;

    int ret = M_TileForDesc(map, tile, &curr_tile);
    assert(ret);

    struct tile_desc ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, 0, -1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &top_tile); 
        assert(ret);
    }

    ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, 0, 1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &bot_tile); 
        assert(ret);
    }

    ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, -1, 0);
    if(ret) {
        ret = M_TileForDesc(map, ref, &left_tile); 
        assert(ret);
    }

    ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, 1, 0);
    if(ret) {
        ret = M_TileForDesc(map, ref, &right_tile); 
        assert(ret);
    }

    ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, 1, -1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &top_right_tile); 
        assert(ret);
    }

    ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, 1, 1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &bot_right_tile); 
        assert(ret);
    }

    ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, -1, -1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &top_left_tile);
        assert(ret);
    }

    ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, -1, 1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &bot_left_tile); 
        assert(ret);
    }

    struct tile_adj_info curr = {.tile = curr_tile};
    bool top_tri_left_aligned;
    tile_mat_indices(&curr, &top_tri_left_aligned);

    /* It may be possible that some of the adjacent tiles are NULL, such as when the current
     * tile as at a chunk edge. In that case, we have no neighbor tile to blend with. In 
     * that case, we make the tile's material go up to the very edge. */

    struct tile_adj_info 
        top = {
           .tile = top_tile,
           .bot_center_idx = curr.top_center_idx,
           .bot_left_mask = curr.top_left_mask,
           .bot_right_mask = curr.top_right_mask,
        },
        bot = {
           .tile = bot_tile,
           .top_center_idx = curr.bot_center_idx,
           .top_left_mask = curr.bot_left_mask,
           .top_right_mask = curr.bot_right_mask,
        },
        left = {
           .tile = left_tile,
           .right_center_idx = curr.left_center_idx,
           .top_right_mask = curr.top_left_mask,
           .bot_right_mask = curr.bot_left_mask,
        },
        right = {
           .tile = right_tile,
           .left_center_idx = curr.right_center_idx, 
           .bot_left_mask = curr.bot_right_mask,
           .top_left_mask = curr.top_right_mask,
        },
        top_right = { .tile = top_right_tile, },
        bot_right = { .tile = bot_right_tile, },
        top_left  = { .tile = top_left_tile, },
        bot_left  = { .tile = bot_left_tile, };

    struct tile_adj_info *adjacent[] = {&top, &bot, &left, &right, &top_right, &bot_right, &top_left, &bot_left};

    for(int i = 0; i < ARR_SIZE(adjacent); i++) {
        bool tmp;
        if(adjacent[i]->tile) {
            tile_mat_indices(adjacent[i], &tmp);
        }
    }
    
    if(!top_right.tile) {
        top_right.bot_left_mask = top_tile ? INDICES_MASK_8(curr.top_center_idx, top.bot_center_idx)
                                           : INDICES_MASK_8(curr.right_center_idx, right.left_center_idx); 
    }

    if(!top_left.tile) {
        top_left.bot_right_mask = top_tile ? INDICES_MASK_8(curr.top_center_idx, top.bot_center_idx)
                                           : INDICES_MASK_8(curr.left_center_idx, left.right_center_idx);
    }

    if(!bot_right.tile) {
        bot_right.top_left_mask = bot_tile ? INDICES_MASK_8(curr.bot_center_idx, bot.top_center_idx)
                                           : INDICES_MASK_8(curr.right_center_idx, right.left_center_idx);
    }

    if(!bot_left.tile) {
        bot_left.top_right_mask = bot_tile ? INDICES_MASK_8(curr.bot_center_idx, bot.top_center_idx)
                                           : INDICES_MASK_8(curr.left_center_idx, left.right_center_idx);
    }

    /* Now, update all triangles of the top face 
     *
     * Since 'adjacent_mat_indices' is a flat attribute, we only need to set 
     * it for the provoking vertex of each triangle.
     *
     * The first two 'adjacency_mat_indices' elements hold the 8 surrounding materials for 
     * the triangle's two non-central vertices. If the vertex is surrounded by only
     * 2 different materials, for example, then the weighting of each of these 
     * materials at the vertex is determened by the number of occurences of the 
     * material's index. The final material is the weighted average of the 8 materials,
     * which may contain repeated indices.
     *
     * The next element holds the materials at the midpoints of the edges of this tile and 
     * the last one holds the materials for the middle_mask of the tile.
     */
    size_t offset = VERTS_PER_TILE * (tile.tile_r * TILES_PER_CHUNK_WIDTH + tile.tile_c) * sizeof(struct vertex);
    size_t length = VERTS_PER_TILE * sizeof(struct vertex);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    struct vertex *tile_verts_base = glMapBufferRange(GL_ARRAY_BUFFER, offset, length, GL_MAP_WRITE_BIT);
    GL_ASSERT_OK();
    assert(tile_verts_base);

    struct vertex *south_provoking[2] = {tile_verts_base + (5 * VERTS_PER_SIDE_FACE) + 0*3,
                                         tile_verts_base + (5 * VERTS_PER_SIDE_FACE) + 1*3};
    struct vertex *west_provoking[2]  = {tile_verts_base + (5 * VERTS_PER_SIDE_FACE) + 2*3,
                                         tile_verts_base + (5 * VERTS_PER_SIDE_FACE) + 3*3};
    struct vertex *north_provoking[2] = {tile_verts_base + (5 * VERTS_PER_SIDE_FACE) + 4*3,
                                         tile_verts_base + (5 * VERTS_PER_SIDE_FACE) + 5*3};
    struct vertex *east_provoking[2]  = {tile_verts_base + (5 * VERTS_PER_SIDE_FACE) + 6*3,
                                         tile_verts_base + (5 * VERTS_PER_SIDE_FACE) + 7*3};

    for(int i = 0; i < 2; i++) {
        south_provoking[i]->adjacent_mat_indices[0] = 
            INDICES_MASK_32(bot.top_left_mask, bot_left.top_right_mask, left.bot_right_mask, curr.bot_left_mask);
        south_provoking[i]->adjacent_mat_indices[1] = 
            INDICES_MASK_32(bot_right.top_left_mask, bot.top_right_mask, curr.bot_right_mask, right.bot_left_mask);
        south_provoking[i]->blend_mode = optimal_blendmode(south_provoking[i]);
    }

    for(int i = 0; i < 2; i++) {
        north_provoking[i]->adjacent_mat_indices[0] = 
            INDICES_MASK_32(curr.top_left_mask, left.top_right_mask, top_left.bot_right_mask, top.bot_left_mask);
        north_provoking[i]->adjacent_mat_indices[1] = 
            INDICES_MASK_32(right.top_left_mask, curr.top_right_mask, top.bot_right_mask, top_right.bot_left_mask);
        north_provoking[i]->blend_mode = optimal_blendmode(north_provoking[i]);
    }

    for(int i = 0; i < 2; i++) {
        west_provoking[i]->adjacent_mat_indices[0] = south_provoking[0]->adjacent_mat_indices[0];
        west_provoking[i]->adjacent_mat_indices[1] = north_provoking[0]->adjacent_mat_indices[0];
        west_provoking[i]->blend_mode = optimal_blendmode(west_provoking[i]);
    }

    for(int i = 0; i < 2; i++) {
        east_provoking[i]->adjacent_mat_indices[0] = south_provoking[0]->adjacent_mat_indices[1];
        east_provoking[i]->adjacent_mat_indices[1] = north_provoking[0]->adjacent_mat_indices[1];
        east_provoking[i]->blend_mode = optimal_blendmode(east_provoking[i]);
    }

    GLint adj_center_mask = INDICES_MASK_32(
        INDICES_MASK_8(curr.top_center_idx,     top.bot_center_idx),
        INDICES_MASK_8(curr.right_center_idx,   right.left_center_idx),
        INDICES_MASK_8(curr.bot_center_idx,     bot.top_center_idx),
        INDICES_MASK_8(curr.left_center_idx,    left.right_center_idx)
    );

    struct vertex *provoking[] = {south_provoking[0], south_provoking[1], north_provoking[0], north_provoking[1], 
                                  west_provoking[0], west_provoking[1], east_provoking[0], east_provoking[1]};
    for(int i = 0; i < ARR_SIZE(provoking); i++) {

        provoking[i]->adjacent_mat_indices[2] = adj_center_mask;
        provoking[i]->adjacent_mat_indices[3] = curr.middle_mask;
    }

    glUnmapBuffer(GL_ARRAY_BUFFER);
    GL_ASSERT_OK();
}

void R_GL_TilePatchVertsSmooth(void *chunk_rprivate, const struct map *map, struct tile_desc tile)
{
    const struct render_private *priv = chunk_rprivate;
    GLuint VBO = priv->mesh.VBO;

    size_t offset = VERTS_PER_TILE * (tile.tile_r * TILES_PER_CHUNK_WIDTH + tile.tile_c) * sizeof(struct vertex);
    size_t length = VERTS_PER_TILE * sizeof(struct vertex);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    union top_face_vbuff *tfvb = glMapBufferRange(GL_ARRAY_BUFFER, offset, length, GL_MAP_WRITE_BIT);
    GL_ASSERT_OK();
    assert(tfvb);
    tfvb = (union top_face_vbuff*)(((struct vertex*)tfvb) + (5 * VERTS_PER_SIDE_FACE));

    struct map_resolution res;
    M_GetResolution(map, &res);

    struct tile *curr_tile = NULL;
    M_TileForDesc(map, tile, &curr_tile);
    assert(curr_tile);

    vec3_t normals[2];
    bool top_tri_left_aligned;
    tile_top_normals(curr_tile, normals, &top_tri_left_aligned);
    
    struct tile *tiles[4] = {0};
    struct tile_desc td;

    /* NW (top-left) corner */
    memset(tiles, 0, sizeof(tiles));
    td = tile; if(M_Tile_RelativeDesc(res, &td, -1, -1)) M_TileForDesc(map, td, &tiles[0]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0, -1)) M_TileForDesc(map, td, &tiles[1]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[2]);
    td = tile; if(M_Tile_RelativeDesc(res, &td, -1,  0)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_corner(tiles, &tfvb->nw0);
    tile_smooth_normals_corner(tiles, &tfvb->nw1);

    /* NE (top-right) corner */
    memset(tiles, 0, sizeof(tiles));
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0, -1)) M_TileForDesc(map, td, &tiles[0]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  1, -1)) M_TileForDesc(map, td, &tiles[1]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  1,  0)) M_TileForDesc(map, td, &tiles[2]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_corner(tiles, &tfvb->ne0);
    tile_smooth_normals_corner(tiles, &tfvb->ne1);

    /* SE (bot-right) corner */
    memset(tiles, 0, sizeof(tiles));
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[0]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  1,  0)) M_TileForDesc(map, td, &tiles[1]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  1,  1)) M_TileForDesc(map, td, &tiles[2]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0,  1)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_corner(tiles, &tfvb->se0);
    tile_smooth_normals_corner(tiles, &tfvb->se1);

    /* SW (bot-left) corner */
    memset(tiles, 0, sizeof(tiles));
    td = tile; if(M_Tile_RelativeDesc(res, &td, -1,  0)) M_TileForDesc(map, td, &tiles[0]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[1]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0,  1)) M_TileForDesc(map, td, &tiles[2]);
    td = tile; if(M_Tile_RelativeDesc(res, &td, -1,  1)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_corner(tiles, &tfvb->sw0);
    tile_smooth_normals_corner(tiles, &tfvb->sw1);

    /* Top edge */
    memset(tiles, 0, sizeof(tiles));
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0, -1)) M_TileForDesc(map, td, &tiles[2]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_edge(tiles, &tfvb->n0);
    tile_smooth_normals_edge(tiles, &tfvb->n1);

    /* Bot edge */
    memset(tiles, 0, sizeof(tiles));
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[2]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0,  1)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_edge(tiles, &tfvb->s0);
    tile_smooth_normals_edge(tiles, &tfvb->s1);

    /* Left edge */
    memset(tiles, 0, sizeof(tiles));
    td = tile; if(M_Tile_RelativeDesc(res, &td, -1,  0)) M_TileForDesc(map, td, &tiles[0]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[1]);
    tile_smooth_normals_edge(tiles, &tfvb->w0);
    tile_smooth_normals_edge(tiles, &tfvb->w1);

    /* Right edge */
    memset(tiles, 0, sizeof(tiles));
    td = tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[0]);
    td = tile; if(M_Tile_RelativeDesc(res, &td,  1,  0)) M_TileForDesc(map, td, &tiles[1]);
    tile_smooth_normals_edge(tiles, &tfvb->e0);
    tile_smooth_normals_edge(tiles, &tfvb->e1);

    /* Center */
    vec3_t center_norm = {0};
    PFM_Vec3_Add(&center_norm, normals + 0, &center_norm);
    PFM_Vec3_Add(&center_norm, normals + 1, &center_norm);
    PFM_Vec3_Normal(&center_norm, &center_norm);

    tfvb->center0.normal = center_norm;
    tfvb->center1.normal = center_norm;
    tfvb->center2.normal = center_norm;
    tfvb->center3.normal = center_norm;
    tfvb->center4.normal = center_norm;
    tfvb->center5.normal = center_norm;
    tfvb->center6.normal = center_norm;
    tfvb->center7.normal = center_norm;

    glUnmapBuffer(GL_ARRAY_BUFFER);
    GL_ASSERT_OK();
}

void R_GL_TileGetVertices(const struct tile *tile, struct vertex *out, size_t r, size_t c)
{
    /* Bottom face is always the same (just shifted over based on row and column), and the 
     * front, back, left, right faces just connect the top and bottom faces. The only 
     * variations are in the top face, which has some corners raised based on tile type. 
     */

    struct face bot = {
        .nw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), (-1.0f * Y_COORDS_PER_TILE), 
                                 0.0f + (r * Z_COORDS_PER_TILE) },
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE), (-1.0f * Y_COORDS_PER_TILE), 
                                 0.0f + (r * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE), (-1.0f * Y_COORDS_PER_TILE), 
                                 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), (-1.0f * Y_COORDS_PER_TILE), 
                                 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
    };

    /* Normals for top face get set at the end */
    struct face top = {
        .nw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE),
                                 M_Tile_NWHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + (r * Z_COORDS_PER_TILE) },
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), 
                                 M_Tile_NEHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + (r * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), 
                                 M_Tile_SEHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE), 
                                 M_Tile_SWHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
    };

#define V_COORD(width, height) (((float)height)/width)

    GLint side_adjacent_indices = ((tile->sides_mat_idx & 0xf) << 0) | ((tile->sides_mat_idx & 0xf) << 4)
                                | ((tile->sides_mat_idx & 0xf) << 8) | ((tile->sides_mat_idx & 0xf) << 12);
    struct face back = {
        .nw = (struct vertex) {
            .pos    = top.ne.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, back.nw.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = top.nw.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, back.ne.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = bot.ne.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = bot.nw.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

    struct face front = {
        .nw = (struct vertex) {
            .pos    = top.sw.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, front.nw.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = top.se.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, front.ne.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = bot.sw.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = bot.se.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

    struct face left = {
        .nw = (struct vertex) {
            .pos    = top.nw.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, left.nw.pos.y) },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = top.sw.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, left.ne.pos.y) },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = bot.se.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = bot.ne.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

    struct face right = {
        .nw = (struct vertex) {
            .pos    = top.se.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, right.nw.pos.y) },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = top.ne.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, right.ne.pos.y) },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = bot.nw.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = bot.sw.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

#undef V_COORD

    struct face *faces[] = {
        &bot, &front, &back, &left, &right 
    };

    for(int i = 0; i < ARR_SIZE(faces); i++) {

        struct face *curr = faces[i];

        /* First triangle */
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 0, &curr->nw, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 1, &curr->ne, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 2, &curr->sw, sizeof(struct vertex));

        /* Second triangle */
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 3, &curr->se, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 4, &curr->sw, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 5, &curr->ne, sizeof(struct vertex));
    }

    /* Lastly, the top face. Unlike the other five faces, it can have different 
     * normals for its' two triangles, and the triangles can be arranged differently 
     * at corner tiles. 
     */

    vec3_t top_tri_normals[2];
    bool   top_tri_left_aligned;
    tile_top_normals(tile, top_tri_normals, &top_tri_left_aligned);

    /*
     * CONFIG 1 (left-aligned)   CONFIG 2
     * (nw)      (ne)            (nw)      (ne)
     * +---------+               +---------+
     * |Tri1   / |               | \   Tri1|
     * |     /   |               |   \     |
     * |   /     |               |     \   |
     * | /   Tri0|               |Tri0   \ |
     * +---------+               +---------+
     * (sw)      (se)            (sw)      (se)
     */

    float center_height = 
          TILETYPE_IS_RAMP(tile->type)          ? (tile->base_height + tile->ramp_height / 2.0f) 
        : TILETYPE_IS_CORNER_CONVEX(tile->type) ? (tile->base_height + tile->ramp_height) 
        : (tile->base_height);

    vec3_t center_vert_pos = (vec3_t) {
        top.nw.pos.x - X_COORDS_PER_TILE / 2.0f, 
        center_height * Y_COORDS_PER_TILE, 
        top.nw.pos.z + Z_COORDS_PER_TILE / 2.0f
    };

    bool tri0_side_mat = fabs(top_tri_normals[0].y) < 1.0 && (tile->ramp_height > 1);
    bool tri1_side_mat = fabs(top_tri_normals[1].y) < 1.0 && (tile->ramp_height > 1);
	int tri0_idx = tri0_side_mat ? tile->sides_mat_idx : tile->top_mat_idx;
	int tri1_idx = tri1_side_mat ? tile->sides_mat_idx : tile->top_mat_idx;

    struct vertex center_vert_tri0 = (struct vertex) {
        .pos    = center_vert_pos,
        .uv     = (vec2_t){0.5f, 0.5f},
        .normal = top_tri_normals[0],
        .material_idx = tri0_idx
    };
    struct vertex center_vert_tri1 = (struct vertex) {
        .pos    = center_vert_pos,
        .uv     = (vec2_t){0.5f, 0.5f},
        .normal = top_tri_normals[1],
        .material_idx = tri1_idx
    };

    struct vertex north_vert = (struct vertex) {
        .pos    = (vec3_t){
            (top.ne.pos.x + top.nw.pos.x)/2, 
            (top.ne.pos.y + top.nw.pos.y)/2, 
            (top.ne.pos.z + top.nw.pos.z)/2},
        .uv     = (vec2_t){0.5f, 1.0f}, 
        .normal = top_tri_normals[1],
        .material_idx = tri1_idx
    };
    struct vertex south_vert = (struct vertex) {
        .pos    = (vec3_t){
            (top.se.pos.x + top.sw.pos.x)/2, 
            (top.se.pos.y + top.sw.pos.y)/2, 
            (top.se.pos.z + top.sw.pos.z)/2},
        .uv     = (vec2_t){0.5f, 0.0f},
        .normal = top_tri_normals[0],
        .material_idx = tri0_idx
    };
    struct vertex west_vert = (struct vertex) {
        .pos    = (vec3_t){
            (top.sw.pos.x + top.nw.pos.x)/2, 
            (top.sw.pos.y + top.nw.pos.y)/2, 
            (top.sw.pos.z + top.nw.pos.z)/2},
        .uv     = (vec2_t){0.0f, 0.5f},
        .normal = top_tri_left_aligned ? top_tri_normals[1] : top_tri_normals[0],
        .material_idx = (top_tri_left_aligned ? tri1_idx : tri0_idx),
    };
    struct vertex east_vert = (struct vertex) {
        .pos    = (vec3_t){
            (top.se.pos.x + top.ne.pos.x)/2, 
            (top.se.pos.y + top.ne.pos.y)/2, 
            (top.se.pos.z + top.ne.pos.z)/2},
        .uv     = (vec2_t){1.0f, 0.5f},
        .normal = top_tri_left_aligned ? top_tri_normals[0] : top_tri_normals[1],
        .material_idx = (top_tri_left_aligned ? tri0_idx : tri1_idx),
    };

    assert(sizeof(union top_face_vbuff) == VERTS_PER_TOP_FACE * sizeof(struct vertex));
    union top_face_vbuff *tfvb = (union top_face_vbuff*)(out + 5 * VERTS_PER_SIDE_FACE);
    tfvb->se0 = top.se;
    tfvb->s0 = south_vert;
    tfvb->center0 = center_vert_tri0;
    tfvb->center1 = center_vert_tri0;
    tfvb->s1 = south_vert;
    tfvb->sw0 = top.sw;
    tfvb->sw1 = top.sw;
    tfvb->w0 = west_vert;
    tfvb->center2 = top_tri_left_aligned ? center_vert_tri1 : center_vert_tri0;
    tfvb->center3 = top_tri_left_aligned ? center_vert_tri1 : center_vert_tri0;
    tfvb->w1 = west_vert;
    tfvb->nw0 = top.nw;
    tfvb->nw1 = top.nw;
    tfvb->n0 = north_vert;
    tfvb->center4 = center_vert_tri1;
    tfvb->center5 = center_vert_tri1;
    tfvb->n1 = north_vert;
    tfvb->ne0 = top.ne;
    tfvb->ne1 = top.ne;
    tfvb->e0 = east_vert;
    tfvb->center6 = top_tri_left_aligned ? center_vert_tri0 : center_vert_tri1;
    tfvb->center7 = top_tri_left_aligned ? center_vert_tri0 : center_vert_tri1;
    tfvb->e1 = east_vert;
    tfvb->se1 = top.se;

    /* Give a slight overlap to the triangles of the top face to make sure there 
     * no gap can appear between adjacent triangles due to interpolation errors */
    tfvb->center0.pos.z -= 0.005;
    tfvb->center1.pos.z -= 0.005;
    tfvb->center2.pos.x -= 0.005;
    tfvb->center3.pos.x -= 0.005;
    tfvb->center4.pos.z += 0.005;
    tfvb->center5.pos.z += 0.005;
    tfvb->center6.pos.x += 0.005;
    tfvb->center7.pos.x += 0.005;

    if(top_tri_left_aligned) {
        tfvb->se0.material_idx = tri0_idx;
        tfvb->sw0.material_idx = tri0_idx;
        tfvb->sw1.material_idx = tri1_idx;
        tfvb->nw0.material_idx = tri1_idx;
        tfvb->nw1.material_idx = tri1_idx;
        tfvb->ne0.material_idx = tri1_idx;
        tfvb->ne1.material_idx = tri0_idx;
        tfvb->se1.material_idx = tri0_idx;

        tfvb->se0.normal = top_tri_normals[0];
        tfvb->sw0.normal = top_tri_normals[0];
        tfvb->sw1.normal = top_tri_normals[1];
        tfvb->nw0.normal = top_tri_normals[1];
        tfvb->nw1.normal = top_tri_normals[1];
        tfvb->ne0.normal = top_tri_normals[1];
        tfvb->ne1.normal = top_tri_normals[0];
        tfvb->se1.normal = top_tri_normals[0];
    }else{
        tfvb->se0.material_idx = tri0_idx;
        tfvb->sw0.material_idx = tri0_idx;
        tfvb->sw1.material_idx = tri0_idx;
        tfvb->nw0.material_idx = tri0_idx;
        tfvb->nw1.material_idx = tri1_idx;
        tfvb->ne0.material_idx = tri1_idx;
        tfvb->ne1.material_idx = tri1_idx;
        tfvb->se1.material_idx = tri1_idx;

        tfvb->se0.normal = top_tri_normals[0];
        tfvb->sw0.normal = top_tri_normals[0];
        tfvb->sw1.normal = top_tri_normals[0];
        tfvb->nw0.normal = top_tri_normals[0];
        tfvb->nw1.normal = top_tri_normals[1];
        tfvb->ne0.normal = top_tri_normals[1];
        tfvb->ne1.normal = top_tri_normals[1];
        tfvb->se1.normal = top_tri_normals[1];
    }

    for(struct vertex *curr_provoking = out; 
        curr_provoking < out + (5 * VERTS_PER_SIDE_FACE); 
        curr_provoking += 3) {

        curr_provoking->blend_mode = BLEND_MODE_NOBLEND;
    }
    for(struct vertex *curr_provoking = out + (5 * VERTS_PER_SIDE_FACE); 
        curr_provoking < out + VERTS_PER_TILE; 
        curr_provoking += 3) {

        curr_provoking->blend_mode = tile->blend_mode;
    }
}

int R_GL_TileGetTriMesh(const struct tile_desc *in, const void *chunk_rprivate, 
                        mat4x4_t *model, int tiles_per_chunk_x, vec3_t out[])
{
    const struct render_private *priv = chunk_rprivate;

    size_t offset = (in->tile_r * tiles_per_chunk_x + in->tile_c) * VERTS_PER_TILE * sizeof(struct vertex);
    size_t length = VERTS_PER_TILE * sizeof(struct vertex);
    glBindBuffer(GL_ARRAY_BUFFER, priv->mesh.VBO);
    const struct vertex *vert_base = glMapBufferRange(GL_ARRAY_BUFFER, offset, length, GL_MAP_READ_BIT);
    assert(vert_base);
    int i = 0;

    for(; i < VERTS_PER_TILE; i++) {
    
        vec4_t pos_homo = (vec4_t){vert_base[i].pos.x, vert_base[i].pos.y, vert_base[i].pos.z, 1.0f};
        vec4_t ws_pos_homo;
        PFM_Mat4x4_Mult4x1(model, &pos_homo, &ws_pos_homo);
        
        out[i] = (vec3_t){
            ws_pos_homo.x / ws_pos_homo.w, 
            ws_pos_homo.y / ws_pos_homo.w, 
            ws_pos_homo.z / ws_pos_homo.w
        };
    }

    glUnmapBuffer(GL_ARRAY_BUFFER);
    assert(i % 3 == 0);
    return i;
}

void R_GL_TileUpdate(void *chunk_rprivate, const struct map *map, struct tile_desc desc)
{
    struct render_private *priv = chunk_rprivate;

    struct tile *tile;
    int ret = M_TileForDesc(map, desc, &tile);
    assert(ret);

    size_t offset = (desc.tile_r * TILES_PER_CHUNK_WIDTH + desc.tile_c) * VERTS_PER_TILE * sizeof(struct vertex);
    size_t length = VERTS_PER_TILE * sizeof(struct vertex);
    glBindBuffer(GL_ARRAY_BUFFER, priv->mesh.VBO);
    struct vertex *vert_base = glMapBufferRange(GL_ARRAY_BUFFER, offset, length, GL_MAP_WRITE_BIT);
    assert(vert_base);
    
    R_GL_TileGetVertices(tile, vert_base, desc.tile_r, desc.tile_c);
    glUnmapBuffer(GL_ARRAY_BUFFER);

    R_GL_TilePatchVertsBlend(chunk_rprivate, map, desc);
    if(tile->blend_normals) {
        R_GL_TilePatchVertsSmooth(chunk_rprivate, map, desc);
    }

    GL_ASSERT_OK();
}

