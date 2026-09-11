#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "crf_format.h"
#include "lvlx_format.h"
#include "environment_settings.h"
#include "environment_cube.h"
#include "effect_preview.h"
#include "physics_preview.h"
#include "animation_preview.h"
#include "mod_project.h"
#include "map_picker.h"
#include "keyboard_shortcuts.h"

#include <cfloat>
#include <climits>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

struct RenderAsset {
    bool attempted = false;
    bool loaded = false;
    std::vector<Texture2D> textures;
    std::vector<Model> models;
    std::vector<unsigned char> modelHasOverlay;
    std::vector<float> modelAlphaCutoffs;
    std::vector<int> modelRecoveredMaterial;
    std::vector<std::array<float,5>> modelRimLights;
    animation_preview::Rig animation;
    float animationRate = 1.0f;
    bool animatedPose = false;
    std::vector<uint32_t> subsetIdentifiers;
    std::vector<unsigned int> lightmapUvVboIds;
    std::vector<std::vector<Vector2>> modelLightmapUvs;
};

struct LmdRecord {
    uint32_t attachmentKey = 0;
    uint32_t subsetIdentifier = 0;
    float offsetU = 0, offsetV = 0, scaleU = 0, scaleV = 0;
    uint32_t textureSlot = UINT32_MAX;
};

struct LevelLightmaps {
    bool present = false;
    bool valid = false;
    std::vector<LmdRecord> records;
    std::unordered_map<uint64_t, size_t> recordLookup;
    std::vector<std::vector<unsigned char>> serializedNames;
    std::vector<Texture2D> textures;
    uint64_t lmdFingerprint = 0;
    std::vector<uint64_t> textureFingerprints;
};

struct ElementClipboardItem {
    LvlxElement element{};
    std::vector<LvlxVec3> waypoints;
};

static Shader gCrfShader{};
static Shader gSkyShader{};
static Shader gShadowDepthShader{};
static RenderTexture2D gLiveShadowMap{};
static Matrix gLiveShadowMatrix = MatrixIdentity();
static bool gLiveShadowValid = false;
static bool gLiveShadowEnabled = false;
static int gLightDirectionLoc = -1;
static int gLightColorLoc = -1;
static int gAmbientColorLoc = -1;
static int gOverlayEnabledLoc = -1;
static int gLightmapEnabledLoc = -1;
static int gLightmapTransformLoc = -1;
static int gLightmapUvAttribLoc = -1;
static int gAlphaCutoffLoc = -1;
static environment::Document gEnvironment;
static TextureCubemap gEnvironmentCube{};
static std::string gEnvironmentTextures, gEnvironmentCubePath;
static bool gFogPreview = true;
static bool gDeathGridVisible = true;
static constexpr float kDeathShadowFadeDepth = 4.0f;
static constexpr float kDeathShadowSurfaceOpacity = 0.00f;
// Keep the lightmap implementation available while its unfinished editor
// controls are hidden. Set this true when the bake workflow is ready again.
static constexpr bool kShowLightmapControls = false;
// Preview policy: the original material-to-ambient-slot selector is unresolved.
static int gAmbientPreviewSlot = 0;

static void LoadEnvironment(const std::string& bin, const std::string& level) {
    gEnvironmentTextures = (std::filesystem::path(bin) / "textures").string();
    auto companion = std::filesystem::path(level);
    companion.replace_extension(".txt");
    gEnvironment.load((std::filesystem::path(bin) / "settings/environment_01.txt").string(), companion.string());
    for (const auto& warning : gEnvironment.diagnostics)
        std::fprintf(stderr, "Environment: %s\n", warning.c_str());
}

static bool DefinitionSupportsWaylists(const LvlxElementDefinition* definition);
static bool DefinitionUsesDestination(const LvlxElementDefinition* definition);

static constexpr float kSantaVisualScale = 1.50f;

static bool IsElementSelected(const std::vector<int>& selection, int index);

static Vector3 ToRaylib(LvlxVec3 v) { return Vector3{ v.x, v.y, -v.z }; }
static LvlxVec3 FromRaylib(Vector3 v) { return LvlxVec3{ v.x, v.y, -v.z }; }

static std::string NormalizePath(std::string path) {
    for (char& c : path) if (c == '\\') c = '/';
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

static std::string DirectoryOf(const std::string& path) {
    const std::string p = NormalizePath(path);
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

static std::string FilenameOf(const std::string& path) {
    const std::string p = NormalizePath(path);
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

static std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty() || left == ".") return right;
    return NormalizePath(left) + "/" + right;
}

static std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

static std::string FindBinDirectory(const char* levelPath) {
    const auto shared = mod_project::findBin(levelPath);
    if (!shared.empty()) return NormalizePath(shared.string());
    const std::string levelDirectory = DirectoryOf(levelPath);
    const std::string leaf = FilenameOf(levelDirectory);
    if (leaf == "levels" || leaf == "LEVELS" || leaf == "Levels")
        return DirectoryOf(levelDirectory);
    return levelDirectory;
}

static std::string gFallbackAssetsDirectory,gFallbackTexturesDirectory,gFallbackEffectsDirectory;
static std::string ResolveResourceFile(const std::string& primary,
    const std::string& fallback,const std::string& relative) {
    const std::string candidate=JoinPath(primary,relative);
    if(std::filesystem::is_regular_file(candidate)||fallback.empty())return candidate;
    const std::string inherited=JoinPath(fallback,relative);
    return std::filesystem::is_regular_file(inherited)?inherited:candidate;
}

static std::string FindModDirectory(const std::string& levelPath) {
    namespace fs=std::filesystem;const fs::path level=fs::absolute(levelPath);
    std::string leaf=level.parent_path().filename().string();
    for(char& c:leaf)c=(char)std::tolower((unsigned char)c);
    if(leaf!="levels")return {};
    const fs::path root=level.parent_path().parent_path();
    return fs::is_regular_file(root/"mod.json")?NormalizePath(root.string()):std::string{};
}

static void ConfigureResourceDirectories(const std::string& levelPath,const std::string& bin,
    std::string& mod,std::string& assets,std::string& textures,std::string& effects) {
    mod=FindModDirectory(levelPath);const std::string& primary=mod.empty()?bin:mod;
    assets=JoinPath(primary,"assets");textures=JoinPath(primary,"textures");effects=JoinPath(primary,"effects");
    gFallbackAssetsDirectory=mod.empty()?std::string{}:JoinPath(bin,"assets");
    gFallbackTexturesDirectory=mod.empty()?std::string{}:JoinPath(bin,"textures");
    gFallbackEffectsDirectory=mod.empty()?std::string{}:JoinPath(bin,"effects");
}

static std::string FindModElementDefinitions(const std::string& levelPath) {
    namespace fs = std::filesystem;
    const fs::path level = fs::absolute(levelPath);
    if (Lowercase(level.parent_path().filename().string()) != "levels") return {};
    const fs::path candidate = level.parent_path().parent_path() / "settings" / "elements.txt";
    return fs::is_regular_file(candidate) ? NormalizePath(candidate.string()) : std::string{};
}

static bool PromptModProject(std::string& modId, std::string& levelId, bool blank) {
    std::string input = "MyMod/first_level";
    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_ESCAPE)) return false;
        if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsLetterPressed('A')) input.clear();
        for (int c = GetCharPressed(); c; c = GetCharPressed())
            if (c >= 32 && c < 127 && input.size() < 129) input += (char)c;
        if (IsKeyPressed(KEY_BACKSPACE) && !input.empty()) input.pop_back();
        const auto slash = input.find('/');
        const bool valid = slash != std::string::npos &&
            mod_project::validId(input.substr(0, slash)) && mod_project::validId(input.substr(slash + 1));
        if (valid && IsKeyPressed(KEY_ENTER)) {
            modId = input.substr(0, slash); levelId = input.substr(slash + 1); return true;
        }
        BeginDrawing();
        ClearBackground(Color{23,25,29,255});
        DrawText(blank ? "New mod level" : "Export current level as a mod", 30, 35, 26, RAYWHITE);
        DrawText("Mod ID / level ID (a new mod folder will be created)", 30, 95, 18, LIGHTGRAY);
        DrawText(input.c_str(), 30, 140, 22, valid ? SKYBLUE : ORANGE);
        DrawText("Ctrl+A clears | Backspace edits | Enter creates | Esc cancels", 30, 195, 18, LIGHTGRAY);
        DrawText(blank ? "Empty geometry; keeps current spawn and environment. Add a platform before playing."
                       : "Exports current edits, environment and optional lightmap metadata.", 30, 240, 16, LIGHTGRAY);
        EndDrawing();
    }
    return false;
}

static void print_uv_range(
    const char* texture,
    const char* channel,
    const float* uv,
    uint32_t count)
{
    float minU = FLT_MAX, minV = FLT_MAX;
    float maxU = -FLT_MAX, maxV = -FLT_MAX;

    for (uint32_t i = 0; i < count; ++i) {
        float u = uv[i * 2u];
        float v = uv[i * 2u + 1u];

        if (u < minU) minU = u;
        if (u > maxU) maxU = u;
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
    }

    printf("%s %s: U %.4f..%.4f span %.4f, V %.4f..%.4f span %.4f\n",
        texture, channel,
        minU, maxU, maxU - minU,
        minV, maxV, maxV - minV);
}

static Color CategoryColor(const char* category) {
    if (!category) return MAGENTA;
    if (std::strcmp(category, "Rectform") == 0) return GRAY;
    if (std::strcmp(category, "Decoration") == 0) return GREEN;
    if (std::strcmp(category, "Enemy") == 0) return RED;
    if (std::strcmp(category, "EnemyWaylist") == 0) return ORANGE;
    if (std::strcmp(category, "EnemyElevator") == 0) return MAROON;
    if (std::strcmp(category, "Bonus") == 0) return GOLD;
    return PURPLE;
}

static Vector3 CalculateMapCenter(const LvlxLevel& level) {
    Vector3 minimum{ FLT_MAX, FLT_MAX, FLT_MAX };
    Vector3 maximum{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    bool found = false;
    for (uint32_t i = 0; i < level.element_count; ++i) {
        if (!level.elements[i].present) continue;
        const Vector3 p = ToRaylib(level.elements[i].vector1);
        minimum = Vector3Min(minimum, p);
        maximum = Vector3Max(maximum, p);
        found = true;
    }
    return found ? Vector3Scale(Vector3Add(minimum, maximum), 0.5f)
        : Vector3{ 0, 0, 0 };
}

static void PositionCameraAtLevelStart(Camera3D& camera, const LvlxLevel& level) {
    const Vector3 focus = level.has_spawn_data
        ? ToRaylib(level.spawn_position)
        : CalculateMapCenter(level);
    camera.target = Vector3Add(focus, Vector3{ 0.0f, 1.0f, 0.0f });
    camera.position = Vector3Add(focus, Vector3{ 12.0f, 8.0f, 12.0f });
}

static bool LoadCrfShader()
{
    const char* vertexShaderCode = R"(
#version 330

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec2 vertexTexCoord2;
in vec2 vertexTexCoord3;
in vec3 vertexNormal;
in vec4 vertexColor;

uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
uniform vec4 lightmapTransform;
uniform mat4 lightViewProjection;

out vec2 fragTexCoord;
out vec2 fragOverlayTexCoord;
out vec2 fragLightmapTexCoord;
out vec3 fragNormal;
out vec4 fragColor;
out vec3 fragWorldPosition;
out vec4 fragShadowPosition;

void main()
{
    fragTexCoord = vertexTexCoord;
    fragOverlayTexCoord = vertexTexCoord2;
    fragLightmapTexCoord = vertexTexCoord3 * lightmapTransform.xy + lightmapTransform.zw;

    fragNormal = normalize(
        vec3(matNormal * vec4(vertexNormal, 0.0))
    );

    fragColor = vertexColor;
    fragWorldPosition = vec3(matModel * vec4(vertexPosition, 1.0));
    fragShadowPosition = lightViewProjection * vec4(fragWorldPosition, 1.0);

    gl_Position =
        mvp * vec4(vertexPosition, 1.0);
}
)";

    const char* fragmentShaderCode = R"(
#version 330

in vec2 fragTexCoord;
in vec2 fragOverlayTexCoord;
in vec2 fragLightmapTexCoord;
in vec3 fragNormal;
in vec4 fragColor;
in vec3 fragWorldPosition;
in vec4 fragShadowPosition;

uniform sampler2D texture0;
uniform sampler2D texture1;
uniform sampler2D texture2;
uniform sampler2D specialTexture;
uniform sampler2D overlaySpecialTexture;
uniform int recoveredMaterial;
uniform samplerCube environmentCube;
uniform int environmentCubeEnabled;
uniform vec2 rimRange;
uniform vec3 rimColor;
uniform vec4 colDiffuse;
uniform int overlayEnabled;
uniform int lightmapEnabled;
uniform float alphaCutoff;

uniform vec3 lightDirection;
uniform vec3 lightColor;
uniform vec3 ambientColor;
uniform vec3 ambientBottomColor;
uniform vec3 cameraPosition;
uniform vec3 fogColor;
uniform vec4 fogRanges;
uniform int fogEnabled;
uniform float deathHeight;
uniform float deathFadeDepth;
uniform int deathShadowEnabled;
uniform int effectLightCount;
uniform vec3 effectLightPositions[8];
uniform vec3 effectLightColors[8];
uniform float effectLightRadii[8];
uniform sampler2D shadowMap;
uniform int liveShadowEnabled;
uniform vec2 shadowTexelSize;

out vec4 finalColor;

void main()
{
    vec4 texel =
        texture(texture0, fragTexCoord);

    // Fully transparent cutout texels must not write depth. Otherwise foliage
    // drawn earlier hides later geometry while contributing no visible color,
    // leaving background-colored holes between the branches.
    if (texel.a <= max(1.0 / 255.0, alphaCutoff))
        discard;

    vec4 special = recoveredMaterial != 0 ? texture(specialTexture, fragTexCoord)
        : vec4(0.0, 0.0, 0.0, 1.0);
    if (overlayEnabled != 0)
    {
        vec4 overlayTexel =
            texture(texture1, fragOverlayTexCoord);
        if (recoveredMaterial != 0)
            special = mix(special, texture(overlaySpecialTexture, fragOverlayTexCoord), overlayTexel.a);

        texel.rgb = mix(
            texel.rgb,
            overlayTexel.rgb,
            overlayTexel.a
        );
    }

    vec3 normal =
        normalize(fragNormal);

    vec3 lightDir =
        normalize(-lightDirection);

    float diffuse =
        max(dot(normal, lightDir), 0.0);

    // Explicit preview-selected ambient slot; game slot selection is unresolved.
    // Recovered MPDiffuseSpecular hemisphere equation; slot pairing is preview policy.
    vec3 ambientGradient = mix(ambientBottomColor, ambientColor,
        clamp(0.5 * (normal.y + 1.0), 0.0, 1.0));
    float bakedLight = lightmapEnabled != 0
        ? texture(texture2, fragLightmapTexCoord).g : 1.0;
    float liveShadow = 1.0;
    if (liveShadowEnabled != 0 && fragShadowPosition.w > 0.0) {
        vec3 projected = fragShadowPosition.xyz / fragShadowPosition.w;
        vec2 shadowUv = projected.xy * 0.5 + 0.5;
        float receiverDepth = projected.z * 0.5 + 0.5;
        if (shadowUv.x >= 0.0 && shadowUv.x <= 1.0 &&
            shadowUv.y >= 0.0 && shadowUv.y <= 1.0 &&
            receiverDepth >= 0.0 && receiverDepth <= 1.0) {
            float visibility = 0.0;
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x) {
                    float casterDepth = texture(shadowMap,
                        shadowUv + vec2(x, y) * shadowTexelSize).r;
                    visibility += receiverDepth - 0.0025 <= casterDepth ? 1.0 : 0.0;
                }
            liveShadow = 0.28 + 0.72 * visibility / 9.0;
        }
    }
    // CONFIRMED MPDiffuseSpecular: L scales ambient and k=min(L,1) only
    // scales the first (directional) light. Live shadow visibility is preview policy.
    float directionalVisibility = min(bakedLight, 1.0);
    if (liveShadowEnabled != 0)
        directionalVisibility = min(directionalVisibility, liveShadow);
    vec3 lighting = ambientGradient * bakedLight +
        lightColor * diffuse * directionalVisibility;
    vec3 viewDelta = cameraPosition - fragWorldPosition;
    vec3 viewDir = viewDelta / max(length(viewDelta), 0.0001);
    float exponent = 2.0 + pow(12.0 * special.g, 3.0);
    vec3 halfDelta = lightDir + viewDir;
    vec3 halfDir = halfDelta / max(length(halfDelta), 0.0001);
    vec3 specular = lightColor * directionalVisibility *
        pow(max(dot(normal, halfDir), 0.0), exponent);
    // CONFIRMED FXT +1c=1.25; CContext doubles authored light RGB (0069519c).
    for (int i = 0; i < min(effectLightCount, 7); ++i) {
        vec3 delta = effectLightPositions[i] - fragWorldPosition;
        float radius = max(effectLightRadii[i], 0.0001);
        float range = clamp(1.25 * (1.0 - dot(delta, delta) / (radius * radius)), 0.0, 1.0);
        vec3 q = 2.0 * effectLightColors[i] * range * range * range;
        vec3 direction = delta / max(length(delta), 0.0001);
        lighting += q * max(dot(normal, direction), 0.0);
        vec3 h = direction + viewDir;
        h /= max(length(h), 0.0001);
        // Recovered PS3 computes specular for six lights, diffuse for all eight.
        if (i < 5) specular += q * pow(max(dot(normal, h), 0.0), exponent);
    }

    vec3 materialColor = texel.rgb;
    if (recoveredMaterial != 0 && environmentCubeEnabled != 0) {
        // Mesh/world Z is negated on import; DDS cube faces retain game space.
        vec3 reflected = reflect(-viewDir, normal);
        reflected.z = -reflected.z;
        materialColor = mix(materialColor,
            texture(environmentCube, reflected).rgb, special.b);
    }
    vec3 color =
        materialColor *
        colDiffuse.rgb *
        fragColor.rgb *
        mix(vec3(1.0), lighting, special.a);
    float rimWidth = rimRange.y - rimRange.x;
    float rimAngle = 1.0 - dot(normal, viewDir);
    float rimT = abs(rimWidth) > 0.000001
        ? clamp((rimAngle - rimRange.x) / rimWidth, 0.0, 1.0)
        : step(rimRange.x, rimAngle);
    vec3 rim = recoveredMaterial != 0
        ? rimT * rimT * (3.0 - 2.0 * rimT) * rimColor : vec3(0.0);
    color += max(bakedLight * special.r * specular, rim);

    // Preview policy: linear depth/height fog. Parameter endpoints are confirmed;
    // the original shader's combination formula has not been recovered.
    if (fogEnabled != 0) {
        float depth = clamp((distance(cameraPosition, fragWorldPosition) - fogRanges.x)
            / max(abs(fogRanges.y), 0.0001), 0.0, 1.0);
        float height = 1.0 - clamp((fragWorldPosition.y - fogRanges.z)
            / max(abs(fogRanges.w), 0.0001), 0.0, 1.0);
        color = mix(color, fogColor, max(depth, height));
    }

    // Editor visualization: everything beneath the gameplay death threshold
    // belongs to the black void, including geometry visible from the side.
    if (deathShadowEnabled != 0 && fragWorldPosition.y < deathHeight) {
        float deathFade = smoothstep(0.0, deathFadeDepth,
            deathHeight - fragWorldPosition.y);
        color = mix(color, vec3(0.0), deathFade);
    }

    finalColor = vec4(
        color,
        texel.a *
        colDiffuse.a *
        fragColor.a
    );
}
)";

    gCrfShader = LoadShaderFromMemory(
        vertexShaderCode,
        fragmentShaderCode
    );

    if (gCrfShader.id == 0) {
        TraceLog(
            LOG_ERROR,
            "Could not create CRF shader"
        );

        return false;
    }

    const char* skyVertexShaderCode = R"(
#version 330

in vec3 vertexPosition;
in vec2 vertexTexCoord;

uniform mat4 mvp;

out vec2 fragTexCoord;
out vec3 skyDirection;

void main()
{
    fragTexCoord = vertexTexCoord;
    skyDirection = vertexPosition;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

    const char* skyFragmentShaderCode = R"(
#version 330

in vec2 fragTexCoord;
in vec3 skyDirection;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 skyTop;
uniform vec3 skyBottom;
uniform vec3 sunDirection;
uniform vec3 sunColor0;
uniform vec3 sunColor1;
uniform vec2 sunExponents;
out vec4 finalColor;

void main()
{
    // Approximate visualization of confirmed inputs, not recovered game shader math.
    vec3 direction = normalize(skyDirection);
    vec3 gradient = mix(skyBottom, skyTop, clamp(direction.y * 0.5 + 0.5, 0.0, 1.0));
    float alignment = max(dot(direction, sunDirection), 0.0);
    vec3 sun = sunColor0 * pow(alignment, max(sunExponents.x, 0.0001))
             + sunColor1 * pow(alignment, max(sunExponents.y, 0.0001));
    finalColor = vec4(texture(texture0, fragTexCoord).rgb * gradient + sun, 1.0) * colDiffuse;
}
)";

    gSkyShader = LoadShaderFromMemory(
        skyVertexShaderCode,
        skyFragmentShaderCode
    );

    if (gSkyShader.id == 0)
    {
        TraceLog(LOG_ERROR, "Could not create sky shader");
        UnloadShader(gCrfShader);
        gCrfShader = Shader{};
        return false;
    }

    const char* shadowVertexShaderCode = R"(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
uniform mat4 mvp;
out vec2 fragTexCoord;
void main() {
    fragTexCoord = vertexTexCoord;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";
    const char* shadowFragmentShaderCode = R"(
#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform float alphaCutoff;
out vec4 finalColor;
void main() {
    if (texture(texture0, fragTexCoord).a <= max(1.0 / 255.0, alphaCutoff)) discard;
    float depth = gl_FragCoord.z;
    finalColor = vec4(depth, depth, depth, 1.0);
}
)";
    gShadowDepthShader = LoadShaderFromMemory(shadowVertexShaderCode,
        shadowFragmentShaderCode);
    if (gShadowDepthShader.id == 0) {
        TraceLog(LOG_ERROR, "Could not create live-shadow depth shader");
        UnloadShader(gSkyShader);
        UnloadShader(gCrfShader);
        gSkyShader = Shader{};
        gCrfShader = Shader{};
        return false;
    }
    gShadowDepthShader.locs[SHADER_LOC_MAP_DIFFUSE] =
        GetShaderLocation(gShadowDepthShader, "texture0");

    gLightDirectionLoc =
        GetShaderLocation(
            gCrfShader,
            "lightDirection"
        );

    gLightColorLoc =
        GetShaderLocation(
            gCrfShader,
            "lightColor"
        );

    gAmbientColorLoc =
        GetShaderLocation(
            gCrfShader,
            "ambientColor"
        );

    gOverlayEnabledLoc =
        GetShaderLocation(
            gCrfShader,
            "overlayEnabled"
        );
    gLightmapEnabledLoc = GetShaderLocation(gCrfShader, "lightmapEnabled");
    gLightmapTransformLoc = GetShaderLocation(gCrfShader, "lightmapTransform");
    gLightmapUvAttribLoc = GetShaderLocationAttrib(gCrfShader, "vertexTexCoord3");
    gAlphaCutoffLoc = GetShaderLocation(gCrfShader, "alphaCutoff");

    gCrfShader.locs[SHADER_LOC_MAP_METALNESS] =
        GetShaderLocation(
            gCrfShader,
            "texture1"
        );
    gCrfShader.locs[SHADER_LOC_MAP_NORMAL] = GetShaderLocation(gCrfShader, "texture2");
    gCrfShader.locs[SHADER_LOC_MAP_ROUGHNESS] = GetShaderLocation(gCrfShader, "specialTexture");
    gCrfShader.locs[SHADER_LOC_MAP_OCCLUSION] = GetShaderLocation(gCrfShader, "overlaySpecialTexture");
    gCrfShader.locs[SHADER_LOC_MAP_CUBEMAP] = GetShaderLocation(gCrfShader, "environmentCube");
    // Keep unlike sampler types on distinct units even when no cube is available.
    const int cubeUnit = MATERIAL_MAP_CUBEMAP;
    SetShaderValue(gCrfShader,gCrfShader.locs[SHADER_LOC_MAP_CUBEMAP],&cubeUnit,SHADER_UNIFORM_INT);

    // Per-level environment values are uploaded by ApplyEnvironmentPreview.
    return true;
}

static void ApplyEnvironmentPreview(const Camera3D& camera) {
    const auto set = [](Shader shader, const char* name, const float* values, int type) {
        SetShaderValue(shader, GetShaderLocation(shader, name), values, type);
    };
    auto ambient = gEnvironment.numbers((size_t)gAmbientPreviewSlot);
    auto ambientBottom = gEnvironment.numbers((size_t)(gAmbientPreviewSlot ^ 1));
    auto light = gEnvironment.numbers(4);
    // CONFIRMED 004ba330/004ba570: renderer RGB scale is float32 2.0.
    for (int i=0; i<3; ++i) { ambient[i]*=2; ambientBottom[i]*=2; light[i]*=2; }
    std::string cubeName = gEnvironment.value(6);
    if (cubeName.size() >= 2 && cubeName.front() == '"' && cubeName.back() == '"')
        cubeName = cubeName.substr(1,cubeName.size()-2);
    if (std::filesystem::path(cubeName).extension().empty()) cubeName += ".dds";
    const std::string cubePath = (std::filesystem::path(gEnvironmentTextures) / cubeName).string();
    if (cubePath != gEnvironmentCubePath) {
        if (gEnvironmentCube.id) UnloadTexture(gEnvironmentCube);
        gEnvironmentCube = LoadEnvironmentCube(cubePath.c_str());
        gEnvironmentCubePath = cubePath;
        if (!gEnvironmentCube.id) TraceLog(LOG_WARNING,"Environment cube unavailable: %s",cubePath.c_str());
    }
    const int cubeEnabled = gEnvironmentCube.id != 0;
    SetShaderValue(gCrfShader, GetShaderLocation(gCrfShader,"environmentCubeEnabled"),
        &cubeEnabled, SHADER_UNIFORM_INT);
    auto angles = gEnvironment.numbers(5);
    // CONFIRMED 00489070: (cos(b)cos(a),sin(b),cos(b)sin(a)); negate imported Z.
    const float a = angles[0] * DEG2RAD, b = angles[1] * DEG2RAD;
    const float direction[] = {cosf(b)*cosf(a), sinf(b), -cosf(b)*sinf(a)};
    // The game submits the directional record only when average RGB is
    // strictly greater than 0.001 (004552b0). Match that boundary so very dim
    // authored lights do not appear in the editor when the game omits them.
    if ((light[0] + light[1] + light[2]) / 3.0f <= 0.001f) light = {};
    set(gCrfShader, "ambientColor", ambient.data(), SHADER_UNIFORM_VEC3);
    set(gCrfShader, "ambientBottomColor", ambientBottom.data(), SHADER_UNIFORM_VEC3);
    set(gCrfShader, "lightColor", light.data(), SHADER_UNIFORM_VEC3);
    set(gCrfShader, "lightDirection", direction, SHADER_UNIFORM_VEC3);
    set(gCrfShader, "cameraPosition", &camera.position.x, SHADER_UNIFORM_VEC3);
    const int liveShadowEnabled = gLiveShadowEnabled && gLiveShadowValid ? 1 : 0;
    SetShaderValue(gCrfShader, GetShaderLocation(gCrfShader, "liveShadowEnabled"),
        &liveShadowEnabled, SHADER_UNIFORM_INT);
    SetShaderValueMatrix(gCrfShader,
        GetShaderLocation(gCrfShader, "lightViewProjection"), gLiveShadowMatrix);
    if (gLiveShadowMap.texture.id) {
        SetShaderValueTexture(gCrfShader, GetShaderLocation(gCrfShader, "shadowMap"),
            gLiveShadowMap.texture);
        const float shadowTexelSize[]{ 1.0f / (float)gLiveShadowMap.texture.width,
            1.0f / (float)gLiveShadowMap.texture.height };
        SetShaderValue(gCrfShader, GetShaderLocation(gCrfShader, "shadowTexelSize"),
            shadowTexelSize, SHADER_UNIFORM_VEC2);
    }
    auto fog = gEnvironment.numbers(7), depth = gEnvironment.numbers(8), height = gEnvironment.numbers(9);
    const float ranges[] = {depth[0], depth[1], height[0], height[1]};
    set(gCrfShader, "fogColor", fog.data(), SHADER_UNIFORM_VEC3);
    set(gCrfShader, "fogRanges", ranges, SHADER_UNIFORM_VEC4);
    int enabled = gFogPreview && depth[1] > 0 && height[1] > 0;
    SetShaderValue(gCrfShader, GetShaderLocation(gCrfShader, "fogEnabled"), &enabled, SHADER_UNIFORM_INT);
    const float deathHeight = gEnvironment.numbers(20)[0];
    const int deathShadowEnabled = 1;
    SetShaderValue(gCrfShader, GetShaderLocation(gCrfShader, "deathHeight"),
        &deathHeight, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gCrfShader, GetShaderLocation(gCrfShader, "deathFadeDepth"),
        &kDeathShadowFadeDepth, SHADER_UNIFORM_FLOAT);
    SetShaderValue(gCrfShader, GetShaderLocation(gCrfShader, "deathShadowEnabled"),
        &deathShadowEnabled, SHADER_UNIFORM_INT);
    auto sky = gEnvironment.numbers(10), sun = gEnvironment.numbers(12);
    auto exponents = gEnvironment.numbers(13), sunAngles = gEnvironment.numbers(14);
    const float sa = sunAngles[0]*DEG2RAD, sb = sunAngles[1]*DEG2RAD;
    // CONFIRMED negated trigonometric triple, with the editor's Z reflection.
    const float sunDirection[] = {-cosf(sb)*cosf(sa), -sinf(sb), cosf(sb)*sinf(sa)};
    set(gSkyShader, "skyTop", sky.data(), SHADER_UNIFORM_VEC3);
    set(gSkyShader, "skyBottom", sky.data()+3, SHADER_UNIFORM_VEC3);
    set(gSkyShader, "sunColor0", sun.data(), SHADER_UNIFORM_VEC3);
    set(gSkyShader, "sunColor1", sun.data()+3, SHADER_UNIFORM_VEC3);
    set(gSkyShader, "sunDirection", sunDirection, SHADER_UNIFORM_VEC3);
    set(gSkyShader, "sunExponents", exponents.data(), SHADER_UNIFORM_VEC2);
}

