#ifndef BUILDING_CONSTRUCTION_BUILDING_H
#define BUILDING_CONSTRUCTION_BUILDING_H

#include "building/type.h"

int building_construction_place_building(building_type type, int x, int y);
int building_construction_is_granary_cross_tile(int tile_no);
int building_construction_is_warehouse_corner(int tile_no);

#endif // BUILDING_CONSTRUCTION_BUILDING_H
