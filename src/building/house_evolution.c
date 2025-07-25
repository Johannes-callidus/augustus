#include "house_evolution.h"

#include "building/house.h"
#include "building/model.h"
#include "building/monument.h"
#include "city/houses.h"
#include "city/resource.h"
#include "core/calc.h"
#include "core/config.h"
#include "core/time.h"
#include "game/resource.h"
#include "game/time.h"
#include "game/undo.h"
#include "map/building.h"
#include "map/grid.h"
#include "map/routing_terrain.h"
#include "map/terrain.h"
#include "map/tiles.h"

#define DEVOLVE_DELAY 2
#define DEVOLVE_DELAY_WITH_VENUS 20

typedef enum {
    EVOLVE = 1,
    NONE = 0,
    DEVOLVE = -1
} evolve_status;

static int active_devolve_delay;

static int check_evolve_desirability(building *house, int bonus)
{
    int level = house->subtype.house_level;
    level -= bonus;
    level = calc_bound(level, HOUSE_MIN, HOUSE_MAX);
    const model_house *model = model_get_house(level);
    int evolve_des = model->evolve_desirability;
    if (level >= HOUSE_LUXURY_PALACE) {
        evolve_des = 1000;
    }
    int current_des = house->desirability;
    int status;
    if (current_des <= model->devolve_desirability) {
        status = DEVOLVE;
    } else if (current_des >= evolve_des) {
        status = EVOLVE;
    } else {
        status = NONE;
    }
    house->data.house.evolve_text_id = status; // BUG? -1 in an unsigned char?
    return status;
}

static int has_required_goods_and_services(building *house, int for_upgrade, int with_bonus, house_demands *demands)
{
    int level = house->subtype.house_level;
    if (for_upgrade) {
        ++level;
    }
    if (with_bonus) {
        --level;
    }
    level = calc_bound(level, HOUSE_MIN, HOUSE_MAX);
    const model_house *model = model_get_house(level);
    // water
    int water = model->water;
    if (!house->has_water_access) {
        if (water >= 2) {
            if (level > HOUSE_SMALL_CASA) {
                ++demands->missing.fountain;
                return  0;
            } else if (!house->has_well_access) {
                ++demands->missing.well;
                return 0;
            } else if (level > HOUSE_LARGE_SHACK && !house->has_latrines_access) {
                return 0;
            }
        }
        if (water == 1 && !house->has_well_access) {
            ++demands->missing.well;
            return 0;
        }
    }

    // entertainment
    int entertainment = model->entertainment;
    if (house->data.house.entertainment < entertainment) {
        if (house->data.house.entertainment) {
            ++demands->missing.more_entertainment;
        } else {
            ++demands->missing.entertainment;
        }
        return 0;
    }
    // education
    int education = model->education;
    if (house->data.house.education < education) {
        if (house->data.house.education) {
            ++demands->missing.more_education;
        } else {
            ++demands->missing.education;
        }
        return 0;
    }
    if (education == 2) {
        ++demands->requiring.school;
        ++demands->requiring.library;
    } else if (education == 1) {
        ++demands->requiring.school;
    }
    // religion
    int religion = model->religion;
    if (religion > 3) {
        religion = 3;
    }
    if (house->data.house.num_gods < religion) {
        if (religion == 1) {
            ++demands->missing.religion;
            return 0;
        } else if (religion == 2) {
            ++demands->missing.second_religion;
            return 0;
        } else if (religion >= 3) {
            ++demands->missing.third_religion;
            return 0;
        }
    } else if (religion > 0) {
        ++demands->requiring.religion;
    }
    // barber
    int barber = model->barber;
    if (house->data.house.barber < barber) {
        ++demands->missing.barber;
        return 0;
    }
    if (barber == 1) {
        ++demands->requiring.barber;
    }
    // bathhouse
    int bathhouse = model->bathhouse;
    if (house->data.house.bathhouse < bathhouse) {
        ++demands->missing.bathhouse;
        return 0;
    }
    if (bathhouse == 1) {
        ++demands->requiring.bathhouse;
    }
    // health
    int health = model->health;
    if (house->data.house.health < health) {
        if (health < 2) {
            ++demands->missing.clinic;
        } else {
            ++demands->missing.hospital;
        }
        return 0;
    }
    if (health >= 1) {
        ++demands->requiring.clinic;
    }
    // food types
    int foodtypes_required = model->food_types;
    int foodtypes_available = 0;
    for (resource_type r = RESOURCE_MIN_FOOD; r < RESOURCE_MAX_FOOD; r++) {
        if (house->resources[r] && resource_is_inventory(r)) {
            foodtypes_available++;
        }
    }
    if (foodtypes_available < foodtypes_required) {
        ++demands->missing.food;
        return 0;
    }
    // goods
    if (house->resources[RESOURCE_POTTERY] < model->pottery) {
        return 0;
    }
    if (house->resources[RESOURCE_OIL] < model->oil) {
        return 0;
    }
    if (house->resources[RESOURCE_FURNITURE] < model->furniture) {
        return 0;
    }
    int wine = model->wine;
    if (wine && house->resources[RESOURCE_WINE] <= 0) {
        return 0;
    }
    if (wine > 1 && !city_resource_multiple_wine_available()) {
        ++demands->missing.second_wine;
        return 0;
    }
    return 1;
}

