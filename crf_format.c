#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "crf_format.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CRF_MAGIC 0x636e6b66u /* "fknc" */
#define CRF_MESHFILE_CHUNK 1u
#define CRF_RENDER_VERSION 8u
#define CRF_POSITION_FORMAT 0x0c018021u
#define CRF_SKY_VERTEX_FORMAT 0x00020801u
#define CRF_SECOND_UV_FORMAT 0x00080000u

typedef struct Reader {
    const uint8_t* data;
    size_t size;
    size_t position;
    int failed;
} Reader;

static int reader_has(const Reader* r, size_t amount)
{
    return !r->failed && r->position <= r->size &&
        amount <= r->size - r->position;
}

static uint8_t read_u8(Reader* r)
{
    if (!reader_has(r, 1)) { r->failed = 1; return 0; }
    return r->data[r->position++];
}

static uint16_t read_u16(Reader* r)
{
    uint16_t v;
    if (!reader_has(r, 2)) { r->failed = 1; return 0; }
    v = (uint16_t)r->data[r->position] |
        (uint16_t)((uint16_t)r->data[r->position + 1] << 8);
    r->position += 2;
    return v;
}

static uint32_t read_u32(Reader* r)
{
    uint32_t v;
    if (!reader_has(r, 4)) { r->failed = 1; return 0; }
    v = (uint32_t)r->data[r->position] |
        ((uint32_t)r->data[r->position + 1] << 8) |
        ((uint32_t)r->data[r->position + 2] << 16) |
        ((uint32_t)r->data[r->position + 3] << 24);
    r->position += 4;
    return v;
}

