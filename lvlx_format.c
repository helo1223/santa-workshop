#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "lvlx_format.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LVLX_HEADER_SIZE 13u

static uint32_t read_u32_le(const uint8_t* p)
{
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static void write_u32_le(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void write_f32_le(uint8_t* p, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write_u32_le(p, bits);
}

static float read_f32_le(const uint8_t* p)
{
    uint32_t bits = read_u32_le(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int copy_match(uint8_t* dst, size_t dst_size, size_t* dst_pos,
    size_t distance, size_t length)
{
    size_t i;

    if (distance == 0 || distance > *dst_pos || length > dst_size - *dst_pos)
        return 0;

    /* Byte-at-a-time copying is intentional: matches may overlap. */
    for (i = 0; i < length; ++i)
        dst[*dst_pos + i] = dst[*dst_pos + i - distance];

    *dst_pos += length;
    return 1;
}

static size_t decompress_type1(const uint8_t* src, size_t src_size,
    uint8_t* dst, size_t dst_size)
{
    size_t sp = 1;
    size_t dp = 0;
    unsigned control;

    if (src_size == 0)
        return 0;

    control = src[0] & 0x1fu;

    for (;;) {
        if (control < 0x20u) {
            size_t literal_length = (size_t)control + 1u;

            if (literal_length > src_size - sp ||
                literal_length > dst_size - dp)
                return 0;

            memcpy(dst + dp, src + sp, literal_length);
            sp += literal_length;
            dp += literal_length;
        }
        else {
            size_t match_length = (size_t)(control >> 5) - 1u;
            size_t distance = (size_t)(control & 0x1fu) << 8;

            if (match_length == 6u) {
                if (sp >= src_size)
                    return 0;
                match_length += src[sp++];
            }

            if (sp >= src_size)
                return 0;

            distance += (size_t)src[sp++] + 1u;
            match_length += 3u;

            if (!copy_match(dst, dst_size, &dp, distance, match_length))
                return 0;
        }

        if (sp == src_size)
            return dp;
        if (sp > src_size)
            return 0;

        control = src[sp++];
    }
}

static size_t decompress_type2(const uint8_t* src, size_t src_size,
    uint8_t* dst, size_t dst_size)
{
    size_t sp = 1;
    size_t dp = 0;
    unsigned control;

    if (src_size == 0)
        return 0;

    control = src[0] & 0x1fu;

    for (;;) {
        if (control < 0x20u) {
            size_t literal_length = (size_t)control + 1u;

            if (literal_length > src_size - sp ||
                literal_length > dst_size - dp)
                return 0;

            memcpy(dst + dp, src + sp, literal_length);
            sp += literal_length;
            dp += literal_length;
        }
        else {
            size_t match_length = (size_t)(control >> 5) - 1u;
            size_t high_distance = (size_t)(control & 0x1fu) << 8;
            size_t distance;
            unsigned low;

            if (match_length == 6u) {
                unsigned extension;
                do {
                    if (sp >= src_size)
                        return 0;
                    extension = src[sp++];
                    match_length += extension;
                } while (extension == 0xffu);
            }

            if (sp >= src_size)
                return 0;

            low = src[sp++];

            if (low == 0xffu && high_distance == 0x1f00u) {
                size_t extended;

                if (src_size - sp < 2u)
                    return 0;

                extended = ((size_t)src[sp] << 8) | src[sp + 1u];
                sp += 2u;
                distance = extended + 0x2000u;
            }
            else {
                distance = high_distance + low + 1u;
            }

            match_length += 3u;

            if (!copy_match(dst, dst_size, &dp, distance, match_length))
                return 0;
        }

        if (sp == src_size)
            return dp;
        if (sp > src_size)
            return 0;

        control = src[sp++];
    }
}

static size_t decompress_payload(const uint8_t* src, size_t src_size,
    uint8_t* dst, size_t dst_size,
    unsigned* type_out)
{
    unsigned type;

    if (src_size == 0)
        return 0;

    type = (src[0] >> 5) + 1u;
    *type_out = type;

    if (type == 1u)
        return decompress_type1(src, src_size, dst, dst_size);
    if (type == 2u)
        return decompress_type2(src, src_size, dst, dst_size);

    return 0;
}

static uint8_t* read_file(const char* path, size_t* size_out)
{
    FILE* file;
    long length;
    uint8_t* data;

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Cannot determine the size of %s\n", path);
        fclose(file);
        return NULL;
    }

    if ((size_t)length == SIZE_MAX) {
        fclose(file);
        return NULL;
    }
    data = (uint8_t*)malloc((size_t)length + 1u);
    if (data == NULL) {
        fprintf(stderr, "Out of memory while reading %s\n", path);
        fclose(file);
        return NULL;
    }

    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "Cannot read all of %s\n", path);
        free(data);
        fclose(file);
        return NULL;
    }

    data[(size_t)length] = 0;

    fclose(file);
    *size_out = (size_t)length;
    return data;
}

typedef LvlxVec3 Vec3;

typedef struct Reader {
    const uint8_t* data;
    size_t size;
    size_t position;
    int failed;
} Reader;

typedef LvlxElementDefinition ElementDefinition;
typedef LvlxDefinitionTable DefinitionTable;
typedef LvlxElement MapElement;
typedef LvlxWaylist Waylist;
typedef LvlxLevel Level;

static char* copy_string(const char* text)
{
    size_t length = strlen(text);
    char* result = (char*)malloc(length + 1u);

    if (result != NULL)
        memcpy(result, text, length + 1u);
    return result;
}

static int reader_has(const Reader* reader, size_t amount)
{
    return !reader->failed && amount <= reader->size - reader->position;
}

static uint8_t reader_u8(Reader* reader)
{
    if (!reader_has(reader, 1u)) {
        reader->failed = 1;
        return 0;
    }
    return reader->data[reader->position++];
}

static uint16_t reader_u16(Reader* reader)
{
    uint16_t value;

    if (!reader_has(reader, 2u)) {
        reader->failed = 1;
        return 0;
    }

    value = (uint16_t)reader->data[reader->position]
        | (uint16_t)((uint16_t)reader->data[reader->position + 1u] << 8);
    reader->position += 2u;
    return value;
}

static uint32_t reader_u32(Reader* reader)
{
    uint32_t value;

    if (!reader_has(reader, 4u)) {
        reader->failed = 1;
        return 0;
    }

    value = read_u32_le(reader->data + reader->position);
    reader->position += 4u;
    return value;
}

static float reader_f32(Reader* reader)
{
    uint32_t bits = reader_u32(reader);
    float value = 0.0f;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static Vec3 reader_vec3(Reader* reader)
{
    Vec3 value;
    value.x = reader_f32(reader);
    value.y = reader_f32(reader);
    value.z = reader_f32(reader);
    return value;
}

static void free_definitions(DefinitionTable* table)
{
    size_t i;

    for (i = 0; i < table->count; ++i) {
        free(table->items[i].name);
        free(table->items[i].category);
        free(table->items[i].resource);
        free(table->items[i].physics_body);
        free(table->items[i].preview_animation);
        for (size_t j = 0; j < table->items[i].steady_effect_count; ++j)
            free(table->items[i].steady_effects[j]);
        free(table->items[i].steady_effects);
        for (size_t j = 0; j < table->items[i].triggered_effect_count; ++j)
            free(table->items[i].triggered_effects[j]);
        free(table->items[i].triggered_effects);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
}

static void trim_right(char* text)
{
    size_t length = strlen(text);

    while (length != 0u &&
        (text[length - 1u] == ' ' || text[length - 1u] == '\t' ||
            text[length - 1u] == '\r' || text[length - 1u] == '\n')) {
        text[--length] = '\0';
    }
}

static int32_t element_type_value(const char* name)
{
    static const struct { const char* name; int32_t value; } types[] = {
        { "Platform", 2 }, { "Rectform", 3 }, { "Wall", 14 },
        { "Pillar", 15 }, { "Decoration", 1 }, { "Mover", 5 },
        { "Jumper", 7 }, { "EnemyElevator", 12 }, { "Enemy", 9 },
        { "EnemyWaylist", 13 }, { "EnemyShooting", 16 },
        { "EnemyRollOver", 18 }, { "Bullet", 17 }, { "Exit", 8 },
        { "Bonus", 6 }, { "Savepoint", 10 }, { "Extralife", 11 },
        { "Dynamic", 19 }, { "SelfRotating", 20 }, { "SelfMoving", 21 }
    };
    size_t i;
    for (i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
        if (strcmp(name, types[i].name) == 0) return types[i].value;
    return -1;
}

static uint8_t type_uses_destination(int32_t type)
{
    return type == 5 || type == 9 || type == 12 ||
        type == 13 || type == 18 || type == 21;
}

static uint8_t type_uses_waylist(int32_t type)
{
    return type == 13 || type == 18;
}

static int load_definitions(const char* path, DefinitionTable* table)
{
    FILE* file = fopen(path, "r");
    char line[2048];
    size_t current_id = SIZE_MAX;

    if (file == NULL) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned id;
        char name[1024];
        char category[512];

        if (sscanf(line, " Element (%u, \"%1023[^\"]\", %511[^)])",
            &id, name, category) == 3) {
            ElementDefinition* resized;
            size_t old_count;

            trim_right(category);
            if ((size_t)id >= table->count) {
                size_t new_count = (size_t)id + 1u;

                if (new_count > SIZE_MAX / sizeof(*table->items)) {
                    fclose(file);
                    return 0;
                }

                old_count = table->count;
                resized = (ElementDefinition*)realloc(
                    table->items, new_count * sizeof(*table->items));
                if (resized == NULL) {
                    fclose(file);
                    return 0;
                }

                table->items = resized;
                table->count = new_count;
                memset(table->items + old_count, 0,
                    (new_count - old_count) * sizeof(*table->items));
                while (old_count < new_count)
                    table->items[old_count++].scaling = 1.0f;
            }

            free(table->items[id].name);
            free(table->items[id].category);
            table->items[id].name = copy_string(name);
            table->items[id].category = copy_string(category);
            table->items[id].scaling = 1.0f;
            table->items[id].type_value = element_type_value(category);
            table->items[id].random_rotation = 0u;
            table->items[id].set_static = 0u;
            table->items[id].endpoint_movement_enabled =
                type_uses_destination(table->items[id].type_value);
            table->items[id].waylist_enabled =
                type_uses_waylist(table->items[id].type_value);
            current_id = (size_t)id;
            if (table->items[id].name == NULL ||
                table->items[id].category == NULL) {
                fclose(file);
                return 0;
            }
        }
        else if (current_id != SIZE_MAX && current_id < table->count) {
            float rim[5];
            char resource[1024];
            char effect_type[64];
            char effect_name[1024];
            float scaling = 1.0f;

            if (sscanf(line, " Resource \"%1023[^\"]\"", resource) == 1) {
                /* Use the first resource. Later resources are normally LODs. */
                if (table->items[current_id].resource == NULL) {
                    table->items[current_id].resource = copy_string(resource);
                    if (table->items[current_id].resource == NULL) {
                        fclose(file);
                        return 0;
                    }
                }
            }
            else if (sscanf(line, " Effect %63s \"%1023[^\"]\"", effect_type,
                effect_name) == 2) {
                ElementDefinition* item = &table->items[current_id];
                char*** names = NULL;
                size_t* count = NULL;
                if (_stricmp(effect_type, "Steady") == 0) {
                    names = &item->steady_effects;
                    count = &item->steady_effect_count;
                }
                else if (_stricmp(effect_type, "Triggered") == 0) {
                    names = &item->triggered_effects;
                    count = &item->triggered_effect_count;
                }
                if (names != NULL && *count < SIZE_MAX / sizeof(**names)) {
                    char** resized = (char**)realloc(*names,
                        (*count + 1u) * sizeof(**names));
                    if (resized == NULL) {
                        fclose(file);
                        return 0;
                    }
                    *names = resized;
                    (*names)[*count] = copy_string(effect_name);
                    if ((*names)[*count] == NULL) {
                        fclose(file);
                        return 0;
                    }
                    ++*count;
                }
            }
            else if (sscanf(line, " Physic \"%1023[^\"]\"", resource) == 1) {
                free(table->items[current_id].physics_body);
                table->items[current_id].physics_body = copy_string(resource);
                if (!table->items[current_id].physics_body) { fclose(file); return 0; }
            }
            else if (sscanf(line, " Animation %63s \"%1023[^\"]\" %f",
                effect_type, resource, &scaling) >= 2) {
                /* Preview one explicitly authored action; never infer a CAF filename. */
                if (!table->items[current_id].preview_animation) {
                    table->items[current_id].preview_animation = copy_string(resource);
                    table->items[current_id].preview_animation_rate = scaling;
                    if (!table->items[current_id].preview_animation) { fclose(file); return 0; }
                }
            }
            else if (sscanf(line, " RimLight %f %f %f %f %f",
                &rim[0], &rim[1], &rim[2], &rim[3], &rim[4]) == 5) {
                int valid = 1, j;
                for (j = 0; j < 5; ++j) if (!isfinite(rim[j])) valid = 0;
                if (valid) {
                    memcpy(table->items[current_id].rim_light, rim, sizeof(rim));
                    table->items[current_id].has_rim_light = 1;
                }
            }
            else if (sscanf(line, " Scaling %f", &scaling) == 1) {
                table->items[current_id].scaling = scaling;
            }
            else if (sscanf(line, " VerticalOffset %f", &scaling) == 1) {
                table->items[current_id].vertical_offset = scaling;
            }
            else if (sscanf(line, " Speed %f", &scaling) == 1) {
                table->items[current_id].speed = scaling;
            }
            else if (strstr(line, "RandomRotation") != NULL) {
                table->items[current_id].random_rotation = 1u;
            }
            else if (strstr(line, "SetStatic") != NULL) {
                table->items[current_id].set_static = 1u;
                table->items[current_id].endpoint_movement_enabled = 0u;
            }
            else if (strchr(line, '}') != NULL) {
                current_id = SIZE_MAX;
            }
        }
    }

    if (ferror(file)) {
        fprintf(stderr, "Error while reading %s\n", path);
        fclose(file);
        return 0;
    }

    fclose(file);
    return 1;
}

static void free_level(Level* level)
{
    uint32_t i;

    for (i = 0; i < level->waylist_count; ++i)
        free(level->waylists[i].points);
    free(level->waylists);
    free(level->elements);
    free(level->trailing_data);
    memset(level, 0, sizeof(*level));
}

static int parse_level(const uint8_t* data, size_t size,
    unsigned version, Level* level)
{
    Reader reader;
    uint32_t i;

    reader.data = data;
    reader.size = size;
    reader.position = 0;
    reader.failed = 0;
    level->version = version;

    level->element_count = reader_u32(&reader);
    if (reader.failed ||
        level->element_count > (reader.size - reader.position)) {
        fprintf(stderr, "Invalid element count\n");
        return 0;
    }

    level->elements = (MapElement*)calloc(
        level->element_count, sizeof(*level->elements));
    if (level->element_count != 0 && level->elements == NULL)
        return 0;

    for (i = 0; i < level->element_count; ++i) {
        MapElement* element = &level->elements[i];

        element->present = reader_u8(&reader);
        if (!element->present)
            continue;

        element->definition_id = reader_u32(&reader);
        if (version > 2u)
            element->lightmap_attachment_key = reader_u32(&reader);
        element->vector1 = reader_vec3(&reader);
        element->vector2 = reader_vec3(&reader);
        element->orientation = reader_u8(&reader);
        element->waylist_index = reader_u32(&reader);

        if (reader.failed) {
            fprintf(stderr, "Element %" PRIu32 " is truncated\n", i);
            return 0;
        }
    }

    level->waylist_count = reader_u32(&reader);
    if (reader.failed ||
        level->waylist_count > (reader.size - reader.position) / 6u) {
        fprintf(stderr, "Invalid waylist count\n");
        return 0;
    }

    level->waylists = (Waylist*)calloc(
        level->waylist_count, sizeof(*level->waylists));
    if (level->waylist_count != 0 && level->waylists == NULL)
        return 0;

    for (i = 0; i < level->waylist_count; ++i) {
        Waylist* waylist = &level->waylists[i];
        uint16_t j;

        waylist->index = reader_u32(&reader);
        waylist->point_count = reader_u16(&reader);
        if (reader.failed ||
            (size_t)waylist->point_count >
            (reader.size - reader.position) / 12u) {
            fprintf(stderr, "Waylist %" PRIu32 " is invalid\n", i);
            return 0;
        }

        waylist->points = (Vec3*)calloc(
            waylist->point_count, sizeof(*waylist->points));
        if (waylist->point_count != 0 && waylist->points == NULL)
            return 0;

        for (j = 0; j < waylist->point_count; ++j)
            waylist->points[j] = reader_vec3(&reader);
    }

    if (reader.failed) {
        fprintf(stderr, "Level data is truncated\n");
        return 0;
    }

    if (reader.position != reader.size) {
        fprintf(stderr, "Warning: %zu unparsed decompressed byte(s)\n",
            reader.size - reader.position);
    }

    return 1;
}

static void json_string(FILE* file, const char* text)
{
    const unsigned char* p = (const unsigned char*)text;

    fputc('"', file);
    while (*p != '\0') {
        switch (*p) {
        case '"': fputs("\\\"", file); break;
        case '\\': fputs("\\\\", file); break;
        case '\b': fputs("\\b", file); break;
        case '\f': fputs("\\f", file); break;
        case '\n': fputs("\\n", file); break;
        case '\r': fputs("\\r", file); break;
        case '\t': fputs("\\t", file); break;
        default:
            if (*p < 0x20u)
                fprintf(file, "\\u%04x", (unsigned)*p);
            else
                fputc(*p, file);
            break;
        }
        ++p;
    }
    fputc('"', file);
}

static void json_vec3(FILE* file, Vec3 value)
{
    fprintf(file, "[%.9g, %.9g, %.9g]",
        (double)value.x, (double)value.y, (double)value.z);
}

static int write_level_json(const char* path, const Level* level,
    const DefinitionTable* definitions,
    const uint8_t* trailing_data,
    size_t trailing_size)
{
    FILE* file = fopen(path, "w");
    uint32_t i;

    if (file == NULL) {
        fprintf(stderr, "Cannot create %s: %s\n", path, strerror(errno));
        return 0;
    }

    size_t trailing_index;

    fprintf(file, "{\n  \"format\": \"lvlx\",\n  \"version\": %u,\n",
        level->version);
    fputs("  \"trailingDataHex\": \"", file);
    for (trailing_index = 0; trailing_index < trailing_size; ++trailing_index)
        fprintf(file, "%02X", trailing_data[trailing_index]);
    fputs("\",\n", file);
    fprintf(file, "  \"elements\": [\n");

    for (i = 0; i < level->element_count; ++i) {
        const MapElement* element = &level->elements[i];
        const ElementDefinition* definition = NULL;

        fprintf(file, "    {\n      \"index\": %" PRIu32, i);
        if (!element->present) {
            fprintf(file, ",\n      \"present\": false\n    }");
        }
        else {
            if ((size_t)element->definition_id < definitions->count)
                definition = &definitions->items[element->definition_id];

            fprintf(file,
                ",\n      \"present\": true,\n"
                "      \"definitionId\": %" PRIu32 ",\n"
                "      \"name\": ",
                element->definition_id);
            if (definition != NULL && definition->name != NULL)
                json_string(file, definition->name);
            else
                fputs("null", file);

            fputs(",\n      \"category\": ", file);
            if (definition != NULL && definition->category != NULL)
                json_string(file, definition->category);
            else
                fputs("null", file);

            if (level->version > 2u)
                fprintf(file, ",\n      \"lightmapAttachmentKey\": %" PRIu32,
                element->lightmap_attachment_key);

            fputs(",\n      \"vector1\": ", file);
            json_vec3(file, element->vector1);
            fputs(",\n      \"vector2\": ", file);
            json_vec3(file, element->vector2);
            fprintf(file, ",\n      \"orientation\": %u,\n"
                "      \"waylistIndex\": ",
                (unsigned)element->orientation);
            if (element->waylist_index == UINT32_MAX)
                fputs("null", file);
            else
                fprintf(file, "%" PRIu32, element->waylist_index);
            fputs("\n    }", file);
        }

        fputs(i + 1u == level->element_count ? "\n" : ",\n", file);
    }

    fputs("  ],\n  \"waylists\": [\n", file);
    for (i = 0; i < level->waylist_count; ++i) {
        const Waylist* waylist = &level->waylists[i];
        uint16_t j;

        fprintf(file,
            "    {\n      \"index\": %" PRIu32 ",\n"
            "      \"points\": [",
            waylist->index);

        for (j = 0; j < waylist->point_count; ++j) {
            if (j != 0)
                fputs(", ", file);
            json_vec3(file, waylist->points[j]);
        }

        fputs("]\n    }", file);
        fputs(i + 1u == level->waylist_count ? "\n" : ",\n", file);
    }

    fputs("  ]\n}\n", file);

    if (fclose(file) != 0) {
        fprintf(stderr, "Cannot finish writing %s\n", path);
        return 0;
    }
    return 1;
}

typedef struct JsonReader {
    const char* data;
    size_t size;
    size_t position;
    int failed;
} JsonReader;

typedef struct ByteBuffer {
    uint8_t* data;
    size_t size;
    size_t capacity;
} ByteBuffer;

static int buffer_append(ByteBuffer* buffer, const void* data, size_t size);

static void json_skip_space(JsonReader* json)
{
    while (json->position < json->size) {
        char c = json->data[json->position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        ++json->position;
    }
}

static int json_consume(JsonReader* json, char expected)
{
    json_skip_space(json);
    if (json->position >= json->size ||
        json->data[json->position] != expected) {
        json->failed = 1;
        return 0;
    }
    ++json->position;
    return 1;
}

static int json_literal(JsonReader* json, const char* literal)
{
    size_t length = strlen(literal);

    json_skip_space(json);
    if (length > json->size - json->position ||
        memcmp(json->data + json->position, literal, length) != 0) {
        json->failed = 1;
        return 0;
    }
    json->position += length;
    return 1;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char* json_string_value(JsonReader* json)
{
    ByteBuffer text = { 0 };

    if (!json_consume(json, '"'))
        return NULL;

    while (json->position < json->size) {
        unsigned char c = (unsigned char)json->data[json->position++];

        if (c == '"') {
            uint8_t zero = 0;
            if (!buffer_append(&text, &zero, 1u)) {
                json->failed = 1;
                free(text.data);
                return NULL;
            }
            return (char*)text.data;
        }

        if (c == '\\') {
            if (json->position >= json->size) {
                json->failed = 1;
                break;
            }
            c = (unsigned char)json->data[json->position++];
            switch (c) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                int h0, h1, h2, h3;
                unsigned code;
                if (json->size - json->position < 4u) {
                    json->failed = 1;
                    break;
                }
                h0 = hex_value(json->data[json->position]);
                h1 = hex_value(json->data[json->position + 1u]);
                h2 = hex_value(json->data[json->position + 2u]);
                h3 = hex_value(json->data[json->position + 3u]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                    json->failed = 1;
                    break;
                }
                json->position += 4u;
                code = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                if (code > 0x7fu) {
                    json->failed = 1;
                    fprintf(stderr,
                        "Only ASCII JSON escapes are supported in editable fields\n");
                    break;
                }
                c = (unsigned char)code;
                break;
            }
            default:
                json->failed = 1;
                break;
            }
        }

        if (json->failed || !buffer_append(&text, &c, 1u)) {
            json->failed = 1;
            break;
        }
    }

    free(text.data);
    json->failed = 1;
    return NULL;
}

static double json_number(JsonReader* json)
{
    char* end;
    double value;

    json_skip_space(json);
    if (json->position >= json->size) {
        json->failed = 1;
        return 0.0;
    }

    errno = 0;
    value = strtod(json->data + json->position, &end);
    if (end == json->data + json->position || errno == ERANGE ||
        (size_t)(end - json->data) > json->size) {
        json->failed = 1;
        return 0.0;
    }

    json->position = (size_t)(end - json->data);
    return value;
}

static uint32_t json_u32(JsonReader* json)
{
    double value = json_number(json);
    uint32_t converted;

    if (json->failed || value < 0.0 || value > 4294967295.0) {
        json->failed = 1;
        return 0;
    }
    converted = (uint32_t)value;
    if ((double)converted != value)
        json->failed = 1;
    return converted;
}

static int json_bool(JsonReader* json)
{
    json_skip_space(json);
    if (json->size - json->position >= 4u &&
        memcmp(json->data + json->position, "true", 4u) == 0) {
        json->position += 4u;
        return 1;
    }
    if (json->size - json->position >= 5u &&
        memcmp(json->data + json->position, "false", 5u) == 0) {
        json->position += 5u;
        return 0;
    }
    json->failed = 1;
    return 0;
}

static Vec3 json_vec3_value(JsonReader* json)
{
    Vec3 value = { 0 };

    if (!json_consume(json, '['))
        return value;
    value.x = (float)json_number(json);
    json_consume(json, ',');
    value.y = (float)json_number(json);
    json_consume(json, ',');
    value.z = (float)json_number(json);
    json_consume(json, ']');
    return value;
}

static int json_skip_value(JsonReader* json);

static int json_skip_object(JsonReader* json)
{
    if (!json_consume(json, '{')) return 0;
    json_skip_space(json);
    if (json->position < json->size && json->data[json->position] == '}') {
        ++json->position;
        return 1;
    }
    for (;;) {
        char* key = json_string_value(json);
        free(key);
        if (!json_consume(json, ':') || !json_skip_value(json)) return 0;
        json_skip_space(json);
        if (json->position < json->size && json->data[json->position] == '}') {
            ++json->position;
            return 1;
        }
        if (!json_consume(json, ',')) return 0;
    }
}

static int json_skip_array(JsonReader* json)
{
    if (!json_consume(json, '[')) return 0;
    json_skip_space(json);
    if (json->position < json->size && json->data[json->position] == ']') {
        ++json->position;
        return 1;
    }
    for (;;) {
        if (!json_skip_value(json)) return 0;
        json_skip_space(json);
        if (json->position < json->size && json->data[json->position] == ']') {
            ++json->position;
            return 1;
        }
        if (!json_consume(json, ',')) return 0;
    }
}

static int json_skip_value(JsonReader* json)
{
    char* text;

    json_skip_space(json);
    if (json->position >= json->size) return 0;
    if (json->data[json->position] == '{') return json_skip_object(json);
    if (json->data[json->position] == '[') return json_skip_array(json);
    if (json->data[json->position] == '"') {
        text = json_string_value(json);
        free(text);
        return !json->failed;
    }
    if (json->data[json->position] == 't') return json_literal(json, "true");
    if (json->data[json->position] == 'f') return json_literal(json, "false");
    if (json->data[json->position] == 'n') return json_literal(json, "null");
    (void)json_number(json);
    return !json->failed;
}

static int append_element(Level* level, const MapElement* element)
{
    MapElement* resized;
    size_t count = (size_t)level->element_count + 1u;

    if (count > SIZE_MAX / sizeof(*level->elements)) return 0;
    resized = (MapElement*)realloc(level->elements,
        count * sizeof(*level->elements));
    if (resized == NULL) return 0;
    level->elements = resized;
    level->elements[level->element_count++] = *element;
    return 1;
}

static int append_waylist(Level* level, const Waylist* waylist)
{
    Waylist* resized;
    size_t count = (size_t)level->waylist_count + 1u;

    if (count > SIZE_MAX / sizeof(*level->waylists)) return 0;
    resized = (Waylist*)realloc(level->waylists,
        count * sizeof(*level->waylists));
    if (resized == NULL) return 0;
    level->waylists = resized;
    level->waylists[level->waylist_count++] = *waylist;
    return 1;
}

static int parse_json_element(JsonReader* json, Level* level)
{
    MapElement element;
    int has_present = 0;

    memset(&element, 0, sizeof(element));
    element.waylist_index = UINT32_MAX;
    if (!json_consume(json, '{')) return 0;

    for (;;) {
        char* key;
        json_skip_space(json);
        if (json->position < json->size && json->data[json->position] == '}') {
            ++json->position;
            break;
        }
        key = json_string_value(json);
        if (key == NULL || !json_consume(json, ':')) {
            free(key);
            return 0;
        }

        if (strcmp(key, "present") == 0) {
            element.present = (uint8_t)json_bool(json);
            has_present = 1;
        }
        else if (strcmp(key, "definitionId") == 0) {
            element.definition_id = json_u32(json);
        }
        else if (strcmp(key, "lightmapAttachmentKey") == 0 ||
            strcmp(key, "version3Value") == 0) {
            element.lightmap_attachment_key = json_u32(json);
        }
        else if (strcmp(key, "vector1") == 0) {
            element.vector1 = json_vec3_value(json);
        }
        else if (strcmp(key, "vector2") == 0) {
            element.vector2 = json_vec3_value(json);
        }
        else if (strcmp(key, "orientation") == 0) {
            uint32_t orientation = json_u32(json);
            if (orientation > 255u) json->failed = 1;
            element.orientation = (uint8_t)orientation;
        }
        else if (strcmp(key, "waylistIndex") == 0) {
            json_skip_space(json);
            if (json->size - json->position >= 4u &&
                memcmp(json->data + json->position, "null", 4u) == 0) {
                json->position += 4u;
                element.waylist_index = UINT32_MAX;
            }
            else {
                element.waylist_index = json_u32(json);
            }
        }
        else {
            json_skip_value(json);
        }
        free(key);
        if (json->failed) return 0;

        json_skip_space(json);
        if (json->position < json->size && json->data[json->position] == '}') {
            ++json->position;
            break;
        }
        if (!json_consume(json, ',')) return 0;
    }

    if (!has_present) {
        fprintf(stderr, "JSON element is missing 'present'\n");
        return 0;
    }
    return append_element(level, &element);
}

static int parse_json_elements(JsonReader* json, Level* level)
{
    if (!json_consume(json, '[')) return 0;
    json_skip_space(json);
    if (json->position < json->size && json->data[json->position] == ']') {
        ++json->position;
        return 1;
    }
    for (;;) {
        if (!parse_json_element(json, level)) return 0;
        json_skip_space(json);
        if (json->position < json->size && json->data[json->position] == ']') {
            ++json->position;
            return 1;
        }
        if (!json_consume(json, ',')) return 0;
    }
}

static int append_point(Waylist* waylist, Vec3 point)
{
    Vec3* resized;
    size_t count = (size_t)waylist->point_count + 1u;

    if (count > UINT16_MAX || count > SIZE_MAX / sizeof(*waylist->points))
        return 0;
    resized = (Vec3*)realloc(waylist->points,
        count * sizeof(*waylist->points));
    if (resized == NULL) return 0;
    waylist->points = resized;
    waylist->points[waylist->point_count++] = point;
    return 1;
}

static int parse_json_points(JsonReader* json, Waylist* waylist)
{
    if (!json_consume(json, '[')) return 0;
    json_skip_space(json);
    if (json->position < json->size && json->data[json->position] == ']') {
        ++json->position;
        return 1;
    }
    for (;;) {
        Vec3 point = json_vec3_value(json);
        if (json->failed || !append_point(waylist, point)) return 0;
        json_skip_space(json);
        if (json->position < json->size && json->data[json->position] == ']') {
            ++json->position;
            return 1;
        }
        if (!json_consume(json, ',')) return 0;
    }
}

static int parse_json_waylist(JsonReader* json, Level* level)
{
    Waylist waylist;

    memset(&waylist, 0, sizeof(waylist));
    if (!json_consume(json, '{')) return 0;

    for (;;) {
        char* key;
        json_skip_space(json);
        if (json->position < json->size && json->data[json->position] == '}') {
            ++json->position;
            break;
        }
        key = json_string_value(json);
        if (key == NULL || !json_consume(json, ':')) {
            free(key);
            free(waylist.points);
            return 0;
        }
        if (strcmp(key, "index") == 0)
            waylist.index = json_u32(json);
        else if (strcmp(key, "points") == 0) {
            if (!parse_json_points(json, &waylist)) {
                free(key);
                free(waylist.points);
                return 0;
            }
        }
        else
            json_skip_value(json);
        free(key);
        if (json->failed) {
            free(waylist.points);
            return 0;
        }
        json_skip_space(json);
        if (json->position < json->size && json->data[json->position] == '}') {
            ++json->position;
            break;
        }
        if (!json_consume(json, ',')) {
            free(waylist.points);
            return 0;
        }
    }

    if (!append_waylist(level, &waylist)) {
        free(waylist.points);
        return 0;
    }
    return 1;
}

static int parse_json_waylists(JsonReader* json, Level* level)
{
    if (!json_consume(json, '[')) return 0;
    json_skip_space(json);
    if (json->position < json->size && json->data[json->position] == ']') {
        ++json->position;
        return 1;
    }
    for (;;) {
        if (!parse_json_waylist(json, level)) return 0;
        json_skip_space(json);
        if (json->position < json->size && json->data[json->position] == ']') {
            ++json->position;
            return 1;
        }
        if (!json_consume(json, ',')) return 0;
    }
}

static int parse_trailing_hex(JsonReader* json, ByteBuffer* trailing)
{
    char* hex = json_string_value(json);
    size_t length;
    size_t i;

    if (hex == NULL) return 0;
    length = strlen(hex);
    if ((length & 1u) != 0u) {
        free(hex);
        return 0;
    }
    for (i = 0; i < length; i += 2u) {
        int high = hex_value(hex[i]);
        int low = hex_value(hex[i + 1u]);
        uint8_t byte;
        if (high < 0 || low < 0) {
            free(hex);
            return 0;
        }
        byte = (uint8_t)((high << 4) | low);
        if (!buffer_append(trailing, &byte, 1u)) {
            free(hex);
            return 0;
        }
    }
    free(hex);
    return 1;
}

static int load_level_json(const char* path, Level* level,
    ByteBuffer* trailing)
{
    size_t size;
    uint8_t* data = read_file(path, &size);
    JsonReader json;

    if (data == NULL) return 0;
    json.data = (const char*)data;
    json.size = size;
    json.position = 0;
    json.failed = 0;

    if (!json_consume(&json, '{')) goto failure;
    for (;;) {
        char* key;
        json_skip_space(&json);
        if (json.position < json.size && json.data[json.position] == '}') {
            ++json.position;
            break;
        }
        key = json_string_value(&json);
        if (key == NULL || !json_consume(&json, ':')) {
            free(key);
            goto failure;
        }
        if (strcmp(key, "version") == 0) {
            uint32_t version = json_u32(&json);
            if (version > 255u)
                json.failed = 1;
            else
                level->version = (uint8_t)version;
        }
        else if (strcmp(key, "trailingDataHex") == 0) {
            if (!parse_trailing_hex(&json, trailing)) {
                free(key);
                goto failure;
            }
        }
        else if (strcmp(key, "elements") == 0) {
            if (!parse_json_elements(&json, level)) {
                free(key);
                goto failure;
            }
        }
        else if (strcmp(key, "waylists") == 0) {
            if (!parse_json_waylists(&json, level)) {
                free(key);
                goto failure;
            }
        }
        else
            json_skip_value(&json);
        free(key);
        if (json.failed) goto failure;
        json_skip_space(&json);
        if (json.position < json.size && json.data[json.position] == '}') {
            ++json.position;
            break;
        }
        if (!json_consume(&json, ',')) goto failure;
    }

    json_skip_space(&json);
    if (json.failed || json.position != json.size) {
        goto failure;
    }
    free(data);
    return 1;

failure:
    fprintf(stderr, "Invalid or unsupported JSON near byte %zu\n",
        json.position);
    free(data);
    return 0;
}

static int buffer_reserve(ByteBuffer* buffer, size_t extra)
{
    size_t required;
    size_t capacity;
    uint8_t* resized;

    if (extra > SIZE_MAX - buffer->size) return 0;
    required = buffer->size + extra;
    if (required <= buffer->capacity) return 1;
    capacity = buffer->capacity != 0u ? buffer->capacity : 256u;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = required;
            break;
        }
        capacity *= 2u;
    }
    resized = (uint8_t*)realloc(buffer->data, capacity);
    if (resized == NULL) return 0;
    buffer->data = resized;
    buffer->capacity = capacity;
    return 1;
}

static int buffer_append(ByteBuffer* buffer, const void* data, size_t size)
{
    if (!buffer_reserve(buffer, size)) return 0;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 1;
}

static int buffer_u8(ByteBuffer* buffer, uint8_t value)
{
    return buffer_append(buffer, &value, 1u);
}

static int buffer_u16(ByteBuffer* buffer, uint16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    return buffer_append(buffer, bytes, sizeof(bytes));
}

static int buffer_u32(ByteBuffer* buffer, uint32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
    return buffer_append(buffer, bytes, sizeof(bytes));
}

static int buffer_f32(ByteBuffer* buffer, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return buffer_u32(buffer, bits);
}

static int buffer_vec3(ByteBuffer* buffer, Vec3 value)
{
    return buffer_f32(buffer, value.x) &&
        buffer_f32(buffer, value.y) &&
        buffer_f32(buffer, value.z);
}

static int serialize_level(const Level* level, ByteBuffer* raw)
{
    uint32_t i;

    if (!buffer_u32(raw, level->element_count)) return 0;
    for (i = 0; i < level->element_count; ++i) {
        const MapElement* element = &level->elements[i];
        if (!buffer_u8(raw, element->present)) return 0;
        if (!element->present) continue;
        if (!buffer_u32(raw, element->definition_id)) return 0;
        if (level->version > 2u &&
            !buffer_u32(raw, element->lightmap_attachment_key)) return 0;
        if (!buffer_vec3(raw, element->vector1) ||
            !buffer_vec3(raw, element->vector2) ||
            !buffer_u8(raw, element->orientation) ||
            !buffer_u32(raw, element->waylist_index)) return 0;
    }

    if (!buffer_u32(raw, level->waylist_count)) return 0;
    for (i = 0; i < level->waylist_count; ++i) {
        const Waylist* waylist = &level->waylists[i];
        uint16_t j;
        if (!buffer_u32(raw, waylist->index) ||
            !buffer_u16(raw, waylist->point_count)) return 0;
        for (j = 0; j < waylist->point_count; ++j) {
            if (!buffer_vec3(raw, waylist->points[j])) return 0;
        }
    }
    return 1;
}

static int compress_literal_type1(const uint8_t* raw, size_t raw_size,
    ByteBuffer* compressed)
{
    size_t position = 0;

    while (position < raw_size) {
        size_t length = raw_size - position;
        uint8_t control;
        if (length > 32u) length = 32u;
        control = (uint8_t)(length - 1u);
        if (!buffer_u8(compressed, control) ||
            !buffer_append(compressed, raw + position, length)) return 0;
        position += length;
    }
    return raw_size != 0u;
}

static int write_dat(const char* path, const Level* level,
    const ByteBuffer* trailing)
{
    ByteBuffer raw = { 0 };
    ByteBuffer compressed = { 0 };
    FILE* file = NULL;
    uint8_t header[LVLX_HEADER_SIZE];
    int success = 0;

    if (!serialize_level(level, &raw) || raw.size > UINT32_MAX ||
        !compress_literal_type1(raw.data, raw.size, &compressed) ||
        compressed.size > UINT32_MAX) goto cleanup;

    memcpy(header, "lvlx", 4u);
    header[4] = (uint8_t)level->version;
    header[5] = (uint8_t)raw.size;
    header[6] = (uint8_t)(raw.size >> 8);
    header[7] = (uint8_t)(raw.size >> 16);
    header[8] = (uint8_t)(raw.size >> 24);
    header[9] = (uint8_t)compressed.size;
    header[10] = (uint8_t)(compressed.size >> 8);
    header[11] = (uint8_t)(compressed.size >> 16);
    header[12] = (uint8_t)(compressed.size >> 24);

    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Cannot create %s: %s\n", path, strerror(errno));
        goto cleanup;
    }
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
        fwrite(compressed.data, 1, compressed.size, file) != compressed.size ||
        fwrite(trailing->data, 1, trailing->size, file) != trailing->size) {
        fprintf(stderr, "Cannot write all of %s\n", path);
        goto cleanup;
    }
    if (fclose(file) != 0) {
        file = NULL;
        goto cleanup;
    }
    file = NULL;
    printf("Serialized bytes:   %zu\n", raw.size);
    printf("Literal payload:    %zu\n", compressed.size);
    printf("Trailing bytes:     %zu\n", trailing->size);
    printf("Wrote DAT to %s\n", path);
    success = 1;

