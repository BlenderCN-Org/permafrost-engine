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

#ifndef NAV_DAT_H
#define NAV_DAT_H

#include <stddef.h>
#include <stdint.h>

#define MAX_PORTALS_PER_CHUNK 64
#define FIELD_RES_R           64
#define FIELD_RES_C           64
#define COST_IMPASSABLE       0xff

struct coord{
    int r, c;
};

struct edge{
    struct portal *neighbour;
    /* Cost of moving from the center of one portal to the center
     * of the next. */
    float          cost;
};

struct portal{
    struct coord   chunk;
    struct coord   endpoints[2]; 
    size_t         num_neighbours;
    struct edge    edges[MAX_PORTALS_PER_CHUNK-1];
    struct portal *connected;
};

struct nav_chunk{
    size_t        num_portals; 
    struct portal portals[MAX_PORTALS_PER_CHUNK];
    uint8_t       cost_base[FIELD_RES_R][FIELD_RES_C]; 
};

#endif