static void DrawDeathHeight(const LvlxLevel& level, const Camera3D& camera) {
    const float y = gEnvironment.numbers(20)[0];
    float minX = camera.target.x - 100.0f, maxX = camera.target.x + 100.0f;
    float minZ = camera.target.z - 100.0f, maxZ = camera.target.z + 100.0f;
    const auto include = [&](LvlxVec3 source) {
        const Vector3 p = ToRaylib(source);
        minX = fminf(minX, p.x - 50.0f); maxX = fmaxf(maxX, p.x + 50.0f);
        minZ = fminf(minZ, p.z - 50.0f); maxZ = fmaxf(maxZ, p.z + 50.0f);
    };
    for (uint32_t i = 0; i < level.element_count; ++i) if (level.elements[i].present) {
        include(level.elements[i].vector1);
        include(level.elements[i].vector2);
    }
    for (uint32_t i = 0; i < level.waylist_count; ++i)
        for (uint16_t p = 0; p < level.waylists[i].point_count; ++p)
            include(level.waylists[i].points[p]);
    if (level.has_spawn_data) include(level.spawn_position);

    const Vector2 shadowSize{maxX-minX, maxZ-minZ};
    const Vector3 shadowCenter{(minX+maxX)*0.5f, y, (minZ+maxZ)*0.5f};
    // Do not submit a fully transparent plane: it may still write depth and hide
    // the lower black cap. Model shading supplies the vertical fade.
    if (kDeathShadowSurfaceOpacity > 0.0f)
        DrawPlane(shadowCenter, shadowSize, Fade(BLACK, kDeathShadowSurfaceOpacity));

    // A solid cap at the end of the fade keeps the bottom of the map black even
    // where there is no model geometry for the fragment shader to darken.
    DrawPlane({shadowCenter.x, y-kDeathShadowFadeDepth, shadowCenter.z},
        shadowSize, BLACK);

    // H controls only the grid; the death shadow remains part of the preview.
    if (gDeathGridVisible) {
        const float x = floorf(camera.target.x / 10)*10, z = floorf(camera.target.z / 10)*10;
        for (int i = -10; i <= 10; ++i) {
            DrawLine3D({x+i*10,y+0.01f,z-100}, {x+i*10,y+0.01f,z+100}, Fade(RED, .5f));
            DrawLine3D({x-100,y+0.01f,z+i*10}, {x+100,y+0.01f,z+i*10}, Fade(RED, .5f));
        }
    }
    if (level.has_spawn_data && level.spawn_position.y < y)
        DrawSphereWires(ToRaylib(level.spawn_position), 1.0f, 8, 8, RED);
}

static Vector2 DisplayUv(float u, float v)
{
    return Vector2{ u, v };
}

static Mesh MakeRaylibMesh(const CrfMesh& source, unsigned int& lightmapUvVboId)
{
    Mesh mesh{};
    lightmapUvVboId = 0;

    const size_t vertex3Size =
        (size_t)source.vertex_count *
        3u *
        sizeof(float);

    const size_t vertex2Size =
        (size_t)source.vertex_count *
        2u *
        sizeof(float);

    const size_t indexSize =
        (size_t)source.triangle_count *
        3u *
        sizeof(unsigned short);

    mesh.vertexCount =
        (int)source.vertex_count;

    mesh.triangleCount =
        (int)source.triangle_count;

    mesh.vertices =
        (float*)MemAlloc(
            (unsigned int)vertex3Size
        );

    mesh.normals =
        (float*)MemAlloc(
            (unsigned int)vertex3Size
        );

    mesh.texcoords =
        (float*)MemAlloc(
            (unsigned int)vertex2Size
        );

    mesh.texcoords2 =
        (float*)MemAlloc(
            (unsigned int)vertex2Size
        );

    mesh.indices =
        (unsigned short*)MemAlloc(
            (unsigned int)indexSize
        );

    if (!mesh.vertices ||
        !mesh.normals ||
        !mesh.texcoords ||
        !mesh.texcoords2 ||
        !mesh.indices)
    {
        if (mesh.vertices)
            MemFree(mesh.vertices);

        if (mesh.normals)
            MemFree(mesh.normals);

        if (mesh.texcoords)
            MemFree(mesh.texcoords);

        if (mesh.texcoords2)
            MemFree(mesh.texcoords2);
        if (mesh.indices)
            MemFree(mesh.indices);

        return Mesh{};
    }

    // ---------------------------------------------------------
    // Vertices
    // ---------------------------------------------------------

    for (uint32_t i = 0;
        i < source.vertex_count;
        ++i)
    {
        // -----------------------------------------------------
        // Position
        //
        // Game -> raylib:
        //
        // X ->  X
        // Y ->  Y
        // Z -> -Z
        // -----------------------------------------------------

        mesh.vertices[i * 3u + 0u] =
            source.positions[i * 3u + 0u];

        mesh.vertices[i * 3u + 1u] =
            source.positions[i * 3u + 1u];

        mesh.vertices[i * 3u + 2u] =
            -source.positions[i * 3u + 2u];

        // -----------------------------------------------------
        // Normal
        //
        // Same coordinate-system conversion.
        // -----------------------------------------------------

        mesh.normals[i * 3u + 0u] =
            source.normals[i * 3u + 0u];

        mesh.normals[i * 3u + 1u] =
            source.normals[i * 3u + 1u];

        mesh.normals[i * 3u + 2u] =
            -source.normals[i * 3u + 2u];

        // -----------------------------------------------------
        // UV
        //
        // IMPORTANT:
        //
        // Use exactly what is stored in the CRF.
        // No U flip.
        // No V flip.
        // No swap.
        // -----------------------------------------------------

        const Vector2 baseUv = DisplayUv(
            source.texcoords[i * 2u + 0u],
            source.texcoords[i * 2u + 1u]
        );
        const Vector2 overlayUv = DisplayUv(
            source.packed_uv_b[i * 2u + 0u],
            source.packed_uv_b[i * 2u + 1u]
        );

        mesh.texcoords[i * 2u + 0u] = baseUv.x;
        mesh.texcoords[i * 2u + 1u] = baseUv.y;
        mesh.texcoords2[i * 2u + 0u] = overlayUv.x;
        mesh.texcoords2[i * 2u + 1u] = overlayUv.y;
    }

    // ---------------------------------------------------------
    // Indices
    //
    // Z mirroring changes handedness.
    //
    // A B C -> A C B
    // ---------------------------------------------------------

    for (uint32_t i = 0;
        i < source.triangle_count;
        ++i)
    {
        mesh.indices[i * 3u + 0u] =
            source.indices[i * 3u + 0u];

        mesh.indices[i * 3u + 1u] =
            source.indices[i * 3u + 2u];

        mesh.indices[i * 3u + 2u] =
            source.indices[i * 3u + 1u];
    }

    UploadMesh(
        &mesh,
        false
    );

    // raylib exposes two UV arrays in Mesh. Keep the CRF lightmap UVs in a
    // separate VBO and attach them to this mesh's VAO as a third UV attribute.
    if (source.second_stream_uv && gLightmapUvAttribLoc >= 0 && mesh.vaoId != 0) {
        lightmapUvVboId = rlLoadVertexBuffer(source.second_stream_uv,
            (int)vertex2Size, false);
        if (lightmapUvVboId != 0 && rlEnableVertexArray(mesh.vaoId)) {
            rlSetVertexAttribute((unsigned int)gLightmapUvAttribLoc, 2, RL_FLOAT,
                false, 0, 0);
            rlEnableVertexAttribute((unsigned int)gLightmapUvAttribLoc);
            rlDisableVertexArray();
            rlDisableVertexBuffer();
        }
    }

    return mesh;
}

static float DetectAlphaCutoff(const char* path)
{
    // raylib deliberately leaves DDS data compressed after LoadImage(), so
    // inspect DXT5 alpha blocks directly instead of asking it for RGBA pixels.
    std::ifstream stream(path, std::ios::binary);
    std::array<unsigned char, 128> header{};
    if (!stream.read(reinterpret_cast<char*>(header.data()), header.size()))
        return 0.0f;

    const auto readU32 = [](const unsigned char* bytes) {
        return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
            ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
    };
    if (std::memcmp(header.data(), "DDS ", 4) != 0 ||
        readU32(header.data() + 4) != 124u ||
        std::memcmp(header.data() + 84, "DXT5", 4) != 0)
        return 0.0f;

    const uint32_t height = readU32(header.data() + 12);
    const uint32_t width = readU32(header.data() + 16);
    if (!width || !height) return 0.0f;
    const size_t blockCount = (size_t)((width + 3u) / 4u) *
        (size_t)((height + 3u) / 4u);
    if (blockCount > std::numeric_limits<size_t>::max() / 16u)
        return 0.0f;
    std::vector<unsigned char> blocks(blockCount * 16u);
    if (!stream.read(reinterpret_cast<char*>(blocks.data()),
            static_cast<std::streamsize>(blocks.size())))
        return 0.0f;

    const size_t step = std::max<size_t>(1, blockCount / 8192u);
    size_t transparent = 0, opaque = 0, intermediate = 0, sampled = 0;
    for (size_t blockIndex = 0; blockIndex < blockCount; blockIndex += step) {
        const unsigned char* block = blocks.data() + blockIndex * 16u;
        std::array<unsigned char, 8> alpha{ block[0], block[1] };
        if (alpha[0] > alpha[1]) {
            for (unsigned int i = 1; i <= 6; ++i)
                alpha[i + 1] = (unsigned char)(((7u - i) * alpha[0] +
                    i * alpha[1]) / 7u);
        } else {
            for (unsigned int i = 1; i <= 4; ++i)
                alpha[i + 1] = (unsigned char)(((5u - i) * alpha[0] +
                    i * alpha[1]) / 5u);
            alpha[6] = 0;
            alpha[7] = 255;
        }
        uint64_t indices = 0;
        for (unsigned int i = 0; i < 6; ++i)
            indices |= (uint64_t)block[2u + i] << (8u * i);
        for (unsigned int pixel = 0; pixel < 16; ++pixel) {
            const unsigned char value = alpha[(indices >> (pixel * 3u)) & 7u];
            ++sampled;
            if (value <= 8) ++transparent;
            else if (value >= 247) ++opaque;
            else ++intermediate;
        }
    }
    if (!sampled) return 0.0f;
    // Preview policy: endpoint-heavy alpha with both empty and solid regions is
    // foliage/fence-style cutout data. Continuous gradients retain soft alpha.
    const bool cutout = transparent * 100 >= sampled && opaque * 10 >= sampled &&
        intermediate * 4 <= sampled;
    return cutout ? 0.50f : 0.0f;
}