cleanup:
    if (file != NULL) fclose(file);
    free(compressed.data);
    free(raw.data);
    return success;
}

static int unpack_command(const char* dat_path, const char* definitions_path,
    const char* json_path)
{
    uint8_t* file_data = NULL;
    uint8_t* decompressed = NULL;
    const uint8_t* payload;
    size_t file_size;
    size_t produced;
    uint32_t decompressed_size;
    uint32_t compressed_size;
    unsigned type = 0;
    unsigned version;
    DefinitionTable definitions = { 0 };
    Level level = { 0 };
    int status = EXIT_FAILURE;

    file_data = read_file(dat_path, &file_size);
    if (file_data == NULL)
        goto cleanup;

    if (file_size < LVLX_HEADER_SIZE || memcmp(file_data, "lvlx", 4) != 0) {
        fprintf(stderr, "Input is not a valid LVLX file\n");
        goto cleanup;
    }

    version = file_data[4];
    decompressed_size = read_u32_le(file_data + 5);
    compressed_size = read_u32_le(file_data + 9);

    if ((size_t)compressed_size > file_size - LVLX_HEADER_SIZE ||
        compressed_size == 0 || decompressed_size == 0) {
        fprintf(stderr, "Invalid LVLX size fields\n");
        goto cleanup;
    }

    if ((size_t)compressed_size != file_size - LVLX_HEADER_SIZE) {
        fprintf(stderr,
            "Warning: ignoring %zu trailing byte(s) after the payload\n",
            file_size - LVLX_HEADER_SIZE - (size_t)compressed_size);
    }

    decompressed = (uint8_t*)malloc(decompressed_size);
    if (decompressed == NULL)
        goto cleanup;

    payload = file_data + LVLX_HEADER_SIZE;
    produced = decompress_payload(payload, compressed_size, decompressed,
        decompressed_size, &type);
    if (produced != decompressed_size) {
        fprintf(stderr,
            "Decompression failed: produced %zu of %" PRIu32 " bytes\n",
            produced, decompressed_size);
        goto cleanup;
    }

    if (!load_definitions(definitions_path, &definitions)) {
        fprintf(stderr, "Could not load element definitions\n");
        goto cleanup;
    }

    if (!parse_level(decompressed, produced, version, &level))
        goto cleanup;

    if (!write_level_json(
        json_path, &level, &definitions,
        file_data + LVLX_HEADER_SIZE + compressed_size,
        file_size - LVLX_HEADER_SIZE - compressed_size))
        goto cleanup;

    printf("LVLX version:       %u\n", version);
    printf("Compression type:   %u\n", type);
    printf("Elements:           %" PRIu32 "\n", level.element_count);
    printf("Waylists:           %" PRIu32 "\n", level.waylist_count);
    printf("Wrote JSON to %s\n", json_path);
    status = EXIT_SUCCESS;

cleanup:
    free_level(&level);
    free_definitions(&definitions);
    free(decompressed);
    free(file_data);
    return status;
}