static int check_requirements(building *house, house_demands *demands)
{
    int bonus = 0;
    if (building_monument_pantheon_module_is_active(PANTHEON_MODULE_2_HOUSING_EVOLUTION) && house->house_pantheon_access) {
        bonus++;
    }
    int status = check_evolve_desirability(house, bonus);
    if (!has_required_goods_and_services(house, 0, bonus, demands)) {
        status = DEVOLVE;
    } else if (status == EVOLVE) {
        status = has_required_goods_and_services(house, 1, bonus, demands);
    }
    return status;
}

static int has_devolve_delay(building *house, evolve_status status)
{
    if (status == DEVOLVE && house->data.house.devolve_delay < active_devolve_delay) {
        house->data.house.devolve_delay++;
        return 1;
    } else {
        house->data.house.devolve_delay = 0;
        return 0;
    }
}

static int evolve_small_tent(building *house, house_demands *demands)
{
    if (house->house_population > 0) {
        building_house_merge(house);
        evolve_status status = check_requirements(house, demands);
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_LARGE_TENT);
        }
    }
    return 0;
}

static int evolve_large_tent(building *house, house_demands *demands)
{
    if (house->house_population > 0) {
        building_house_merge(house);
        evolve_status status = check_requirements(house, demands);
        if (!has_devolve_delay(house, status)) {
            if (status == EVOLVE) {
                building_house_change_to(house, BUILDING_HOUSE_SMALL_SHACK);
            } else if (status == DEVOLVE) {
                building_house_change_to(house, BUILDING_HOUSE_SMALL_TENT);
            }
        }
    }
    return 0;
}

static int evolve_small_shack(building *house, house_demands *demands)
{
    building_house_merge(house);
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_LARGE_SHACK);
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_LARGE_TENT);
        }
    }
    return 0;
}

static int evolve_large_shack(building *house, house_demands *demands)
{
    building_house_merge(house);
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_SMALL_HOVEL);
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_SMALL_SHACK);
        }
    }
    return 0;
}

static int evolve_small_hovel(building *house, house_demands *demands)
{
    building_house_merge(house);
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_LARGE_HOVEL);
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_LARGE_SHACK);
        }
    }
    return 0;
}

static int evolve_large_hovel(building *house, house_demands *demands)
{
    building_house_merge(house);
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_SMALL_CASA);
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_SMALL_HOVEL);
        }
    }
    return 0;
}

static int evolve_small_casa(building *house, house_demands *demands)
{
    building_house_merge(house);
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_LARGE_CASA);
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_LARGE_HOVEL);
        }
    }
    return 0;
}

static int evolve_large_casa(building *house, house_demands *demands)
{
    building_house_merge(house);
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_SMALL_INSULA);
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_SMALL_CASA);
        }
    }
    return 0;
}

static int evolve_small_insula(building *house, house_demands *demands)
{
    building_house_merge(house);
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_MEDIUM_INSULA);
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_LARGE_CASA);
        }
    }
    return 0;
}

static int evolve_medium_insula(building *house, house_demands *demands)
{
    building_house_merge(house);
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            if (building_house_can_expand(house, 4)) {
                game_undo_disable();
                house->house_is_merged = 0;
                building_house_expand_to_large_insula(house);
                map_tiles_update_all_gardens();
                return 1;
            }
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_SMALL_INSULA);
        }
    }
    return 0;
}

static int evolve_large_insula(building *house, house_demands *demands)
{
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_GRAND_INSULA);
        } else if (status == DEVOLVE) {
            game_undo_disable();
            building_house_devolve_from_large_insula(house);
        }
    }
    return 0;
}