static bool LoadRenderAsset(
    RenderAsset& asset,
    const LvlxElementDefinition& definition,
    const std::string& assetsDirectory,
    const std::string& texturesDirectory,
    Shader shaderOverride = Shader{})
{
    CrfModelData source{};

    asset.attempted = true;

    if (!definition.resource ||
        !definition.resource[0])
    {
        return false;
    }

    const std::string modelPath=ResolveResourceFile(assetsDirectory,
        gFallbackAssetsDirectory,definition.resource);
    const bool isGameSky =
        Lowercase(FilenameOf(modelPath)) == "sky_01.crf";

    if (!crf_load(
        modelPath.c_str(),
        &source))
    {
        TraceLog(
            LOG_WARNING,
            "Could not read CRF: %s",
            modelPath.c_str()
        );

        return false;
    }

    for (uint32_t i = 0;
        i < source.mesh_count;
        ++i)
    {
        const CrfMesh& sourceMesh =
            source.meshes[i];

        Texture2D texture{};
        Texture2D overlayTexture{};
        Texture2D specialTexture{}, overlaySpecialTexture{};
        float alphaCutoff = 0.0f;

        // -----------------------------------------------------
        // Load diffuse/color texture: sffd -> *_c.dds
        // -----------------------------------------------------

        const char* colorTextureName = isGameSky
            ? "sky_01_dm"
            : (sourceMesh.sky_texture[0]
                ? sourceMesh.sky_texture
                : sourceMesh.color_texture);

        if (colorTextureName[0])
        {
            const std::string texturePath =
                ResolveResourceFile(texturesDirectory,gFallbackTexturesDirectory,
                    std::string(colorTextureName) + ".dds");

            if (FileExists(
                texturePath.c_str()))
            {
                if (!isGameSky) alphaCutoff = DetectAlphaCutoff(texturePath.c_str());
                texture =
                    LoadTexture(
                        texturePath.c_str()
                    );
                if (texture.id != 0)
                {
                    SetTextureWrap(
                        texture,
                        // CONFIRMED MPDiffuseSpecular base sampler: flag bit 1
                        // enables tiling; otherwise clamp to the texture edge.
                        // Hill skirts sit at V~1 and must not sample snowy V~0.
                        (isGameSky || (sourceMesh.material_flags & 2u))
                            ? TEXTURE_WRAP_REPEAT : TEXTURE_WRAP_CLAMP
                    );
                    if (isGameSky) {
                        GenTextureMipmaps(&texture);
                        SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
                        rlTextureParameters(texture.id, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_REPEAT);
                        rlTextureParameters(texture.id, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
                    }
                }
            }
            else
            {
                TraceLog(
                    LOG_WARNING,
                    "Texture not found: %s",
                    texturePath.c_str()
                );
            }
        }

        // The second color stage is an alpha-bearing overlay texture. Its UVs
        // come from the second int16 pair in CCompressed32ByteVertex.
        if (sourceMesh.overlay_color_texture[0])
        {
            const std::string overlayTexturePath =
                ResolveResourceFile(texturesDirectory,gFallbackTexturesDirectory,
                    std::string(sourceMesh.overlay_color_texture) + ".dds");

            if (FileExists(overlayTexturePath.c_str()))
            {
                overlayTexture = LoadTexture(overlayTexturePath.c_str());
                if (overlayTexture.id != 0)
                {
                    SetTextureWrap(overlayTexture, TEXTURE_WRAP_CLAMP);
                }
            }
            else
            {
                TraceLog(
                    LOG_WARNING,
                    "Overlay texture not found: %s",
                    overlayTexturePath.c_str()
                );
            }
        }

        // -----------------------------------------------------
        // Convert mesh
        // -----------------------------------------------------

        auto loadSpecial = [&](const char* name, bool overlay) {
            Texture2D result{};
            if (name[0]) result = LoadTexture(ResolveResourceFile(texturesDirectory,
                gFallbackTexturesDirectory,std::string(name) + ".dds").c_str());
            if (!result.id && !overlay) {
                Image fallback = GenImageColor(1, 1, Color{128,128,128,128});
                result = LoadTextureFromImage(fallback); UnloadImage(fallback);
            }
            if (result.id) {
                SetTextureWrap(result, !overlay && (sourceMesh.material_flags & 2u)
                    ? TEXTURE_WRAP_REPEAT : TEXTURE_WRAP_CLAMP);
                SetTextureFilter(result, TEXTURE_FILTER_BILINEAR);
            }
            return result;
        };
        if (sourceMesh.mp_material && !isGameSky) {
            specialTexture = loadSpecial(sourceMesh.auxiliary_texture, false);
            if (sourceMesh.overlay_color_texture[0] && sourceMesh.overlay_auxiliary_texture[0])
                overlaySpecialTexture = loadSpecial(sourceMesh.overlay_auxiliary_texture, true);
            // The game enables overlays only as a diffuse+special pair.
            if (overlayTexture.id && !overlaySpecialTexture.id) {
                UnloadTexture(overlayTexture); overlayTexture = {};
            }
        }
        unsigned int lightmapUvVboId = 0;
        Mesh mesh = MakeRaylibMesh(sourceMesh, lightmapUvVboId);

        if (!mesh.vertexCount)
        {
            if (texture.id)
                UnloadTexture(texture);
            if (overlayTexture.id)
                UnloadTexture(overlayTexture);
            if (specialTexture.id) UnloadTexture(specialTexture);
            if (overlaySpecialTexture.id) UnloadTexture(overlaySpecialTexture);
            if (lightmapUvVboId)
                rlUnloadVertexBuffer(lightmapUvVboId);

            continue;
        }

        Model model =
            LoadModelFromMesh(mesh);

        // -----------------------------------------------------
        // Material
        // -----------------------------------------------------

        if (model.materialCount > 0)
        {
            Material& material =
                model.materials[0];

            material.shader = shaderOverride.id != 0
                ? shaderOverride
                : gCrfShader;

            if (texture.id)
            {
                material
                    .maps[MATERIAL_MAP_DIFFUSE]
                    .texture = texture;
            }

            if (overlayTexture.id)
            {
                material
                    .maps[MATERIAL_MAP_METALNESS]
                    .texture = overlayTexture;
            }

            material
                .maps[MATERIAL_MAP_DIFFUSE]
                .color = WHITE;
            material.maps[MATERIAL_MAP_ROUGHNESS].texture = specialTexture;
            material.maps[MATERIAL_MAP_OCCLUSION].texture = overlaySpecialTexture;
        }

        asset.textures.push_back(
            texture
        );
        asset.textures.push_back(
            overlayTexture
        );

        asset.models.push_back(
            model
        );
        asset.modelHasOverlay.push_back(
            overlayTexture.id != 0 ? 1u : 0u
        );
        asset.modelAlphaCutoffs.push_back(alphaCutoff);
        asset.textures.push_back(specialTexture);
        asset.textures.push_back(overlaySpecialTexture);
        asset.modelRecoveredMaterial.push_back(sourceMesh.mp_material && !isGameSky);
        std::array<float,5> rim = {sourceMesh.rim_start,sourceMesh.rim_end,
            sourceMesh.rim_color[0],sourceMesh.rim_color[1],sourceMesh.rim_color[2]};
        if (definition.has_rim_light)
            std::copy_n(definition.rim_light,5,rim.begin());
        for (float& value : rim) if (!std::isfinite(value)) value=0;
        asset.modelRimLights.push_back(rim);
        asset.subsetIdentifiers.push_back(sourceMesh.subset_identifier);
        asset.lightmapUvVboIds.push_back(lightmapUvVboId);
        std::vector<Vector2> bakeUvs;
        if (sourceMesh.second_stream_uv) {
            bakeUvs.reserve(sourceMesh.vertex_count);
            for (uint32_t vertex = 0; vertex < sourceMesh.vertex_count; ++vertex)
                bakeUvs.push_back(Vector2{ sourceMesh.second_stream_uv[vertex * 2u],
                    sourceMesh.second_stream_uv[vertex * 2u + 1u] });
        }
        asset.modelLightmapUvs.push_back(std::move(bakeUvs));
    }

    if (definition.preview_animation && asset.models.size() == source.mesh_count) {
        asset.animationRate = definition.preview_animation_rate;
        if (!std::isfinite(asset.animationRate) || asset.animationRate <= 0)
            asset.animation.error = "invalid authored animation rate";
        else if (!asset.animation.load(modelPath,
            ResolveResourceFile(assetsDirectory,gFallbackAssetsDirectory,definition.preview_animation), source))
            TraceLog(LOG_WARNING, "Animation preview %s: %s", definition.resource,
                asset.animation.error.c_str());
    }
    crf_free(&source);

    asset.loaded =
        !asset.models.empty();

    return asset.loaded;
}

static void UpdateAnimationPreview(RenderAsset& asset, bool enabled, double seconds) {
    auto restore = [&]() {
        for (size_t i=0; i<asset.animation.skins.size() && i<asset.models.size(); ++i) {
            Mesh& mesh=asset.models[i].meshes[0]; const auto& s=asset.animation.skins[i];
            for (size_t v=0;v<s.positions.size();++v) {
                mesh.vertices[v]=s.positions[v]*(v%3==2?-1:1);
                mesh.normals[v]=s.normals[v]*(v%3==2?-1:1);
            }
            UpdateMeshBuffer(mesh,0,mesh.vertices,mesh.vertexCount*3*sizeof(float),0);
            UpdateMeshBuffer(mesh,2,mesh.normals,mesh.vertexCount*3*sizeof(float),0);
        }
        asset.animatedPose=false;
    };
    if (!enabled || !asset.animation.ready) { if(asset.animatedPose) restore(); return; }
    try {
        double phase=seconds*asset.animationRate/asset.animation.clip.duration;
        if(asset.animation.clip.mode==1) phase-=std::floor(phase);
        auto palette=asset.animation.pose(float(phase));
        std::vector<std::vector<float>> positions(asset.models.size()),normals(asset.models.size());
        for(size_t i=0;i<asset.models.size();++i) {
            const Mesh& mesh=asset.models[i].meshes[0];
            positions[i].resize(mesh.vertexCount*3); normals[i].resize(mesh.vertexCount*3);
            asset.animation.deform(i,palette,positions[i].data(),normals[i].data());
        }
        for(size_t i=0;i<asset.models.size();++i) {
            Mesh& mesh=asset.models[i].meshes[0]; size_t bytes=positions[i].size()*sizeof(float);
            std::memcpy(mesh.vertices,positions[i].data(),bytes); std::memcpy(mesh.normals,normals[i].data(),bytes);
            UpdateMeshBuffer(mesh,0,mesh.vertices,int(bytes),0); UpdateMeshBuffer(mesh,2,mesh.normals,int(bytes),0);
        }
        asset.animatedPose=true;
    } catch(const std::exception& e) {
        asset.animation.error=e.what(); asset.animation.ready=false; restore();
        TraceLog(LOG_WARNING,"Animation preview stopped: %s",e.what());
    }
}

static void UnloadRenderAssets(
    std::vector<RenderAsset>& assets)
{
    for (RenderAsset& asset : assets)
    {
        for (size_t i = 0;
            i < asset.models.size();
            ++i)
        {
            Model& model =
                asset.models[i];

            if (i < asset.lightmapUvVboIds.size() && asset.lightmapUvVboIds[i])
                rlUnloadVertexBuffer(asset.lightmapUvVboIds[i]);

            // We own the texture separately.
            // Remove it from the material before unloading
            // the model to avoid ownership confusion.
            if (model.materialCount > 0)
            {
                model.materials[0]
                    .maps[MATERIAL_MAP_DIFFUSE]
                    .texture = Texture2D{};
                model.materials[0]
                    .maps[MATERIAL_MAP_METALNESS]
                    .texture = Texture2D{};
                model.materials[0]
                    .maps[MATERIAL_MAP_NORMAL]
                    .texture = Texture2D{};
                model.materials[0].maps[MATERIAL_MAP_ROUGHNESS].texture = {};
                model.materials[0].maps[MATERIAL_MAP_OCCLUSION].texture = {};
                model.materials[0].maps[MATERIAL_MAP_CUBEMAP].texture = {};
            }

            UnloadModel(model);
        }

        for (Texture2D texture :
        asset.textures)
        {
            if (texture.id)
            {
                UnloadTexture(
                    texture
                );
            }
        }

        asset.models.clear();
        asset.textures.clear();
        asset.modelHasOverlay.clear();
        asset.modelAlphaCutoffs.clear();
        asset.modelRecoveredMaterial.clear();
        asset.modelRimLights.clear();
        asset.animation = {};
        asset.animatedPose = false;
        asset.subsetIdentifiers.clear();
        asset.lightmapUvVboIds.clear();
        asset.modelLightmapUvs.clear();

        asset.loaded = false;
        asset.attempted = false;
    }
}

static void SetOverlayEnabled(const RenderAsset& asset, size_t modelIndex)
{
    if (gOverlayEnabledLoc < 0) return;
    const int enabled = modelIndex < asset.modelHasOverlay.size() &&
        asset.modelHasOverlay[modelIndex] != 0;
    SetShaderValue(
        gCrfShader,
        gOverlayEnabledLoc,
        &enabled,
        SHADER_UNIFORM_INT
    );
}

static void SetAlphaCutoff(const RenderAsset& asset, size_t modelIndex)
{
    if (gAlphaCutoffLoc < 0) return;
    const float cutoff = modelIndex < asset.modelAlphaCutoffs.size()
        ? asset.modelAlphaCutoffs[modelIndex] : 0.0f;
    SetShaderValue(gCrfShader, gAlphaCutoffLoc, &cutoff, SHADER_UNIFORM_FLOAT);
    const int recovered = modelIndex < asset.modelRecoveredMaterial.size()
        ? asset.modelRecoveredMaterial[modelIndex] : 0;
    SetShaderValue(gCrfShader, GetShaderLocation(gCrfShader, "recoveredMaterial"),
        &recovered, SHADER_UNIFORM_INT);
    const std::array<float,5> rim = modelIndex < asset.modelRimLights.size()
        ? asset.modelRimLights[modelIndex] : std::array<float,5>{.5f,1,0,0,0};
    SetShaderValue(gCrfShader,GetShaderLocation(gCrfShader,"rimRange"),rim.data(),SHADER_UNIFORM_VEC2);
    SetShaderValue(gCrfShader,GetShaderLocation(gCrfShader,"rimColor"),rim.data()+2,SHADER_UNIFORM_VEC3);
}

static void SetLightmapBinding(Model& model, const LevelLightmaps& lightmaps,
    uint32_t attachmentKey, uint32_t subsetIdentifier)
{
    model.materials[0].maps[MATERIAL_MAP_CUBEMAP].texture = gEnvironmentCube;
    const LmdRecord* match = nullptr;
    const uint64_t lookupKey = ((uint64_t)attachmentKey << 32) | subsetIdentifier;
    const auto found = lightmaps.recordLookup.find(lookupKey);
    if (found != lightmaps.recordLookup.end() && found->second < lightmaps.records.size())
        match = &lightmaps.records[found->second];
    int enabled = 0;
    float transform[4] = { 1, 1, 0, 0 };
    Texture2D texture{};
    if (match && match->textureSlot < lightmaps.textures.size() &&
        lightmaps.textures[match->textureSlot].id) {
        enabled = 1;
        texture = lightmaps.textures[match->textureSlot];
        transform[0] = match->scaleU; transform[1] = match->scaleV;
        transform[2] = match->offsetU; transform[3] = match->offsetV;
    }
    if (model.materialCount > 0)
        model.materials[0].maps[MATERIAL_MAP_NORMAL].texture = texture;
    if (gLightmapEnabledLoc >= 0)
        SetShaderValue(gCrfShader, gLightmapEnabledLoc, &enabled, SHADER_UNIFORM_INT);
    if (gLightmapTransformLoc >= 0)
        SetShaderValue(gCrfShader, gLightmapTransformLoc, transform, SHADER_UNIFORM_VEC4);
}

static void DrawGameSky(const Camera3D& camera, RenderAsset& skyAsset,
    const std::string& assetsDirectory, const std::string& texturesDirectory,
    unsigned& loadedCount, unsigned& missingCount)
{
    LvlxElementDefinition skyDefinition{};
    skyDefinition.resource = const_cast<char*>("sky_01.crf");

    if (!skyAsset.attempted)
    {
        if (LoadRenderAsset(
            skyAsset,
            skyDefinition,
            assetsDirectory,
            texturesDirectory,
            gSkyShader))
            ++loadedCount;
        else
            ++missingCount;
    }

    if (!skyAsset.loaded) return;

    Vector3 minimum{ FLT_MAX, FLT_MAX, FLT_MAX };
    Vector3 maximum{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const Model& model : skyAsset.models)
    {
        const BoundingBox bounds = GetModelBoundingBox(model);
        minimum = Vector3Min(minimum, bounds.min);
        maximum = Vector3Max(maximum, bounds.max);
    }

    const Vector3 center = Vector3Scale(Vector3Add(minimum, maximum), 0.5f);
    const Vector3 halfSize = Vector3Scale(Vector3Subtract(maximum, minimum), 0.5f);
    const float sourceRadius = fmaxf(fmaxf(halfSize.x, halfSize.z), halfSize.y);
    const float scale = sourceRadius > 0.0001f ? 100.0f / sourceRadius : 1.0f;
    const auto offset = gEnvironment.numbers(11);
    const Vector3 skyOffset = ToRaylib({offset[0], offset[1], offset[2]});
    const Vector3 position = Vector3Subtract(
        Vector3Add(camera.position, skyOffset),
        Vector3Scale(center, scale)
    );

    // The camera is inside the sky mesh. It must neither be culled nor write
    // into the depth buffer before the level geometry is rendered.
    rlDisableBackfaceCulling();
    rlDisableDepthMask();
    for (const Model& model : skyAsset.models)
    {
        DrawModelEx(
            model,
            position,
            Vector3{ 0.0f, 1.0f, 0.0f },
            0.0f,
            Vector3{ scale, scale, scale },
            WHITE
        );
    }
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}

static BoundingBox TransformBounds(BoundingBox bounds, Matrix transform);

static void BindObjectPointLights(const EffectPreview* effects,const Model& model,
    Vector3 position,float angle,float scale) {
    if(effects) {
        Matrix world=MatrixMultiply(MatrixMultiply(MatrixScale(scale,scale,scale),
            MatrixRotateY(angle*DEG2RAD)),MatrixTranslate(position.x,position.y,position.z));
        effects->ApplyPointLights(gCrfShader,TransformBounds(GetModelBoundingBox(model),world));
    } else {
        const int zero=0;
        SetShaderValue(gCrfShader,GetShaderLocation(gCrfShader,"effectLightCount"),&zero,SHADER_UNIFORM_INT);
    }
}

static void DrawElements(const LvlxLevel& level,
    const LvlxDefinitionTable& definitions,
    std::vector<RenderAsset>& assets,
    const std::string& assetsDirectory,
    const std::string& texturesDirectory,
    const LevelLightmaps& lightmaps,
    bool lightmapsEnabled,
    unsigned& loadedCount, unsigned& missingCount,
    const std::vector<int>& selectedElements,
    int rotationOverrideElement = -1, float rotationOverrideDegrees = 0.0f,
    const EffectPreview* effectLights = nullptr) {
    static const LevelLightmaps emptyLightmaps;
    const LevelLightmaps& activeLightmaps = lightmapsEnabled ? lightmaps : emptyLightmaps;
    uint32_t runtimeIndex = 0;
    for (uint32_t i = 0; i < level.element_count; ++i) {
        const LvlxElement& element = level.elements[i];
        if (!element.present) continue;
        const uint32_t renderedDefinitionId = element.definition_id;
        const LvlxElementDefinition* definition =
            lvlx_find_definition(&definitions, renderedDefinitionId);
        const Vector3 position = ToRaylib(element.vector1);
        bool rendered = false;
        if (definition && renderedDefinitionId < assets.size() && definition->resource) {
            RenderAsset& asset = assets[renderedDefinitionId];
            if (!asset.attempted) {
                if (LoadRenderAsset(asset, *definition, assetsDirectory, texturesDirectory))
                    ++loadedCount;
                else ++missingCount;
            }
            if (asset.loaded) {
                const float scale = definition->scaling > 0 ? definition->scaling : 1.0f;
                float angle = element.orientation <= 3u
                    ? -(float)element.orientation * 90.0f : 0.0f;
                if ((int)i == rotationOverrideElement) angle += rotationOverrideDegrees;
                const bool selected = IsElementSelected(selectedElements, (int)i);
                for (size_t modelIndex = 0; modelIndex < asset.models.size(); ++modelIndex) {
                    Model& model = asset.models[modelIndex];
                    SetAlphaCutoff(asset, modelIndex);
                    SetOverlayEnabled(asset, modelIndex);
                    const uint32_t attachmentKey = level.version > 2u
                        ? element.lightmap_attachment_key : runtimeIndex;
                    const uint32_t subsetIdentifier = modelIndex < asset.subsetIdentifiers.size()
                        ? asset.subsetIdentifiers[modelIndex] : (uint32_t)modelIndex;
                    SetLightmapBinding(model, activeLightmaps,
                        attachmentKey, subsetIdentifier);
                    BindObjectPointLights(effectLights,model,position,angle,scale);
                    DrawModelEx(model, position, Vector3{ 0, 1, 0 }, angle,
                        Vector3{ scale, scale, scale }, WHITE);

                    // Highlight the actual selected geometry instead of drawing
                    // a marker sphere around the element origin.
                    if (selected) {
                        DrawModelWiresEx(model, position, Vector3{ 0, 1, 0 }, angle,
                            Vector3{ scale, scale, scale }, YELLOW);
                    }
                }
                rendered = true;
            }
        }
        if (!rendered) {
            const float deathHeight = gEnvironment.numbers(20)[0];
            const Color color = position.y < deathHeight ? BLACK
                : CategoryColor(definition ? definition->category : nullptr);
            DrawCube(position, 0.75f, 0.75f, 0.75f, color);
            DrawCubeWires(position, 0.75f, 0.75f, 0.75f,
                IsElementSelected(selectedElements, (int)i) ? YELLOW : Fade(BLACK, 0.7f));
        }

        const Vector3 second = ToRaylib(element.vector2);
        if (DefinitionUsesDestination(definition) &&
            Vector3Distance(position, second) > 0.01f) {
            DrawLine3D(position, second, SKYBLUE);
            DrawSphere(second, 0.15f, SKYBLUE);
        }
        ++runtimeIndex;
    }
}

static void DrawSantaSpawn(const LvlxLevel& level, RenderAsset& santaAsset,
    const std::string& assetsDirectory, const std::string& texturesDirectory,
    unsigned& loadedCount, unsigned& missingCount,const EffectPreview* effectLights=nullptr) {
    if (!level.has_spawn_data) return;

    LvlxElementDefinition santaDefinition{};
    santaDefinition.resource = const_cast<char*>("santa_claus_01.crf");
    santaDefinition.scaling = kSantaVisualScale;
    if (!santaAsset.attempted) {
        if (LoadRenderAsset(santaAsset, santaDefinition, assetsDirectory, texturesDirectory))
            ++loadedCount;
        else ++missingCount;
    }
    if (!santaAsset.loaded) return;

    const Vector3 position = ToRaylib(level.spawn_position);
    // The model faces opposite the spawn's zero heading and uses reversed rotation.
    const float angle = 180.0f - level.initial_camera_heading_degrees;
    for (size_t modelIndex = 0; modelIndex < santaAsset.models.size(); ++modelIndex) {
        Model& model = santaAsset.models[modelIndex];
        SetAlphaCutoff(santaAsset, modelIndex);
        SetOverlayEnabled(santaAsset, modelIndex);
        SetLightmapBinding(model, LevelLightmaps{}, UINT32_MAX, UINT32_MAX);
        BindObjectPointLights(effectLights,model,position,angle,kSantaVisualScale);
        DrawModelEx(model, position, Vector3{ 0, 1, 0 }, angle,
            Vector3{ kSantaVisualScale, kSantaVisualScale, kSantaVisualScale }, WHITE);
    }
}

static bool RayGroundIntersection(const Camera3D& camera, Vector2 screenPosition, float groundY, Vector3& out) {
    const Ray ray = GetMouseRay(screenPosition, camera);
    if (fabsf(ray.direction.y) < 0.00001f) return false;
    const float distance = (groundY - ray.position.y) / ray.direction.y;
    if (distance < 0.0f) return false;
    out = Vector3Add(ray.position, Vector3Scale(ray.direction, distance));
    return true;
}

static float SnapValue(float value, float step) {
    if (step <= 0.00001f) return value;
    return roundf(value / step) * step;
}

static Vector3 SnapPosition(Vector3 value, float step, bool snapY) {
    if (step <= 0.00001f) return value;
    value.x = SnapValue(value.x, step);
    value.z = SnapValue(value.z, step);
    if (snapY) value.y = SnapValue(value.y, step);
    return value;
}

static BoundingBox TransformBounds(BoundingBox bounds, Matrix transform) {
    BoundingBox result{ Vector3{ FLT_MAX, FLT_MAX, FLT_MAX },
        Vector3{ -FLT_MAX, -FLT_MAX, -FLT_MAX } };
    for (unsigned int corner = 0; corner < 8; ++corner) {
        const Vector3 point{
            (corner & 1u) ? bounds.max.x : bounds.min.x,
            (corner & 2u) ? bounds.max.y : bounds.min.y,
            (corner & 4u) ? bounds.max.z : bounds.min.z
        };
        const Vector3 transformed = Vector3Transform(point, transform);
        result.min = Vector3Min(result.min, transformed);
        result.max = Vector3Max(result.max, transformed);
    }
    return result;
}

static BoundingBox RelativeElementBounds(uint32_t definitionId, uint8_t orientation,
    const LvlxDefinitionTable& definitions, const std::vector<RenderAsset>& assets) {
    // This matches the placeholder cube closely when an asset is unavailable.
    BoundingBox result{ Vector3{ -0.45f, -0.45f, -0.45f },
        Vector3{ 0.45f, 0.45f, 0.45f } };
    if (definitionId >= assets.size() || !assets[definitionId].loaded ||
        assets[definitionId].models.empty()) return result;

    const LvlxElementDefinition* definition =
        lvlx_find_definition(&definitions, definitionId);
    const float scale = definition && definition->scaling > 0.0f
        ? definition->scaling : 1.0f;
    const float angle = orientation <= 3u
        ? -(float)orientation * 90.0f * DEG2RAD : 0.0f;
    const Matrix elementTransform = MatrixMultiply(
        MatrixScale(scale, scale, scale), MatrixRotateY(angle));
    result = BoundingBox{ Vector3{ FLT_MAX, FLT_MAX, FLT_MAX },
        Vector3{ -FLT_MAX, -FLT_MAX, -FLT_MAX } };
    for (const Model& model : assets[definitionId].models) {
        const BoundingBox transformed = TransformBounds(GetModelBoundingBox(model),
            MatrixMultiply(model.transform, elementTransform));
        result.min = Vector3Min(result.min, transformed.min);
        result.max = Vector3Max(result.max, transformed.max);
    }
    return result;
}

static BoundingBox TranslateBounds(BoundingBox bounds, Vector3 position) {
    bounds.min = Vector3Add(bounds.min, position);
    bounds.max = Vector3Add(bounds.max, position);
    return bounds;
}

static bool FootprintsOverlap(const BoundingBox& a, const BoundingBox& b) {
    constexpr float epsilon = 0.001f;
    return a.max.x > b.min.x + epsilon && a.min.x < b.max.x - epsilon &&
        a.max.z > b.min.z + epsilon && a.min.z < b.max.z - epsilon;
}

static Vector3 ResolvePlacementPosition(const LvlxLevel& level,
    const LvlxDefinitionTable& definitions, const std::vector<RenderAsset>& assets,
    uint32_t definitionId, uint8_t orientation, Vector3 groundPosition) {
    const BoundingBox relative = RelativeElementBounds(definitionId, orientation,
        definitions, assets);
    BoundingBox candidate = TranslateBounds(relative, groundPosition);
    float resolvedY = groundPosition.y;
    for (uint32_t i = 0; i < level.element_count; ++i) {
        const LvlxElement& element = level.elements[i];
        if (!element.present) continue;
        const BoundingBox existingRelative = RelativeElementBounds(element.definition_id,
            element.orientation, definitions, assets);
        const BoundingBox existing = TranslateBounds(existingRelative,
            ToRaylib(element.vector1));
        if (FootprintsOverlap(candidate, existing))
            resolvedY = fmaxf(resolvedY, existing.max.y - relative.min.y);
    }
    groundPosition.y = resolvedY;
    return groundPosition;
}

static void DrawActiveGrid(Vector3 center, float spacing = 1.0f,
    int halfCells = 8) {
    center.y += 0.012f;
    const float radius = spacing * (float)halfCells;
    const auto edgeAlpha = [radius](float x, float z) {
        const float normalized = fmaxf(fabsf(x), fabsf(z)) / radius;
        const float fade = std::clamp((1.0f - normalized) / 0.38f, 0.0f, 1.0f);
        return fade * fade * (3.0f - 2.0f * fade);
    };
    for (int line = -halfCells; line <= halfCells; ++line) {
        const float offset = (float)line * spacing;
        for (int segment = -halfCells; segment < halfCells; ++segment) {
            const float start = (float)segment * spacing;
            const float end = start + spacing;
            const float midpoint = (start + end) * 0.5f;
            const float xAlpha = edgeAlpha(midpoint, offset);
            const float zAlpha = edgeAlpha(offset, midpoint);
            if (xAlpha > 0.0f)
                DrawLine3D(Vector3{ center.x + start, center.y, center.z + offset },
                    Vector3{ center.x + end, center.y, center.z + offset },
                    Fade(Color{ 150, 165, 176, 255 }, 0.68f * xAlpha));
            if (zAlpha > 0.0f)
                DrawLine3D(Vector3{ center.x + offset, center.y, center.z + start },
                    Vector3{ center.x + offset, center.y, center.z + end },
                    Fade(Color{ 150, 165, 176, 255 }, 0.68f * zAlpha));
        }
    }
}

static Vector3 EnvironmentDirectionToLight() {
    const auto angles = gEnvironment.numbers(5);
    const float a = angles[0] * DEG2RAD;
    const float b = angles[1] * DEG2RAD;
    Vector3 direction{ -cosf(b) * cosf(a), -sinf(b), cosf(b) * sinf(a) };
    if (Vector3LengthSqr(direction) < 0.000001f)
        direction = Vector3{ 0.35f, 0.82f, 0.45f };
    return Vector3Normalize(direction);
}

static bool SceneBounds(const LvlxLevel& level,
    const LvlxDefinitionTable& definitions, const std::vector<RenderAsset>& assets,
    BoundingBox& result) {
    result = BoundingBox{ Vector3{ FLT_MAX, FLT_MAX, FLT_MAX },
        Vector3{ -FLT_MAX, -FLT_MAX, -FLT_MAX } };
    bool found = false;
    for (uint32_t i = 0; i < level.element_count; ++i) {
        const LvlxElement& element = level.elements[i];
        if (!element.present) continue;
        const BoundingBox relative = RelativeElementBounds(element.definition_id,
            element.orientation, definitions, assets);
        const BoundingBox world = TranslateBounds(relative, ToRaylib(element.vector1));
        result.min = Vector3Min(result.min, world.min);
        result.max = Vector3Max(result.max, world.max);
        found = true;
    }
    return found;
}

static void RenderLiveShadowMap(const LvlxLevel& level,
    const LvlxDefinitionTable& definitions, std::vector<RenderAsset>& assets) {
    gLiveShadowValid = false;
    if (!gLiveShadowMap.id || !gShadowDepthShader.id) return;
    BoundingBox bounds{};
    if (!SceneBounds(level, definitions, assets, bounds)) return;

    const Vector3 center = Vector3Scale(Vector3Add(bounds.min, bounds.max), 0.5f);
    const Vector3 size = Vector3Subtract(bounds.max, bounds.min);
    const float span = fmaxf(12.0f, Vector3Length(size) + 10.0f);
    const Vector3 toLight = EnvironmentDirectionToLight();
    Camera3D lightCamera{};
    lightCamera.target = center;
    lightCamera.position = Vector3Add(center, Vector3Scale(toLight, span * 1.15f));
    lightCamera.up = fabsf(Vector3DotProduct(toLight, Vector3{ 0, 1, 0 })) > 0.94f
        ? Vector3{ 0, 0, 1 } : Vector3{ 0, 1, 0 };
    lightCamera.fovy = span;
    lightCamera.projection = CAMERA_ORTHOGRAPHIC;

    BeginTextureMode(gLiveShadowMap);
    ClearBackground(WHITE);
    BeginMode3D(lightCamera);
    gLiveShadowMatrix = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    for (uint32_t i = 0; i < level.element_count; ++i) {
        const LvlxElement& element = level.elements[i];
        if (!element.present || element.definition_id >= assets.size()) continue;
        RenderAsset& asset = assets[element.definition_id];
        if (!asset.loaded) continue;
        const LvlxElementDefinition* definition =
            lvlx_find_definition(&definitions, element.definition_id);
        const float scale = definition && definition->scaling > 0.0f
            ? definition->scaling : 1.0f;
        const float angle = element.orientation <= 3u
            ? -(float)element.orientation * 90.0f : 0.0f;
        for (size_t modelIndex = 0; modelIndex < asset.models.size(); ++modelIndex) {
            Model& model = asset.models[modelIndex];
            std::vector<Shader> previous;
            previous.reserve((size_t)model.materialCount);
            for (int material = 0; material < model.materialCount; ++material) {
                previous.push_back(model.materials[material].shader);
                model.materials[material].shader = gShadowDepthShader;
            }
            const float cutoff = modelIndex < asset.modelAlphaCutoffs.size()
                ? asset.modelAlphaCutoffs[modelIndex] : 0.0f;
            SetShaderValue(gShadowDepthShader,
                GetShaderLocation(gShadowDepthShader, "alphaCutoff"), &cutoff,
                SHADER_UNIFORM_FLOAT);
            DrawModelEx(model, ToRaylib(element.vector1), Vector3{ 0, 1, 0 }, angle,
                Vector3{ scale, scale, scale }, WHITE);
            for (int material = 0; material < model.materialCount; ++material)
                model.materials[material].shader = previous[(size_t)material];
        }
    }
    EndMode3D();
    EndTextureMode();
    gLiveShadowValid = true;
}

static float DistanceFromRayToPoint(const Ray& ray, Vector3 point, float& rayDistance) {
    const Vector3 toPoint = Vector3Subtract(point, ray.position);
    rayDistance = Vector3DotProduct(toPoint, ray.direction);
    if (rayDistance < 0.0f) return FLT_MAX;
    const Vector3 closest = Vector3Add(ray.position, Vector3Scale(ray.direction, rayDistance));
    return Vector3Distance(closest, point);
}

static Matrix ElementModelTransform(const LvlxElement& element, float scale) {
    const Vector3 position = ToRaylib(element.vector1);
    const float angle = (element.orientation <= 3u
        ? -(float)element.orientation * 90.0f : 0.0f) * DEG2RAD;

    // Match raylib DrawModelEx(): scale -> rotate -> translate.
    const Matrix matScale = MatrixScale(scale, scale, scale);
    const Matrix matRotation = MatrixRotateY(angle);
    const Matrix matTranslation = MatrixTranslate(position.x, position.y, position.z);
    return MatrixMultiply(MatrixMultiply(matScale, matRotation), matTranslation);
}

static int PickElement(const LvlxLevel& level, const LvlxDefinitionTable& definitions,
    const std::vector<RenderAsset>& assets, const Camera3D& camera, Vector2 screenPosition,
    Vector3* closestPoint = nullptr) {
    const Ray ray = GetMouseRay(screenPosition, camera);
    int bestIndex = -1;
    float bestDistance = FLT_MAX;
    Vector3 bestPoint{};

    for (uint32_t i = 0; i < level.element_count; ++i) {
        const LvlxElement& element = level.elements[i];
        if (!element.present) continue;

        const uint32_t renderedDefinitionId = element.definition_id;
        const LvlxElementDefinition* definition =
            lvlx_find_definition(&definitions, renderedDefinitionId);
        const float scale = definition && definition->scaling > 0.0f
            ? definition->scaling : 1.0f;

        bool testedMesh = false;

        // Prefer the exact rendered CRF triangles. This means a small object in
        // front only blocks selection where its visible geometry actually is.
        const auto testAsset = [&](const RenderAsset& asset, float assetScale) {
            if (asset.loaded) {
                const Matrix elementTransform = ElementModelTransform(element, assetScale);

                for (const Model& model : asset.models) {
                    const Matrix transform = MatrixMultiply(model.transform, elementTransform);
                    for (int meshIndex = 0; meshIndex < model.meshCount; ++meshIndex) {
                        testedMesh = true;
                        const RayCollision collision =
                            GetRayCollisionMesh(ray, model.meshes[meshIndex], transform);
                        if (collision.hit && collision.distance >= 0.0f &&
                            collision.distance < bestDistance) {
                            bestDistance = collision.distance;
                            bestIndex = (int)i;
                            bestPoint = collision.point;
                        }
                    }
                }
            }
        };
        if (renderedDefinitionId < assets.size()) {
            testAsset(assets[renderedDefinitionId], scale);
        }

        if (testedMesh) continue;

        // Unsupported/missing CRFs are drawn as a small placeholder cube. Pick
        // approximately that cube rather than using the old broad scale radius.
        const Vector3 position = ToRaylib(element.vector1);
        const float halfSize = 0.42f;
        const BoundingBox box{
            Vector3{ position.x - halfSize, position.y - halfSize, position.z - halfSize },
            Vector3{ position.x + halfSize, position.y + halfSize, position.z + halfSize }
        };
        const RayCollision collision = GetRayCollisionBox(ray, box);
        if (collision.hit && collision.distance >= 0.0f &&
            collision.distance < bestDistance) {
            bestDistance = collision.distance;
            bestIndex = (int)i;
            bestPoint = collision.point;
        }
    }

    if (bestIndex >= 0 && closestPoint) *closestPoint = bestPoint;
    return bestIndex;
}

static bool PlacementAnchor(const LvlxLevel& level,
    const LvlxDefinitionTable& definitions, const std::vector<RenderAsset>& assets,
    const Camera3D& camera, Vector2 screenPosition, Vector3& out) {
    Vector3 surfacePoint{};
    if (PickElement(level, definitions, assets, camera, screenPosition,
            &surfacePoint) >= 0) {
        // Preserve the cursor's exact horizontal location on the hovered mesh;
        // ResolvePlacementPosition supplies the non-overlapping vertical level.
        out = Vector3{ surfacePoint.x, 0.0f, surfacePoint.z };
        return true;
    }
    return RayGroundIntersection(camera, screenPosition, 0.0f, out);
}

static void DrawElementPreview(uint32_t definitionId, Vector3 position, uint8_t orientation,
    const LvlxDefinitionTable& definitions, std::vector<RenderAsset>& assets,
    const std::string& assetsDirectory, const std::string& texturesDirectory,
    unsigned& loadedCount, unsigned& missingCount,const EffectPreview* effectLights=nullptr) {
    const LvlxElementDefinition* definition = lvlx_find_definition(&definitions, definitionId);
    bool rendered = false;
    if (definition&& definitionId < assets.size() && definition->resource) {
        RenderAsset& asset = assets[definitionId];
        if (!asset.attempted) {
            if (LoadRenderAsset(asset, *definition, assetsDirectory, texturesDirectory)) ++loadedCount;
            else ++missingCount;
        }
        if (asset.loaded) {
            const float scale = definition->scaling > 0 ? definition->scaling : 1.0f;
            const float angle = orientation <= 3u ? -(float)orientation * 90.0f : 0.0f;
            for (size_t modelIndex = 0; modelIndex < asset.models.size(); ++modelIndex) {
                Model& model = asset.models[modelIndex];
                SetAlphaCutoff(asset, modelIndex);
                SetOverlayEnabled(asset, modelIndex);
                SetLightmapBinding(model, LevelLightmaps{}, UINT32_MAX, UINT32_MAX);
                BindObjectPointLights(effectLights,model,position,angle,scale);
                DrawModelEx(model, position, Vector3{ 0, 1, 0 }, angle,
                    Vector3{ scale, scale, scale }, Fade(WHITE, 0.55f));
            }
            rendered = true;
        }
    }
    if (!rendered) {
        const Color color = CategoryColor(definition ? definition->category : nullptr);
        DrawCubeWires(position, 0.9f, 0.9f, 0.9f, color);
    }
    DrawCircle3D(position, 0.55f, Vector3{ 1,0,0 }, 90.0f, Fade(YELLOW, 0.8f));
}

static int BuildDefinitionList(const LvlxDefinitionTable& definitions,
    const std::string& search, std::vector<uint32_t>& out) {
    out.clear();
    const std::string needle = Lowercase(search);
    for (uint32_t i = 0; i < definitions.count; ++i) {
        const LvlxElementDefinition& d = definitions.items[i];
        if (!d.name || !d.name[0]) continue;
        const std::string searchable = Lowercase(std::to_string(i) + " " + d.name + " " +
            (d.category ? d.category : "") + " " + (d.resource ? d.resource : ""));
        if (needle.empty() || searchable.find(needle) != std::string::npos) out.push_back(i);
    }
    return (int)out.size();
}

static bool DefinitionSupportsWaylists(const LvlxElementDefinition* definition) {
    if (!definition) return false;
    if (definition->name && std::strcmp(definition->name,"TD_Enemy_Spawn")==0) return true;
    return definition->waylist_enabled != 0 &&
        std::isfinite(definition->speed) && definition->speed > 0.0f;
}

static uint32_t ReadLe32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float ReadLeFloat(const unsigned char* p) {
    const uint32_t bits = ReadLe32(p);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t FingerprintBytes(const unsigned char* bytes, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t FingerprintFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return 0;
    uint64_t hash = 1469598103934665603ull;
    std::array<unsigned char, 65536> buffer{};
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const std::streamsize count = stream.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= buffer[(size_t)i];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

static void UnloadLevelLightmaps(LevelLightmaps& lightmaps) {
    for (Texture2D texture : lightmaps.textures)
        if (texture.id) UnloadTexture(texture);
    lightmaps = LevelLightmaps{};
}

static Texture2D LoadDxt1Lightmap(const std::string& path) {
    Texture2D texture{};
    FILE* file = nullptr;
#ifdef _MSC_VER
    fopen_s(&file, path.c_str(), "rb");
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (!file) return texture;
    unsigned char header[128];
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, "DDS ", 4) != 0 ||
        std::memcmp(header + 84, "DXT1", 4) != 0) {
        std::fclose(file); return texture;
    }
    const uint32_t height = ReadLe32(header + 12);
    const uint32_t width = ReadLe32(header + 16);
    const size_t payloadSize = ((size_t)width + 3u) / 4u *
        (((size_t)height + 3u) / 4u) * 8u;
    std::vector<unsigned char> payload(payloadSize);
    if (!width || !height || std::fread(payload.data(), 1, payloadSize, file) != payloadSize) {
        std::fclose(file); return texture;
    }
    std::fclose(file);
    texture.id = rlLoadTexture(payload.data(), (int)width, (int)height,
        RL_PIXELFORMAT_COMPRESSED_DXT1_RGB, 1);
    texture.width = (int)width;
    texture.height = (int)height;
    texture.mipmaps = 1;
    texture.format = PIXELFORMAT_COMPRESSED_DXT1_RGB;
    return texture;
}

static bool LoadLevelLightmaps(const std::string& levelPath,
    const std::string& texturesDirectory, LevelLightmaps& result) {
    UnloadLevelLightmaps(result);
    std::filesystem::path lmdPath(levelPath);
    lmdPath.replace_extension(".lmd");
    FILE* file = nullptr;
#ifdef _MSC_VER
    fopen_s(&file, lmdPath.string().c_str(), "rb");
#else
    file = std::fopen(lmdPath.string().c_str(), "rb");
#endif
    if (!file) { result.valid = true; return true; }
    result.present = true;
    const auto fail = [&]() {
        UnloadLevelLightmaps(result);
        result.present = true;
        return false;
    };
    if (std::fseek(file, 0, SEEK_END) != 0) { std::fclose(file); return fail(); }
    const long length = std::ftell(file);
    if (length < 8 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file); return fail();
    }
    std::vector<unsigned char> bytes((size_t)length);
    if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
        std::fclose(file); return fail();
    }
    std::fclose(file);
    result.lmdFingerprint = FingerprintBytes(bytes.data(), bytes.size());
    size_t position = 0;
    auto take32 = [&](uint32_t& value) -> bool {
        if (bytes.size() - position < 4) return false;
        value = ReadLe32(bytes.data() + position); position += 4; return true;
    };
    uint32_t recordCount = 0;
    if (!take32(recordCount) || recordCount > (bytes.size() - position) / 28u)
        return fail();
    result.records.reserve(recordCount);
    for (uint32_t i = 0; i < recordCount; ++i) {
        const unsigned char* record = bytes.data() + position;
        result.records.push_back(LmdRecord{ ReadLe32(record), ReadLe32(record + 4),
            ReadLeFloat(record + 8), ReadLeFloat(record + 12),
            ReadLeFloat(record + 16), ReadLeFloat(record + 20),
            ReadLe32(record + 24) });
        const LmdRecord& added = result.records.back();
        const uint64_t lookupKey = ((uint64_t)added.attachmentKey << 32) |
            added.subsetIdentifier;
        result.recordLookup.emplace(lookupKey, result.records.size() - 1u);
        position += 28;
    }
    uint32_t stringCount = 0;
    if (!take32(stringCount) || stringCount > (bytes.size() - position) / 4u)
        return fail();
    result.serializedNames.reserve(stringCount);
    const std::string stem = lmdPath.stem().string();
    for (uint32_t i = 0; i < stringCount; ++i) {
        uint32_t byteLength = 0;
        if (!take32(byteLength) || byteLength > bytes.size() - position) return fail();
        result.serializedNames.emplace_back(bytes.begin() + position,
            bytes.begin() + position + byteLength);
        position += byteLength;
        const std::string logical = TextFormat("lmaps_%s_%02u", stem.c_str(), i);
        const std::string texturePath = ResolveResourceFile(texturesDirectory,gFallbackTexturesDirectory,logical + ".dds");
        Texture2D texture{};
        if (FileExists(texturePath.c_str())) {
            texture = LoadDxt1Lightmap(texturePath);
            if (texture.id) {
                SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
                SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
            }
        }
        result.textures.push_back(texture);
        result.textureFingerprints.push_back(FingerprintFile(texturePath));
    }
    result.valid = position == bytes.size();
    if (!result.valid) return fail();
    return result.valid;
}