static int pack_command(const char* json_path, const char* dat_path)
{
    Level level = { 0 };
    ByteBuffer trailing = { 0 };
    int status = EXIT_FAILURE;

    if (!load_level_json(json_path, &level, &trailing))
        goto cleanup;
    if (!write_dat(dat_path, &level, &trailing))
        goto cleanup;
    status = EXIT_SUCCESS;

cleanup:
    free(trailing.data);
    free_level(&level);
    return status;
}

int lvlx_load_level(const char* dat_path, LvlxLevel* result)
{
    uint8_t* file_data = NULL;
    uint8_t* decompressed = NULL;
    size_t file_size;
    size_t produced;
    size_t trailing_size;
    uint32_t decompressed_size;
    uint32_t compressed_size;
    unsigned type = 0;
    int success = 0;

    if (dat_path == NULL || result == NULL)
        return 0;
    memset(result, 0, sizeof(*result));

    file_data = read_file(dat_path, &file_size);
    if (file_data == NULL || file_size < LVLX_HEADER_SIZE ||
        memcmp(file_data, "lvlx", 4u) != 0)
        goto cleanup;

    result->version = file_data[4];
    decompressed_size = read_u32_le(file_data + 5u);
    compressed_size = read_u32_le(file_data + 9u);
    if (compressed_size == 0 || decompressed_size == 0 ||
        (size_t)compressed_size > file_size - LVLX_HEADER_SIZE)
        goto cleanup;

    decompressed = (uint8_t*)malloc(decompressed_size);
    if (decompressed == NULL)
        goto cleanup;

    produced = decompress_payload(file_data + LVLX_HEADER_SIZE,
        compressed_size, decompressed,
        decompressed_size, &type);
    if (produced != decompressed_size ||
        !parse_level(decompressed, produced, result->version, result))
        goto cleanup;

    trailing_size = file_size - LVLX_HEADER_SIZE - compressed_size;
    if (trailing_size != 0u) {
        result->trailing_data = (uint8_t*)malloc(trailing_size);
        if (result->trailing_data == NULL)
            goto cleanup;
        memcpy(result->trailing_data,
            file_data + LVLX_HEADER_SIZE + compressed_size,
            trailing_size);
        result->trailing_size = trailing_size;
    }
    if (trailing_size == 24u) {
        const uint8_t* spawn = result->trailing_data;
        result->has_spawn_data = 1u;
        result->level_score_multiplier = read_u32_le(spawn + 0u);
        result->level_time_limit_seconds = read_u32_le(spawn + 4u);
        result->spawn_position.x = read_f32_le(spawn + 8u);
        result->spawn_position.y = read_f32_le(spawn + 12u);
        result->spawn_position.z = read_f32_le(spawn + 16u);
        result->initial_camera_heading_degrees = read_f32_le(spawn + 20u);
    }

    success = 1;

cleanup:
    if (!success)
        free_level(result);
    free(decompressed);
    free(file_data);
    return success;
}

