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

#include "public/map.h"
#include "pfchunk.h"
#include "../asset_load.h"
#include "../render/public/render.h"
#include "../navigation/public/nav.h"
#include "map_private.h"

#include <stdlib.h>
#include <assert.h>
#ifndef __USE_POSIX
    #define __USE_POSIX /* strtok_r */
#endif
#include <string.h>

/* ASCII to integer - argument must be an ascii digit */
#define A2I(_a) ((_a) - '0')

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool m_al_parse_tile(const char *str, struct tile *out)
{
    if(strlen(str) != 24)
        return false;

    char type_hexstr[2] = {str[0], '\0'};

    memset(out, 0, sizeof(struct tile));
    out->type          = (enum tiletype) strtol(type_hexstr, NULL, 16);
    out->base_height   = (int)           (str[1] == '-' ? -1 : 1) * (10  * A2I(str[2]) + A2I(str[3]));
    out->ramp_height   = (int)           (10  * A2I(str[4]) + A2I(str[5]));
    out->top_mat_idx   = (int)           (100 * A2I(str[6]) + 10 * A2I(str[7 ]) + A2I(str[8 ]));
    out->sides_mat_idx = (int)           (100 * A2I(str[9]) + 10 * A2I(str[10]) + A2I(str[11]));
    out->pathable      = (bool)          A2I(str[12]);
    out->blend_mode    = (int)           A2I(str[13]);
    out->blend_normals = (bool)          A2I(str[14]);

    return true;
}

static bool m_al_read_row(SDL_RWops *stream, struct tile *out, size_t *out_nread)
{
    char line[MAX_LINE_LEN];
    READ_LINE(stream, line, fail); 

    char *saveptr;
    /* String points to the first token - the first tile of this row */
    char *string = strtok_r(line, " \t\n", &saveptr);
    assert(out_nread);
    *out_nread = 0;

    while(string) {

        if(!m_al_parse_tile(string, out + *out_nread))
            goto fail;
        (*out_nread)++;
        string = strtok_r(NULL, " \t\n", &saveptr);
    }

    return true;

fail:
    return false;
}

static bool m_al_read_pfchunk(SDL_RWops *stream, struct pfchunk *out)
{
    size_t tiles_read = 0;
    while(tiles_read < TILES_PER_CHUNK_WIDTH * TILES_PER_CHUNK_HEIGHT) {
        
        size_t tiles_in_row;
        if(!m_al_read_row(stream, out->tiles + tiles_read, &tiles_in_row))
            return false;
        tiles_read += tiles_in_row;
    }

    return true;
}

static bool m_al_read_material(SDL_RWops *stream, char *out_texname)
{
    char line[MAX_LINE_LEN];
    READ_LINE(stream, line, fail); 

    char *saveptr;
    char *string = strtok_r(line, " \t\n", &saveptr);

    if(strcmp(string, "material") != 0)
        goto fail;

    string = strtok_r(NULL, " \t\n", &saveptr); /* skip name */
    string = strtok_r(NULL, " \t\n", &saveptr);

    strncpy(out_texname, string, MAX_LINE_LEN);
    return true;

fail:
    return false;
}

static void m_al_patch_adjacency_info(struct map *map)
{
    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width; c++) {

        void *chunk_rprivate = map->chunks[r * map->width + c].render_private;
        for(int tile_r = 0; tile_r < TILES_PER_CHUNK_HEIGHT; tile_r++) {
        for(int tile_c = 0; tile_c < TILES_PER_CHUNK_HEIGHT; tile_c++) {
        
            struct tile_desc desc = (struct tile_desc){r, c, tile_r, tile_c};
            const struct tile *tile = &map->chunks[r * map->width + c].tiles[tile_r * TILES_PER_CHUNK_WIDTH + tile_c];

            R_GL_TilePatchVertsBlend(chunk_rprivate, map, desc);
            if(tile->blend_normals) {
                R_GL_TilePatchVertsSmooth(chunk_rprivate, map, desc);
            }
        }}
    }}
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/
 
bool M_AL_InitMapFromStream(const struct pfmap_hdr *header, const char *basedir,
                            SDL_RWops *stream, void *outmap)
{
    struct map *map = outmap;
    char line[MAX_LINE_LEN];

    map->width = header->num_cols;
    map->height = header->num_rows;
    map->pos = (vec3_t) {0.0f, 0.0f, 0.0f};

    map->minimap_vres = (vec2_t){1920, 1080};
    map->minimap_center_pos = (vec2_t){192, 1080-192};
    map->minimap_sz = 256;

    /* Read materials */
    char texnames[header->num_materials][256];
    for(int i = 0; i < header->num_materials; i++) {
        if(!m_al_read_material(stream, texnames[i]))
            return false;
    }

    if(!R_GL_MapInit(texnames, header->num_materials)) {
        return false; 
    }

    /* Read chunks */
    size_t num_chunks = header->num_rows * header->num_cols;
    char *unused_base = (char*)(map + 1);
    unused_base += num_chunks * sizeof(struct pfchunk);

    for(int i = 0; i < num_chunks; i++) {

        map->chunks[i].render_private = (void*)unused_base;

        if(!m_al_read_pfchunk(stream, map->chunks + i))
            return false;

        size_t renderbuff_sz = R_AL_PrivBuffSizeForChunk(
                               TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT, 0);
        unused_base += renderbuff_sz;

        if(!R_AL_InitPrivFromTiles(map->chunks[i].tiles, TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT,
                                   map->chunks[i].render_private, basedir)) {
            return false;
        }
    }
    m_al_patch_adjacency_info(map);

    /* Build navigation grid */
    const struct tile *chunk_tiles[map->width * map->height];
    for(int r = 0; r < map->height; r++) {
        for(int c = 0; c < map->width; c++) {
            chunk_tiles[r * map->width + c] = map->chunks[r * map->width + c].tiles;
        }
    }
    map->nav_private = N_BuildForMapData(map->width, map->height, 
        TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT, chunk_tiles);
    if(!map->nav_private)
        return false;

    return true;
}

size_t M_AL_BuffSizeFromHeader(const struct pfmap_hdr *header)
{
    size_t num_chunks = header->num_rows * header->num_cols;

    return sizeof(struct map) + num_chunks * 
           (sizeof(struct pfchunk) + R_AL_PrivBuffSizeForChunk(
                                     TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT, 0));
}

bool M_AL_UpdateTile(struct map *map, const struct tile_desc *desc, const struct tile *tile)
{
    if(desc->chunk_r >= map->height || desc->chunk_c >= map->width)
        return false;

    struct pfchunk *chunk = &map->chunks[desc->chunk_r * map->width + desc->chunk_c];
    chunk->tiles[desc->tile_r * TILES_PER_CHUNK_WIDTH + desc->tile_c] = *tile;

    struct map_resolution res;
    M_GetResolution(map, &res);

    for(int dr = -1; dr <= 1; dr++) {
        for(int dc = -1; dc <= 1; dc++) {
        
            struct tile_desc curr = *desc;
            int ret = M_Tile_RelativeDesc(res, &curr, dc, dr);
            if(ret) {
            
                struct pfchunk *chunk = &map->chunks[curr.chunk_r * map->width + curr.chunk_c];
                R_GL_TileUpdate(chunk->render_private, map, curr);
            }
        }
    }

    return true;
}

void M_AL_FreePrivate(struct map *map)
{
    //TODO: Clean up OpenGL buffers
    assert(map->nav_private);
    N_FreePrivate(map->nav_private);
}

