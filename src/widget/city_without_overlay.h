#ifndef WIDGET_CITY_WITHOUT_OVERLAY_H
#define WIDGET_CITY_WITHOUT_OVERLAY_H

#include "map/point.h"
#include "widget/city.h"

void city_without_overlay_draw(int selected_figure_id, pixel_coordinate *figure_coord,
                            const map_tile *tile, unsigned int roamer_preview_building_id);

#endif // WIDGET_CITY_WITHOUT_OVERLAY_H