int lvlx_save_level(const char* dat_path, const LvlxLevel* level)
{
    ByteBuffer trailing;

    if (dat_path == NULL || level == NULL)
        return 0;
    trailing.data = level->trailing_data;
    trailing.size = level->trailing_size;
    trailing.capacity = level->trailing_size;
    return write_dat(dat_path, level, &trailing);
}

void lvlx_free_level(LvlxLevel* level)
{
    if (level != NULL)
        free_level(level);
}

int lvlx_load_definitions(const char* elements_path,
    LvlxDefinitionTable* result)
{
    if (elements_path == NULL || result == NULL)
        return 0;
    memset(result, 0, sizeof(*result));
    if (!load_definitions(elements_path, result)) {
        free_definitions(result);
        return 0;
    }
    return 1;
}

void lvlx_free_definitions(LvlxDefinitionTable* definitions)
{
    if (definitions != NULL)
        free_definitions(definitions);
}

const LvlxElementDefinition* lvlx_find_definition(
    const LvlxDefinitionTable* definitions,
    uint32_t definition_id)
{
    if (definitions == NULL ||
        (size_t)definition_id >= definitions->count ||
        definitions->items[definition_id].name == NULL)
        return NULL;
    return &definitions->items[definition_id];
}

LvlxElement* lvlx_get_element(LvlxLevel* level, uint32_t element_index)
{
    if (level == NULL || element_index >= level->element_count)
        return NULL;
    return &level->elements[element_index];
}

