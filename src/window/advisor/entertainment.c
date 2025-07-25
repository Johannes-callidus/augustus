#include "entertainment.h"

#include "assets/assets.h"
#include "building/count.h"
#include "city/culture.h"
#include "city/entertainment.h"
#include "city/festival.h"
#include "city/finance.h"
#include "city/games.h"
#include "city/gods.h"
#include "city/houses.h"
#include "core/calc.h"
#include "graphics/button.h"
#include "graphics/generic_button.h"
#include "graphics/image.h"
#include "graphics/lang_text.h"
#include "graphics/panel.h"
#include "graphics/text.h"
#include "graphics/window.h"
#include "translation/translation.h"
#include "window/hold_games.h"

#define ADVISOR_HEIGHT 27

#define PEOPLE_OFFSET 330
#define COVERAGE_OFFSET 470
#define COVERAGE_WIDTH 130

static unsigned int focus_button_id;

static void button_hold_games(const generic_button *button);

static generic_button hold_games_button[] = {
    {102, 370, 300, 20, button_hold_games},
};

struct games_text {
    translation_key preparation_text;
    translation_key ongoing_text;
} text_data[] = {
    {0,0}, // 0 element unused
    {TR_WINDOW_ADVISOR_ENTERTAINMENT_PREPARING_NG, TR_WINDOW_ADVISOR_ENTERTAINMENT_UNDERWAY_NG},
    {TR_WINDOW_ADVISOR_ENTERTAINMENT_PREPARING_IG, TR_WINDOW_ADVISOR_ENTERTAINMENT_UNDERWAY_IG},
    {TR_WINDOW_ADVISOR_ENTERTAINMENT_PREPARING_AG, TR_WINDOW_ADVISOR_ENTERTAINMENT_UNDERWAY_AG},
};


static int get_entertainment_advice(void)
{
    const house_demands *demands = city_houses_demands();
    if (demands->missing.entertainment > demands->missing.more_entertainment) {
        return 3;
    } else if (!demands->missing.more_entertainment) {
        return city_culture_average_entertainment() ? 1 : 0;
    } else if (city_entertainment_venue_needing_shows()) {
        return 3 + city_entertainment_venue_needing_shows();
    } else {
        return 2;
    }
}

void window_entertainment_draw_games_text(int x, int y)
{
    games_type *game = city_games_get_game_type(city_festival_selected_game_id());
    int cooldown = city_festival_games_cooldown();

    if (cooldown) {
        text_draw_centered(translation_for(TR_WINDOW_ADVISOR_ENTERTAINMENT_GAMES_COOLDOWN_TEXT), x, y + 15,
            400, FONT_NORMAL_WHITE, 0);
        int width = text_draw(translation_for(TR_WINDOW_ADVISOR_ENTERTAINMENT_GAMES_COOLDOWN), x + 46, y + 50,
            FONT_NORMAL_WHITE, 0);
        text_draw_number(cooldown, '@', "", x + 46 + width, y + 50, FONT_NORMAL_WHITE, 0);
    } else if (city_festival_games_planning_time()) {
        text_draw_centered(translation_for(TR_WINDOW_ADVISOR_ENTERTAINMENT_GAMES_PREPARING), x, y + 15, 400,
            FONT_NORMAL_WHITE, 0);
        int width = text_draw(translation_for(text_data[game->id].preparation_text), x + 56, y + 50,
            FONT_NORMAL_WHITE, 0);
        text_draw_number(city_festival_games_planning_time(), '@', "", x + 56 + width, y + 50, FONT_NORMAL_WHITE, 0);
    } else if (city_festival_games_active()) {
        text_draw_multiline(translation_for(text_data[game->id].ongoing_text), x + 4, y, 400, 0, FONT_NORMAL_WHITE, 0);
    } else {
        text_draw_multiline(translation_for(TR_WINDOW_ADVISOR_ENTERTAINMENT_GAMES_DESC), x + 4, y,
            400, 0, FONT_NORMAL_WHITE, 0);
        text_draw_centered(translation_for(TR_WINDOW_ADVISOR_ENTERTAINMENT_GAMES_BUTTON), x + 56, y + 60,
            300, FONT_NORMAL_WHITE, 0);
    }
}