static int evolve_grand_insula(building *house, house_demands *demands)
{
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_SMALL_VILLA);
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_LARGE_INSULA);
        }
    }
    return 0;
}

static int evolve_small_villa(building *house, house_demands *demands)
{
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_MEDIUM_VILLA);
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_GRAND_INSULA);
        }
    }
    return 0;
}

static int evolve_medium_villa(building *house, house_demands *demands)
{
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            if (building_house_can_expand(house, 9)) {
                game_undo_disable();
                building_house_expand_to_large_villa(house);
                map_tiles_update_all_gardens();
                return 1;
            }
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_SMALL_VILLA);
        }
    }
    return 0;
}

static int evolve_large_villa(building *house, house_demands *demands)
{
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_GRAND_VILLA);
        } else if (status == DEVOLVE) {
            game_undo_disable();
            if (config_get(CONFIG_GP_CH_PATRICIAN_DEVOLUTION_FIX)) {
                building_house_desize_patrician(house);
            } else {
                building_house_devolve_from_large_villa(house);
            }
        }
    }
    return 0;
}

static int evolve_grand_villa(building *house, house_demands *demands)
{
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_SMALL_PALACE);
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_LARGE_VILLA);
        }
    }
    return 0;
}

static int evolve_small_palace(building *house, house_demands *demands)
{
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_MEDIUM_PALACE);
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_GRAND_VILLA);
        }
    }
    return 0;
}

static int evolve_medium_palace(building *house, house_demands *demands)
{
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            if (building_house_can_expand(house, 16)) {
                game_undo_disable();
                building_house_expand_to_large_palace(house);
                map_tiles_update_all_gardens();
                return 1;
            }
        } else if (status == DEVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_SMALL_PALACE);
        }
    }
    return 0;
}

static int evolve_large_palace(building *house, house_demands *demands)
{
    int status = check_requirements(house, demands);
    if (!has_devolve_delay(house, status)) {
        if (status == EVOLVE) {
            building_house_change_to(house, BUILDING_HOUSE_LUXURY_PALACE);
        } else if (status == DEVOLVE) {
            game_undo_disable();
            if (config_get(CONFIG_GP_CH_PATRICIAN_DEVOLUTION_FIX)) {
                building_house_desize_patrician(house);
            } else {
                building_house_devolve_from_large_palace(house);
            }
        }
    }
    return 0;
}

static int evolve_luxury_palace(building *house, house_demands *demands)
{
    int bonus = (int) (building_monument_pantheon_module_is_active(PANTHEON_MODULE_2_HOUSING_EVOLUTION) && house->house_pantheon_access);
    int status = check_evolve_desirability(house, bonus);
    if (!has_required_goods_and_services(house, 0, bonus, demands)) {
        status = DEVOLVE;
    }
    if (!has_devolve_delay(house, status) && status == DEVOLVE) {
        building_house_change_to(house, BUILDING_HOUSE_LARGE_PALACE);
    }
    return 0;
}

static void consume_resource(building *b, int inventory, int amount)
{
    if (amount > 0) {
        if (amount > b->resources[inventory]) {
            b->resources[inventory] = 0;
        } else {
            b->resources[inventory] -= amount;
        }
    }
}

static void consume_resources(building *b)
{
    int consumption_reduction[RESOURCE_MAX] = { 0 };

    // mercury module 1 - pottery and furniture reduced by 20%
    if (building_monument_gt_module_is_active(MERCURY_MODULE_1_POTTERY_FURN)) {
        consumption_reduction[RESOURCE_POTTERY] += 20;
        consumption_reduction[RESOURCE_FURNITURE] += 20;
    }
    // mercury module 2 - oil and wine reduced by 20%
    if (b->data.house.temple_mercury && building_monument_gt_module_is_active(MERCURY_MODULE_2_OIL_WINE)) {
        consumption_reduction[RESOURCE_WINE] += 20;
        consumption_reduction[RESOURCE_OIL] += 20;
    }
    // mars module 2 - all goods reduced by 10% 
    if (b->data.house.temple_mars && building_monument_gt_module_is_active(MARS_MODULE_2_ALL_GOODS)) {
        consumption_reduction[RESOURCE_WINE] += 10;
        consumption_reduction[RESOURCE_OIL] += 10;
        consumption_reduction[RESOURCE_POTTERY] += 10;
        consumption_reduction[RESOURCE_FURNITURE] += 10;
    }

    for (resource_type r = RESOURCE_MIN_NON_FOOD; r < RESOURCE_MAX_NON_FOOD; r++) {
        if (!resource_is_inventory(r)) {
            continue;
        }
        if (!consumption_reduction[r] ||
            (game_time_total_months() % (100 / consumption_reduction[r]))) {
            consume_resource(b, r, model_house_uses_inventory(b->subtype.house_level, r));
        }
    }
}