static void AppendLe32(std::vector<unsigned char>& bytes, uint32_t value) {
    bytes.push_back((unsigned char)value);
    bytes.push_back((unsigned char)(value >> 8u));
    bytes.push_back((unsigned char)(value >> 16u));
    bytes.push_back((unsigned char)(value >> 24u));
}

static void AppendLeFloat(std::vector<unsigned char>& bytes, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendLe32(bytes, bits);
}

static std::vector<unsigned char> SerializeLmd(const LevelLightmaps& lightmaps) {
    std::vector<unsigned char> bytes;
    bytes.reserve(8u + lightmaps.records.size() * 28u);
    AppendLe32(bytes, (uint32_t)lightmaps.records.size());
    for (const LmdRecord& record : lightmaps.records) {
        AppendLe32(bytes, record.attachmentKey);
        AppendLe32(bytes, record.subsetIdentifier);
        AppendLeFloat(bytes, record.offsetU);
        AppendLeFloat(bytes, record.offsetV);
        AppendLeFloat(bytes, record.scaleU);
        AppendLeFloat(bytes, record.scaleV);
        AppendLe32(bytes, record.textureSlot);
    }
    AppendLe32(bytes, (uint32_t)lightmaps.serializedNames.size());
    for (const auto& name : lightmaps.serializedNames) {
        AppendLe32(bytes, (uint32_t)name.size());
        bytes.insert(bytes.end(), name.begin(), name.end());
    }
    return bytes;
}

static uint16_t GrayTo565(unsigned char value) {
    const uint16_t r = (uint16_t)((value * 31u + 127u) / 255u);
    const uint16_t g = (uint16_t)((value * 63u + 127u) / 255u);
    const uint16_t b = (uint16_t)((value * 31u + 127u) / 255u);
    return (uint16_t)((r << 11u) | (g << 5u) | b);
}

static unsigned char GreenFrom565(uint16_t value) {
    return (unsigned char)((((value >> 5u) & 63u) * 255u + 31u) / 63u);
}

static std::vector<unsigned char> EncodeDxt1Grayscale(
    const std::vector<unsigned char>& values, uint32_t width, uint32_t height) {
    const uint32_t blocksWide = (width + 3u) / 4u;
    const uint32_t blocksHigh = (height + 3u) / 4u;
    std::vector<unsigned char> encoded((size_t)blocksWide * blocksHigh * 8u);
    for (uint32_t by = 0; by < blocksHigh; ++by) {
        for (uint32_t bx = 0; bx < blocksWide; ++bx) {
            unsigned char minimum = 255, maximum = 0;
            unsigned char samples[16]{};
            for (uint32_t py = 0; py < 4; ++py) for (uint32_t px = 0; px < 4; ++px) {
                const uint32_t x = std::min(bx * 4u + px, width - 1u);
                const uint32_t y = std::min(by * 4u + py, height - 1u);
                const unsigned char value = values[(size_t)y * width + x];
                samples[py * 4u + px] = value;
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
            uint16_t color0 = GrayTo565(maximum);
            uint16_t color1 = GrayTo565(minimum);
            if (color0 <= color1 && color0 < UINT16_MAX) ++color0;
            const unsigned char palette[4]{ GreenFrom565(color0), GreenFrom565(color1),
                (unsigned char)((2u * GreenFrom565(color0) + GreenFrom565(color1)) / 3u),
                (unsigned char)((GreenFrom565(color0) + 2u * GreenFrom565(color1)) / 3u) };
            uint32_t indices = 0;
            for (uint32_t pixel = 0; pixel < 16; ++pixel) {
                unsigned int best = 0, bestDistance = UINT_MAX;
                for (unsigned int choice = 0; choice < 4; ++choice) {
                    const unsigned int distance = (unsigned int)std::abs(
                        (int)samples[pixel] - (int)palette[choice]);
                    if (distance < bestDistance) { bestDistance = distance; best = choice; }
                }
                indices |= best << (pixel * 2u);
            }
            unsigned char* block = encoded.data() +
                ((size_t)by * blocksWide + bx) * 8u;
            block[0] = (unsigned char)color0; block[1] = (unsigned char)(color0 >> 8u);
            block[2] = (unsigned char)color1; block[3] = (unsigned char)(color1 >> 8u);
            block[4] = (unsigned char)indices; block[5] = (unsigned char)(indices >> 8u);
            block[6] = (unsigned char)(indices >> 16u); block[7] = (unsigned char)(indices >> 24u);
        }
    }
    return encoded;
}

static bool ReadDdsHeader(const std::string& path,
    std::array<unsigned char, 128>& header, uint32_t& width, uint32_t& height) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        std::memcmp(header.data(), "DDS ", 4) != 0 ||
        std::memcmp(header.data() + 84, "DXT1", 4) != 0) return false;
    height = ReadLe32(header.data() + 12);
    width = ReadLe32(header.data() + 16);
    return width && height && width <= 8192u && height <= 8192u;
}

struct PendingFileReplacement {
    std::string path;
    std::vector<unsigned char> bytes;
};

static bool ReplaceFilesWithBackups(const std::vector<PendingFileReplacement>& files,
    std::string& message) {
    namespace fs = std::filesystem;
    struct Prepared { fs::path path, temporary, backup; bool hadOriginal = false; };
    std::vector<Prepared> prepared;
    std::error_code ec;
    const auto cleanupPrepared = [&]() {
        for (const Prepared& item : prepared) {
            std::error_code cleanup;
            fs::remove(item.temporary, cleanup);
        }
    };
    for (const auto& file : files) {
        Prepared item;
        item.path = file.path;
        item.temporary = file.path + ".santamapper.tmp";
        if (fs::exists(item.temporary, ec) || ec) {
            cleanupPrepared();
            message = "Lighting temporary file already exists: " + item.temporary.string();
            return false;
        }
        std::ofstream output(item.temporary, std::ios::binary);
        output.write(reinterpret_cast<const char*>(file.bytes.data()),
            (std::streamsize)file.bytes.size());
        output.close();
        if (!output) {
            fs::remove(item.temporary, ec);
            cleanupPrepared();
            message = "Could not write lighting temporary file: " + item.path.string();
            return false;
        }
        item.hadOriginal = fs::exists(item.path, ec) && !ec;
        item.backup = file.path + ".santamapper.bak";
        for (unsigned int suffix = 1; fs::exists(item.backup, ec) && !ec; ++suffix)
            item.backup = file.path + ".santamapper.bak." + std::to_string(suffix);
        if (ec) {
            fs::remove(item.temporary, ec);
            cleanupPrepared();
            message = "Could not inspect lighting backup path";
            return false;
        }
        prepared.push_back(std::move(item));
    }

    size_t committed = 0;
    for (; committed < prepared.size(); ++committed) {
        Prepared& item = prepared[committed];
        if (item.hadOriginal) fs::rename(item.path, item.backup, ec);
        if (!ec) fs::rename(item.temporary, item.path, ec);
        if (ec) {
            if (item.hadOriginal && !fs::exists(item.path)) {
                std::error_code restore;
                fs::rename(item.backup, item.path, restore);
            }
            break;
        }
    }
    if (committed != prepared.size()) {
        for (size_t i = committed; i-- > 0;) {
            Prepared& item = prepared[i];
            std::error_code rollback;
            fs::remove(item.path, rollback);
            if (item.hadOriginal) fs::rename(item.backup, item.path, rollback);
        }
        for (Prepared& item : prepared) {
            std::error_code cleanup;
            fs::remove(item.temporary, cleanup);
        }
        message = "Lighting replacement failed; originals were restored";
        return false;
    }
    message = "lightmaps rebuilt with recoverable backups";
    return true;
}

static const LvlxElement* FindLmdElement(const LvlxLevel& level, uint32_t key) {
    uint32_t runtimeIndex = 0;
    for (uint32_t i = 0; i < level.element_count; ++i) {
        const LvlxElement& element = level.elements[i];
        if (!element.present) continue;
        const uint32_t elementKey = level.version > 2u
            ? element.lightmap_attachment_key : runtimeIndex;
        if (elementKey == key) return &element;
        ++runtimeIndex;
    }
    return nullptr;
}

static float Edge2D(Vector2 a, Vector2 b, Vector2 p) {
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

static unsigned char SampleShadowVisibility(const Color* shadowPixels,
    int width, int height, Vector3 worldPosition) {
    const Matrix& m = gLiveShadowMatrix;
    const float clipX = m.m0 * worldPosition.x + m.m4 * worldPosition.y +
        m.m8 * worldPosition.z + m.m12;
    const float clipY = m.m1 * worldPosition.x + m.m5 * worldPosition.y +
        m.m9 * worldPosition.z + m.m13;
    const float clipZ = m.m2 * worldPosition.x + m.m6 * worldPosition.y +
        m.m10 * worldPosition.z + m.m14;
    const float clipW = m.m3 * worldPosition.x + m.m7 * worldPosition.y +
        m.m11 * worldPosition.z + m.m15;
    if (clipW <= 0.00001f) return 255;
    const float u = clipX / clipW * 0.5f + 0.5f;
    const float v = clipY / clipW * 0.5f + 0.5f;
    const float depth = clipZ / clipW * 0.5f + 0.5f;
    if (u < 0 || u > 1 || v < 0 || v > 1 || depth < 0 || depth > 1) return 255;
    const int centerX = std::clamp((int)(u * width), 0, width - 1);
    const int centerY = std::clamp((int)(v * height), 0, height - 1);
    unsigned int visible = 0, samples = 0;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        const int sx = std::clamp(centerX + x, 0, width - 1);
        const int sy = std::clamp(centerY + y, 0, height - 1);
        const float casterDepth = shadowPixels[(size_t)sy * width + sx].r / 255.0f;
        visible += depth - 0.006f <= casterDepth ? 1u : 0u;
        ++samples;
    }
    // Preserve some indirect/ambient visibility in fully occluded texels.
    return (unsigned char)(64u + (191u * visible + samples / 2u) / samples);
}

static bool BakeLevelLightmaps(const LvlxLevel& level,
    const LvlxDefinitionTable& definitions, std::vector<RenderAsset>& assets,
    const std::string& assetsDirectory, const std::string& texturesDirectory,
    const std::string& levelPath, const LevelLightmaps& lightmaps,
    std::string& message) {
    if (!lightmaps.valid || lightmaps.records.empty() ||
        lightmaps.serializedNames.empty()) {
        message = "No valid existing LMD atlas layout is available to rebuild";
        return false;
    }
    std::filesystem::path lmdPath(levelPath);
    lmdPath.replace_extension(".lmd");
    const std::string stem = lmdPath.stem().string();
    if (FingerprintFile(lmdPath.string()) != lightmaps.lmdFingerprint) {
        message = "LMD changed externally since it was loaded; reload before baking";
        return false;
    }
    for (size_t slot = 0; slot < lightmaps.serializedNames.size(); ++slot) {
        const std::string texturePath = ResolveResourceFile(texturesDirectory,gFallbackTexturesDirectory,
            TextFormat("lmaps_%s_%02u.dds", stem.c_str(), (unsigned)slot));
        if (slot >= lightmaps.textureFingerprints.size() ||
            FingerprintFile(texturePath) != lightmaps.textureFingerprints[slot]) {
            message = "Lightmap atlas changed externally since load: " + texturePath;
            return false;
        }
    }
    for (uint32_t i = 0; i < level.element_count; ++i) {
        const LvlxElement& element = level.elements[i];
        if (!element.present || element.definition_id >= assets.size()) continue;
        const LvlxElementDefinition* definition =
            lvlx_find_definition(&definitions, element.definition_id);
        RenderAsset& asset = assets[element.definition_id];
        if (!asset.attempted && definition && definition->resource)
            LoadRenderAsset(asset, *definition, assetsDirectory, texturesDirectory);
    }

    LevelLightmaps authored;
    authored.valid = true;
    authored.present = true;
    authored.serializedNames = lightmaps.serializedNames;
    // Removed elements no longer need mappings. Preserve every surviving
    // record and its original atlas rectangle bit-for-bit.
    for (const LmdRecord& record : lightmaps.records)
        if (FindLmdElement(level, record.attachmentKey))
            authored.records.push_back(record);

    std::unordered_set<uint64_t> mapped;
    for (const LmdRecord& record : authored.records)
        mapped.insert(((uint64_t)record.attachmentKey << 32u) |
            record.subsetIdentifier);
    const size_t originalAtlasCount = authored.serializedNames.size();
    size_t newChartIndex = 0;
    uint32_t runtimeIndex = 0;
    for (uint32_t elementIndex = 0; elementIndex < level.element_count; ++elementIndex) {
        const LvlxElement& element = level.elements[elementIndex];
        if (!element.present) continue;
        const uint32_t attachmentKey = level.version > 2u
            ? element.lightmap_attachment_key : runtimeIndex;
        ++runtimeIndex;
        const LvlxElementDefinition* definition =
            lvlx_find_definition(&definitions, element.definition_id);
        if (!definition || !definition->set_static || element.definition_id >= assets.size())
            continue;
        const RenderAsset& asset = assets[element.definition_id];
        for (size_t modelIndex = 0; modelIndex < asset.models.size() &&
                modelIndex < asset.subsetIdentifiers.size() &&
                modelIndex < asset.modelLightmapUvs.size(); ++modelIndex) {
            const uint32_t subset = asset.subsetIdentifiers[modelIndex];
            const uint64_t key = ((uint64_t)attachmentKey << 32u) | subset;
            if (mapped.contains(key) || asset.modelLightmapUvs[modelIndex].empty()) continue;
            float minU = FLT_MAX, minV = FLT_MAX, maxU = -FLT_MAX, maxV = -FLT_MAX;
            for (Vector2 uv : asset.modelLightmapUvs[modelIndex]) {
                minU = fminf(minU, uv.x); minV = fminf(minV, uv.y);
                maxU = fmaxf(maxU, uv.x); maxV = fmaxf(maxV, uv.y);
            }
            const float rangeU = maxU - minU, rangeV = maxV - minV;
            if (!std::isfinite(rangeU) || !std::isfinite(rangeV) ||
                rangeU < 0.000001f || rangeV < 0.000001f) continue;
            constexpr size_t cellsPerRow = 16;
            constexpr float atlasSize = 2048.0f;
            constexpr float cellSize = 128.0f;
            constexpr float padding = 4.0f;
            constexpr float contentSize = cellSize - padding * 2.0f;
            const size_t atlasOffset = newChartIndex /
                (cellsPerRow * cellsPerRow);
            const size_t cell = newChartIndex % (cellsPerRow * cellsPerRow);
            const size_t textureSlot = originalAtlasCount + atlasOffset;
            while (authored.serializedNames.size() <= textureSlot) {
                const std::string logical = TextFormat("lmaps_%s_%02u",
                    std::filesystem::path(levelPath).stem().string().c_str(),
                    (unsigned)authored.serializedNames.size());
                authored.serializedNames.emplace_back(logical.begin(), logical.end());
            }
            const float originU = ((float)(cell % cellsPerRow) * cellSize + padding) /
                atlasSize;
            const float originV = ((float)(cell / cellsPerRow) * cellSize + padding) /
                atlasSize;
            const float scaleU = (contentSize / atlasSize) / rangeU;
            const float scaleV = (contentSize / atlasSize) / rangeV;
            authored.records.push_back(LmdRecord{ attachmentKey, subset,
                originU - minU * scaleU, originV - minV * scaleV,
                scaleU, scaleV, (uint32_t)textureSlot });
            mapped.insert(key);
            ++newChartIndex;
        }
    }
    RenderLiveShadowMap(level, definitions, assets);
    if (!gLiveShadowValid) {
        message = "Could not render the live shadow source for baking";
        return false;
    }
    Image shadowImage = LoadImageFromTexture(gLiveShadowMap.texture);
    Color* shadowPixels = LoadImageColors(shadowImage);
    if (!shadowPixels) {
        UnloadImage(shadowImage);
        message = "Could not read the live shadow source";
        return false;
    }

    struct AtlasBake {
        std::string path;
        std::array<unsigned char, 128> header{};
        uint32_t width = 0, height = 0;
        std::vector<unsigned char> light;
        std::vector<unsigned char> covered;
    };
    std::vector<AtlasBake> atlases(authored.serializedNames.size());
    bool initialized = true;
    for (size_t slot = 0; slot < atlases.size(); ++slot) {
        AtlasBake& atlas = atlases[slot];
        atlas.path = JoinPath(texturesDirectory,
            TextFormat("lmaps_%s_%02u.dds", stem.c_str(), (unsigned)slot));
        if (slot < originalAtlasCount) {
            if (!ReadDdsHeader(atlas.path, atlas.header, atlas.width, atlas.height)) {
                initialized = false;
                message = "Missing or unsupported DXT1 lightmap atlas: " + atlas.path;
                break;
            }
        }
        else {
            atlas.header = atlases[0].header;
            atlas.width = 2048u;
            atlas.height = 2048u;
            // New atlases use the shipped 2048x2048 one-mip DXT1 shape.
            atlas.header[12] = 0; atlas.header[13] = 8;
            atlas.header[14] = 0; atlas.header[15] = 0;
            atlas.header[16] = 0; atlas.header[17] = 8;
            atlas.header[18] = 0; atlas.header[19] = 0;
        }
        atlas.light.assign((size_t)atlas.width * atlas.height, 255u);
        atlas.covered.assign(atlas.light.size(), 0u);
    }
    if (!initialized) {
        UnloadImageColors(shadowPixels); UnloadImage(shadowImage); return false;
    }

    size_t unresolved = 0;
    for (const LmdRecord& record : authored.records) {
        if (record.textureSlot >= atlases.size()) { ++unresolved; continue; }
        const LvlxElement* element = FindLmdElement(level, record.attachmentKey);
        if (!element || element->definition_id >= assets.size()) { ++unresolved; continue; }
        const RenderAsset& asset = assets[element->definition_id];
        size_t modelIndex = SIZE_MAX;
        for (size_t i = 0; i < asset.subsetIdentifiers.size(); ++i)
            if (asset.subsetIdentifiers[i] == record.subsetIdentifier) {
                modelIndex = i; break;
            }
        // The recovered loader keeps ordinal zero when a subset identifier has
        // no exact match, then applies the record to that first subset.
        if (modelIndex == SIZE_MAX && !asset.models.empty()) modelIndex = 0;
        if (modelIndex >= asset.models.size() ||
            modelIndex >= asset.modelLightmapUvs.size()) { ++unresolved; continue; }
        const Model& model = asset.models[modelIndex];
        if (model.meshCount < 1) { ++unresolved; continue; }
        const Mesh& mesh = model.meshes[0];
        const auto& uvs = asset.modelLightmapUvs[modelIndex];
        if (!mesh.vertices || uvs.size() < (size_t)mesh.vertexCount) {
            ++unresolved; continue;
        }
        const LvlxElementDefinition* definition =
            lvlx_find_definition(&definitions, element->definition_id);
        const float scale = definition && definition->scaling > 0.0f
            ? definition->scaling : 1.0f;
        const Matrix worldTransform = MatrixMultiply(model.transform,
            ElementModelTransform(*element, scale));
        AtlasBake& atlas = atlases[record.textureSlot];
        for (int triangle = 0; triangle < mesh.triangleCount; ++triangle) {
            unsigned int indices[3];
            for (unsigned int corner = 0; corner < 3; ++corner)
                indices[corner] = mesh.indices ? mesh.indices[triangle * 3 + corner]
                    : (unsigned int)triangle * 3u + corner;
            if (indices[0] >= (unsigned)mesh.vertexCount ||
                indices[1] >= (unsigned)mesh.vertexCount ||
                indices[2] >= (unsigned)mesh.vertexCount) continue;
            Vector2 atlasPoint[3];
            Vector3 worldPoint[3];
            for (unsigned int corner = 0; corner < 3; ++corner) {
                const Vector2 uv = uvs[indices[corner]];
                atlasPoint[corner] = Vector2{
                    (uv.x * record.scaleU + record.offsetU) * atlas.width,
                    (uv.y * record.scaleV + record.offsetV) * atlas.height };
                const float* vertex = mesh.vertices + indices[corner] * 3u;
                worldPoint[corner] = Vector3Transform(
                    Vector3{ vertex[0], vertex[1], vertex[2] }, worldTransform);
            }
            const float area = Edge2D(atlasPoint[0], atlasPoint[1], atlasPoint[2]);
            if (fabsf(area) < 0.00001f) continue;
            const int minX = std::clamp((int)floorf(fminf(atlasPoint[0].x,
                fminf(atlasPoint[1].x, atlasPoint[2].x))), 0, (int)atlas.width - 1);
            const int maxX = std::clamp((int)ceilf(fmaxf(atlasPoint[0].x,
                fmaxf(atlasPoint[1].x, atlasPoint[2].x))), 0, (int)atlas.width - 1);
            const int minY = std::clamp((int)floorf(fminf(atlasPoint[0].y,
                fminf(atlasPoint[1].y, atlasPoint[2].y))), 0, (int)atlas.height - 1);
            const int maxY = std::clamp((int)ceilf(fmaxf(atlasPoint[0].y,
                fmaxf(atlasPoint[1].y, atlasPoint[2].y))), 0, (int)atlas.height - 1);
            for (int y = minY; y <= maxY; ++y) for (int x = minX; x <= maxX; ++x) {
                const Vector2 sample{ x + 0.5f, y + 0.5f };
                const float w0 = Edge2D(atlasPoint[1], atlasPoint[2], sample) / area;
                const float w1 = Edge2D(atlasPoint[2], atlasPoint[0], sample) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -0.0001f || w1 < -0.0001f || w2 < -0.0001f) continue;
                const Vector3 world = Vector3Add(Vector3Scale(worldPoint[0], w0),
                    Vector3Add(Vector3Scale(worldPoint[1], w1),
                        Vector3Scale(worldPoint[2], w2)));
                const size_t pixel = (size_t)y * atlas.width + x;
                atlas.light[pixel] = SampleShadowVisibility(shadowPixels,
                    shadowImage.width, shadowImage.height, world);
                atlas.covered[pixel] = 1;
            }
        }
    }
    UnloadImageColors(shadowPixels);
    UnloadImage(shadowImage);
    if (unresolved) {
        message = TextFormat("Bake stopped safely: %u LMD record(s) could not be mapped",
            (unsigned)unresolved);
        return false;
    }

    size_t coveredTexels = 0;
    size_t shadowedTexels = 0;
    unsigned int minimumVisibility = 255;
    unsigned int maximumVisibility = 0;
    for (const AtlasBake& atlas : atlases) {
        for (size_t pixel = 0; pixel < atlas.covered.size(); ++pixel) {
            if (!atlas.covered[pixel]) continue;
            const unsigned int visibility = atlas.light[pixel];
            ++coveredTexels;
            if (visibility < 250u) ++shadowedTexels;
            minimumVisibility = std::min(minimumVisibility, visibility);
            maximumVisibility = std::max(maximumVisibility, visibility);
        }
    }
    if (!coveredTexels) {
        message = "Bake stopped safely: no lightmap UV texels were rasterized";
        return false;
    }

    // Two-texel dilation prevents dark compression seams outside UV islands.
    for (AtlasBake& atlas : atlases) for (int pass = 0; pass < 2; ++pass) {
        std::vector<unsigned char> nextLight = atlas.light;
        std::vector<unsigned char> nextCovered = atlas.covered;
        for (uint32_t y = 0; y < atlas.height; ++y) for (uint32_t x = 0; x < atlas.width; ++x) {
            const size_t pixel = (size_t)y * atlas.width + x;
            if (atlas.covered[pixel]) continue;
            unsigned int sum = 0, count = 0;
            for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) {
                const int sx = (int)x + ox, sy = (int)y + oy;
                if (sx < 0 || sy < 0 || sx >= (int)atlas.width || sy >= (int)atlas.height) continue;
                const size_t neighbor = (size_t)sy * atlas.width + sx;
                if (atlas.covered[neighbor]) { sum += atlas.light[neighbor]; ++count; }
            }
            if (count) { nextLight[pixel] = (unsigned char)(sum / count); nextCovered[pixel] = 1; }
        }
        atlas.light.swap(nextLight); atlas.covered.swap(nextCovered);
    }

    std::vector<PendingFileReplacement> replacements;
    replacements.push_back(PendingFileReplacement{ lmdPath.string(), SerializeLmd(authored) });
    for (AtlasBake& atlas : atlases) {
        std::vector<unsigned char> bytes(atlas.header.begin(), atlas.header.end());
        std::vector<unsigned char> payload =
            EncodeDxt1Grayscale(atlas.light, atlas.width, atlas.height);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        replacements.push_back(PendingFileReplacement{ atlas.path, std::move(bytes) });
    }
    std::string replacementMessage;
    if (!ReplaceFilesWithBackups(replacements, replacementMessage)) {
        message = replacementMessage;
        return false;
    }
    message = TextFormat("%s; %llu texels baked, %llu shadowed (visibility %u-%u)",
        replacementMessage.c_str(), (unsigned long long)coveredTexels,
        (unsigned long long)shadowedTexels, minimumVisibility, maximumVisibility);
    return true;
}