const LvlxElement* lvlx_get_element_const(
    const LvlxLevel* level, uint32_t element_index)
{
    if (level == NULL || element_index >= level->element_count)
        return NULL;
    return &level->elements[element_index];
}

int lvlx_add_element(LvlxLevel* level,
    const LvlxElement* element, uint32_t* element_index_out)
{
    LvlxElement value;
    uint32_t i;

    if (level == NULL || element == NULL)
        return 0;

    value = *element;
    value.present = 1u;

    /* Keep existing indices stable by filling deleted/unused slots first. */
    for (i = 0; i < level->element_count; ++i) {
        if (!level->elements[i].present) {
            level->elements[i] = value;
            if (element_index_out != NULL)
                *element_index_out = i;
            return 1;
        }
    }

    if (level->element_count == UINT32_MAX ||
        (size_t)level->element_count + 1u > SIZE_MAX / sizeof(*level->elements))
        return 0;

    {
        size_t new_count = (size_t)level->element_count + 1u;
        LvlxElement* resized = (LvlxElement*)realloc(
            level->elements, new_count * sizeof(*level->elements));
        if (resized == NULL)
            return 0;
        level->elements = resized;
    }

    i = level->element_count;
    level->elements[i] = value;
    level->element_count++;
    if (element_index_out != NULL)
        *element_index_out = i;
    return 1;
}