static int (*evolve_callback[])(building *, house_demands *) = {
    evolve_small_tent, evolve_large_tent, evolve_small_shack, evolve_large_shack,
    evolve_small_hovel, evolve_large_hovel, evolve_small_casa, evolve_large_casa,
    evolve_small_insula, evolve_medium_insula, evolve_large_insula, evolve_grand_insula,
    evolve_small_villa, evolve_medium_villa, evolve_large_villa, evolve_grand_villa,
    evolve_small_palace, evolve_medium_palace, evolve_large_palace, evolve_luxury_palace
};

void building_house_process_evolve_and_consume_goods(void)
{
    city_houses_reset_demands();
    house_demands *demands = city_houses_demands();
    int has_expanded = 0;

    if (building_monument_working(BUILDING_GRAND_TEMPLE_VENUS)) {
        active_devolve_delay = DEVOLVE_DELAY_WITH_VENUS;
    } else {
        active_devolve_delay = DEVOLVE_DELAY;
    }

    time_millis last_update = time_get_millis();

    for (building_type type = BUILDING_HOUSE_VACANT_LOT; type <= BUILDING_HOUSE_LUXURY_PALACE; type++) {
        building *next_of_type = 0; // evolve_callback changes the building type
        for (building *b = building_first_of_type(type); b; b = next_of_type) {
            next_of_type = b->next_of_type;
            if (b->state != BUILDING_STATE_IN_USE || b->last_update == last_update) {
                continue;
            }
            building_house_check_for_corruption(b);
            if (!b->has_plague) {
                has_expanded |= evolve_callback[b->type - BUILDING_HOUSE_VACANT_LOT](b, demands);
            }
            // 1x1 houses only consume half of the goods
            if (game_time_day() == 0 || (game_time_day() == 7 && b->house_size > 1)) {
                consume_resources(b);
            }
            b->last_update = last_update;
        }
    }
    if (has_expanded) {
        map_routing_update_land();
    }
}