static bool DefinitionUsesDestination(const LvlxElementDefinition* definition) {
    return definition && definition->endpoint_movement_enabled != 0 &&
        std::isfinite(definition->speed) && definition->speed > 0.0f;
}

static LvlxWaylist* FindWaylist(LvlxLevel& level, uint32_t index) {
    for (uint32_t i = 0; i < level.waylist_count; ++i)
        if (level.waylists[i].index == index) return &level.waylists[i];
    return nullptr;
}

static LvlxWaylist* EnsureElementWaylist(LvlxLevel& level, LvlxElement& element) {
    if (LvlxWaylist* existing = FindWaylist(level, element.waylist_index)) return existing;
    if (level.waylist_count == UINT32_MAX) return nullptr;

    uint32_t nextIndex = 0;
    for (uint32_t i = 0; i < level.waylist_count; ++i)
        if (level.waylists[i].index >= nextIndex && level.waylists[i].index != UINT32_MAX)
            nextIndex = level.waylists[i].index + 1u;

    void* resized = std::realloc(level.waylists,
        ((size_t)level.waylist_count + 1u) * sizeof(*level.waylists));
    if (!resized) return nullptr;
    level.waylists = static_cast<LvlxWaylist*>(resized);
    LvlxWaylist& waylist = level.waylists[level.waylist_count++];
    waylist = LvlxWaylist{};
    waylist.index = nextIndex;
    element.waylist_index = nextIndex;
    return &waylist;
}

static bool AppendWaypoint(LvlxWaylist& waylist, LvlxVec3 point) {
    if (waylist.point_count == UINT16_MAX) return false;
    void* resized = std::realloc(waylist.points,
        ((size_t)waylist.point_count + 1u) * sizeof(*waylist.points));
    if (!resized) return false;
    waylist.points = static_cast<LvlxVec3*>(resized);
    waylist.points[waylist.point_count++] = point;
    return true;
}

static bool RemoveWaypoint(LvlxWaylist& waylist, uint16_t pointIndex) {
    if (pointIndex >= waylist.point_count) return false;
    if (pointIndex + 1u < waylist.point_count) {
        std::memmove(waylist.points + pointIndex, waylist.points + pointIndex + 1u,
            ((size_t)waylist.point_count - pointIndex - 1u) * sizeof(*waylist.points));
    }
    --waylist.point_count;
    return true;
}

static std::string CompanionLmdPath(const std::string& levelPath) {
    std::filesystem::path path(levelPath);
    path.replace_extension(".lmd");
    return NormalizePath(path.string());
}

static std::unordered_set<uint32_t> LoadLmdAttachmentKeys(const std::string& levelPath) {
    std::unordered_set<uint32_t> keys;
    FILE* file = nullptr;
    const std::string companionPath = CompanionLmdPath(levelPath);
#ifdef _MSC_VER
    fopen_s(&file, companionPath.c_str(), "rb");
#else
    file = std::fopen(companionPath.c_str(), "rb");
#endif
    if (!file) return keys;
    uint8_t bytes[4];
    if (std::fread(bytes, 1, 4, file) != 4) { std::fclose(file); return keys; }
    const uint32_t count = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t record[28];
        if (std::fread(record, 1, sizeof(record), file) != sizeof(record)) {
            keys.clear(); break;
        }
        keys.insert((uint32_t)record[0] | ((uint32_t)record[1] << 8) |
            ((uint32_t)record[2] << 16) | ((uint32_t)record[3] << 24));
    }
    std::fclose(file);
    return keys;
}

static uint32_t AllocateAttachmentKey(const LvlxLevel& level,
    const std::unordered_set<uint32_t>& companionKeys) {
    if (level.version <= 2u) return 0u; // The field is absent on disk.
    uint32_t maximum = 0;
    bool any = false;
    std::unordered_set<uint32_t> used = companionKeys;
    uint32_t runtimeIndex = 0;
    for (uint32_t i = 0; i < level.element_count; ++i) {
        if (!level.elements[i].present) continue;
        used.insert(level.elements[i].lightmap_attachment_key);
        maximum = any ? std::max(maximum, level.elements[i].lightmap_attachment_key)
            : level.elements[i].lightmap_attachment_key;
        any = true;
    }
    for (uint32_t key : companionKeys) {
        maximum = any ? std::max(maximum, key) : key;
        any = true;
    }
    if (!any) return 0u;
    if (maximum != UINT32_MAX) return maximum + 1u;
    for (uint32_t candidate = 0; candidate != UINT32_MAX; ++candidate)
        if (!used.contains(candidate)) return candidate;
    return UINT32_MAX; // Exhaustion is only possible with every key occupied.
}

static int RemoveUnreferencedWaylists(LvlxLevel& level,
    const std::unordered_set<uint32_t>& candidates) {
    std::unordered_set<uint32_t> referenced;
    for (uint32_t i = 0; i < level.element_count; ++i)
        if (level.elements[i].present && level.elements[i].waylist_index != UINT32_MAX)
            referenced.insert(level.elements[i].waylist_index);

    uint32_t write = 0;
    int removed = 0;
    for (uint32_t read = 0; read < level.waylist_count; ++read) {
        if (candidates.contains(level.waylists[read].index) &&
            !referenced.contains(level.waylists[read].index)) {
            std::free(level.waylists[read].points);
            ++removed;
            continue;
        }
        if (write != read) level.waylists[write] = level.waylists[read];
        ++write;
    }
    level.waylist_count = write;
    return removed;
}

static uint32_t ActiveElementCount(const LvlxLevel& level) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < level.element_count; ++i)
        if (level.elements[i].present) ++count;
    return count;
}

struct LevelValidation {
    bool canSave = true;
    int errors = 0;
    int warnings = 0;
    std::string summary;
};

static bool Finite(LvlxVec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

static LevelValidation ValidateLevel(const LvlxLevel& level,
    const LvlxDefinitionTable& definitions,
    const std::unordered_set<uint32_t>& companionKeys) {
    LevelValidation result;
    std::unordered_set<uint32_t> waylistKeys;
    std::unordered_map<uint32_t, bool> attachmentKeys;
    int invalidMasters = 0, invalidOrientations = 0, invalidFloats = 0;
    int duplicateWaylists = 0, danglingWaylists = 0, duplicateAttachments = 0;
    int emptyRoutes = 0, inactiveRouteRefs = 0, orphanRoutes = 0;
    int routeStartMisses = 0, unusualMarkers = 0;
    std::unordered_set<uint32_t> referencedWaylists;

    // The HD loader treats every version >2 as the same attachment-key record
    // layout and accepts the full byte range. Versions 0/1 use the unsupported
    // legacy no-trailer contract; v2+ retains the required 24-byte trailer.
    const bool invalidVersion = level.version < 2u;
    const bool invalidTrailer = level.trailing_size != 24u || !level.has_spawn_data;
    if (invalidVersion) ++result.errors;
    if (invalidTrailer) ++result.errors;
    for (uint32_t i = 0; i < level.waylist_count; ++i) {
        const LvlxWaylist& route = level.waylists[i];
        if (!waylistKeys.insert(route.index).second) ++duplicateWaylists;
        if (route.point_count == 0) ++emptyRoutes;
        for (uint16_t p = 0; p < route.point_count; ++p)
            if (!Finite(route.points[p])) ++invalidFloats;
    }
    for (uint32_t i = 0; i < level.element_count; ++i) {
        const LvlxElement& element = level.elements[i];
        if (!element.present) continue;
        if (element.present != 1u) ++unusualMarkers;
        const LvlxElementDefinition* definition = lvlx_find_definition(&definitions,
            element.definition_id);
        if (!definition) ++invalidMasters;
        if (element.orientation > 3u) ++invalidOrientations;
        if (!Finite(element.vector1) || !Finite(element.vector2)) ++invalidFloats;
        if (element.waylist_index != UINT32_MAX) {
            referencedWaylists.insert(element.waylist_index);
            if (!waylistKeys.contains(element.waylist_index)) ++danglingWaylists;
            else if (!DefinitionSupportsWaylists(definition)) ++inactiveRouteRefs;
            else {
                const LvlxWaylist* route = nullptr;
                for (uint32_t w = 0; w < level.waylist_count; ++w)
                    if (level.waylists[w].index == element.waylist_index) {
                        route = &level.waylists[w]; break;
                    }
                bool startFound = false;
                if (route) for (uint16_t p = 0; p < route->point_count; ++p) {
                    const LvlxVec3 point = route->points[p];
                    const float dx = point.x - element.vector1.x;
                    const float dy = point.y - element.vector1.y;
                    const float dz = point.z - element.vector1.z;
                    if (dx * dx + dy * dy + dz * dz < 0.0001f) { startFound = true; break; }
                }
                if (route && route->point_count && !startFound) ++routeStartMisses;
            }
        }
        if (level.version > 2u) {
            const bool renderBearing = definition && definition->resource &&
                definition->resource[0];
            const auto [existing, inserted] = attachmentKeys.emplace(
                element.lightmap_attachment_key, renderBearing);
            // Only a non-render-bearing first match can suppress a later usable
            // match. Repeated placeholder keys after a usable owner are common
            // in shipped levels and do not change the game's first-match result.
            if (!inserted && companionKeys.contains(element.lightmap_attachment_key) &&
                !existing->second && renderBearing) ++duplicateAttachments;
        }
    }
    for (uint32_t i = 0; i < level.waylist_count; ++i)
        if (!referencedWaylists.contains(level.waylists[i].index)) ++orphanRoutes;
    if (!Finite(level.spawn_position) ||
        !std::isfinite(level.initial_camera_heading_degrees)) ++invalidFloats;

    result.errors += invalidMasters + invalidFloats + duplicateWaylists + danglingWaylists;
    result.warnings += invalidOrientations + duplicateAttachments + emptyRoutes +
        inactiveRouteRefs + orphanRoutes + routeStartMisses + unusualMarkers;
    result.canSave = result.errors == 0;
    std::ostringstream text;
    if (result.errors || result.warnings) {
        text << result.errors << " error(s), " << result.warnings << " warning(s)";
        if (invalidVersion) text << " | unsupported LVLX v" << (unsigned)level.version;
        if (invalidTrailer) text << " | trailer is " << level.trailing_size << " bytes";
        if (invalidMasters) text << " | invalid masters " << invalidMasters;
        if (invalidOrientations) text << " | unsupported rotations " << invalidOrientations;
        if (invalidFloats) text << " | non-finite values " << invalidFloats;
        if (duplicateWaylists) text << " | duplicate routes " << duplicateWaylists;
        if (danglingWaylists) text << " | dangling routes " << danglingWaylists;
        if (duplicateAttachments) text << " | duplicate LMD keys " << duplicateAttachments;
        if (emptyRoutes) text << " | empty routes " << emptyRoutes;
        if (inactiveRouteRefs) text << " | inactive route refs " << inactiveRouteRefs;
        if (orphanRoutes) text << " | orphan routes " << orphanRoutes;
        if (routeStartMisses) text << " | routes missing start " << routeStartMisses;
        if (unusualMarkers) text << " | preserved presence markers " << unusualMarkers;
    } else text << "Validation passed";
    result.summary = text.str();
    return result;
}

enum class NumericField { None, Score, TimeLimit, CameraHeading };

static bool ParseUint32(const std::string& text, uint32_t& value) {
    if (text.empty() || text[0] == '-') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' || parsed > UINT32_MAX)
        return false;
    value = (uint32_t)parsed;
    return true;
}

static bool ParseFiniteFloat(const std::string& text, float& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(text.c_str(), &end);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' || !std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

static const char* NumericFieldName(NumericField field) {
    if (field == NumericField::Score) return "score multiplier";
    if (field == NumericField::TimeLimit) return "time limit";
    if (field == NumericField::CameraHeading) return "camera heading";
    return "value";
}

static bool IsElementSelected(const std::vector<int>& selection, int index) {
    return std::find(selection.begin(), selection.end(), index) != selection.end();
}

static void CopyElementsToClipboard(const LvlxLevel& level,
    const std::vector<int>& selection, std::vector<ElementClipboardItem>& clipboard,
    Vector3& anchor) {
    clipboard.clear();
    anchor = Vector3{};
    for (int index : selection) {
        if (index < 0 || index >= (int)level.element_count || !level.elements[index].present)
            continue;
        ElementClipboardItem item;
        item.element = level.elements[index];
        for (uint32_t w = 0; w < level.waylist_count; ++w) {
            if (level.waylists[w].index != item.element.waylist_index) continue;
            item.waypoints.assign(level.waylists[w].points,
                level.waylists[w].points + level.waylists[w].point_count);
            break;
        }
        anchor = Vector3Add(anchor, ToRaylib(item.element.vector1));
        clipboard.push_back(std::move(item));
    }
    if (!clipboard.empty()) anchor = Vector3Scale(anchor, 1.0f / (float)clipboard.size());
}

static bool PasteElementsFromClipboard(LvlxLevel& level,
    const std::vector<ElementClipboardItem>& clipboard, Vector3 clipboardAnchor,
    Vector3 destination, const std::unordered_set<uint32_t>& companionKeys,
    std::vector<int>& pastedSelection) {
    pastedSelection.clear();
    if (clipboard.empty()) return false;
    const Vector3 delta = Vector3Subtract(destination, clipboardAnchor);

    for (const ElementClipboardItem& item : clipboard) {
        LvlxElement element = item.element;
        element.vector1 = FromRaylib(Vector3Add(ToRaylib(element.vector1), delta));
        element.vector2 = FromRaylib(Vector3Add(ToRaylib(element.vector2), delta));
        element.waylist_index = UINT32_MAX;
        element.lightmap_attachment_key = AllocateAttachmentKey(level, companionKeys);
        uint32_t newIndex = 0;
        if (!lvlx_add_element(&level, &element, &newIndex)) return false;
        pastedSelection.push_back((int)newIndex);

        if (!item.waypoints.empty()) {
            LvlxElement& pasted = level.elements[newIndex];
            LvlxWaylist* path = EnsureElementWaylist(level, pasted);
            if (!path) return false;
            for (LvlxVec3 point : item.waypoints) {
                point = FromRaylib(Vector3Add(ToRaylib(point), delta));
                if (!AppendWaypoint(*path, point)) return false;
            }
        }
    }
    return true;
}

static bool WaylistHasDrawableOwner(const LvlxLevel& level,
    const LvlxDefinitionTable& definitions, uint32_t waylistIndex) {
    for (uint32_t i = 0; i < level.element_count; ++i) {
        const LvlxElement& element = level.elements[i];
        if (!element.present || element.waylist_index != waylistIndex) continue;
        const LvlxElementDefinition* definition = lvlx_find_definition(
            &definitions, element.definition_id);
        if (DefinitionSupportsWaylists(definition)) return true;
    }
    return false;
}

static void DrawWaylists(const LvlxLevel& level,
    const LvlxDefinitionTable& definitions, uint32_t selectedWaylist = UINT32_MAX,
    int selectedPoint = -1) {
    for (uint32_t i = 0; i < level.waylist_count; ++i) {
        const LvlxWaylist& waylist = level.waylists[i];
        if (!WaylistHasDrawableOwner(level, definitions, waylist.index)) continue;
        const bool selected = waylist.index == selectedWaylist;
        for (uint16_t p = 0; p < waylist.point_count; ++p) {
            const Vector3 point = ToRaylib(waylist.points[p]);
            const Color color = selected && (int)p == selectedPoint ? ORANGE
                : (selected ? SKYBLUE : YELLOW);
            DrawSphere(point, selected ? 0.28f : 0.20f, color);
            if (p) DrawLine3D(ToRaylib(waylist.points[p - 1]), point,
                selected ? SKYBLUE : YELLOW);
        }
        if (waylist.point_count > 1)
            DrawLine3D(ToRaylib(waylist.points[waylist.point_count - 1]),
                ToRaylib(waylist.points[0]), selected ? SKYBLUE : YELLOW);
    }
}


enum class TransformMode { Move, Rotate };
enum class GizmoAxis { None, X, Y, Z, Free };

struct GizmoDragState {
    bool active = false;
    GizmoAxis axis = GizmoAxis::None;
    Vector3 startPosition{};
    Vector3 startSecond{};
    Vector3 axisDirection{};
    float startAxisParameter = 0.0f;
    Vector3 planeNormal{};
    Vector3 startPlaneHit{};
    float startAngle = 0.0f;
    uint8_t startOrientation = 0;
};

static const char* TransformModeName(TransformMode mode) {
    switch (mode) {
    case TransformMode::Move: return "MOVE";
    case TransformMode::Rotate: return "ROTATE";
    }
    return "";
}

static Vector3 GizmoAxisVector(GizmoAxis axis) {
    switch (axis) {
    case GizmoAxis::X: return Vector3{ 1, 0, 0 };
    case GizmoAxis::Y: return Vector3{ 0, 1, 0 };
    case GizmoAxis::Z: return Vector3{ 0, 0, 1 };
    default: return Vector3{};
    }
}

static Color GizmoAxisColor(GizmoAxis axis, bool enabled = true) {
    Color c = GRAY;
    if (axis == GizmoAxis::X) c = RED;
    else if (axis == GizmoAxis::Y) c = GREEN;
    else if (axis == GizmoAxis::Z) c = BLUE;
    return enabled ? c : Fade(c, 0.28f);
}

static float GizmoWorldSize(const Camera3D& camera, Vector3 position) {
    return Clamp(Vector3Distance(camera.position, position) * 0.085f, 1.5f, 7.0f);
}

static float PointSegmentDistance2D(Vector2 p, Vector2 a, Vector2 b) {
    const Vector2 ab = Vector2Subtract(b, a);
    const float lengthSquared = Vector2DotProduct(ab, ab);
    if (lengthSquared < 0.0001f) return Vector2Distance(p, a);
    const float t = Clamp(Vector2DotProduct(Vector2Subtract(p, a), ab) / lengthSquared, 0.0f, 1.0f);
    return Vector2Distance(p, Vector2Add(a, Vector2Scale(ab, t)));
}

static bool RayPlaneIntersection(const Camera3D& camera, Vector2 screenPosition,
    Vector3 planePoint, Vector3 planeNormal, Vector3& out) {
    const Ray ray = GetMouseRay(screenPosition, camera);
    const float denominator = Vector3DotProduct(ray.direction, planeNormal);
    if (fabsf(denominator) < 0.00001f) return false;
    const float t = Vector3DotProduct(Vector3Subtract(planePoint, ray.position), planeNormal) / denominator;
    if (t < 0.0f) return false;
    out = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
    return true;
}

static bool ClosestAxisParameter(const Camera3D& camera, Vector2 screenPosition,
    Vector3 axisOrigin, Vector3 axisDirection, float& outParameter) {
    const Ray ray = GetMouseRay(screenPosition, camera);
    const Vector3 w0 = Vector3Subtract(axisOrigin, ray.position);
    const float a = Vector3DotProduct(axisDirection, axisDirection);
    const float b = Vector3DotProduct(axisDirection, ray.direction);
    const float c = Vector3DotProduct(ray.direction, ray.direction);
    const float d = Vector3DotProduct(axisDirection, w0);
    const float e = Vector3DotProduct(ray.direction, w0);
    const float denominator = a * c - b * b;
    if (fabsf(denominator) < 0.00001f) return false;
    outParameter = (b * e - c * d) / denominator;
    return true;
}

static GizmoAxis PickLinearGizmo(const Camera3D& camera, Vector3 origin,
    Vector2 mouse, float size, bool centerHandle) {
    const Vector2 center = GetWorldToScreen(origin, camera);
    if (centerHandle && Vector2Distance(mouse, center) <= 11.0f) return GizmoAxis::Free;

    GizmoAxis best = GizmoAxis::None;
    float bestDistance = 9.0f;
    for (GizmoAxis axis : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z}) {
        const Vector3 end = Vector3Add(origin, Vector3Scale(GizmoAxisVector(axis), size));
        const float d = PointSegmentDistance2D(mouse, center, GetWorldToScreen(end, camera));
        if (d < bestDistance) {
            bestDistance = d;
            best = axis;
        }
    }
    return best;
}

static void DrawMoveGizmo(Vector3 origin, float size, GizmoAxis hotAxis) {
    DrawCubeWires(origin, size * 0.12f, size * 0.12f, size * 0.12f,
        hotAxis == GizmoAxis::Free ? YELLOW : RAYWHITE);
    for (GizmoAxis axis : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z}) {
        const Vector3 direction = GizmoAxisVector(axis);
        const Vector3 end = Vector3Add(origin, Vector3Scale(direction, size));
        const Color color = axis == hotAxis ? YELLOW : GizmoAxisColor(axis);
        DrawLine3D(origin, end, color);
        DrawSphere(end, size * 0.075f, color);
    }
}

static Vector3 RingPoint(Vector3 origin, GizmoAxis axis, float radius, float angle) {
    const float c = cosf(angle), s = sinf(angle);
    if (axis == GizmoAxis::X) return Vector3Add(origin, Vector3{ 0, c * radius, s * radius });
    if (axis == GizmoAxis::Y) return Vector3Add(origin, Vector3{ c * radius, 0, s * radius });
    return Vector3Add(origin, Vector3{ c * radius, s * radius, 0 });
}

static void DrawRotateGizmo(Vector3 origin, float radius, GizmoAxis hotAxis) {
    const int segments = 64;
    for (GizmoAxis axis : {GizmoAxis::Y}) {
        Color color = axis == hotAxis ? YELLOW : GizmoAxisColor(axis);
        Vector3 previous = RingPoint(origin, axis, radius, 0.0f);
        for (int i = 1; i <= segments; ++i) {
            const float angle = (2.0f * PI * i) / segments;
            const Vector3 next = RingPoint(origin, axis, radius, angle);
            DrawLine3D(previous, next, color);
            previous = next;
        }
    }
}

static GizmoAxis PickRotateGizmo(const Camera3D& camera, Vector3 origin,
    Vector2 mouse, float radius) {
    const int segments = 48;
    GizmoAxis best = GizmoAxis::None;
    float bestDistance = 9.0f;
    for (GizmoAxis axis : {GizmoAxis::Y}) {
        Vector2 previous = GetWorldToScreen(RingPoint(origin, axis, radius, 0.0f), camera);
        for (int i = 1; i <= segments; ++i) {
            const float angle = (2.0f * PI * i) / segments;
            const Vector2 next = GetWorldToScreen(RingPoint(origin, axis, radius, angle), camera);
            const float d = PointSegmentDistance2D(mouse, previous, next);
            if (d < bestDistance) {
                bestDistance = d;
                best = axis;
            }
            previous = next;
        }
    }
    return best;
}

static float AngleAroundY(Vector3 origin, Vector3 point) {
    return atan2f(point.z - origin.z, point.x - origin.x);
}

struct EditorSnapshot {
    std::vector<std::string> environmentValues;
    std::vector<LvlxElement> elements;
    struct WaylistSnapshot {
        uint32_t index = 0;
        std::vector<LvlxVec3> points;
    };
    std::vector<WaylistSnapshot> waylists;
    uint32_t scoreMultiplier = 0;
    uint32_t timeLimitSeconds = 0;
    LvlxVec3 spawnPosition{};
    float cameraHeadingDegrees = 0.0f;
};

static EditorSnapshot CaptureEditor(const LvlxLevel& level) {
    EditorSnapshot snapshot;
    for (size_t i = 0; i < environment::schema.size(); ++i)
        snapshot.environmentValues.push_back(gEnvironment.value(i));
    if (level.element_count && level.elements)
        snapshot.elements.assign(level.elements, level.elements + level.element_count);
    snapshot.waylists.reserve(level.waylist_count);
    for (uint32_t i = 0; i < level.waylist_count; ++i) {
        EditorSnapshot::WaylistSnapshot item;
        item.index = level.waylists[i].index;
        if (level.waylists[i].point_count && level.waylists[i].points)
            item.points.assign(level.waylists[i].points,
                level.waylists[i].points + level.waylists[i].point_count);
        snapshot.waylists.push_back(std::move(item));
    }
    snapshot.scoreMultiplier = level.level_score_multiplier;
    snapshot.timeLimitSeconds = level.level_time_limit_seconds;
    snapshot.spawnPosition = level.spawn_position;
    snapshot.cameraHeadingDegrees = level.initial_camera_heading_degrees;
    return snapshot;
}

static bool RestoreEditor(LvlxLevel& level, const EditorSnapshot& snapshot) {
    if (snapshot.elements.size() > UINT32_MAX || snapshot.waylists.size() > UINT32_MAX)
        return false;

    LvlxElement* replacement = nullptr;
    if (!snapshot.elements.empty()) {
        replacement = (LvlxElement*)std::malloc(snapshot.elements.size() * sizeof(LvlxElement));
        if (!replacement) return false;
        std::memcpy(replacement, snapshot.elements.data(),
            snapshot.elements.size() * sizeof(LvlxElement));
    }

    LvlxWaylist* replacementWaylists = nullptr;
    if (!snapshot.waylists.empty()) {
        replacementWaylists = (LvlxWaylist*)std::calloc(snapshot.waylists.size(), sizeof(LvlxWaylist));
        if (!replacementWaylists) { std::free(replacement); return false; }
        for (size_t i = 0; i < snapshot.waylists.size(); ++i) {
            const auto& source = snapshot.waylists[i];
            if (source.points.size() > UINT16_MAX) {
                for (size_t j = 0; j < i; ++j) std::free(replacementWaylists[j].points);
                std::free(replacementWaylists); std::free(replacement); return false;
            }
            replacementWaylists[i].index = source.index;
            replacementWaylists[i].point_count = (uint16_t)source.points.size();
            if (!source.points.empty()) {
                replacementWaylists[i].points = (LvlxVec3*)std::malloc(source.points.size() * sizeof(LvlxVec3));
                if (!replacementWaylists[i].points) {
                    for (size_t j = 0; j < i; ++j) std::free(replacementWaylists[j].points);
                    std::free(replacementWaylists); std::free(replacement); return false;
                }
                std::memcpy(replacementWaylists[i].points, source.points.data(),
                    source.points.size() * sizeof(LvlxVec3));
            }
        }
    }

    std::free(level.elements);
    for (uint32_t i = 0; i < level.waylist_count; ++i) std::free(level.waylists[i].points);
    std::free(level.waylists);
    level.elements = replacement;
    level.element_count = (uint32_t)snapshot.elements.size();
    level.waylists = replacementWaylists;
    level.waylist_count = (uint32_t)snapshot.waylists.size();
    for (size_t i = 0; i < snapshot.environmentValues.size(); ++i)
        gEnvironment.set(i, snapshot.environmentValues[i]);
    if (level.has_spawn_data) {
        lvlx_set_spawn_transform(&level, snapshot.spawnPosition, snapshot.cameraHeadingDegrees);
        lvlx_set_level_properties(&level, snapshot.scoreMultiplier,
            snapshot.timeLimitSeconds, snapshot.cameraHeadingDegrees);
    }
    return true;
}