int lvlx_remove_element(LvlxLevel* level, uint32_t element_index)
{
    LvlxElement* element = lvlx_get_element(level, element_index);

    if (element == NULL || !element->present)
        return 0;

    memset(element, 0, sizeof(*element));
    return 1;
}

int lvlx_set_element_transform(LvlxLevel* level, uint32_t element_index,
    LvlxVec3 vector1, LvlxVec3 vector2, uint8_t orientation)
{
    LvlxElement* element = lvlx_get_element(level, element_index);

    if (element == NULL || !element->present)
        return 0;

    element->vector1 = vector1;
    element->vector2 = vector2;
    element->orientation = orientation;
    return 1;
}

int lvlx_set_spawn_transform(LvlxLevel* level, LvlxVec3 position,
    float initial_camera_heading_degrees)
{
    uint8_t* spawn;
    if (level == NULL || !level->has_spawn_data || level->trailing_size < 24u ||
        level->trailing_data == NULL)
        return 0;

    level->spawn_position = position;
    level->initial_camera_heading_degrees = initial_camera_heading_degrees;
    spawn = level->trailing_data + level->trailing_size - 24u;
    write_f32_le(spawn + 8u, position.x);
    write_f32_le(spawn + 12u, position.y);
    write_f32_le(spawn + 16u, position.z);
    write_f32_le(spawn + 20u, initial_camera_heading_degrees);
    return 1;
}