void building_house_determine_evolve_text(building *house, int worst_desirability_building)
{
    int level = house->subtype.house_level;
    if (building_monument_pantheon_module_is_active(PANTHEON_MODULE_2_HOUSING_EVOLUTION) && house->house_pantheon_access) {
        level--;
    }
    level = calc_bound(level, HOUSE_MIN, HOUSE_MAX);

    // this house will devolve soon because...

    const model_house *model = model_get_house(level);
    // desirability
    if (house->desirability <= model->devolve_desirability) {
        house->data.house.evolve_text_id = 0;
        return;
    }
    // water
    int water = model->water;
    if (water == 1 && !house->has_water_access) {
        if (!house->has_well_access) {
            house->data.house.evolve_text_id = 1;
            return;
        } else if (!house->has_latrines_access) {
            house->data.house.evolve_text_id = 68;
            return;
        }
    }

    if (water == 2 && !house->has_water_access) {
        if (!house->has_latrines_access) {
            house->data.house.evolve_text_id = 67;
            return;
        } else if (level >= HOUSE_LARGE_CASA) {
            house->data.house.evolve_text_id = 2;
            return;
        }
    }

    // entertainment
    int entertainment = model->entertainment;
    if (house->data.house.entertainment < entertainment) {
        if (!house->data.house.entertainment) {
            house->data.house.evolve_text_id = 3;
        } else if (entertainment < 10) {
            house->data.house.evolve_text_id = 4;
        } else if (entertainment < 25) {
            house->data.house.evolve_text_id = 5;
        } else if (entertainment < 50) {
            house->data.house.evolve_text_id = 6;
        } else if (entertainment < 80) {
            house->data.house.evolve_text_id = 7;
        } else {
            house->data.house.evolve_text_id = 8;
        }
        return;
    }
    // food types
    int foodtypes_required = model->food_types;
    int foodtypes_available = 0;
    for (resource_type r = RESOURCE_MIN_FOOD; r < RESOURCE_MAX_FOOD; r++) {
        if (house->resources[r] && resource_is_inventory(r)) {
            foodtypes_available++;
        }
    }
    if (foodtypes_available < foodtypes_required) {
        if (foodtypes_required == 1) {
            house->data.house.evolve_text_id = 9;
            return;
        } else if (foodtypes_required == 2) {
            house->data.house.evolve_text_id = 10;
            return;
        } else if (foodtypes_required == 3) {
            house->data.house.evolve_text_id = 11;
            return;
        }
    }
    // education
    int education = model->education;
    if (house->data.house.education < education) {
        if (education == 1) {
            house->data.house.evolve_text_id = 14;
            return;
        } else if (education == 2) {
            if (house->data.house.school) {
                house->data.house.evolve_text_id = 15;
                return;
            } else if (house->data.house.library) {
                house->data.house.evolve_text_id = 16;
                return;
            }
        } else if (education == 3) {
            house->data.house.evolve_text_id = 17;
            return;
        }
    }
    // bathhouse
    if (house->data.house.bathhouse < model->bathhouse) {
        house->data.house.evolve_text_id = 18;
        return;
    }
    // pottery
    if (house->resources[RESOURCE_POTTERY] < model->pottery) {
        house->data.house.evolve_text_id = 19;
        return;
    }
    // religion
    int religion = model->religion;
    if (religion > 3) {
        religion = 3;
    }
    if (house->data.house.num_gods < religion) {
        if (religion == 1) {
            house->data.house.evolve_text_id = 20;
            return;
        } else if (religion == 2) {
            house->data.house.evolve_text_id = 21;
            return;
        } else if (religion == 3) {
            house->data.house.evolve_text_id = 22;
            return;
        }
    }
    // barber
    if (house->data.house.barber < model->barber) {
        house->data.house.evolve_text_id = 23;
        return;
    }
    // health
    int health = model->health;
    if (house->data.house.health < health) {
        if (health == 1) {
            house->data.house.evolve_text_id = 24;
        } else if (house->data.house.clinic) {
            house->data.house.evolve_text_id = 25;
        } else {
            house->data.house.evolve_text_id = 26;
        }
        return;
    }
    // oil
    if (house->resources[RESOURCE_OIL] < model->oil) {
        house->data.house.evolve_text_id = 27;
        return;
    }
    // furniture
    if (house->resources[RESOURCE_FURNITURE] < model->furniture) {
        house->data.house.evolve_text_id = 28;
        return;
    }
    // wine
    int wine = model->wine;
    if (house->resources[RESOURCE_WINE] < wine) {
        house->data.house.evolve_text_id = 29;
        return;
    }
    if (wine > 1 && !city_resource_multiple_wine_available()) {
        house->data.house.evolve_text_id = 65;
        return;
    }
    if (house->subtype.house_level >= HOUSE_LUXURY_PALACE) { // max level!
        house->data.house.evolve_text_id = 60;
        return;
    }

    // this house will evolve if ...

    // desirability
    if (house->desirability < model->evolve_desirability) {
        if (worst_desirability_building) {
            house->data.house.evolve_text_id = 62;
        } else {
            house->data.house.evolve_text_id = 30;
        }
        return;
    }
    model = model_get_house(++level);
    // water
    water = model->water;
    if (water == 1 && !house->has_water_access) {
        if (!house->has_well_access) {
            house->data.house.evolve_text_id = 31;
            return;
        } else if (!house->has_latrines_access) {
            house->data.house.evolve_text_id = 68;
            return;
        }
    }

    if (water == 2 && !house->has_water_access) {
        if (level >= HOUSE_LARGE_CASA && house->has_well_access && house->has_latrines_access) {
            house->data.house.evolve_text_id = 32;
            return;
        }
    }


    // entertainment
    entertainment = model->entertainment;
    if (house->data.house.entertainment < entertainment) {
        if (!house->data.house.entertainment) {
            house->data.house.evolve_text_id = 33;
        } else if (entertainment < 10) {
            house->data.house.evolve_text_id = 34;
        } else if (entertainment < 25) {
            house->data.house.evolve_text_id = 35;
        } else if (entertainment < 50) {
            house->data.house.evolve_text_id = 36;
        } else if (entertainment < 80) {
            house->data.house.evolve_text_id = 37;
        } else {
            house->data.house.evolve_text_id = 38;
        }
        return;
    }
    // food types
    foodtypes_required = model->food_types;
    if (foodtypes_available < foodtypes_required) {
        if (foodtypes_required == 1) {
            house->data.house.evolve_text_id = 39;
            return;
        } else if (foodtypes_required == 2) {
            house->data.house.evolve_text_id = 40;
            return;
        } else if (foodtypes_required == 3) {
            house->data.house.evolve_text_id = 41;
            return;
        }
    }
    // education
    education = model->education;
    if (house->data.house.education < education) {
        if (education == 1) {
            house->data.house.evolve_text_id = 44;
            return;
        } else if (education == 2) {
            if (house->data.house.school) {
                house->data.house.evolve_text_id = 45;
                return;
            } else if (house->data.house.library) {
                house->data.house.evolve_text_id = 46;
                return;
            }
        } else if (education == 3) {
            house->data.house.evolve_text_id = 47;
            return;
        }
    }
    // bathhouse
    if (house->data.house.bathhouse < model->bathhouse) {
        house->data.house.evolve_text_id = 48;
        return;
    }
    // pottery
    if (house->resources[RESOURCE_POTTERY] < model->pottery) {
        house->data.house.evolve_text_id = 49;
        return;
    }
    // religion
    religion = model->religion;
    if (religion > 3) {
        religion = 3;
    }
    if (house->data.house.num_gods < religion) {
        if (religion == 1) {
            house->data.house.evolve_text_id = 50;
            return;
        } else if (religion == 2) {
            house->data.house.evolve_text_id = 51;
            return;
        } else if (religion == 3) {
            house->data.house.evolve_text_id = 52;
            return;
        }
    }
    // barber
    if (house->data.house.barber < model->barber) {
        house->data.house.evolve_text_id = 53;
        return;
    }
    // health
    health = model->health;
    if (house->data.house.health < health) {
        if (health == 1) {
            house->data.house.evolve_text_id = 54;
        } else if (house->data.house.clinic) {
            house->data.house.evolve_text_id = 55;
        } else {
            house->data.house.evolve_text_id = 56;
        }
        return;
    }
    // oil
    if (house->resources[RESOURCE_OIL] < model->oil) {
        house->data.house.evolve_text_id = 57;
        return;
    }
    // furniture
    if (house->resources[RESOURCE_FURNITURE] < model->furniture) {
        house->data.house.evolve_text_id = 58;
        return;
    }
    // wine
    wine = model->wine;
    if (house->resources[RESOURCE_WINE] < wine) {
        house->data.house.evolve_text_id = 59;
        return;
    }
    if (wine > 1 && !city_resource_multiple_wine_available()) {
        house->data.house.evolve_text_id = 66;
        return;
    }
    // house is evolving
    house->data.house.evolve_text_id = 61;
    if (house->data.house.no_space_to_expand == 1) {
        // house would like to evolve but can't
        house->data.house.evolve_text_id = 64;
    }
}

