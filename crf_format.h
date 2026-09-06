#ifndef CRF_FORMAT_H
#define CRF_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct CrfMesh {
        uint32_t subset_identifier;
        uint32_t material_flags;
        int mp_material; /* serialized sm material; recovered shader applies here */
        float rim_start, rim_end, rim_color[3];
        int compressed_material_uv;
        uint32_t vertex_count;
        uint32_t triangle_count;

        float* positions;       /* xyz, vertex_count * 3 */
        float* normals;         /* xyz, vertex_count * 3 */

        /* First UV channel decoded from CCompressed32ByteVertex. This is the
           channel used to display the diffuse/color (*_c.dds) texture. */
        float* texcoords;       /* vertex_count * 2 */

        float* packed_uv_a;     /* base UV after default material mapping */
        float* packed_uv_b;     /* overlay UV after default material mapping */

        uint16_t* indices;

        char color_texture[260];
        char auxiliary_texture[260];

        char overlay_color_texture[260];
        char overlay_auxiliary_texture[260];
        char sky_texture[260];

        float* second_stream_uv;
        uint8_t* skin_weights; /* four original UNORM8 weights; no renormalization */
        uint8_t* skin_indices; /* four palette-slot bytes (rigid uses the first) */

    } CrfMesh;

    typedef struct CrfModelData {
        CrfMesh* meshes;
        uint32_t mesh_count;
    } CrfModelData;

    int crf_load(const char* path, CrfModelData* result);
    void crf_free(CrfModelData* model);

#ifdef __cplusplus
}
#endif

#endif
