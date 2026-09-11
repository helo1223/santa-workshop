#ifndef LVLX_FORMAT_H
#define LVLX_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct LvlxVec3 {
        float x;
        float y;
        float z;
    } LvlxVec3;

    typedef struct LvlxElement {
        uint8_t present;
        uint32_t definition_id;
        uint32_t lightmap_attachment_key;
        LvlxVec3 vector1;
        LvlxVec3 vector2;
        uint8_t orientation;
        uint32_t waylist_index;
    } LvlxElement;

    typedef struct LvlxWaylist {
        uint32_t index;
        uint16_t point_count;
        LvlxVec3* points;
    } LvlxWaylist;

    typedef struct LvlxLevel {
        uint8_t version;
        uint32_t element_count;
        LvlxElement* elements;
        uint32_t waylist_count;
        LvlxWaylist* waylists;
        uint8_t* trailing_data;
        size_t trailing_size;
        uint8_t has_spawn_data;
        uint32_t level_score_multiplier;
        uint32_t level_time_limit_seconds;
        LvlxVec3 spawn_position;
        float initial_camera_heading_degrees;
    } LvlxLevel;

    typedef struct LvlxElementDefinition {
        char* name;
        char* category;
        char* resource;
        char* physics_body;
        char* preview_animation;
        float preview_animation_rate;
        uint8_t has_rim_light;
        float rim_light[5]; /* start, end, RGB; optional definition override */
        char** steady_effects;
        size_t steady_effect_count;
        char** triggered_effects;
        size_t triggered_effect_count;
        float scaling;
        float vertical_offset;
        float speed;
        int32_t type_value;
        uint8_t random_rotation;
        uint8_t set_static;
        uint8_t endpoint_movement_enabled;
        uint8_t waylist_enabled;
    } LvlxElementDefinition;

    typedef struct LvlxDefinitionTable {
        LvlxElementDefinition* items;
        size_t count;
    } LvlxDefinitionTable;

    int lvlx_load_level(const char* dat_path, LvlxLevel* result);
    int lvlx_load_level_memory(const uint8_t* data, size_t size, LvlxLevel* result);
    int lvlx_save_level(const char* dat_path, const LvlxLevel* level);
    void lvlx_free_level(LvlxLevel* level);

    int lvlx_load_definitions(const char* elements_path,
        LvlxDefinitionTable* result);
    int lvlx_append_definitions(const char* elements_path,
        LvlxDefinitionTable* definitions);
    void lvlx_free_definitions(LvlxDefinitionTable* definitions);

    const LvlxElementDefinition* lvlx_find_definition(
        const LvlxDefinitionTable* definitions,
        uint32_t definition_id);

    /* Runtime editing helpers.
       Element indices are stable: adding reuses an empty slot when possible,
       and removing marks the slot as not present instead of shifting entries. */
    LvlxElement* lvlx_get_element(LvlxLevel* level, uint32_t element_index);
    const LvlxElement* lvlx_get_element_const(
        const LvlxLevel* level, uint32_t element_index);

    int lvlx_add_element(LvlxLevel* level,
        const LvlxElement* element, uint32_t* element_index_out);
    int lvlx_remove_element(LvlxLevel* level, uint32_t element_index);

    int lvlx_set_element_transform(LvlxLevel* level, uint32_t element_index,
        LvlxVec3 vector1, LvlxVec3 vector2, uint8_t orientation);

    int lvlx_set_spawn_transform(LvlxLevel* level, LvlxVec3 position,
        float initial_camera_heading_degrees);
    int lvlx_set_level_properties(LvlxLevel* level,
        uint32_t score_multiplier, uint32_t time_limit_seconds,
        float initial_camera_heading_degrees);

    /* Convenience functions used by the command-line converter. */
    int lvlx_export_json(const char* dat_path,
        const char* elements_path,
        const char* json_path);
    int lvlx_import_json(const char* json_path, const char* dat_path);

#ifdef __cplusplus
}
#endif

#endif