static building_type get_building_type_at_tile(const building *house, int x, int y)
{
    int grid_offset = map_grid_offset(x, y);
    int building_id = map_building_at(grid_offset);
    if (building_id <= 0) {
        if (map_terrain_is(grid_offset, TERRAIN_HIGHWAY)) {
            return BUILDING_HIGHWAY;
        } else {
            return BUILDING_NONE;
        }
    }
    building *b = building_get(building_id);
    if (b->state != BUILDING_STATE_IN_USE || building_id == house->id) {
        return BUILDING_NONE;
    }
    if (b->house_size && b->type >= house->type) {
        return BUILDING_NONE;
    }
    return b->type;
}

building_type building_house_determine_worst_desirability_building_type(const building *house)
{
    int lowest_desirability = 0;
    building_type lowest_building_type = BUILDING_NONE;
    int x_min, y_min, x_max, y_max;
    map_grid_get_area(house->x, house->y, 1, 8, &x_min, &y_min, &x_max, &y_max);

    for (int y = y_min; y <= y_max; y++) {
        for (int x = x_min; x <= x_max; x++) {
            building_type type = get_building_type_at_tile(house, x, y);
            const model_building *model = model_get_building(type);
            // simplified desirability calculation
            int des = model->desirability_value;
            int step = model->desirability_step;
            int step_size = model->desirability_step_size;
            int range = model->desirability_range;
            int tiles_within_step = 0;
            int dist = calc_maximum_distance(x, y, house->x, house->y);
            if (dist <= range) {
                while (dist >= 1) {
                    tiles_within_step++;
                    if (tiles_within_step >= step) {
                        des += step_size;
                        tiles_within_step = 0;
                    }
                    dist--;
                }
                if (des < lowest_desirability) {
                    lowest_desirability = des;
                    lowest_building_type = type;
                }
            }
        }
    }
    return lowest_building_type;
}
