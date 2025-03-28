#pragma once

#include "../rf/os/timestamp.h"
#include "../rf/math/vector.h"

struct WorldHUDAssets
{
    int flag_red_d;
    int flag_blue_d;
    int flag_red_a;
    int flag_blue_a;
    int flag_red_s;
    int flag_blue_s;
    int mp_respawn;
};

struct WorldHUDRender
{
    static constexpr float base_scale = 1.0f;
    static constexpr float reference_distance = 17.5f;
    static constexpr float min_scale = 0.5f;
    static constexpr float max_scale = 2.0f;
    static constexpr float ctf_flag_offset = 1.75f;
    static constexpr float fog_dist_multi = 0.85f;
    static constexpr float fog_dist_min = 5.0f;
    static constexpr float fog_dist_max = 100.0f;
};

enum class WorldHUDRenderMode : int
{
    no_overdraw,
    no_overdraw_glow,
    overdraw
};

struct EphemeralWorldHUDSprite
{
    int bitmap = -1;
    rf::Vector3 pos;
    std::string label = "";
    int player_id = -1;
    WorldHUDRenderMode render_mode = WorldHUDRenderMode::overdraw;
    rf::Timestamp timestamp;
    int duration = 10000;
    bool float_away = false;
    float wind_phase_offset = 0.0f;
};

struct EphemeralWorldHUDString
{
    rf::Vector3 pos;
    uint8_t player_id;
    uint16_t damage;
    WorldHUDRenderMode render_mode = WorldHUDRenderMode::overdraw;
    rf::Timestamp timestamp;
    int duration = 10000;
    bool float_away = false;
    float wind_phase_offset = 0.0f;
};

void hud_world_do_frame();
void load_world_hud_assets();
void populate_world_hud_sprite_events();
void add_location_ping_world_hud_sprite(rf::Vector3 pos, std::string player_name, int player_id);
void add_damage_notify_world_hud_string(rf::Vector3 pos, uint8_t damaged_player_id, uint16_t damage, bool died);
void do_render_world_hud_sprite(rf::Vector3 pos, float base_scale, int bitmap_handle, WorldHUDRenderMode render_mode,
                                bool stay_inside_fog, bool distance_scaling, bool only_draw_during_gameplay);