static float read_f32(Reader* r)
{
    uint32_t bits = read_u32(r);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int skip_bytes(Reader* r, size_t amount)
{
    if (!reader_has(r, amount)) { r->failed = 1; return 0; }
    r->position += amount;
    return 1;
}

static uint8_t* read_entire_file(const char* path, size_t* size_out)
{
    FILE* file = NULL;
    long length;
    uint8_t* bytes;

#ifdef _MSC_VER
    if (fopen_s(&file, path, "rb") != 0) file = NULL;
#else
    file = fopen(path, "rb");
#endif
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = (uint8_t*)malloc((size_t)length);
    if (bytes == NULL || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size_out = (size_t)length;
    return bytes;
}

static uint16_t read_u16_ptr(const uint8_t* p)
{
    return (uint16_t)p[0] |
        (uint16_t)((uint16_t)p[1] << 8);
}

static float decode_material_uv16(uint16_t raw)
{
    /* The vertex declaration exposes SHORT2N in [-1, 1], but the original
       material shader subsequently maps the base coordinate into its
       texture-space range. Until those per-material constants are carried by
       this loader, reproduce the default shader result rather than passing
       raw vertex-input values directly to raylib. */
    return ((float)(int16_t)raw + 32768.0f) / 65535.0f;
}

static void generate_normals(CrfMesh* mesh)
{
    uint32_t i;
    memset(mesh->normals, 0, (size_t)mesh->vertex_count * 3u * sizeof(float));

    for (i = 0; i < mesh->triangle_count; ++i) {
        uint16_t ia = mesh->indices[i * 3u + 0u];
        uint16_t ib = mesh->indices[i * 3u + 1u];
        uint16_t ic = mesh->indices[i * 3u + 2u];
        const float* a;
        const float* b;
        const float* c;
        float abx, aby, abz, acx, acy, acz, nx, ny, nz;
        uint16_t ids[3];
        int k;

        if (ia >= mesh->vertex_count || ib >= mesh->vertex_count ||
            ic >= mesh->vertex_count) continue;
        a = mesh->positions + (size_t)ia * 3u;
        b = mesh->positions + (size_t)ib * 3u;
        c = mesh->positions + (size_t)ic * 3u;
        abx = b[0] - a[0]; aby = b[1] - a[1]; abz = b[2] - a[2];
        acx = c[0] - a[0]; acy = c[1] - a[1]; acz = c[2] - a[2];
        nx = aby * acz - abz * acy;
        ny = abz * acx - abx * acz;
        nz = abx * acy - aby * acx;
        ids[0] = ia; ids[1] = ib; ids[2] = ic;
        for (k = 0; k < 3; ++k) {
            float* n = mesh->normals + (size_t)ids[k] * 3u;
            n[0] += nx; n[1] += ny; n[2] += nz;
        }
    }

    for (i = 0; i < mesh->vertex_count; ++i) {
        float* n = mesh->normals + (size_t)i * 3u;
        float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length > 0.000001f) {
            n[0] /= length; n[1] /= length; n[2] /= length;
        }
        else {
            n[1] = 1.0f;
        }
    }
}

static int parse_submesh(Reader* r, CrfMesh* mesh)
{
    int has_positions = 0, has_normals = 0;
    uint32_t stream_count;
    uint32_t stream_index;
    uint32_t index_count;
    uint32_t i;

    mesh->vertex_count = read_u32(r);
    mesh->triangle_count = read_u32(r);

    if (mesh->vertex_count == 0 ||
        mesh->vertex_count > 65535u ||
        mesh->triangle_count > UINT32_MAX / 3u) {
        return 0;
    }

    index_count = mesh->triangle_count * 3u;

    mesh->indices = (uint16_t*)malloc(
        (size_t)index_count * sizeof(uint16_t)
    );

    mesh->positions = (float*)calloc(
        (size_t)mesh->vertex_count * 3u,
        sizeof(float)
    );

    mesh->normals = (float*)calloc(
        (size_t)mesh->vertex_count * 3u,
        sizeof(float)
    );

    mesh->texcoords = (float*)calloc(
        (size_t)mesh->vertex_count * 2u,
        sizeof(float)
    );

    mesh->packed_uv_a = (float*)calloc(
        (size_t)mesh->vertex_count * 2u,
        sizeof(float)
    );

    mesh->packed_uv_b = (float*)calloc(
        (size_t)mesh->vertex_count * 2u,
        sizeof(float)
    );

    if (!mesh->indices ||
        !mesh->positions ||
        !mesh->normals ||
        !mesh->texcoords ||
        !mesh->packed_uv_a ||
        !mesh->packed_uv_b)
    {
        return 0;
    }

    // ---------------------------------------------------------
    // Triangle indices
    // ---------------------------------------------------------

    for (i = 0; i < index_count; ++i) {
        mesh->indices[i] = read_u16(r);
        if (mesh->indices[i] >= mesh->vertex_count) return 0;
    }

    if (r->failed) {
        return 0;
    }

    // ---------------------------------------------------------
    // Vertex streams
    // ---------------------------------------------------------

    stream_count = read_u8(r);

    if (r->failed ||
        stream_count == 0 ||
        stream_count > 16u) {
        return 0;
    }

    for (stream_index = 0;
        stream_index < stream_count;
        ++stream_index) {

        uint32_t format = read_u32(r);
        uint32_t stride = read_u32(r);

        printf(
            "CRF stream %u/%u: format=0x%08X stride=%u vertices=%u\n",
            stream_index + 1,
            stream_count,
            format,
            stride,
            mesh->vertex_count
        );

        size_t byte_count;
        const uint8_t* stream;

        if (stride == 0 ||
            stride > 1024u ||
            mesh->vertex_count > SIZE_MAX / stride) {
            return 0;
        }

        byte_count =
            (size_t)mesh->vertex_count * stride;

        if (!reader_has(r, byte_count)) {
            return 0;
        }

        stream = r->data + r->position;

        // -----------------------------------------------------
        // Position stream
        // -----------------------------------------------------

        if (format == CRF_POSITION_FORMAT && stride >= 32u)
        {
            mesh->compressed_material_uv = 1;
            has_positions = has_normals = 1;
            for (i = 0; i < mesh->vertex_count; ++i)
            {
                const uint8_t* vertex =
                    stream + (size_t)i * stride;

                Reader vr = {
                    vertex,
                    stride,
                    0,
                    0
                };

                mesh->positions[i * 3u + 0u] =
                    read_f32(&vr);

                mesh->positions[i * 3u + 1u] =
                    read_f32(&vr);

                mesh->positions[i * 3u + 2u] =
                    read_f32(&vr);

                // 32-byte vertex: Pos[3], Tng[4], Nrm[4], UV[2][2], BinNrm[4].
                mesh->normals[i * 3u + 0u] =
                    ((float)vertex[16] - 128.0f) / 127.0f;

                mesh->normals[i * 3u + 1u] =
                    ((float)vertex[17] - 128.0f) / 127.0f;

                mesh->normals[i * 3u + 2u] =
                    ((float)vertex[18] - 128.0f) / 127.0f;

                // Serialized as D3DDECLTYPE_SHORT2N; decode to the material's
                // default texture-space range for the editor renderer.
                const uint16_t a0 =
                    read_u16_ptr(vertex + 20);

                const uint16_t a1 =
                    read_u16_ptr(vertex + 22);

                mesh->packed_uv_a[i * 2u + 0u] = decode_material_uv16(a0);
                mesh->packed_uv_a[i * 2u + 1u] = decode_material_uv16(a1);

                /*
                 * CCompressed32ByteVertex stores the first (diffuse/color)
                 * UV channel as two little-endian signed-normalized int16 values at offsets
                 * 20 and 22.  This is the channel used by the *_c.dds texture.
                 *
                 * The separate 0x00080000 stream is C2ndUVVertex and must not
                 * replace these coordinates; it is intended for secondary
                 * material data such as lightmaps.
                 */
                mesh->texcoords[i * 2u + 0u] =
                    mesh->packed_uv_a[i * 2u + 0u];

                mesh->texcoords[i * 2u + 1u] =
                    mesh->packed_uv_a[i * 2u + 1u];

                // Overlay UV uses the same signed-normalized representation.
                const uint16_t b0 =
                    read_u16_ptr(vertex + 24);

                const uint16_t b1 =
                    read_u16_ptr(vertex + 26);

                mesh->packed_uv_b[i * 2u + 0u] = decode_material_uv16(b0);

                mesh->packed_uv_b[i * 2u + 1u] = decode_material_uv16(b1);
            }
        }

        else if (format == CRF_SKY_VERTEX_FORMAT && stride >= 32u)
        {
            has_positions = has_normals = 1;
            /* sky_01 uses the uncompressed 32-byte layout:
               vec3f position, vec3f normal, vec2f texture coordinate. */
            for (i = 0; i < mesh->vertex_count; ++i)
            {
                const uint8_t* vertex = stream + (size_t)i * stride;
                Reader vr = { vertex, stride, 0, 0 };

                mesh->positions[i * 3u + 0u] = read_f32(&vr);
                mesh->positions[i * 3u + 1u] = read_f32(&vr);
                mesh->positions[i * 3u + 2u] = read_f32(&vr);
                mesh->normals[i * 3u + 0u] = read_f32(&vr);
                mesh->normals[i * 3u + 1u] = read_f32(&vr);
                mesh->normals[i * 3u + 2u] = read_f32(&vr);
                mesh->texcoords[i * 2u + 0u] = read_f32(&vr);
                mesh->texcoords[i * 2u + 1u] = read_f32(&vr);
                mesh->packed_uv_a[i * 2u + 0u] = mesh->texcoords[i * 2u + 0u];
                mesh->packed_uv_a[i * 2u + 1u] = mesh->texcoords[i * 2u + 1u];
                mesh->packed_uv_b[i * 2u + 0u] = mesh->texcoords[i * 2u + 0u];
                mesh->packed_uv_b[i * 2u + 1u] = mesh->texcoords[i * 2u + 1u];
            }
        }

        // CONFIRMED declarations: rendering.md, Non-lightmapped stream evidence.
        else if ((format == 0x00000001u && stride == 12u) ||
            (format == 0x00000801u && stride == 24u) ||
            (format == 0x00020001u && stride == 20u) ||
            (format == 0x04000001u && stride == 16u)) {
            has_positions = 1;
            has_normals = format == 0x00000801u;
            for (i = 0; i < mesh->vertex_count; ++i) {
                Reader vertex = { stream + (size_t)i*stride, stride, 0, 0 };
                uint32_t component;
                for (component = 0; component < 3; ++component)
                    mesh->positions[i*3u+component] = read_f32(&vertex);
                if (has_normals) {
                    for (component = 0; component < 3; ++component)
                        mesh->normals[i*3u+component] = read_f32(&vertex);
                }
                if (format == 0x00020001u || format == 0x04000001u) {
                    for (component = 0; component < 2; ++component) {
                        float uv;
                        if (format == 0x00020001u) uv = read_f32(&vertex);
                        else { // D3D SHORT2N: -32768 and -32767 both map to -1.
                            uv = fmaxf(-1.0f, (float)(int16_t)read_u16(&vertex)/32767.0f);
                        }
                        mesh->texcoords[i*2u+component] = uv;
                        mesh->packed_uv_a[i*2u+component] = uv;
                        mesh->packed_uv_b[i*2u+component] = uv;
                    }
                }
            }
        }

        else if ((format == 12u && stride == 8u) || (format == 8u && stride == 4u)) {
            if (mesh->skin_indices) { r->failed = 1; return 0; }
            mesh->skin_indices = (uint8_t*)malloc((size_t)mesh->vertex_count * 4u);
            if (format == 12u)
                mesh->skin_weights = (uint8_t*)malloc((size_t)mesh->vertex_count * 4u);
            if (!mesh->skin_indices || (format == 12u && !mesh->skin_weights)) return 0;
            for (i = 0; i < mesh->vertex_count; ++i) {
                if (mesh->skin_weights) memcpy(mesh->skin_weights + i*4u, stream + i*stride, 4u);
                memcpy(mesh->skin_indices + i*4u, stream + i*stride + (format == 12u ? 4u : 0u), 4u);
            }
        }
        else if (format == CRF_SECOND_UV_FORMAT &&
            stride >= 8u) {
            free(mesh->second_stream_uv);
            mesh->second_stream_uv = (float*)malloc(
                (size_t)mesh->vertex_count * 2u * sizeof(float));
            if (!mesh->second_stream_uv) return 0;
            for (i = 0; i < mesh->vertex_count; ++i) {
                const uint8_t* vertex = stream + (size_t)i * stride;
                Reader uv = { vertex, stride, 0, 0 };
                mesh->second_stream_uv[i * 2u] = read_f32(&uv);
                mesh->second_stream_uv[i * 2u + 1u] = read_f32(&uv);
            }
            printf("  -> C2ndUVVertex/lightmap UV\n");
        }
        else {
            printf("  -> UNKNOWN\n");

        }

        // Advance past this vertex stream.
        r->position += byte_count;
    }

    // ---------------------------------------------------------
    // IMPORTANT:
    // Bounding box: min XYZ + max XYZ = 6 floats = 24 bytes.
    //
    // Without this, parse_material_block() starts 24 bytes
    // too early and no texture names are found.
    // ---------------------------------------------------------

    if (!skip_bytes(r, 6u * sizeof(float)) ||
        r->failed) {
        return 0;
    }

    if (!has_positions) return 0;
    if (!has_normals) generate_normals(mesh); // Preview geometry fallback only.
    else {
        /* Some shipped compressed streams contain (128,128,128), which decodes
           to a zero vector. Such vertices cannot receive directional lighting.
           Repair only missing/invalid normals using the existing triangle winding;
           keep every valid authored normal (and all resource bytes) unchanged. */
        int needs_repair = 0;
        float* authored = mesh->normals;
        for (i = 0; i < mesh->vertex_count; ++i) {
            const float* n = authored + i*3u;
            float length2 = n[0]*n[0] + n[1]*n[1] + n[2]*n[2];
            if (!isfinite(length2) || length2 < 1e-12f) { needs_repair = 1; break; }
        }
        if (needs_repair) {
            float* repaired = (float*)calloc((size_t)mesh->vertex_count*3u, sizeof(float));
            if (!repaired) return 0;
            mesh->normals = repaired;
            generate_normals(mesh);
            for (i = 0; i < mesh->vertex_count; ++i) {
                const float* n = authored + i*3u;
                float length2 = n[0]*n[0] + n[1]*n[1] + n[2]*n[2];
                if (isfinite(length2) && length2 >= 1e-12f)
                    memcpy(repaired+i*3u, n, 3u*sizeof(float));
            }
            free(authored);
        }
    }
    return !r->failed;
}

static void copy_texture_name(char destination[260], const uint8_t* source,
    uint32_t length)
{
    size_t amount = length < 259u ? (size_t)length : 259u;
    memcpy(destination, source, amount);
    destination[amount] = '\0';
}

static void parse_material_block(Reader* r, CrfMesh* mesh)
{
    uint32_t count, i, version;
    if (!reader_has(r, 10) || r->data[r->position] != 's' ||
        r->data[r->position + 1] != 'm') return;
    mesh->mp_material = 1;
    mesh->rim_start = 0.5f;
    mesh->rim_end = 1.0f;
    r->position += 2;
    version = read_u32(r);
    count = read_u32(r);
    if (count > 64u) { r->failed = 1; return; }

    for (i = 0; i < count; ++i) {
        char semantic[5];
        uint32_t length;
        if (!reader_has(r, 8)) { r->failed = 1; return; }
        memcpy(semantic, r->data + r->position, 4);
        semantic[4] = '\0';
        r->position += 4;
        length = read_u32(r);
        if (!reader_has(r, length)) { r->failed = 1; return; }
        if (memcmp(semantic, "sffd", 4) == 0)
        {
            copy_texture_name(
                mesh->color_texture,
                r->data + r->position,
                length
            );
        }
        else if (memcmp(semantic, "lcps", 4) == 0)
        {
            copy_texture_name(
                mesh->auxiliary_texture,
                r->data + r->position,
                length
            );
        }
        else if (memcmp(semantic, "1tsc", 4) == 0)
        {
            copy_texture_name(
                mesh->overlay_color_texture,
                r->data + r->position,
                length
            );
        }
        else if (memcmp(semantic, "2tsc", 4) == 0)
        {
            copy_texture_name(
                mesh->overlay_auxiliary_texture,
                r->data + r->position,
                length
            );
        }
        else if (memcmp(semantic, "1txs", 4) == 0)
        {
            /* Reversed FourCC "sxt1": the visible texture used by sky_01. */
            copy_texture_name(
                mesh->sky_texture,
                r->data + r->position,
                length
            );
        }
        r->position += length;
        /* Each material entry ends with an unused/variant index. */
        if (!skip_bytes(r, 4u)) return;
    }
    /* CONFIRMED 004eb5a0/004eba70: vector properties (tag + float3),
       scalar properties (tag + float), then a dword array. Consume all of
       them before the flags and the owning mesh's subset identifier. */
    {
        const size_t strides[3] = {16u, 8u, 4u};
        unsigned table;
        for (table = 0; table < 3; ++table) {
            count = read_u32(r);
            if (r->failed || count > (r->size - r->position) / strides[table]) {
                r->failed = 1;
                return;
            }
            for (i = 0; i < count; ++i) {
                uint32_t tag = read_u32(r);
                if (table == 0) {
                    float value[3];
                    unsigned component;
                    for (component = 0; component < 3; ++component)
                        value[component] = read_f32(r);
                    if (tag == 0x63737432u)
                        memcpy(mesh->rim_color, value, sizeof(value));
                } else if (table == 1) {
                    float value = read_f32(r);
                    if (tag == 0x63737432u) mesh->rim_start = value;
                    if (tag == 0x63737433u) mesh->rim_end = value;
                }
            }
        }
    }
    mesh->material_flags = version ? read_u32(r) : 0;
    if (r->failed) return;
    /* MPDiffuseSpecular bit 1 changes the base UV transform coefficients
       from 0.5 to 3.0 (004a6f80, 00645ee0/00645f00). The existing decoded
       coordinates already include the default transform, hence factor six.
       Overlay and independent lightmap UV streams retain their own mapping. */
    if (mesh->compressed_material_uv && (mesh->material_flags & 2u)) {
        for (i = 0; i < mesh->vertex_count * 2u; ++i) {
            mesh->packed_uv_a[i] *= 6.0f;
            mesh->texcoords[i] = mesh->packed_uv_a[i];
        }
    }
}

static int looks_like_submesh(const Reader* source, size_t position)
{
    Reader r = *source;
    uint32_t vertex_count, triangle_count, stream_count, i;
    int has_positions = 0;
    r.position = position;
    r.failed = 0;
    vertex_count = read_u32(&r);
    triangle_count = read_u32(&r);
    if (vertex_count == 0 || vertex_count > 65535u || triangle_count == 0 ||
        triangle_count > UINT32_MAX / 6u ||
        !skip_bytes(&r, (size_t)triangle_count * 6u)) return 0;
    stream_count = read_u8(&r);
    if (stream_count == 0 || stream_count > 16u) return 0;
    for (i = 0; i < stream_count; ++i) {
        uint32_t format = read_u32(&r);
        uint32_t stride = read_u32(&r);
        size_t bytes;
        if (stride == 0 || stride > 1024u || vertex_count > SIZE_MAX / stride)
            return 0;
        if (((format == CRF_POSITION_FORMAT || format == CRF_SKY_VERTEX_FORMAT) && stride >= 32u) ||
            (format == 0x00000001u && stride == 12u) ||
            (format == 0x00000801u && stride == 24u) ||
            (format == 0x00020001u && stride == 20u) ||
            (format == 0x04000001u && stride == 16u))
        {
            has_positions = 1;
        }
        bytes = (size_t)vertex_count * stride;
        if (!skip_bytes(&r, bytes)) return 0;
    }
    return has_positions && reader_has(&r, 24u);
}

static int seek_next_submesh(Reader* r)
{
    size_t position;
    for (position = r->position; position + 16u <= r->size; ++position) {
        if (looks_like_submesh(r, position)) {
            r->position = position;
            return 1;
        }
    }
    r->failed = 1;
    return 0;
}

static int parse_meshfile(const uint8_t* data, size_t size,
    CrfModelData* model)
{
    Reader r = { data, size, 0, 0 };
    uint32_t encoded = read_u32(&r);
    uint32_t version = encoded & 0x3fffu;
    uint32_t mesh_count = (encoded & 0xffff0000u) ? read_u32(&r) : encoded;
    uint32_t i;

    if (r.failed || version != CRF_RENDER_VERSION || mesh_count == 0 ||
        mesh_count > 1024u) return 0;
    if (!skip_bytes(&r, 6u * sizeof(float))) return 0;

    model->meshes = (CrfMesh*)calloc(mesh_count, sizeof(CrfMesh));
    if (!model->meshes) return 0;
    model->mesh_count = mesh_count;
    for (i = 0; i < mesh_count; ++i) {
        if (i != 0 && !seek_next_submesh(&r)) return 0;
        if (!parse_submesh(&r, &model->meshes[i])) return 0;
        parse_material_block(&r, &model->meshes[i]);
        if (r.failed) return 0;
        model->meshes[i].subset_identifier = version >= 4u ? read_u32(&r) : i;
    }
    return !r.failed;
}

int crf_load(const char* path, CrfModelData* result)
{
    uint8_t* file_data;
    size_t file_size;
    Reader r;
    uint32_t version, directory_offset, metadata_offset, chunk_count;
    uint32_t i;
    int success = 0;

    if (!result) return 0;
    memset(result, 0, sizeof(*result));
    file_data = read_entire_file(path, &file_size);
    if (!file_data) return 0;
    r.data = file_data; r.size = file_size; r.position = 0; r.failed = 0;

    if (read_u32(&r) != CRF_MAGIC || (version = read_u32(&r)) != 1u) goto done;
    directory_offset = read_u32(&r);
    metadata_offset = read_u32(&r);
    chunk_count = read_u32(&r);
    (void)metadata_offset;
    if (chunk_count == 0 || chunk_count > 65536u ||
        directory_offset > file_size ||
        (size_t)chunk_count > (file_size - directory_offset) / 32u) goto done;

    r.position = directory_offset;
    for (i = 0; i < chunk_count; ++i) {
        uint32_t id = read_u32(&r);
        uint32_t type = read_u32(&r);
        uint32_t offset = read_u32(&r);
        uint32_t size = read_u32(&r);
        (void)id;
        if (!skip_bytes(&r, 16)) goto done;
        if (type == CRF_MESHFILE_CHUNK && offset <= file_size &&
            size <= file_size - offset) {
            success = parse_meshfile(file_data + offset, size, result);
            break;
        }
    }

done:
    free(file_data);
    if (!success) crf_free(result);
    return success;
}

void crf_free(CrfModelData* model)
{
    uint32_t i;
    if (!model) return;
    for (i = 0; i < model->mesh_count; ++i) {
        free(model->meshes[i].positions);
        free(model->meshes[i].normals);
        free(model->meshes[i].texcoords);
        free(model->meshes[i].indices);
        free(model->meshes[i].packed_uv_a);
        free(model->meshes[i].packed_uv_b);
        free(model->meshes[i].second_stream_uv);
        free(model->meshes[i].skin_weights);
        free(model->meshes[i].skin_indices);
    }
    free(model->meshes);
    memset(model, 0, sizeof(*model));
}