int lvlx_set_level_properties(LvlxLevel* level, uint32_t score_multiplier,
    uint32_t time_limit_seconds, float initial_camera_heading_degrees)
{
    uint8_t* trailer;
    if (level == NULL || !level->has_spawn_data || level->trailing_size < 24u ||
        level->trailing_data == NULL)
        return 0;
    level->level_score_multiplier = score_multiplier;
    level->level_time_limit_seconds = time_limit_seconds;
    level->initial_camera_heading_degrees = initial_camera_heading_degrees;
    trailer = level->trailing_data + level->trailing_size - 24u;
    write_u32_le(trailer + 0u, score_multiplier);
    write_u32_le(trailer + 4u, time_limit_seconds);
    write_f32_le(trailer + 20u, initial_camera_heading_degrees);
    return 1;
}

int lvlx_export_json(const char* dat_path,
    const char* elements_path,
    const char* json_path)
{
    if (dat_path == NULL || elements_path == NULL || json_path == NULL)
        return 0;
    return unpack_command(dat_path, elements_path, json_path) == EXIT_SUCCESS;
}

int lvlx_import_json(const char* json_path, const char* dat_path)
{
    if (json_path == NULL || dat_path == NULL)
        return 0;
    return pack_command(json_path, dat_path) == EXIT_SUCCESS;
}