static void draw_games_info(void)
{
    inner_panel_draw(48, 302, 34, 6);
    text_draw(translation_for(TR_WINDOW_ADVISOR_ENTERTAINMENT_GAMES_HEADER), 52, 274, FONT_LARGE_BLACK, 0);
    image_draw(assets_get_image_id("UI", "HoldGames Banner"), 460, 305, COLOR_MASK_NONE, SCALE_NONE);
    image_draw_border(assets_get_image_id("UI", "HoldGames Banner Border"), 460, 305, COLOR_MASK_NONE);
    window_entertainment_draw_games_text(56, 315);
}

static int draw_background(void)
{
    city_gods_calculate_moods(0);
    city_culture_calculate();

    outer_panel_draw(0, 0, 40, ADVISOR_HEIGHT);
    image_draw(image_group(GROUP_ADVISOR_ICONS) + 8, 10, 10, COLOR_MASK_NONE, SCALE_NONE);

    lang_text_draw(58, 0, 60, 12, FONT_LARGE_BLACK); // Entertainment

    lang_text_draw_centered(58, 1, 149, 46, 100, FONT_SMALL_PLAIN); // Working
    lang_text_draw_centered(58, 2, 231, 46, 100, FONT_SMALL_PLAIN); // Shows
    lang_text_draw(58, 3, 336, 46, FONT_SMALL_PLAIN);               // Can entertain
    lang_text_draw_centered(58, 4, 465, 46, 140, FONT_SMALL_PLAIN); // City coverage

    inner_panel_draw(32, 60, 36, 8);

    // taverns
    lang_text_draw_amount(CUSTOM_TRANSLATION, TR_WINDOW_ADVISOR_ENTERTAINMENT_TAVERN_COVERAGE,
        building_count_total(BUILDING_TAVERN), 40, 67, FONT_NORMAL_WHITE);
    text_draw_number_centered(building_count_active(BUILDING_TAVERN), 150, 67, 100, FONT_NORMAL_WHITE);
    lang_text_draw_centered(56, 2, 230, 67, 100, FONT_NORMAL_WHITE);
    int width = text_draw_number(city_culture_get_tavern_person_coverage(), '_', " ",
        PEOPLE_OFFSET, 67, FONT_NORMAL_WHITE, 0);
    lang_text_draw(58, 5, PEOPLE_OFFSET + width, 67, FONT_NORMAL_WHITE);
    int pct_tavern = city_culture_coverage_tavern();
    if (pct_tavern == 0) {
        lang_text_draw_centered(57, 10, COVERAGE_OFFSET, 67, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    } else if (pct_tavern < 100) {
        lang_text_draw_centered(57, 11 + pct_tavern / 10, COVERAGE_OFFSET, 67, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    } else {
        lang_text_draw_centered(57, 21, COVERAGE_OFFSET, 67, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    }

    // theaters
    lang_text_draw_amount(8, 34, building_count_total(BUILDING_THEATER), 40, 87, FONT_NORMAL_WHITE);
    text_draw_number_centered(building_count_active(BUILDING_THEATER), 150, 87, 100, FONT_NORMAL_WHITE);
    text_draw_number_centered(city_entertainment_theater_shows(), 230, 87, 100, FONT_NORMAL_WHITE);
    width = text_draw_number(city_culture_get_theatre_person_coverage(), '_', " ",
        PEOPLE_OFFSET, 87, FONT_NORMAL_WHITE, 0);
    lang_text_draw(58, 5, PEOPLE_OFFSET + width, 87, FONT_NORMAL_WHITE);
    int pct_theater = city_culture_coverage_theater();
    if (pct_theater == 0) {
        lang_text_draw_centered(57, 10, COVERAGE_OFFSET, 87, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    } else if (pct_theater < 100) {
        lang_text_draw_centered(57, 11 + pct_theater / 10, COVERAGE_OFFSET, 87, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    } else {
        lang_text_draw_centered(57, 21, COVERAGE_OFFSET, 87, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    }

    // amphitheaters
    lang_text_draw_amount(8, 36, building_count_total(BUILDING_AMPHITHEATER), 40, 107, FONT_NORMAL_WHITE);
    text_draw_number_centered(building_count_active(BUILDING_AMPHITHEATER), 150, 107, 100, FONT_NORMAL_WHITE);
    text_draw_number_centered(city_entertainment_amphitheater_shows(), 230, 107, 100, FONT_NORMAL_WHITE);
    width = text_draw_number(city_culture_get_ampitheatre_person_coverage(), '@', " ",
        PEOPLE_OFFSET, 107, FONT_NORMAL_WHITE, 0);
    lang_text_draw(58, 5, PEOPLE_OFFSET + width, 107, FONT_NORMAL_WHITE);
    int pct_amphitheater = city_culture_coverage_amphitheater();
    if (pct_amphitheater == 0) {
        lang_text_draw_centered(57, 10, COVERAGE_OFFSET, 107, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    } else if (pct_amphitheater < 100) {
        lang_text_draw_centered(57, 11 + pct_amphitheater / 10,
            COVERAGE_OFFSET, 107, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    } else {
        lang_text_draw_centered(57, 21, COVERAGE_OFFSET, 107, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    }

    // arenas
    lang_text_draw_amount(CUSTOM_TRANSLATION, TR_WINDOW_ADVISOR_ENTERTAINMENT_ARENA_COVERAGE,
        building_count_total(BUILDING_ARENA), 40, 127, FONT_NORMAL_WHITE);
    text_draw_number_centered(building_count_active(BUILDING_ARENA), 150, 127, 100, FONT_NORMAL_WHITE);
    width = text_draw_number(city_culture_get_arena_person_coverage(), '_', " ", PEOPLE_OFFSET, 127, FONT_NORMAL_WHITE, 0);
    lang_text_draw(58, 5, PEOPLE_OFFSET + width, 127, FONT_NORMAL_WHITE);
    text_draw_number_centered(city_entertainment_arena_shows(), 230, 127, 100, FONT_NORMAL_WHITE);
    int pct = city_culture_coverage_arena();
    if (pct == 0) {
        lang_text_draw_centered(57, 10, COVERAGE_OFFSET, 127, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    } else if (pct < 100) {
        lang_text_draw_centered(57, 11 + pct / 10, COVERAGE_OFFSET, 127, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    } else {
        lang_text_draw_centered(57, 21, COVERAGE_OFFSET, 127, COVERAGE_WIDTH, FONT_NORMAL_WHITE);
    }

    // colosseums
    int has_colosseum = building_count_active(BUILDING_COLOSSEUM) ? 1 : 0;
    lang_text_draw(CUSTOM_TRANSLATION, TR_ADVISOR_NO_ACTIVE_COLOSSEUM + has_colosseum, 45, 148, FONT_NORMAL_WHITE);
    lang_text_draw_centered(57, has_colosseum ? 21 : 10, COVERAGE_OFFSET, 148, COVERAGE_WIDTH, FONT_NORMAL_WHITE);

    // hippodromes
    int has_hippodrome = building_count_active(BUILDING_HIPPODROME) ? 1 : 0;
    lang_text_draw(CUSTOM_TRANSLATION, TR_ADVISOR_NO_ACTIVE_HIPPODROME + has_hippodrome, 45, 168, FONT_NORMAL_WHITE);
    lang_text_draw_centered(57, has_hippodrome ? 21 : 10, COVERAGE_OFFSET, 168, COVERAGE_WIDTH, FONT_NORMAL_WHITE);

    lang_text_draw_multiline(58, 7 + get_entertainment_advice(), 52, 208, 540, FONT_NORMAL_BLACK);

    draw_games_info();

    return ADVISOR_HEIGHT;
}

static void draw_foreground(void)
{
    if (!city_festival_games_cooldown() && !city_festival_games_planning_time() && !city_festival_games_active()) {
        button_border_draw(102, 370, 300, 20, focus_button_id == 1);
    }

}

static int handle_mouse(const mouse *m)
{
    return generic_buttons_handle_mouse(m, 0, 0, hold_games_button, 1, &focus_button_id);
}

static void button_hold_games(const generic_button *button)
{
    window_hold_games_show(0);
}

static void get_tooltip_text(advisor_tooltip_result *r)
{
    if (focus_button_id) {
        r->translation_key = TR_TOOLTIP_ADVISOR_ENTERTAINMENT_GAMES_BUTTON;
    }
}

const advisor_window_type *window_advisor_entertainment(void)
{
    static const advisor_window_type window = {
        draw_background,
        draw_foreground,
        handle_mouse,
        get_tooltip_text
    };
    return &window;
}