static bool EditorStatesEqual(const EditorSnapshot& a, const EditorSnapshot& b) {
    if (a.environmentValues != b.environmentValues) return false;
    if (a.elements.size() != b.elements.size()) return false;
    for (size_t i = 0; i < a.elements.size(); ++i) {
        const LvlxElement& x = a.elements[i];
        const LvlxElement& y = b.elements[i];
        if (x.present != y.present ||
            x.definition_id != y.definition_id ||
            x.lightmap_attachment_key != y.lightmap_attachment_key ||
            x.vector1.x != y.vector1.x || x.vector1.y != y.vector1.y || x.vector1.z != y.vector1.z ||
            x.vector2.x != y.vector2.x || x.vector2.y != y.vector2.y || x.vector2.z != y.vector2.z ||
            x.orientation != y.orientation ||
            x.waylist_index != y.waylist_index)
            return false;
    }
    if (a.waylists.size() != b.waylists.size() ||
        a.scoreMultiplier != b.scoreMultiplier ||
        a.timeLimitSeconds != b.timeLimitSeconds ||
        a.spawnPosition.x != b.spawnPosition.x || a.spawnPosition.y != b.spawnPosition.y ||
        a.spawnPosition.z != b.spawnPosition.z ||
        a.cameraHeadingDegrees != b.cameraHeadingDegrees) return false;
    for (size_t i = 0; i < a.waylists.size(); ++i) {
        if (a.waylists[i].index != b.waylists[i].index ||
            a.waylists[i].points.size() != b.waylists[i].points.size()) return false;
        for (size_t p = 0; p < a.waylists[i].points.size(); ++p) {
            const LvlxVec3& x = a.waylists[i].points[p];
            const LvlxVec3& y = b.waylists[i].points[p];
            if (x.x != y.x || x.y != y.y || x.z != y.z) return false;
        }
    }
    return true;
}

static bool LightingStatesEqual(const EditorSnapshot& a, const EditorSnapshot& b) {
    // SantaMapper's generated atlas stores sun visibility only. Color, fog,
    // death height, and other environment properties remain runtime inputs.
    constexpr size_t sunDirectionProperty = 5;
    if (a.environmentValues.size() <= sunDirectionProperty ||
        b.environmentValues.size() <= sunDirectionProperty ||
        a.environmentValues[sunDirectionProperty] !=
            b.environmentValues[sunDirectionProperty] ||
        a.elements.size() != b.elements.size()) return false;
    for (size_t i = 0; i < a.elements.size(); ++i) {
        const LvlxElement& x = a.elements[i];
        const LvlxElement& y = b.elements[i];
        if (x.present != y.present || x.definition_id != y.definition_id ||
            x.vector1.x != y.vector1.x || x.vector1.y != y.vector1.y ||
            x.vector1.z != y.vector1.z || x.orientation != y.orientation)
            return false;
    }
    return true;
}

static void PushUndo(std::vector<EditorSnapshot>& undoStack,
    std::vector<EditorSnapshot>& redoStack, EditorSnapshot before) {
    undoStack.push_back(std::move(before));
    redoStack.clear();
}

int main(int argc, char** argv) {
    const bool previewKnowledge = argc >= 4 && std::strcmp(argv[3], "--preview-knowledge") == 0;
    const bool previewLiveShadows = argc >= 4 &&
        std::strcmp(argv[3], "--preview-live-shadows") == 0;
    const bool previewScene = previewKnowledge || previewLiveShadows ||
        (argc >= 4 && std::strcmp(argv[3], "--preview-scene") == 0);
    const bool previewSmoke = previewScene || (argc >= 4 && std::strcmp(argv[3], "--preview-smoke") == 0);
    int previewFrames = 0;
    std::string levelPath = argc >= 2 ? argv[1] : "bin_win32/levels/001.dat";
    std::string binDirectory = FindBinDirectory(levelPath.c_str());
    const std::string defaultDefinitions = JoinPath(binDirectory, "settings/elements.txt");
    const char* definitionsPath = argc >= 3 ? argv[2] : defaultDefinitions.c_str();
    if(argc>=3)binDirectory=DirectoryOf(DirectoryOf(NormalizePath(definitionsPath)));
    std::string modDirectory,assetsDirectory,texturesDirectory,effectsDirectory;
    ConfigureResourceDirectories(levelPath,binDirectory,modDirectory,assetsDirectory,texturesDirectory,effectsDirectory);
    LvlxLevel level{};
    LvlxDefinitionTable definitions{};
    if (!lvlx_load_level(levelPath.c_str(), &level)) {
        std::fprintf(stderr, "Could not load level: %s\n", levelPath.c_str());
        return 1;
    }
    if (!lvlx_load_definitions(definitionsPath, &definitions)) {
        std::fprintf(stderr, "Could not load definitions: %s\n", definitionsPath);
        lvlx_free_level(&level);
        return 1;
    }
    const std::string modDefinitions = FindModElementDefinitions(levelPath);
    if (!modDefinitions.empty() && Lowercase(NormalizePath(definitionsPath)) != Lowercase(modDefinitions) &&
        !lvlx_append_definitions(modDefinitions.c_str(), &definitions)) {
        std::fprintf(stderr, "Could not append mod definitions: %s\n", modDefinitions.c_str());
        lvlx_free_definitions(&definitions);lvlx_free_level(&level);return 1;
    }
    std::unordered_set<uint32_t> companionLmdKeys = LoadLmdAttachmentKeys(levelPath);
    LoadEnvironment(binDirectory, levelPath);
    physics_preview::Library physics;
    physics.load(JoinPath(DirectoryOf(definitionsPath), "physics.txt"));
    if (!physics.error.empty()) std::fprintf(stderr, "Collision preview: %s\n", physics.error.c_str());
    if (argc >= 4 && std::strcmp(argv[3], "--validate-effects") == 0) {
        unsigned validEffects = 0, invalidEffects = 0;
        std::error_code error;
        for (std::filesystem::directory_iterator it(effectsDirectory, error), end;
            !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error) ||
                Lowercase(it->path().extension().string()) != ".fxt") continue;
            if (EffectPreview::ValidateTemplateFile(it->path().string())) ++validEffects;
            else {
                ++invalidEffects;
                std::fprintf(stderr, "Invalid or unsupported FXT: %s\n",
                    it->path().string().c_str());
            }
        }
        std::printf("Effects: %u valid, %u invalid%s\n", validEffects,
            invalidEffects, error ? "; directory scan failed" : "");
        lvlx_free_definitions(&definitions);
        lvlx_free_level(&level);
        return error || invalidEffects ? 2 : 0;
    }
    if (argc >= 4 && std::strcmp(argv[3], "--validate") == 0) {
        const LevelValidation validation = ValidateLevel(level, definitions, companionLmdKeys);
        std::printf("%s\n", validation.summary.c_str());
        const float deathHeight = gEnvironment.numbers(20)[0];
        std::printf("Environment: %zu diagnostics; death height %.9g%s\n",
            gEnvironment.diagnostics.size(), deathHeight,
            level.has_spawn_data && level.spawn_position.y < deathHeight ? " WARNING: spawn below threshold" : "");
        lvlx_free_definitions(&definitions);
        lvlx_free_level(&level);
        return validation.canSave ? 0 : 2;
    }

    InitWindow(1366, 768, "SantaWorkshop");
    // Escape is an editor cancel action. Do not let raylib also close the app.
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    if (!LoadCrfShader())
    {
        std::fprintf(
            stderr,
            "Could not initialize CRF shader\n"
        );

        CloseWindow();

        lvlx_free_definitions(
            &definitions
        );

        lvlx_free_level(
            &level
        );

        return 1;
    }
    gLiveShadowMap = LoadRenderTexture(1536, 1536);
    if (gLiveShadowMap.texture.id) {
        SetTextureWrap(gLiveShadowMap.texture, TEXTURE_WRAP_CLAMP);
        SetTextureFilter(gLiveShadowMap.texture, TEXTURE_FILTER_POINT);
    }
    std::vector<RenderAsset> assets(definitions.count);
    RenderAsset santaAsset;
    RenderAsset skyAsset;
    LevelLightmaps lightmaps;
    if (!LoadLevelLightmaps(levelPath, texturesDirectory, lightmaps))
        TraceLog(LOG_WARNING, "Invalid LMD companion for %s", levelPath.c_str());
    std::vector<uint32_t> paletteItems;
    std::string elementSearch;
    bool searchFocused = false;
    NumericField numericField = NumericField::None;
    std::string numericBuffer;
    BuildDefinitionList(definitions, elementSearch, paletteItems);
    unsigned loadedAssetCount = 0, missingAssetCount = 0;

    Camera3D camera{};
    camera.up = Vector3{ 0, 1, 0 };
    camera.fovy = 45;
    camera.projection = CAMERA_PERSPECTIVE;
    PositionCameraAtLevelStart(camera, level);
    EffectPreview effectPreview;
    bool effectsEnabled = true;
    bool collisionsEnabled = previewKnowledge, animationsEnabled = previewKnowledge;
    double animationSeconds = 0;
    float effectElapsed = 0.0f;

    const float panelWidth = 330.0f;
    const float rowHeight = 28.0f;
    const float paletteTop = 320.0f;
    int paletteScroll = 0;
    int selectedDefinition = -1;
    int selectedElement = -1;
    std::vector<int> selectedElements;
    bool spawnSelected = false;
    LvlxVec3 spawnTransformStart{};
    float cameraHeadingStart = 0.0f;
    std::vector<ElementClipboardItem> elementClipboard;
    Vector3 clipboardAnchor{};
    bool marqueeActive = false;
    Vector2 marqueeStart{};
    Vector2 marqueeCurrent{};
    bool waypointMode = false;
    int selectedWaypoint = -1;
    bool waypointDragging = false;
    uint8_t placementOrientation = 0;
    TransformMode transformMode = TransformMode::Move;
    GizmoDragState gizmoDrag{};
    bool moveSnapEnabled = true;
    bool lightmapsEnabled = true;
    float moveSnapStep = 1.0f;
    bool cameraActive = false;
    bool dirty = false;
    bool environmentPanel = previewSmoke && !previewScene;
    int environmentScroll = 0, environmentField = -1;
    std::string environmentBuffer;
    std::vector<EditorSnapshot> undoStack;
    std::vector<EditorSnapshot> redoStack;
    EditorSnapshot savedSnapshot = CaptureEditor(level);
    EditorSnapshot bakedLightingSnapshot = savedSnapshot;
    if (previewLiveShadows && !bakedLightingSnapshot.elements.empty())
        bakedLightingSnapshot.elements[0].vector1.x += 0.125f;
    bool lightingDirty = false;
    EditorSnapshot liveShadowSnapshot;
    bool liveShadowSnapshotValid = false;
    EditorSnapshot pendingTransformSnapshot;
    EditorSnapshot pendingWaypointSnapshot;
    EditorSnapshot pendingSpawnSnapshot;
    bool pendingWaypointHistory = false;
    bool pendingSpawnHistory = false;
    bool pendingTransformHistory = false;
    std::string status = "Click an element to select it; W = move, E = rotate";
    const auto rebuildLighting = [&]() -> bool {
        if (dirty) {
            status = "Save the DAT/environment first (Ctrl+S) so lighting stays synchronized";
            return false;
        }
        lightingDirty = !LightingStatesEqual(CaptureEditor(level),
            bakedLightingSnapshot);
        if (!lightingDirty) {
            status = "Lighting is already current";
            return true;
        }
        std::string bakeStatus;
        // Preview playback must never affect a static geometry bake.
        for (auto& asset : assets) UpdateAnimationPreview(asset, false, 0);
        if (!BakeLevelLightmaps(level, definitions, assets, assetsDirectory,
                texturesDirectory, levelPath, lightmaps, bakeStatus)) {
            status = "LIGHTING STILL DIRTY: " + bakeStatus;
            return false;
        }
        if (!LoadLevelLightmaps(levelPath, texturesDirectory, lightmaps)) {
            status = "Lightmaps were written but could not be reloaded; backups are available";
            return false;
        }
        companionLmdKeys = LoadLmdAttachmentKeys(levelPath);
        bakedLightingSnapshot = CaptureEditor(level);
        lightingDirty = false;
        gLiveShadowEnabled = false;
        status = "Lighting rebuilt: " + bakeStatus;
        return true;
    };
    const auto saveLevel = [&]() {
        const LevelValidation validation = ValidateLevel(level, definitions, companionLmdKeys);
        if (!validation.canSave) {
            status = std::string("SAVE BLOCKED: ") + validation.summary;
        } else if (lvlx_save_level(levelPath.c_str(), &level)) {
            std::string environmentStatus;
            if (gEnvironment.save(environmentStatus)) {
                savedSnapshot = CaptureEditor(level);
                dirty = false;
                lightingDirty = !LightingStatesEqual(savedSnapshot,
                    bakedLightingSnapshot);
                status = std::string("Saved: ") + levelPath + " | " +
                    validation.summary + " | " + environmentStatus;
                if (kShowLightmapControls && lightingDirty)
                    status += " | lighting bake pending (Ctrl+B)";
            } else status = "DAT saved; " + environmentStatus + "; environment remains unsaved";
        } else status = "Save failed - see console";
    };

    while (!WindowShouldClose()) {
        if (!cameraActive && !gizmoDrag.active && !waypointDragging &&
            !searchFocused && numericField == NumericField::None && environmentField < 0 &&
            (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) &&
            (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) &&
            (IsLetterPressed('M') || IsLetterPressed('N'))) {
            const bool blank = IsLetterPressed('N');
            const auto validation = ValidateLevel(level, definitions, companionLmdKeys);
            if (blank && dirty) status = "Save or undo current edits before creating an empty level";
            else if (!gEnvironment.readable) status = "MOD EXPORT BLOCKED: environment source is unreadable";
            else if (!validation.canSave) status = "MOD EXPORT BLOCKED: " + validation.summary;
            else {
                std::string modId, levelId, error;
                std::filesystem::path exported;
                if (PromptModProject(modId, levelId, blank) &&
                    mod_project::create(binDirectory, modId, levelId, level,
                        gEnvironment.serialize(), blank, levelPath, exported, error)) {
                    LvlxLevel replacement{};
                    if (lvlx_load_level(exported.string().c_str(), &replacement)) {
                        lvlx_free_level(&level); level = replacement;
                        levelPath = NormalizePath(exported.string());
                        LoadEnvironment(binDirectory, levelPath);
                        companionLmdKeys = LoadLmdAttachmentKeys(levelPath);
                        LoadLevelLightmaps(levelPath, texturesDirectory, lightmaps);
                        selectedElement = -1; selectedElements.clear();
                        waypointMode = false; selectedWaypoint = -1; spawnSelected = false;
                        selectedDefinition = -1; gizmoDrag = GizmoDragState{};
                        undoStack.clear(); redoStack.clear();
                        savedSnapshot = CaptureEditor(level); bakedLightingSnapshot = savedSnapshot;
                        dirty = false; lightingDirty = !blank;
                        liveShadowSnapshotValid = false;
                        PositionCameraAtLevelStart(camera, level);
                        status = "Editing mod: " + levelPath + " (in-game loader not installed)";
                    } else status = "Export created, but reopening failed: " + exported.string();
                } else if (!error.empty()) status = "MOD EXPORT FAILED: " + error;
            }
            continue;
        }
        effectElapsed += std::min(GetFrameTime(), 0.1f);
        const Vector2 mouse = GetMousePosition();
        const bool mouseOverPanel = environmentPanel || mouse.x < panelWidth;
        if (!cameraActive && !gizmoDrag.active && !waypointDragging &&
            !searchFocused && numericField == NumericField::None && IsKeyPressed(KEY_F2)) {
            environmentPanel = !environmentPanel;
            environmentField = -1;
        }
        if (environmentPanel) {
            const int rows = (GetScreenHeight() - 245) / 25;
            if (environmentField < 0) {
                const bool control = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                if (control && IsLetterPressed('S')) saveLevel();
                if (kShowLightmapControls && control && IsLetterPressed('B')) rebuildLighting();
                if (control && (IsLetterPressed('Z') || IsLetterPressed('Y'))) {
                    const bool redo = IsLetterPressed('Y') || IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                    auto& from = redo ? redoStack : undoStack;
                    auto& to = redo ? undoStack : redoStack;
                    if (!from.empty()) {
                        EditorSnapshot current = CaptureEditor(level);
                        if (RestoreEditor(level, from.back())) {
                            to.push_back(std::move(current));
                            from.pop_back();
                            dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                            status = redo ? "Redo" : "Undo";
                        }
                    }
                }
                environmentScroll = std::clamp(environmentScroll - (int)GetMouseWheelMove(),
                    0, std::max(0, (int)environment::schema.size() - rows));
                if (IsKeyPressed(KEY_ESCAPE)) environmentPanel = false;
                if (IsLetterPressed('F')) gFogPreview = !gFogPreview;
                if (IsLetterPressed('H')) gDeathGridVisible = !gDeathGridVisible;
                if (!control && IsLetterPressed('A')) gAmbientPreviewSlot = (gAmbientPreviewSlot + 1) % 4;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && mouse.x >= 360 && mouse.x < GetScreenWidth()-30 &&
                    mouse.y >= 165 && mouse.y < 165 + rows*25) {
                    int i = environmentScroll + (int)((mouse.y-165)/25);
                    if (i < (int)environment::schema.size()) {
                        environmentField = i;
                        environmentBuffer = gEnvironment.value(i);
                    }
                }
            } else {
                if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsLetterPressed('A'))
                    environmentBuffer.clear();
                for (int c = GetCharPressed(); c > 0; c = GetCharPressed())
                    if (c >= 32 && c <= 126 && environmentBuffer.size() < 512) environmentBuffer += (char)c;
                if (IsKeyPressed(KEY_BACKSPACE) && !environmentBuffer.empty()) environmentBuffer.pop_back();
                if (IsKeyPressed(KEY_ESCAPE)) environmentField = -1;
                else if (IsKeyPressed(KEY_ENTER)) {
                    EditorSnapshot before = CaptureEditor(level);
                    if (gEnvironment.set((size_t)environmentField, environmentBuffer)) {
                        if (!EditorStatesEqual(before, CaptureEditor(level))) PushUndo(undoStack, redoStack, std::move(before));
                        dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                        environmentField = -1;
                        status = kShowLightmapControls
                            ? "Environment updated; Ctrl+S saves, Ctrl+B bakes lighting"
                            : "Environment updated; Ctrl+S saves";
                    } else status = "Invalid value: check component count, finite numbers, int32 bounds or quoted resource name";
                }
            }
        }

        if (!mouseOverPanel && !gizmoDrag.active && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            cameraActive = true;
            DisableCursor();
        }
        if (cameraActive && IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
            cameraActive = false;
            EnableCursor();
        }
        if (cameraActive) UpdateCamera(&camera, CAMERA_FREE);

        GizmoAxis hotAxis = GizmoAxis::None;
        Vector3 selectedPosition{};
        float gizmoSize = 0.0f;
        if (!waypointMode && selectedDefinition < 0 &&
            (spawnSelected || (selectedElement >= 0 && selectedElement < (int)level.element_count &&
                level.elements[selectedElement].present))) {
            selectedPosition = spawnSelected ? ToRaylib(level.spawn_position)
                : ToRaylib(level.elements[selectedElement].vector1);
            gizmoSize = GizmoWorldSize(camera, selectedPosition);
            if (!mouseOverPanel && !cameraActive && !gizmoDrag.active) {
                if (transformMode == TransformMode::Move)
                    hotAxis = PickLinearGizmo(camera, selectedPosition, mouse, gizmoSize, true);
                else
                    hotAxis = PickRotateGizmo(camera, selectedPosition, mouse, gizmoSize * 0.78f);
            }
        }

        if (!cameraActive && !environmentPanel) {
            bool searchChanged = false;
            bool escapeConsumed = false;
            if (numericField != NumericField::None) {
                int character = GetCharPressed();
                while (character > 0) {
                    if (character >= 32 && character <= 126 && numericBuffer.size() < 32)
                        numericBuffer.push_back((char)character);
                    character = GetCharPressed();
                }
                if (IsKeyPressed(KEY_BACKSPACE) && !numericBuffer.empty())
                    numericBuffer.pop_back();
                if (IsKeyPressed(KEY_ESCAPE)) {
                    numericField = NumericField::None;
                    numericBuffer.clear();
                    escapeConsumed = true;
                    status = "Numeric edit cancelled";
                }
                else if (IsKeyPressed(KEY_ENTER)) {
                    EditorSnapshot before = CaptureEditor(level);
                    bool valid = false;
                    if (numericField == NumericField::CameraHeading) {
                        float heading = 0.0f;
                        valid = ParseFiniteFloat(numericBuffer, heading) &&
                            lvlx_set_level_properties(&level, level.level_score_multiplier,
                                level.level_time_limit_seconds, heading);
                    }
                    else {
                        uint32_t value = 0;
                        valid = ParseUint32(numericBuffer, value);
                        if (valid && numericField == NumericField::Score)
                            valid = lvlx_set_level_properties(&level, value,
                                level.level_time_limit_seconds,
                                level.initial_camera_heading_degrees);
                        else if (valid && numericField == NumericField::TimeLimit)
                            valid = lvlx_set_level_properties(&level,
                                level.level_score_multiplier, value,
                                level.initial_camera_heading_degrees);
                    }
                    if (valid) {
                        const char* name = NumericFieldName(numericField);
                        if (!EditorStatesEqual(before, CaptureEditor(level)))
                            PushUndo(undoStack, redoStack, std::move(before));
                        dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                        status = std::string("Updated ") + name;
                        numericField = NumericField::None;
                        numericBuffer.clear();
                    }
                    else status = std::string("Invalid ") + NumericFieldName(numericField);
                }
            }
            else if (searchFocused) {
                int character = GetCharPressed();
                while (character > 0) {
                    if (character >= 32 && character <= 126 && elementSearch.size() < 80) {
                        elementSearch.push_back((char)character);
                        searchChanged = true;
                    }
                    character = GetCharPressed();
                }
                if (IsKeyPressed(KEY_BACKSPACE) && !elementSearch.empty()) {
                    elementSearch.pop_back();
                    searchChanged = true;
                }
                if (IsKeyPressed(KEY_ENTER)) searchFocused = false;
            }
            if (searchChanged) {
                BuildDefinitionList(definitions, elementSearch, paletteItems);
                paletteScroll = 0;
            }

            if (!escapeConsumed && IsKeyPressed(KEY_ESCAPE)) {
                if (searchFocused) {
                    searchFocused = false;
                    status = "Search closed";
                }
                else if (waypointMode) {
                    if (waypointDragging && pendingWaypointHistory)
                        RestoreEditor(level, pendingWaypointSnapshot);
                    waypointMode = false;
                    selectedWaypoint = -1;
                    waypointDragging = false;
                    pendingWaypointHistory = false;
                    dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                    status = "Waypoint mode closed";
                }
                else if (gizmoDrag.active) {
                    if (spawnSelected && pendingSpawnHistory)
                        RestoreEditor(level, pendingSpawnSnapshot);
                    else if (pendingTransformHistory)
                        RestoreEditor(level, pendingTransformSnapshot);
                    gizmoDrag = GizmoDragState{};
                    pendingTransformHistory = false;
                    pendingSpawnHistory = false;
                    dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                    status = "Transform cancelled";
                }
                else if (selectedDefinition >= 0) {
                    selectedDefinition = -1;
                    status = "Placement cancelled";
                }
                else {
                    selectedElement = -1;
                    selectedElements.clear();
                    spawnSelected = false;
                    status = "Selection cleared";
                }
            }

            const bool ctrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            const bool shiftDown = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            if (kShowLightmapControls && !searchFocused && numericField == NumericField::None && ctrlDown &&
                !gizmoDrag.active && !waypointDragging && IsLetterPressed('B'))
                rebuildLighting();
            if (kShowLightmapControls && !searchFocused && numericField == NumericField::None && !ctrlDown &&
                !gizmoDrag.active && !waypointDragging && IsLetterPressed('L')) {
                lightmapsEnabled = !lightmapsEnabled;
                status = lightmapsEnabled ? "Lightmaps enabled" : "Lightmaps disabled";
            }
            if (!searchFocused && numericField == NumericField::None && !gizmoDrag.active && !waypointDragging && ctrlDown &&
                (IsLetterPressed('Y') || (shiftDown && IsLetterPressed('Z')))) {
                if (!redoStack.empty()) {
                    EditorSnapshot current = CaptureEditor(level);
                    EditorSnapshot next = std::move(redoStack.back());
                    redoStack.pop_back();
                    if (RestoreEditor(level, next)) {
                        undoStack.push_back(std::move(current));
                        if (selectedElement >= (int)level.element_count ||
                            (selectedElement >= 0 && !level.elements[selectedElement].present))
                            selectedElement = -1;
                        selectedElements.erase(std::remove_if(selectedElements.begin(), selectedElements.end(),
                            [&](int index) { return index < 0 || index >= (int)level.element_count ||
                                !level.elements[index].present; }), selectedElements.end());
                        dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                        status = "Redo";
                    } else {
                        redoStack.push_back(std::move(next));
                        status = "Redo failed: out of memory";
                    }
                } else status = "Nothing to redo";
            } else if (!searchFocused && numericField == NumericField::None && !gizmoDrag.active && !waypointDragging && ctrlDown && !shiftDown && IsLetterPressed('Z')) {
                if (!undoStack.empty()) {
                    EditorSnapshot current = CaptureEditor(level);
                    EditorSnapshot previous = std::move(undoStack.back());
                    undoStack.pop_back();
                    if (RestoreEditor(level, previous)) {
                        redoStack.push_back(std::move(current));
                        if (selectedElement >= (int)level.element_count ||
                            (selectedElement >= 0 && !level.elements[selectedElement].present))
                            selectedElement = -1;
                        selectedElements.erase(std::remove_if(selectedElements.begin(), selectedElements.end(),
                            [&](int index) { return index < 0 || index >= (int)level.element_count ||
                                !level.elements[index].present; }), selectedElements.end());
                        dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                        status = "Undo";
                    } else {
                        undoStack.push_back(std::move(previous));
                        status = "Undo failed: out of memory";
                    }
                } else status = "Nothing to undo";
            }

            if (!searchFocused && numericField == NumericField::None && selectedDefinition < 0 && !waypointMode) {
                if (IsLetterPressed('W')) { transformMode = TransformMode::Move; status = "Move mode"; }
                if (IsLetterPressed('E')) { transformMode = TransformMode::Rotate; status = "Rotate mode"; }
            }
            else if (!searchFocused && numericField == NumericField::None && selectedDefinition >= 0 && IsLetterPressed('E')) {
                placementOrientation = (uint8_t)((placementOrientation + 1u) & 3u);
            }

            if (waypointMode && selectedElement >= 0 && selectedWaypoint >= 0 &&
                (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE))) {
                LvlxElement& element = level.elements[selectedElement];
                if (LvlxWaylist* waylist = FindWaylist(level, element.waylist_index)) {
                    EditorSnapshot before = CaptureEditor(level);
                    if (RemoveWaypoint(*waylist, (uint16_t)selectedWaypoint)) {
                        PushUndo(undoStack, redoStack, std::move(before));
                        dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                        status = TextFormat("Deleted waypoint %d", selectedWaypoint);
                        if (selectedWaypoint >= waylist->point_count)
                            selectedWaypoint = (int)waylist->point_count - 1;
                    }
                }
            }
            else if (waypointMode && selectedElement >= 0 && IsLetterPressed('D')) {
                LvlxElement& element = level.elements[selectedElement];
                if (element.waylist_index == UINT32_MAX) {
                    status = "This element has no route to detach";
                }
                else {
                    EditorSnapshot before = CaptureEditor(level);
                    std::unordered_set<uint32_t> candidate{ element.waylist_index };
                    element.waylist_index = UINT32_MAX;
                    const int removed = RemoveUnreferencedWaylists(level, candidate);
                    PushUndo(undoStack, redoStack, std::move(before));
                    dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                    selectedWaypoint = -1;
                    waypointMode = false;
                    status = TextFormat("Route detached%s", removed ? " and unreferenced record removed" : "");
                }
            }
            else if (!waypointMode && selectedDefinition < 0 && !selectedElements.empty() &&
                (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE))) {
                EditorSnapshot before = CaptureEditor(level);
                int removed = 0;
                std::unordered_set<uint32_t> removedRouteKeys;
                for (int index : selectedElements) {
                    if (index >= 0 && index < (int)level.element_count &&
                        level.elements[index].present &&
                        level.elements[index].waylist_index != UINT32_MAX)
                        removedRouteKeys.insert(level.elements[index].waylist_index);
                    if (index >= 0 && lvlx_remove_element(&level, (uint32_t)index)) ++removed;
                }
                if (removed > 0) {
                    const int routesRemoved = RemoveUnreferencedWaylists(level, removedRouteKeys);
                    PushUndo(undoStack, redoStack, std::move(before));
                    status = TextFormat("Deleted %d element%s and %d orphan route%s", removed,
                        removed == 1 ? "" : "s", routesRemoved, routesRemoved == 1 ? "" : "s");
                    selectedElement = -1;
                    selectedElements.clear();
                    gizmoDrag = GizmoDragState{};
                    pendingTransformHistory = false;
                    dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                }
            }

            if (!searchFocused && numericField == NumericField::None && !waypointMode && ctrlDown && IsLetterPressed('C') &&
                !selectedElements.empty()) {
                CopyElementsToClipboard(level, selectedElements, elementClipboard, clipboardAnchor);
                status = TextFormat("Copied %d element%s", (int)elementClipboard.size(),
                    elementClipboard.size() == 1 ? "" : "s");
            }
            if (!searchFocused && numericField == NumericField::None && !waypointMode && ctrlDown && IsLetterPressed('X') &&
                !selectedElements.empty()) {
                CopyElementsToClipboard(level, selectedElements, elementClipboard, clipboardAnchor);
                EditorSnapshot before = CaptureEditor(level);
                int removed = 0;
                std::unordered_set<uint32_t> removedRouteKeys;
                for (int index : selectedElements) {
                    if (index >= 0 && index < (int)level.element_count &&
                        level.elements[index].present &&
                        level.elements[index].waylist_index != UINT32_MAX)
                        removedRouteKeys.insert(level.elements[index].waylist_index);
                    if (index >= 0 && lvlx_remove_element(&level, (uint32_t)index)) ++removed;
                }
                if (removed > 0) {
                    const int routesRemoved = RemoveUnreferencedWaylists(level, removedRouteKeys);
                    PushUndo(undoStack, redoStack, std::move(before));
                    selectedElement = -1;
                    selectedElements.clear();
                    dirty = true;
                    status = TextFormat("Cut %d element%s; removed %d orphan route%s", removed,
                        removed == 1 ? "" : "s", routesRemoved, routesRemoved == 1 ? "" : "s");
                }
            }
            if (!searchFocused && numericField == NumericField::None && !waypointMode && ctrlDown && IsLetterPressed('V') &&
                !elementClipboard.empty()) {
                Vector3 destination = clipboardAnchor;
                if (!mouseOverPanel) {
                    Vector3 mouseWorld{};
                    if (RayGroundIntersection(camera, mouse, clipboardAnchor.y, mouseWorld))
                        destination = mouseWorld;
                }
                if (moveSnapEnabled) destination = SnapPosition(destination, moveSnapStep, false);
                EditorSnapshot before = CaptureEditor(level);
                std::vector<int> pasted;
                if (PasteElementsFromClipboard(level, elementClipboard, clipboardAnchor,
                    destination, companionLmdKeys, pasted)) {
                    PushUndo(undoStack, redoStack, std::move(before));
                    selectedElements = std::move(pasted);
                    selectedElement = selectedElements.empty() ? -1 : selectedElements.back();
                    spawnSelected = false;
                    dirty = true;
                    status = TextFormat("Pasted %d element%s", (int)selectedElements.size(),
                        selectedElements.size() == 1 ? "" : "s");
                }
                else {
                    RestoreEditor(level, before);
                    status = "Paste failed: out of memory";
                }
            }

            if (!searchFocused && numericField == NumericField::None && ctrlDown && IsLetterPressed('S')) {
                saveLevel();
            }

            if (mouseOverPanel) {
                const int visibleRows = (int)((GetScreenHeight() - paletteTop - 18.0f) / rowHeight);
                const int maxScroll = paletteItems.size() > (size_t)visibleRows
                    ? (int)paletteItems.size() - visibleRows : 0;
                paletteScroll -= (int)GetMouseWheelMove();
                paletteScroll = (int)Clamp((float)paletteScroll, 0.0f, (float)maxScroll);

                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    const Rectangle openMapButton{ 12, 48, 106, 24 };
                    const Rectangle waypointButton{ 12, 120, 148, 26 };
                    const Rectangle spawnButton{ 170, 120, 148, 26 };
                    const Rectangle scoreMinus{ 224, 152, 38, 24 };
                    const Rectangle scorePlus{ 270, 152, 38, 24 };
                    const Rectangle timeMinus{ 224, 182, 38, 24 };
                    const Rectangle timePlus{ 270, 182, 38, 24 };
                    const Rectangle headingMinus{ 224, 212, 38, 24 };
                    const Rectangle headingPlus{ 270, 212, 38, 24 };
                    const Rectangle scoreValue{ 120, 152, 96, 24 };
                    const Rectangle timeValue{ 120, 182, 96, 24 };
                    const Rectangle headingValue{ 120, 212, 96, 24 };
                    const Rectangle snapToggle{ 12, 246, 164, 24 };
                    const Rectangle snapMinus{ 224, 246, 38, 24 };
                    const Rectangle snapPlus{ 270, 246, 38, 24 };
                    const Rectangle searchBox{ 12, 286, panelWidth - 24, 26 };
                    searchFocused = CheckCollisionPointRec(mouse, searchBox);
                    if (searchFocused) numericField = NumericField::None;
                    const auto beginNumericEdit = [&](NumericField field, std::string value) {
                        numericField = field;
                        numericBuffer = std::move(value);
                        searchFocused = false;
                        status = std::string("Editing ") + NumericFieldName(field) +
                            "; Enter applies, Esc cancels";
                    };
                    if (level.has_spawn_data && CheckCollisionPointRec(mouse, scoreValue))
                        beginNumericEdit(NumericField::Score,
                            std::to_string(level.level_score_multiplier));
                    else if (level.has_spawn_data && CheckCollisionPointRec(mouse, timeValue))
                        beginNumericEdit(NumericField::TimeLimit,
                            std::to_string(level.level_time_limit_seconds));
                    else if (level.has_spawn_data && CheckCollisionPointRec(mouse, headingValue))
                        beginNumericEdit(NumericField::CameraHeading,
                            TextFormat("%.9g", level.initial_camera_heading_degrees));

                    if (CheckCollisionPointRec(mouse, waypointButton)) {
                        if (selectedElement < 0 || selectedElement >= (int)level.element_count) {
                            status = "Select an element before editing its waypoints";
                        } else {
                            const LvlxElementDefinition* definition = lvlx_find_definition(
                                &definitions, level.elements[selectedElement].definition_id);
                            if (!DefinitionSupportsWaylists(definition))
                                status = "This element type does not use waypoints";
                            else {
                                waypointMode = !waypointMode;
                                selectedWaypoint = -1;
                                waypointDragging = false;
                                selectedDefinition = -1;
                                status = waypointMode
                                    ? "ROUTE: click assigns/adds; drag moves; Delete point; D detaches; Esc exits"
                                    : "Waypoint mode closed";
                            }
                        }
                    }
                    if (CheckCollisionPointRec(mouse, spawnButton) && level.has_spawn_data) {
                        waypointMode = false;
                        selectedDefinition = -1;
                        selectedElement = -1;
                        selectedElements.clear();
                        spawnSelected = true;
                        status = "Spawn selected; use W/E and the gizmo to move or rotate it";
                    }

                    int scoreDelta = CheckCollisionPointRec(mouse, scoreMinus) ? -1
                        : CheckCollisionPointRec(mouse, scorePlus) ? 1 : 0;
                    int timeDelta = CheckCollisionPointRec(mouse, timeMinus) ? -30
                        : CheckCollisionPointRec(mouse, timePlus) ? 30 : 0;
                    int headingDelta = CheckCollisionPointRec(mouse, headingMinus) ? -90
                        : CheckCollisionPointRec(mouse, headingPlus) ? 90 : 0;
                    if (level.has_spawn_data && (scoreDelta || timeDelta || headingDelta)) {
                        EditorSnapshot before = CaptureEditor(level);
                        uint32_t score = level.level_score_multiplier;
                        uint32_t seconds = level.level_time_limit_seconds;
                        if (scoreDelta < 0 && score > 0) --score;
                        if (scoreDelta > 0 && score < UINT32_MAX) ++score;
                        if (timeDelta < 0) seconds = seconds >= 30 ? seconds - 30 : 0;
                        if (timeDelta > 0 && seconds <= UINT32_MAX - 30) seconds += 30;
                        const float heading = level.initial_camera_heading_degrees + (float)headingDelta;
                        if (lvlx_set_level_properties(&level, score, seconds, heading) &&
                            !EditorStatesEqual(before, CaptureEditor(level))) {
                            PushUndo(undoStack, redoStack, std::move(before));
                            dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                            status = TextFormat("Level: score x%u | time %us | camera %.0f deg",
                                score, seconds, heading);
                        }
                    }
                    if (CheckCollisionPointRec(mouse, snapToggle)) moveSnapEnabled = !moveSnapEnabled;
                    if (CheckCollisionPointRec(mouse, snapMinus))
                        moveSnapStep = fmaxf(0.05f, moveSnapStep - 0.25f);
                    if (CheckCollisionPointRec(mouse, snapPlus))
                        moveSnapStep = fminf(100.0f, moveSnapStep + 0.25f);

                    if (CheckCollisionPointRec(mouse, openMapButton)) {
                        if (dirty || (kShowLightmapControls && lightingDirty)) {
                            status = kShowLightmapControls && lightingDirty
                                ? "Bake or undo lighting changes before switching maps"
                                : "Save or undo changes before switching maps";
                        }
                        else {
                            std::string pickerError;
                            const std::string requestedPath = PickMapFile(GetWindowHandle(), levelPath, pickerError);
                            if (requestedPath.empty()) {
                                if (!pickerError.empty()) status = pickerError;
                                continue;
                            }
                            // Mod levels use their installation's shared assets. Loose
                            // maps outside an installation keep the current resource root.
                            const auto discoveredBin = mod_project::findBin(requestedPath);
                            const std::string replacementBin = discoveredBin.empty()
                                ? binDirectory : NormalizePath(discoveredBin.string());
                            const std::string replacementMod=FindModDirectory(requestedPath);
                            const bool resourcesChanged = Lowercase(replacementMod)!=Lowercase(modDirectory)||Lowercase(NormalizePath(
                                std::filesystem::absolute(replacementBin).string())) !=
                                Lowercase(NormalizePath(std::filesystem::absolute(binDirectory).string()));
                            LvlxLevel replacement{};
                            LvlxDefinitionTable replacementDefinitions{};
                            if (resourcesChanged && !lvlx_load_definitions(
                                    JoinPath(replacementBin, "settings/elements.txt").c_str(), &replacementDefinitions)) {
                                lvlx_free_definitions(&replacementDefinitions);
                                status = "Could not load selected map's element definitions - current map retained";
                                continue;
                            }
                            const std::string replacementModDefinitions=FindModElementDefinitions(requestedPath);
                            if(resourcesChanged&&!replacementModDefinitions.empty()&&
                                !lvlx_append_definitions(replacementModDefinitions.c_str(),&replacementDefinitions)){
                                lvlx_free_definitions(&replacementDefinitions);status="Could not append selected mod's element definitions";continue;
                            }
                            if (lvlx_load_level(requestedPath.c_str(), &replacement)) {
                                effectPreview.Unload();
                                effectElapsed = 0;
                                animationSeconds = 0;
                                if (resourcesChanged) {
                                    UnloadRenderAssets(assets);
                                    std::vector<RenderAsset> otherAssets;
                                    otherAssets.push_back(std::move(santaAsset));
                                    otherAssets.push_back(std::move(skyAsset));
                                    UnloadRenderAssets(otherAssets);
                                    santaAsset = {}; skyAsset = {};
                                    lvlx_free_definitions(&definitions);
                                    definitions = replacementDefinitions;
                                    replacementDefinitions = {};
                                    assets.clear(); assets.resize(definitions.count);
                                    binDirectory = replacementBin;
                                    ConfigureResourceDirectories(requestedPath,binDirectory,modDirectory,assetsDirectory,texturesDirectory,effectsDirectory);
                                    physics.load(JoinPath(binDirectory, "settings/physics.txt"));
                                    BuildDefinitionList(definitions, elementSearch, paletteItems);
                                    paletteScroll = 0;
                                }
                                lvlx_free_level(&level);
                                level = replacement;
                                levelPath = NormalizePath(requestedPath);
                                LoadEnvironment(binDirectory, levelPath);
                                companionLmdKeys = LoadLmdAttachmentKeys(levelPath);
                                const bool validLightmaps = LoadLevelLightmaps(levelPath, texturesDirectory, lightmaps);
                                selectedElement = -1;
                                selectedElements.clear();
                                waypointMode = false;
                                selectedWaypoint = -1;
                                waypointDragging = false;
                                spawnSelected = false;
                                selectedDefinition = -1;
                                gizmoDrag = GizmoDragState{};
                                elementClipboard.clear();
                                pendingWaypointHistory = pendingSpawnHistory = pendingTransformHistory = false;
                                marqueeActive = false;
                                undoStack.clear();
                                redoStack.clear();
                                savedSnapshot = CaptureEditor(level);
                                bakedLightingSnapshot = savedSnapshot;
                                lightingDirty = false;
                                liveShadowSnapshotValid = false;
                                gLiveShadowValid = false;
                                PositionCameraAtLevelStart(camera, level);
                                status = std::string("Loaded: ") + levelPath;
                                if (!validLightmaps) status += " | LMD companion is invalid";
                            }
                            else status = "Could not load selected map - see console";
                            lvlx_free_definitions(&replacementDefinitions);
                        }
                    }
                }

                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && mouse.y >= paletteTop) {
                    const int row = (int)((mouse.y - paletteTop) / rowHeight);
                    const int itemIndex = paletteScroll + row;
                    if (itemIndex >= 0 && itemIndex < (int)paletteItems.size()) {
                        selectedDefinition = (int)paletteItems[(size_t)itemIndex];
                        selectedElement = -1;
                        selectedElements.clear();
                        spawnSelected = false;
                        gizmoDrag = GizmoDragState{};
                        placementOrientation = 0;
                        const LvlxElementDefinition& d = definitions.items[selectedDefinition];
                        status = std::string("Placing: ") + (d.name ? d.name : "<unnamed>");
                    }
                }
            }
            else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (waypointMode && selectedElement >= 0) {
                    LvlxElement& element = level.elements[selectedElement];
                    LvlxWaylist* waylist = FindWaylist(level, element.waylist_index);
                    const Ray ray = GetMouseRay(mouse, camera);
                    int hitPoint = -1;
                    float hitDistance = FLT_MAX;
                    if (waylist) {
                        for (uint16_t p = 0; p < waylist->point_count; ++p) {
                            const RayCollision hit = GetRayCollisionSphere(ray,
                                ToRaylib(waylist->points[p]), 0.42f);
                            if (hit.hit && hit.distance < hitDistance) {
                                hitDistance = hit.distance;
                                hitPoint = (int)p;
                            }
                        }
                    }
                    if (hitPoint >= 0) {
                        selectedWaypoint = hitPoint;
                        waypointDragging = true;
                        pendingWaypointSnapshot = CaptureEditor(level);
                        pendingWaypointHistory = true;
                        status = TextFormat("Moving waypoint %d", selectedWaypoint);
                    }
                    else {
                        Vector3 worldPosition{};
                        const float pathY = ToRaylib(element.vector1).y;
                        if (RayGroundIntersection(camera, mouse, pathY, worldPosition)) {
                            if (moveSnapEnabled)
                                worldPosition = SnapPosition(worldPosition, moveSnapStep, true);
                            EditorSnapshot before = CaptureEditor(level);
                            waylist = EnsureElementWaylist(level, element);
                            if (waylist && AppendWaypoint(*waylist, FromRaylib(worldPosition))) {
                                selectedWaypoint = (int)waylist->point_count - 1;
                                PushUndo(undoStack, redoStack, std::move(before));
                                dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                                status = TextFormat("Added waypoint %d", selectedWaypoint);
                            }
                            else {
                                RestoreEditor(level, before);
                                status = "Could not add waypoint";
                            }
                        }
                    }
                }
                else if (selectedDefinition >= 0) {
                    Vector3 worldPosition{};
                    if (PlacementAnchor(level, definitions, assets, camera, mouse,
                            worldPosition)) {
                        if (moveSnapEnabled) worldPosition = SnapPosition(worldPosition, moveSnapStep, false);
                        worldPosition = ResolvePlacementPosition(level, definitions, assets,
                            (uint32_t)selectedDefinition, placementOrientation, worldPosition);
                        LvlxElement element{};
                        element.present = 1;
                        element.definition_id = (uint32_t)selectedDefinition;
                        element.lightmap_attachment_key = AllocateAttachmentKey(level, companionLmdKeys);
                        element.vector1 = FromRaylib(worldPosition);
                        element.vector2 = element.vector1;
                        element.orientation = placementOrientation;
                        element.waylist_index = UINT32_MAX;
                        uint32_t newIndex = 0;
                        EditorSnapshot before = CaptureEditor(level);
                        if (lvlx_add_element(&level, &element, &newIndex)) {
                            PushUndo(undoStack, redoStack, std::move(before));
                            dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                            status = TextFormat("Placed element %u", newIndex);
                        }
                    }
                }
                else if (spawnSelected && hotAxis != GizmoAxis::None) {
                    gizmoDrag = GizmoDragState{};
                    gizmoDrag.active = true;
                    gizmoDrag.axis = hotAxis;
                    gizmoDrag.startPosition = ToRaylib(level.spawn_position);
                    gizmoDrag.startSecond = gizmoDrag.startPosition;
                    spawnTransformStart = level.spawn_position;
                    cameraHeadingStart = level.initial_camera_heading_degrees;
                    pendingSpawnSnapshot = CaptureEditor(level);
                    pendingSpawnHistory = true;
                    if (transformMode == TransformMode::Move) {
                        if (hotAxis == GizmoAxis::Free) {
                            gizmoDrag.planeNormal = Vector3{ 0, 1, 0 };
                            if (!RayPlaneIntersection(camera, mouse, gizmoDrag.startPosition,
                                gizmoDrag.planeNormal, gizmoDrag.startPlaneHit))
                                gizmoDrag.active = false;
                        }
                        else {
                            gizmoDrag.axisDirection = GizmoAxisVector(hotAxis);
                            if (!ClosestAxisParameter(camera, mouse, gizmoDrag.startPosition,
                                gizmoDrag.axisDirection, gizmoDrag.startAxisParameter))
                                gizmoDrag.active = false;
                        }
                    }
                    else if (hotAxis == GizmoAxis::Y) {
                        Vector3 hit{};
                        if (RayGroundIntersection(camera, mouse, gizmoDrag.startPosition.y, hit))
                            gizmoDrag.startAngle = AngleAroundY(gizmoDrag.startPosition, hit);
                        else gizmoDrag.active = false;
                    }
                    else {
                        gizmoDrag.active = false;
                        status = "Initial camera heading rotates around Y only";
                    }
                }
                else if (selectedElement >= 0 && hotAxis != GizmoAxis::None) {
                    LvlxElement* element = lvlx_get_element(&level, (uint32_t)selectedElement);
                    if (element) {
                        if (transformMode == TransformMode::Rotate && hotAxis != GizmoAxis::Y) {
                            status = "LVLX currently exposes only one orientation value; X/Z rotation is not known yet";
                        }
                        else {
                            gizmoDrag = GizmoDragState{};
                            pendingTransformSnapshot = CaptureEditor(level);
                            pendingTransformHistory = true;
                            gizmoDrag.active = true;
                            gizmoDrag.axis = hotAxis;
                            gizmoDrag.startPosition = ToRaylib(element->vector1);
                            gizmoDrag.startSecond = ToRaylib(element->vector2);
                            gizmoDrag.startOrientation = element->orientation;

                            if (transformMode == TransformMode::Move) {
                                if (hotAxis == GizmoAxis::Free) {
                                    // Free movement is horizontal (X/Z) only.
                                    // Vertical movement is deliberately reserved for the Y gizmo axis.
                                    gizmoDrag.planeNormal = Vector3{ 0, 1, 0 };
                                    if (!RayPlaneIntersection(camera, mouse, gizmoDrag.startPosition,
                                        gizmoDrag.planeNormal, gizmoDrag.startPlaneHit))
                                        gizmoDrag.active = false;
                                }
                                else {
                                    gizmoDrag.axisDirection = GizmoAxisVector(hotAxis);
                                    if (!ClosestAxisParameter(camera, mouse, gizmoDrag.startPosition,
                                        gizmoDrag.axisDirection, gizmoDrag.startAxisParameter))
                                        gizmoDrag.active = false;
                                }
                            }
                            else if (transformMode == TransformMode::Rotate) {
                                Vector3 hit{};
                                if (RayGroundIntersection(camera, mouse, gizmoDrag.startPosition.y, hit))
                                    gizmoDrag.startAngle = AngleAroundY(gizmoDrag.startPosition, hit);
                                else gizmoDrag.active = false;
                            }
                            if (!gizmoDrag.active) pendingTransformHistory = false;
                        }
                    }
                }
                else {
                    const Ray spawnRay = GetMouseRay(mouse, camera);
                    const Vector3 spawnPosition = ToRaylib(level.spawn_position);
                    const RayCollision spawnHit = level.has_spawn_data
                        ? GetRayCollisionSphere(spawnRay,
                            Vector3Add(spawnPosition, Vector3{ 0, 0.7f, 0 }), 1.4f)
                        : RayCollision{};
                    if (spawnHit.hit) {
                        selectedElement = -1;
                        selectedElements.clear();
                        if (spawnSelected && transformMode == TransformMode::Move) {
                            gizmoDrag = GizmoDragState{};
                            gizmoDrag.active = true;
                            gizmoDrag.axis = GizmoAxis::Free;
                            gizmoDrag.startPosition = spawnPosition;
                            gizmoDrag.startSecond = spawnPosition;
                            gizmoDrag.planeNormal = Vector3{ 0, 1, 0 };
                            spawnTransformStart = level.spawn_position;
                            cameraHeadingStart = level.initial_camera_heading_degrees;
                            pendingSpawnSnapshot = CaptureEditor(level);
                            pendingSpawnHistory = true;
                            if (!RayPlaneIntersection(camera, mouse, spawnPosition,
                                gizmoDrag.planeNormal, gizmoDrag.startPlaneHit))
                                gizmoDrag = GizmoDragState{};
                            else status = "Moving spawn on X/Z plane";
                        }
                        else {
                            spawnSelected = true;
                            gizmoDrag = GizmoDragState{};
                            status = TextFormat("Respawn selected: %.2f %.2f %.2f | camera %.0f deg",
                                spawnPosition.x, spawnPosition.y, spawnPosition.z,
                                level.initial_camera_heading_degrees);
                        }
                    }
                    else {
                    const int picked = PickElement(level, definitions, assets, camera, mouse);

                    // First click selects only. Once an element is already selected,
                    // dragging its body in Move mode performs free horizontal X/Z movement.
                    // Up/down movement remains Y-axis-gizmo-only.
                    if (transformMode == TransformMode::Move &&
                        selectedElement >= 0 && picked == selectedElement) {
                        LvlxElement* element = lvlx_get_element(&level, (uint32_t)selectedElement);
                        if (element) {
                            gizmoDrag = GizmoDragState{};
                            pendingTransformSnapshot = CaptureEditor(level);
                            pendingTransformHistory = true;
                            gizmoDrag.active = true;
                            gizmoDrag.axis = GizmoAxis::Free;
                            gizmoDrag.startPosition = ToRaylib(element->vector1);
                            gizmoDrag.startSecond = ToRaylib(element->vector2);
                            gizmoDrag.startOrientation = element->orientation;
                            gizmoDrag.planeNormal = Vector3{ 0, 1, 0 };

                            if (!RayPlaneIntersection(camera, mouse, gizmoDrag.startPosition,
                                gizmoDrag.planeNormal, gizmoDrag.startPlaneHit)) {
                                gizmoDrag = GizmoDragState{};
                                pendingTransformHistory = false;
                            }
                            else {
                                status = TextFormat("Free-dragging element %d on X/Z plane", selectedElement);
                            }
                        }
                    }
                    else {
                        // Selection is deliberately separate from transformation:
                        // selecting a new element never moves it immediately.
                        selectedElement = picked;
                        gizmoDrag = GizmoDragState{};
                        if (picked >= 0) {
                            selectedElements.assign(1, picked);
                            spawnSelected = false;
                            const LvlxElement& element = level.elements[picked];
                            const LvlxElementDefinition* d = lvlx_find_definition(&definitions, element.definition_id);
                            status = TextFormat("Selected element %d: %s", picked,
                                d && d->name ? d->name : "<unnamed>");
                        }
                        else {
                            marqueeActive = true;
                            marqueeStart = mouse;
                            marqueeCurrent = mouse;
                            status = "Drag to select multiple elements";
                        }
                    }
                    }
                }
            }

            if (marqueeActive && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
                marqueeCurrent = mouse;
            if (marqueeActive && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                marqueeCurrent = mouse;
                const float left = fminf(marqueeStart.x, marqueeCurrent.x);
                const float top = fminf(marqueeStart.y, marqueeCurrent.y);
                const Rectangle box{ left, top, fabsf(marqueeCurrent.x - marqueeStart.x),
                    fabsf(marqueeCurrent.y - marqueeStart.y) };
                selectedElements.clear();
                if (box.width >= 4.0f || box.height >= 4.0f) {
                    for (uint32_t i = 0; i < level.element_count; ++i) {
                        if (!level.elements[i].present) continue;
                        const Vector2 screen = GetWorldToScreen(ToRaylib(level.elements[i].vector1), camera);
                        if (CheckCollisionPointRec(screen, box)) selectedElements.push_back((int)i);
                    }
                }
                selectedElement = selectedElements.empty() ? -1 : selectedElements.back();
                spawnSelected = false;
                status = selectedElements.empty() ? "No element selected"
                    : TextFormat("Selected %d elements", (int)selectedElements.size());
                marqueeActive = false;
            }

            if (waypointMode && waypointDragging && selectedElement >= 0 &&
                IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                LvlxElement& element = level.elements[selectedElement];
                if (LvlxWaylist* waylist = FindWaylist(level, element.waylist_index)) {
                    if (selectedWaypoint >= 0 && selectedWaypoint < waylist->point_count) {
                        Vector3 worldPosition{};
                        const float pathY = ToRaylib(waylist->points[selectedWaypoint]).y;
                        if (RayGroundIntersection(camera, mouse, pathY, worldPosition)) {
                            if (moveSnapEnabled)
                                worldPosition = SnapPosition(worldPosition, moveSnapStep, true);
                            waylist->points[selectedWaypoint] = FromRaylib(worldPosition);
                            dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                        }
                    }
                }
            }
            if (waypointDragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                waypointDragging = false;
                const EditorSnapshot after = CaptureEditor(level);
                if (pendingWaypointHistory && !EditorStatesEqual(pendingWaypointSnapshot, after)) {
                    PushUndo(undoStack, redoStack, std::move(pendingWaypointSnapshot));
                    status = TextFormat("Waypoint %d moved", selectedWaypoint);
                } else status = "Waypoint unchanged";
                pendingWaypointHistory = false;
            }

            if (gizmoDrag.active && spawnSelected && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                if (transformMode == TransformMode::Move) {
                    Vector3 target = gizmoDrag.startPosition;
                    if (gizmoDrag.axis == GizmoAxis::Free) {
                        Vector3 hit{};
                        if (RayPlaneIntersection(camera, mouse, gizmoDrag.startPosition,
                            gizmoDrag.planeNormal, hit)) {
                            target = Vector3Add(gizmoDrag.startPosition,
                                Vector3Subtract(hit, gizmoDrag.startPlaneHit));
                            if (moveSnapEnabled) target = SnapPosition(target, moveSnapStep, false);
                        }
                    }
                    else {
                        float parameter = 0.0f;
                        if (ClosestAxisParameter(camera, mouse, gizmoDrag.startPosition,
                            gizmoDrag.axisDirection, parameter)) {
                            float delta = parameter - gizmoDrag.startAxisParameter;
                            if (moveSnapEnabled) delta = SnapValue(delta, moveSnapStep);
                            target = Vector3Add(gizmoDrag.startPosition,
                                Vector3Scale(gizmoDrag.axisDirection, delta));
                        }
                    }
                    lvlx_set_spawn_transform(&level, FromRaylib(target), level.initial_camera_heading_degrees);
                    dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                }
                else if (gizmoDrag.axis == GizmoAxis::Y) {
                    Vector3 hit{};
                    if (RayGroundIntersection(camera, mouse, gizmoDrag.startPosition.y, hit)) {
                        const float angle = AngleAroundY(gizmoDrag.startPosition, hit);
                        float deltaDegrees = (angle - gizmoDrag.startAngle) * RAD2DEG;
                        deltaDegrees = SnapValue(deltaDegrees, 90.0f);
                        // The preview uses 180 - heading, so add the drag angle
                        // to keep the visible rotation following the mouse.
                        lvlx_set_spawn_transform(&level, level.spawn_position,
                            cameraHeadingStart + deltaDegrees);
                        dirty = !EditorStatesEqual(CaptureEditor(level), savedSnapshot);
                    }
                }
            }

            if (gizmoDrag.active && !spawnSelected && selectedElement >= 0 &&
                IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                LvlxElement* element = lvlx_get_element(&level, (uint32_t)selectedElement);
                if (element) {
                    if (transformMode == TransformMode::Move) {
                        Vector3 target = gizmoDrag.startPosition;
                        if (gizmoDrag.axis == GizmoAxis::Free) {
                            Vector3 hit{};
                            if (RayPlaneIntersection(camera, mouse, gizmoDrag.startPosition,
                                gizmoDrag.planeNormal, hit)) {
                                Vector3 delta = Vector3Subtract(hit, gizmoDrag.startPlaneHit);
                                target = Vector3Add(gizmoDrag.startPosition, delta);
                                // Free dragging never changes Y; snap X/Z only.
                                if (moveSnapEnabled) target = SnapPosition(target, moveSnapStep, false);
                            }
                        }
                        else {
                            float parameter = 0.0f;
                            if (ClosestAxisParameter(camera, mouse, gizmoDrag.startPosition,
                                gizmoDrag.axisDirection, parameter)) {
                                float delta = parameter - gizmoDrag.startAxisParameter;
                                if (moveSnapEnabled) delta = SnapValue(delta, moveSnapStep);
                                target = Vector3Add(gizmoDrag.startPosition,
                                    Vector3Scale(gizmoDrag.axisDirection, delta));
                            }
                        }
                        const Vector3 delta = Vector3Subtract(target, gizmoDrag.startPosition);
                        for (int index : selectedElements) {
                            if (index < 0 || index >= (int)pendingTransformSnapshot.elements.size()) continue;
                            const LvlxElement& original = pendingTransformSnapshot.elements[index];
                            if (!original.present) continue;
                            lvlx_set_element_transform(&level, (uint32_t)index,
                                FromRaylib(Vector3Add(ToRaylib(original.vector1), delta)),
                                FromRaylib(Vector3Add(ToRaylib(original.vector2), delta)),
                                original.orientation);
                        }
                        dirty = true;
                    }
                    else if (transformMode == TransformMode::Rotate) {
                        Vector3 hit{};
                        if (RayGroundIntersection(camera, mouse, gizmoDrag.startPosition.y, hit)) {
                            const float angle = AngleAroundY(gizmoDrag.startPosition, hit);
                            float deltaDegrees = (angle - gizmoDrag.startAngle) * RAD2DEG;
                            while (deltaDegrees > 180.0f) deltaDegrees -= 360.0f;
                            while (deltaDegrees < -180.0f) deltaDegrees += 360.0f;

                            deltaDegrees = SnapValue(deltaDegrees, 90.0f);
                            const int quarterDelta = (int)roundf(deltaDegrees / 90.0f);
                            for (int index : selectedElements) {
                                if (index < 0 || index >= (int)pendingTransformSnapshot.elements.size()) continue;
                                const LvlxElement& original = pendingTransformSnapshot.elements[index];
                                if (!original.present) continue;
                                int orientation = ((int)(original.orientation & 3u) - quarterDelta) % 4;
                                if (orientation < 0) orientation += 4;
                                level.elements[index].orientation = (uint8_t)orientation;
                            }
                            dirty = true;
                        }
                    }
                }
            }
            if (gizmoDrag.active && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                if (spawnSelected) {
                    const EditorSnapshot after = CaptureEditor(level);
                    if (pendingSpawnHistory && !EditorStatesEqual(pendingSpawnSnapshot, after)) {
                        PushUndo(undoStack, redoStack, std::move(pendingSpawnSnapshot));
                        status = TextFormat("Respawn updated: %.2f %.2f %.2f | camera %.0f deg",
                            level.spawn_position.x, level.spawn_position.y, level.spawn_position.z,
                            level.initial_camera_heading_degrees);
                    } else status = "Respawn unchanged";
                    dirty = !EditorStatesEqual(after, savedSnapshot);
                    pendingSpawnHistory = false;
                }
                else {
                    EditorSnapshot after = CaptureEditor(level);
                    if (pendingTransformHistory && !EditorStatesEqual(pendingTransformSnapshot, after)) {
                        PushUndo(undoStack, redoStack, std::move(pendingTransformSnapshot));
                        dirty = !EditorStatesEqual(after, savedSnapshot);
                        status = TextFormat("%s applied to element %d", TransformModeName(transformMode), selectedElement);
                    } else {
                        dirty = !EditorStatesEqual(after, savedSnapshot);
                        status = "Transform unchanged";
                    }
                }
                pendingTransformHistory = false;
                gizmoDrag = GizmoDragState{};
            }
        }

        Vector3 previewPosition{};
        const bool hasPlacementPreview = !cameraActive && !mouseOverPanel && selectedDefinition >= 0 &&
            PlacementAnchor(level, definitions, assets, camera, mouse, previewPosition);
        if (hasPlacementPreview) {
            if (moveSnapEnabled)
                previewPosition = SnapPosition(previewPosition, moveSnapStep, false);
            previewPosition = ResolvePlacementPosition(level, definitions, assets,
                (uint32_t)selectedDefinition, placementOrientation, previewPosition);
        }
        if (!cameraActive && !searchFocused && numericField == NumericField::None &&
            IsLetterPressed('P')) effectsEnabled = !effectsEnabled;
        if (!cameraActive && !environmentPanel && !searchFocused && numericField == NumericField::None) {
            if (IsKeyPressed(KEY_F3)) {
                collisionsEnabled=!collisionsEnabled;
                status = !physics.error.empty() ? "Collision preview unavailable: " + physics.error
                    : collisionsEnabled ? "Initial collision shapes shown (F3): visual model scale is ignored"
                    : "Collision overlay hidden";
            }
            if (IsKeyPressed(KEY_F4)) {
                animationsEnabled=!animationsEnabled; animationSeconds=0;
                status=animationsEnabled ? "Animation preview on (F4): first authored action, scalar pose route"
                    : "Animation preview off: bind geometry restored";
            }
        }
        if (animationsEnabled) animationSeconds += previewKnowledge ? 1.0/30.0 : GetFrameTime();
        for(auto& asset:assets) UpdateAnimationPreview(asset,animationsEnabled,animationSeconds);

        Vector3 activeGridCenter{};
        bool hasActiveGrid = false;
        if (hasPlacementPreview) {
            activeGridCenter = previewPosition;
            hasActiveGrid = true;
        }
        else if (!waypointMode && spawnSelected && level.has_spawn_data) {
            activeGridCenter = ToRaylib(level.spawn_position);
            hasActiveGrid = true;
        }
        else if (!waypointMode && selectedElement >= 0 &&
            selectedElement < (int)level.element_count && level.elements[selectedElement].present) {
            activeGridCenter = ToRaylib(level.elements[selectedElement].vector1);
            hasActiveGrid = true;
        }

        const EditorSnapshot currentLightingState = CaptureEditor(level);
        lightingDirty = !LightingStatesEqual(currentLightingState,
            bakedLightingSnapshot);
        // Normal editing deliberately keeps the last stable baked preview.
        // The live shadow path is retained only for its explicit visual smoke
        // mode and as the source used by Ctrl+B; it can over-occlude while a
        // transform is in progress and must not darken the interactive view.
        gLiveShadowEnabled = previewLiveShadows && lightingDirty && lightmapsEnabled;
        if (gLiveShadowEnabled && (!liveShadowSnapshotValid ||
                !LightingStatesEqual(currentLightingState, liveShadowSnapshot))) {
            RenderLiveShadowMap(level, definitions, assets);
            liveShadowSnapshot = currentLightingState;
            liveShadowSnapshotValid = gLiveShadowValid;
        }

        if (effectsEnabled) {
            const auto effectNearClipping = gEnvironment.numbers(19);
            effectPreview.BuildFrame(level, definitions, camera, effectsDirectory,
                texturesDirectory,gFallbackEffectsDirectory,gFallbackTexturesDirectory,effectElapsed,effectNearClipping[0],
                effectNearClipping[1]);
        } else {
            const int noEffectLights = 0;
            SetShaderValue(gCrfShader, GetShaderLocation(gCrfShader, "effectLightCount"),
                &noEffectLights, SHADER_UNIFORM_INT);
        }
        BeginDrawing();
        // Keep empty space outside the sky/death-shadow geometry truly black.
        // The editor panels draw their own backgrounds below.
        ClearBackground(BLACK);
        BeginMode3D(camera);
        ApplyEnvironmentPreview(camera);
        DrawGameSky(camera, skyAsset, assetsDirectory, texturesDirectory,
            loadedAssetCount, missingAssetCount);
        if (hasActiveGrid) DrawActiveGrid(activeGridCenter);
        DrawLine3D(Vector3{ 0,0,0 }, Vector3{ 5,0,0 }, RED);
        DrawLine3D(Vector3{ 0,0,0 }, Vector3{ 0,5,0 }, GREEN);
        DrawLine3D(Vector3{ 0,0,0 }, Vector3{ 0,0,5 }, BLUE);
        DrawElements(level, definitions, assets, assetsDirectory, texturesDirectory, lightmaps,
            lightmapsEnabled,
            loadedAssetCount, missingAssetCount, selectedElements,-1,0,
            effectsEnabled ? &effectPreview : nullptr);
        DrawSantaSpawn(level, santaAsset, assetsDirectory, texturesDirectory,
            loadedAssetCount, missingAssetCount,effectsEnabled ? &effectPreview : nullptr);
        if (collisionsEnabled) physics_preview::draw(physics,level,definitions);
        DrawDeathHeight(level, camera);
        uint32_t selectedWaylistIndex = UINT32_MAX;
        if (waypointMode && selectedElement >= 0 &&
            selectedElement < (int)level.element_count && level.elements[selectedElement].present)
            selectedWaylistIndex = level.elements[selectedElement].waylist_index;
        DrawWaylists(level, definitions, selectedWaylistIndex, selectedWaypoint);

        if (!waypointMode && (spawnSelected || (selectedElement >= 0 &&
            selectedElement < (int)level.element_count && level.elements[selectedElement].present))) {
            const Vector3 p = spawnSelected ? ToRaylib(level.spawn_position)
                : ToRaylib(level.elements[selectedElement].vector1);
            const float size = GizmoWorldSize(camera, p);
            if (transformMode == TransformMode::Move) DrawMoveGizmo(p, size, hotAxis);
            else DrawRotateGizmo(p, size * 0.78f, hotAxis);
        }

        if (hasPlacementPreview) {
            DrawElementPreview((uint32_t)selectedDefinition, previewPosition,
                placementOrientation, definitions, assets, assetsDirectory,
                texturesDirectory, loadedAssetCount, missingAssetCount,
                effectsEnabled ? &effectPreview : nullptr);
        }
        // Transparent particles are sorted and drawn after opaque scene geometry.
        if (effectsEnabled) effectPreview.Draw(camera);
        EndMode3D();

        if (marqueeActive) {
            const float left = fminf(marqueeStart.x, marqueeCurrent.x);
            const float top = fminf(marqueeStart.y, marqueeCurrent.y);
            const Rectangle box{ left, top, fabsf(marqueeCurrent.x - marqueeStart.x),
                fabsf(marqueeCurrent.y - marqueeStart.y) };
            DrawRectangleRec(box, Fade(SKYBLUE, 0.16f));
            DrawRectangleLinesEx(box, 1.0f, SKYBLUE);
        }

        DrawRectangle(0, 0, (int)panelWidth, GetScreenHeight(), Color{ 23, 25, 29, 245 });
        DrawText("SantaWorkshop", 16, 14, 24, RAYWHITE);
        DrawRectangle(12, 48, 106, 24,
            CheckCollisionPointRec(GetMousePosition(), Rectangle{12, 48, 106, 24}) ? GRAY : DARKGRAY);
        DrawText("Open map...", 19, 52, 16, RAYWHITE);
        std::string mapLabel = FilenameOf(levelPath);
        if (MeasureText(mapLabel.c_str(), 16) > panelWidth - 138) {
            while (!mapLabel.empty() && MeasureText((mapLabel + "...").c_str(), 16) > panelWidth - 138)
                mapLabel.pop_back();
            mapLabel += "...";
        }
        DrawText(mapLabel.c_str(), 126, 52, 16, dirty ? GOLD : RAYWHITE);
        DrawText(TextFormat("Elements: %u active / %u slots%s",
            ActiveElementCount(level), level.element_count,
            dirty ? " *unsaved*" : ""), 16, 78, 14,
            dirty ? GOLD : LIGHTGRAY);
        DrawText(TextFormat("W Move E Rotate P Effects:%s",
            effectsEnabled ? "on" : "off"), 16, 98, 14, RAYWHITE);
        DrawText(TextFormat("F3 Collisions:%s  F4 Animation:%s",
            collisionsEnabled ? "on" : "off", animationsEnabled ? "on" : "off"),
            360, 102, 15, RAYWHITE);
        if (animationsEnabled && selectedElement >= 0 && selectedElement < (int)level.element_count) {
            const uint32_t id=level.elements[selectedElement].definition_id;
            if (id<assets.size() && !assets[id].animation.error.empty())
                DrawText(("Static preview: " + assets[id].animation.error).c_str(),360,122,14,ORANGE);
        }

        const auto drawButton = [&](Rectangle rect, const char* label, bool active = false) {
            const bool hovered = CheckCollisionPointRec(mouse, rect);
            DrawRectangleRec(rect, active ? Color{ 55, 105, 82, 255 }
                : hovered ? Color{ 58, 64, 73, 255 } : Color{ 40, 44, 51, 255 });
            DrawRectangleLinesEx(rect, 1.0f, active ? LIME : DARKGRAY);
            const int textWidth = MeasureText(label, 13);
            DrawText(label, (int)(rect.x + (rect.width - textWidth) * 0.5f),
                (int)rect.y + 5, 13, RAYWHITE);
        };
        drawButton(Rectangle{ 12, 120, 148, 26 }, "Edit / assign route", waypointMode);
        drawButton(Rectangle{ 170, 120, 148, 26 }, "Select / set spawn", spawnSelected);
        DrawText("Score multiplier", 16, 157, 13, LIGHTGRAY);
        DrawRectangle(120, 152, 96, 24, numericField == NumericField::Score ? Color{ 58,68,82,255 } : Color{ 32,36,42,255 });
        DrawText(numericField == NumericField::Score ? (numericBuffer + "|").c_str()
            : TextFormat("%u", level.level_score_multiplier), 126, 157, 13, RAYWHITE);
        drawButton(Rectangle{ 224, 152, 38, 24 }, "-");
        drawButton(Rectangle{ 270, 152, 38, 24 }, "+");
        DrawText("Time limit", 16, 187, 13, LIGHTGRAY);
        DrawRectangle(120, 182, 96, 24, numericField == NumericField::TimeLimit ? Color{ 58,68,82,255 } : Color{ 32,36,42,255 });
        DrawText(numericField == NumericField::TimeLimit ? (numericBuffer + "|").c_str()
            : TextFormat("%u s", level.level_time_limit_seconds), 126, 187, 13, RAYWHITE);
        drawButton(Rectangle{ 224, 182, 38, 24 }, "-");
        drawButton(Rectangle{ 270, 182, 38, 24 }, "+");
        DrawText("Camera heading", 16, 217, 13, LIGHTGRAY);
        DrawRectangle(120, 212, 96, 24, numericField == NumericField::CameraHeading ? Color{ 58,68,82,255 } : Color{ 32,36,42,255 });
        DrawText(numericField == NumericField::CameraHeading ? (numericBuffer + "|").c_str()
            : TextFormat("%.5g", level.initial_camera_heading_degrees), 126, 217, 13, RAYWHITE);
        drawButton(Rectangle{ 224, 212, 38, 24 }, "-");
        drawButton(Rectangle{ 270, 212, 38, 24 }, "+");
        drawButton(Rectangle{ 12, 246, 164, 24 }, moveSnapEnabled ? "Snapping: on" : "Snapping: off",
            moveSnapEnabled);
        DrawText(TextFormat("Step %.2f", moveSnapStep), 181, 251, 12, LIGHTGRAY);
        drawButton(Rectangle{ 224, 246, 38, 24 }, "-");
        drawButton(Rectangle{ 270, 246, 38, 24 }, "+");
        DrawText("Ctrl+S Save | Ctrl+Shift: M Export mod, N New", 16, 272, 10, GRAY);
        DrawRectangle(12, 286, (int)panelWidth - 24, 26,
            searchFocused ? Color{ 58, 68, 82, 255 } : Color{ 38, 42, 48, 255 });
        DrawRectangleLines(12, 286, (int)panelWidth - 24, 26,
            searchFocused ? SKYBLUE : DARKGRAY);
        const std::string searchLabel = elementSearch.empty() && !searchFocused
            ? "Search elements..." : elementSearch + (searchFocused ? "|" : "");
        DrawText(searchLabel.c_str(), 20, 291, 15,
            elementSearch.empty() && !searchFocused ? GRAY : RAYWHITE);

        const int visibleRows = (int)((GetScreenHeight() - paletteTop - 18.0f) / rowHeight);
        for (int row = 0; row < visibleRows; ++row) {
            const int itemIndex = paletteScroll + row;
            if (itemIndex >= (int)paletteItems.size()) break;
            const uint32_t definitionId = paletteItems[(size_t)itemIndex];
            const LvlxElementDefinition& d = definitions.items[definitionId];
            const int y = (int)(paletteTop + row * rowHeight);
            const Rectangle rect{ 6.0f, (float)y, panelWidth - 12.0f, rowHeight - 2.0f };
            if ((int)definitionId == selectedDefinition) DrawRectangleRec(rect, Color{ 65, 83, 105, 255 });
            else if (CheckCollisionPointRec(mouse, rect)) DrawRectangleRec(rect, Color{ 45, 49, 56, 255 });
            DrawRectangle(12, y + 8, 10, 10, CategoryColor(d.category));
            DrawText(TextFormat("%u", definitionId), 30, y + 5, 13, GRAY);
            DrawText(d.name ? d.name : "<unnamed>", 72, y + 4, 16, RAYWHITE);
        }

        DrawRectangle((int)panelWidth + 10, GetScreenHeight() - 40,
            GetScreenWidth() - (int)panelWidth - 20, 30, Fade(BLACK, 0.72f));
        DrawText(status.c_str(), (int)panelWidth + 20, GetScreenHeight() - 33, 16, LIGHTGRAY);

        if (selectedDefinition >= 0) {
            const LvlxElementDefinition& d = definitions.items[selectedDefinition];
            DrawText(TextFormat("Placing: %u %s | E rotates | %u deg", selectedDefinition,
                d.name ? d.name : "<unnamed>", (unsigned)(placementOrientation & 3u) * 90u),
                (int)panelWidth + 14, 12, 17, RAYWHITE);
        }
        else if (spawnSelected) {
            const Vector3 p = ToRaylib(level.spawn_position);
            DrawText(TextFormat("%s | Respawn | pos %.2f %.2f %.2f | camera %.0f deg%s",
                TransformModeName(transformMode), p.x, p.y, p.z,
                level.initial_camera_heading_degrees, gizmoDrag.active ? " | DRAGGING" : ""),
                (int)panelWidth + 14, 12, 17, LIME);
        }
        else if (selectedElement >= 0 && selectedElement < (int)level.element_count &&
            level.elements[selectedElement].present) {
            const LvlxElement& element = level.elements[selectedElement];
            const LvlxElementDefinition* d = lvlx_find_definition(&definitions, element.definition_id);
            const Vector3 p = ToRaylib(element.vector1);
            if (element.orientation <= 3u) {
                DrawText(TextFormat("%s | Element %d: %s | start %.2f %.2f %.2f | rot %u deg%s",
                    TransformModeName(transformMode), selectedElement,
                    d && d->name ? d->name : "<unnamed>", p.x, p.y, p.z,
                    (unsigned)element.orientation * 90u,
                    gizmoDrag.active ? " | DRAGGING" : ""),
                    (int)panelWidth + 14, 12, 17, RAYWHITE);
            }
            else {
                DrawText(TextFormat("Element %d: %s | INVALID orientation byte %u (preserved)",
                    selectedElement, d && d->name ? d->name : "<unnamed>",
                    (unsigned)element.orientation),
                    (int)panelWidth + 14, 12, 17, ORANGE);
            }
            DrawText(TextFormat("Type %s (%d) | destination %s | waylist %s%s",
                d && d->category ? d->category : "unknown", d ? d->type_value : -1,
                DefinitionUsesDestination(d) ? "active" : "preserved/inactive",
                DefinitionSupportsWaylists(d) ? "supported" : "ignored",
                d && d->random_rotation ? " | random rotation" : ""),
                (int)panelWidth + 14, 34, 14, LIGHTGRAY);
            if (element.present != 1u ||
                (!DefinitionSupportsWaylists(d) && element.waylist_index != UINT32_MAX) ||
                d && d->random_rotation) {
                DrawText(TextFormat("Lossless data: marker %u | serialized rotation %u | route key %s",
                    (unsigned)element.present, (unsigned)element.orientation,
                    element.waylist_index == UINT32_MAX ? "none" : TextFormat("%u", element.waylist_index)),
                    (int)panelWidth + 14, 54, 13, ORANGE);
            }
        }
        DrawFPS(GetScreenWidth() - 100, 34);
        DrawText("F2 Environment", GetScreenWidth() - 170, 58, 16, SKYBLUE);
        if (level.has_spawn_data && level.spawn_position.y < gEnvironment.numbers(20)[0])
            DrawText("WARNING: spawn below death height", 350, 78, 18, RED);
        if (environmentPanel) {
            DrawRectangle(345, 90, GetScreenWidth()-365, GetScreenHeight()-120, Color{24,28,34,250});
            DrawText("Environment overrides (F2 closes) - click a value, Enter applies, Ctrl+A clears", 360, 100, 16, RAYWHITE);
            DrawText(TextFormat("Preview: F Fog %s | H Death grid %s (shadow always on, %.0fu fade) | A Ambient slot %d",
                gFogPreview ? "on" : "off", gDeathGridVisible ? "on" : "off",
                kDeathShadowFadeDepth, gAmbientPreviewSlot), 360, 124, 15, SKYBLUE);
            DrawText("Ranges = start, extent | angles = degrees | colors allow intensity | scroll for more", 360, 146, 13, GRAY);
            const int rows = (GetScreenHeight()-245)/25;
            for (int row = 0; row < rows && row+environmentScroll < (int)environment::schema.size(); ++row) {
                const int i = row+environmentScroll, y = 165+row*25;
                const auto& p = environment::schema[(size_t)i];
                std::string label = p.name;
                if (p.ambient >= 0) label += " " + std::to_string(p.ambient);
                DrawText(label.c_str(), 360, y+4, 14, LIGHTGRAY);
                DrawRectangle(645, y, GetScreenWidth()-790, 23, i == environmentField ? Color{58,68,82,255} : Color{35,40,47,255});
                BeginScissorMode(650, y, GetScreenWidth()-805, 23);
                const std::string value = i == environmentField ? environmentBuffer + "|" : gEnvironment.value((size_t)i);
                DrawText(value.c_str(), 650, y+4, 14, RAYWHITE);
                EndScissorMode();
                DrawText(gEnvironment.edits.count(i) ? "edited" : gEnvironment.local[(size_t)i] ? "level" : "inherited",
                    GetScreenWidth()-130, y+4, 13, gEnvironment.edits.count(i) ? GOLD : GRAY);
            }
            DrawText("Approximate shading: ambient selection, fog and sky formulas.", 360, GetScreenHeight()-78, 13, ORANGE);
            DrawText("Environment reflections enabled; snow field and demo camera are stored only.", 360, GetScreenHeight()-58, 13, GRAY);
        }
        EndDrawing();
        if (previewSmoke && ++previewFrames == 5) {
            TakeScreenshot(previewKnowledge ? "x64/Debug/knowledge-preview.png"
                : previewLiveShadows ? "x64/Debug/live-shadow-preview.png"
                : previewScene ? "x64/Debug/scene-preview.png"
                : "x64/Debug/environment-preview.png");
            break;
        }
    }

    if (cameraActive) EnableCursor();
    effectPreview.Unload();
    UnloadLevelLightmaps(lightmaps);
    UnloadRenderAssets(assets);
    std::vector<RenderAsset> santaAssets;
    santaAssets.push_back(std::move(santaAsset));
    santaAssets.push_back(std::move(skyAsset));
    UnloadRenderAssets(santaAssets);
    if (gLiveShadowMap.id) UnloadRenderTexture(gLiveShadowMap);
    if (gEnvironmentCube.id) UnloadTexture(gEnvironmentCube);
    if (gShadowDepthShader.id) UnloadShader(gShadowDepthShader);
    if (gCrfShader.id)
    {
        UnloadShader(
            gCrfShader
        );
    }
    if (gSkyShader.id)
    {
        UnloadShader(gSkyShader);
    }
    CloseWindow();
    lvlx_free_definitions(&definitions);
    lvlx_free_level(&level);
    return 0;
}
