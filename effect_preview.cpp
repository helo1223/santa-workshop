#include "effect_preview.h"

#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct ColorKey { Vec4 value; };
struct ScalarKey { float value = 0, time = 0; };

struct BaseBlock {
    Vec4 rotation;
    Vec4 translation;
    uint32_t flags = 0;
    float delay = 0;
};

struct TextureBlock {
    Vec4 controls;
    float opaque = 0;
    std::vector<Vec4> keys;
    std::string filename;
    uint32_t blend = 0;
    bool interpolate = false;
    uint32_t animation = 0;
    float animationRate = 1;
};

struct CurveBlock {
    std::vector<ColorKey> colors;
    std::vector<ScalarKey> sizes;
    Vec4 opacity;
    Vec4 ramp;
    bool disableNearFade = false;
};

struct EmitterBlock {
    uint32_t count = 0;
    float duration = 0;
    bool packed = false;
};

struct TailBlock {
    bool lit = false;
    bool optionalTexture = false;
    std::string filename;
    float normalShape = 0;
    float directionalFloor = 0;
};

enum class RecordKind { Particle, Orbiter, PointLight };

struct EffectRecord {
    std::string name;
    RecordKind kind = RecordKind::Particle;
    BaseBlock base;
    EmitterBlock emitter;
    TextureBlock texture;
    CurveBlock curve;
    std::array<Vec4, 7> parameters{};
    uint32_t parameterFlags = 0;
    std::array<Vec4, 10> orbiter{};
    uint32_t orbiterFlags = 0;
    float lightDuration = 0;
    std::vector<ColorKey> lightColors;
    std::vector<ScalarKey> lightRadii;
    TailBlock tail;
};

struct EffectTemplate {
    float duration = 1;
    float loadScale = 1;
    std::vector<EffectRecord> records;
};

class Reader {
public:
    explicit Reader(std::vector<unsigned char> bytes) : bytes_(std::move(bytes)) {}
    size_t position() const { return position_; }
    size_t size() const { return bytes_.size(); }
    bool good() const { return !failed_; }
    uint8_t u8() { if (!need(1)) return 0; return bytes_[position_++]; }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = (uint16_t)(bytes_[position_] | (bytes_[position_ + 1] << 8));
        position_ += 2; return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = (uint32_t)bytes_[position_] |
            ((uint32_t)bytes_[position_ + 1] << 8) |
            ((uint32_t)bytes_[position_ + 2] << 16) |
            ((uint32_t)bytes_[position_ + 3] << 24);
        position_ += 4; return v;
    }
    float f32() { uint32_t bits = u32(); float v = 0; std::memcpy(&v, &bits, 4); return v; }
    Vec4 vec4() { return Vec4{ f32(), f32(), f32(), f32() }; }
    std::string string() {
        uint32_t count = u32();
        if (!need(count)) return {};
        std::string result((const char*)bytes_.data() + position_, count);
        position_ += count; return result;
    }
    std::string fixedString(size_t count) {
        if (!need(count)) return {};
        const char* first = (const char*)bytes_.data() + position_;
        size_t length = 0; while (length < count && first[length]) ++length;
        std::string result(first, length); position_ += count; return result;
    }
private:
    bool need(size_t count) {
        if (failed_ || count > bytes_.size() - position_) { failed_ = true; return false; }
        return true;
    }
    std::vector<unsigned char> bytes_;
    size_t position_ = 0;
    bool failed_ = false;
};

static bool ReadFile(const std::string& path, std::vector<unsigned char>& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0 || size > 0x40000) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize((size_t)size);
    return bytes.empty() || !!input.read((char*)bytes.data(), size);
}

static BaseBlock ParseBase(Reader& r) {
    BaseBlock b;
    const uint32_t version = r.u32();
    if (version != 3) { (void)r.u32(); return b; }
    b.rotation = Vec4{ r.f32(), r.f32(), r.f32(), 0 };
    b.translation = Vec4{ r.f32(), r.f32(), r.f32(), 0 };
    b.flags = r.u32();
    b.delay = r.f32();
    return b;
}

static EmitterBlock ParseEmitter(Reader& r) {
    EmitterBlock e;
    const uint32_t version = r.u32();
    e.count = std::min(r.u32(), 2000u);
    e.duration = r.f32();
    if (version >= 3) e.packed = r.u8() != 0;
    return e;
}

static TextureBlock ParseTexture(Reader& r) {
    TextureBlock t;
    const uint32_t version = r.u32();
    t.controls = r.vec4();
    t.opaque = r.f32();
    const uint32_t count = r.u32();
    if (count > 4096) return t;
    t.keys.reserve(count);
    for (uint32_t i = 0; i < count; ++i) t.keys.push_back(r.vec4());
    t.filename = r.fixedString(256);
    t.blend = r.u32();
    t.interpolate = r.u8() != 0;
    t.animation = r.u32();
    t.animationRate = r.f32();
    (void)version;
    return t;
}

static CurveBlock ParseCurve(Reader& r) {
    CurveBlock c;
    const uint32_t version = r.u32();
    const uint32_t colorCount = r.u32();
    if (colorCount > 4096) return c;
    c.colors.reserve(colorCount);
    for (uint32_t i = 0; i < colorCount; ++i) c.colors.push_back({r.vec4()});
    const uint32_t sizeCount = r.u32();
    if (sizeCount > 4096) return c;
    c.sizes.reserve(sizeCount);
    for (uint32_t i = 0; i < sizeCount; ++i) c.sizes.push_back({r.f32(), r.f32()});
    c.opacity = r.vec4();
    c.ramp = r.vec4();
    if (version >= 3) c.disableNearFade = r.u8() != 0;
    return c;
}

static TailBlock ParseTail(Reader& r) {
    TailBlock t;
    const uint32_t version = r.u32();
    if (version >= 3) {
        t.lit = r.u8() != 0;
        t.optionalTexture = r.u8() != 0;
    } else {
        t.lit = r.u32() == 1;
        t.optionalTexture = r.u32() == 1;
    }
    t.filename = r.string();
    if (version > 1) { t.normalShape = r.f32(); t.directionalFloor = r.f32(); }
    return t;
}

static bool ParseTemplate(const std::string& path, EffectTemplate& result) {
    std::vector<unsigned char> bytes;
    if (!ReadFile(path, bytes)) return false;
    Reader r(std::move(bytes));
    const uint8_t version = r.u8();
    if (version != 8) return false;
    result.duration = r.f32();
    result.loadScale = r.f32();
    (void)r.u32(); (void)r.u32();
    const uint32_t recordCount = r.u32();
    if (recordCount > 4096) return false;
    result.records.reserve(recordCount);
    for (uint32_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
        EffectRecord record;
        record.name = r.string();
        const uint32_t tag = r.u32();
        const uint16_t bodyVersion = r.u16();
        record.base = ParseBase(r);
        if (tag == 0x7074656du || tag == 0x6d657470u) { // bytes "metp"
            record.kind = RecordKind::Particle;
            record.emitter = ParseEmitter(r);
            record.texture = ParseTexture(r);
            record.curve = ParseCurve(r);
            if (bodyVersion >= 10) {
                const uint32_t parameterVersion = r.u32();
                static const int order[7] = {0, 1, 2, 3, 4, 6, 5};
                for (int index : order) record.parameters[index] = r.vec4();
                record.parameterFlags = r.u32();
                (void)parameterVersion;
            }
            if (bodyVersion > 10) record.tail = ParseTail(r);
        } else if (tag == 0x6f726274u || tag == 0x7462726fu) { // bytes "tbro"
            record.kind = RecordKind::Orbiter;
            record.emitter = ParseEmitter(r);
            record.texture = ParseTexture(r);
            record.curve = ParseCurve(r);
            const uint32_t orbiterVersion = r.u32();
            for (Vec4& group : record.orbiter) group = r.vec4();
            record.orbiterFlags = r.u32();
            record.tail = ParseTail(r);
            (void)orbiterVersion;
        } else if (tag == 0x6c677074u || tag == 0x74676c70u) { // bytes "tpgl"
            record.kind = RecordKind::PointLight;
            const uint32_t pointVersion = r.u32();
            record.lightDuration = r.f32();
            const uint32_t colorCount = r.u32();
            if (colorCount > 4096) return false;
            for (uint32_t i = 0; i < colorCount; ++i) record.lightColors.push_back({r.vec4()});
            const uint32_t radiusCount = r.u32();
            if (radiusCount > 4096) return false;
            for (uint32_t i = 0; i < radiusCount; ++i)
                record.lightRadii.push_back({r.f32(), r.f32()});
            (void)pointVersion;
        } else {
            return false;
        }
        if (!r.good()) return false;
        // CONFIRMED: file+05 -> template+20 -> component loader scale.
        // Stock manager scale is 1. Apply only dimensional fields, as the
        // original loaders do; phase, direction and time remain unchanged.
        const float s=result.loadScale;
        record.base.translation.x*=s;
        record.base.translation.y*=s;
        record.base.translation.z*=s;
        for(auto& key:record.curve.sizes) key.value*=s;
        for(auto& key:record.lightRadii) key.value*=s;
        if(record.kind==RecordKind::Particle){
            record.parameters[1].w*=s;
            record.parameters[2].x*=s;
            record.parameters[2].y*=s;
            record.parameters[2].z*=s;
            record.parameters[5].x*=s;
            record.parameters[6].x*=s;
            record.parameters[6].y*=s;
            record.parameters[6].z*=s;
            record.parameters[6].w*=s;
        } else if(record.kind==RecordKind::Orbiter){
            for(int index:{3,4,5,6}){
                record.orbiter[index].x*=s;
                record.orbiter[index].y*=s;
                record.orbiter[index].z*=s;
            }
        }
        result.records.push_back(std::move(record));
    }
    return r.good() && r.position() == r.size();
}

static float Clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }
static float Frac(float v) { return v - std::floor(v); }
static float Mix(float a, float b, float t) { return a + (b - a) * t; }
static Vec4 Mix(Vec4 a, Vec4 b, float t) {
    return { Mix(a.x,b.x,t), Mix(a.y,b.y,t), Mix(a.z,b.z,t), Mix(a.w,b.w,t) };
}

static Vec4 EvalColors(const std::vector<ColorKey>& keys, float t) {
    if (keys.empty()) return Vec4{1,1,1,1};
    if (keys.size() == 1) return keys[0].value;
    size_t lo = 0, hi = 1; float tlo = 0, thi = 1.100000023841858f;
    for (size_t i = 0; i < keys.size(); ++i) {
        const float kt = keys[i].value.w;
        if (kt <= t && tlo < kt) { lo = i; tlo = kt; }
        if (t < kt && kt < thi) { hi = i; thi = kt; }
    }
    const float f = (t - tlo) / (thi - tlo);
    return Mix(keys[lo].value, keys[hi].value, f);
}

static float EvalScalars(const std::vector<ScalarKey>& keys, float t) {
    if (keys.empty()) return 1;
    if (keys.size() == 1) return keys[0].value;
    size_t lo = 0, hi = 1; float tlo = 0, thi = 1.100000023841858f;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].time <= t && tlo < keys[i].time) { lo = i; tlo = keys[i].time; }
        if (t < keys[i].time && keys[i].time < thi) { hi = i; thi = keys[i].time; }
    }
    return Mix(keys[lo].value, keys[hi].value, (t - tlo) / (thi - tlo));
}

static uint32_t Hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16);
}
static float Random01(uint32_t seed) { return (Hash(seed) >> 8) * (1.0f / 16777216.0f); }
static float RandomSigned(uint32_t seed) { return Random01(seed) - 0.5f; }

static Vector3 V3(Vec4 v) { return Vector3{v.x,v.y,v.z}; }
static Vector3 Mul(Vector3 a, Vector3 b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
static Vector3 SafeNormalize(Vector3 v) {
    const float length = Vector3Length(v); return length > 0.000001f ? Vector3Scale(v, 1.0f/length) : Vector3{};
}

static Vector3 RotateBase(Vector3 v, Vec4 rotation) {
    const float ax = -rotation.x, ay = -rotation.y, az = -rotation.z;
    float c = std::cos(ax), s = std::sin(ax);
    v = {v.x, c*v.y-s*v.z, s*v.y+c*v.z};
    c = std::cos(ay); s = std::sin(ay);
    v = {c*v.x+s*v.z, v.y, -s*v.x+c*v.z};
    c = std::cos(az); s = std::sin(az);
    return {c*v.x-s*v.y, s*v.x+c*v.y, v.z};
}

static Vector3 ToWorld(Vector3 local, const EffectRecord& record,
    const LvlxElement& element) {
    local = Vector3Add(RotateBase(local, record.base.rotation), V3(record.base.translation));
    Vector3 rayLocal{local.x, local.y, -local.z};
    rayLocal = Vector3RotateByAxisAngle(rayLocal, Vector3{0,1,0},
        -(float)element.orientation * 90.0f * DEG2RAD);
    return Vector3Add(Vector3{element.vector1.x, element.vector1.y, -element.vector1.z}, rayLocal);
}

static Vector3 ToWorldDirection(Vector3 local, const EffectRecord& record,
    const LvlxElement& element) {
    local = RotateBase(local, record.base.rotation);
    Vector3 rayLocal{local.x, local.y, -local.z};
    return Vector3RotateByAxisAngle(rayLocal, Vector3{0,1,0},
        -(float)element.orientation * 90.0f * DEG2RAD);
}

static Vector3 ParticleCenter(const EffectRecord& r, uint32_t seed, float phase,
    float& age, float& angle, Vector3* directionOut = nullptr) {
    const Vec4& p0=r.parameters[0], &p1=r.parameters[1], &p2=r.parameters[2],
        &p3=r.parameters[3], &p4=r.parameters[4], &p5=r.parameters[5], &p6=r.parameters[6];
    Vec4 S{RandomSigned(seed+1),RandomSigned(seed+2),RandomSigned(seed+3),Random01(seed+4)};
    const float rx=Random01(seed+5), ry=Random01(seed+6), rw=Random01(seed+7);
    float f = 0, t = 0;
    if ((r.parameterFlags & 2u) == 0) {
        const float b = Clamp01(phase - S.w*p0.y);
        age = Clamp01((1.0f + rw*p0.x)*b/(1.0f-S.w*p0.y));
        t = (b*p0.w+p0.z)*r.emitter.duration;
        f = b;
    } else {
        f = Frac(S.w*p0.y+phase);
        age = Clamp01((1.0f+rw*p0.x)*f);
        t = (f*p0.w+p0.z)*r.emitter.duration;
    }
    Vector3 rawDirection{RandomSigned(seed+8),RandomSigned(seed+9),RandomSigned(seed+10)};
    Vector3 j=V3(p3), d=SafeNormalize(Mul(rawDirection,j));
    d=Vector3Add(Mul(d,j),V3(p1));
    if (directionOut) *directionOut = d;
    const float travelLimit=(r.parameterFlags&1u)==0&&p6.w!=0?
        p1.w/(2.0f*p6.w):std::numeric_limits<float>::max();
    const float h=std::min(std::max(t,0.0f),travelLimit);
    const float rho=1.0f+p3.w*(rx-1.0f);
    const float radius=p5.x*(1.0f+p5.z*(ry-1.0f));
    const float omega=p5.y*(1.0f+p5.w*(rx-1.0f));
    const float theta=Frac(h*omega*0.159154996f+0.5f)*6.28318977f-3.14159012f;
    const float sn=std::sin(theta), cs=std::cos(theta);
    const float dd=std::max(Vector3DotProduct(d,d),0.000001f);
    Vector3 orbit{-d.y*sn,d.x*sn,cs};
    orbit=Vector3Add(orbit,Vector3Scale(d,(1.0f-cs)*d.z/dd));
    Vector3 sv{S.x,S.y,S.z};
    const float sl=std::sqrt(S.x*S.x+S.y*S.y+S.z*S.z+S.w*S.w);
    Vector3 spawn=Mul(Vector3Add(sv,Vector3Scale(Vector3Subtract(
        sl>0.000001f?Vector3Scale(sv,1.0f/sl):Vector3{},sv),p2.w)),V3(p2));
    Vector3 center=Vector3Add(spawn,Vector3Scale(d,rho*(p1.w*h-p6.w*h*h)));
    center=Vector3Add(center,Vector3Scale(V3(p6),t*t));
    center=Vector3Add(center,Vector3Scale(orbit,radius));
    angle = ((rw < .5f ? -1.0f : 1.0f)*(1.0f+r.texture.controls.w*(rw-1.0f)))*
        (r.texture.opaque + 3.14159012f);
    return center;
}

static float OrbiterWave(float theta) {
    const float x=6.28318977355957f*Frac(theta*.1591549962759018f+.25f)-3.141590118408203f;
    const float x2=x*x;
    return 1+x2*(-.5f+x2*(.04166660085320473f+x2*(-.0013888400280848145f+
        x2*(.00002476090048730839f+x2*(-2.523989905967028e-7f)))));
}

static Vector3 OrbiterCenter(const EffectRecord& r, uint32_t seed, float phase,
    float& age, float& angle) {
    const Vec4 S{RandomSigned(seed+1),RandomSigned(seed+2),RandomSigned(seed+3),Random01(seed+4)};
    const float rs=Random01(seed+5), ry=Random01(seed+6), rw=Random01(seed+7);
    const auto& o=r.orbiter;
    age=Frac(S.w*o[0].y+phase);
    const float t=o[0].z+o[0].w*age;
    Vector3 v;
    const float input[3]={t+o[1].x,t+o[1].y,t+o[1].z};
    const float freq[3]={o[2].x,o[2].y,o[2].z};
    const float amp[3]={o[3].x,o[3].y,o[3].z};
    const float rnd[3]={o[7].x,o[7].y,o[7].z};
    float* out=&v.x;
    for(int i=0;i<3;++i) out[i]=OrbiterWave(input[i]*freq[i])*amp[i]*(1+rnd[i]*(rs-1));
    v=Vector3Add(v,Vector3{ o[4].x*t*t+o[6].x*t, o[4].y*t*t+o[6].y*t,
        o[4].z*t*t+o[6].z*t });
    const Vector3 vn=SafeNormalize(v);
    const float distortion=Vector3DotProduct(V3(o[8]),vn);
    v=Mul(v,Vector3Add(Vector3{1,1,1},Vector3Scale(V3(o[9]),distortion)));
    v=Vector3Add(v,Mul(Vector3{S.x,S.y,S.z},V3(o[5])));
    angle=((rw<.5f?-1.0f:1.0f)*(1+r.texture.controls.w*(rw-1)))*
        (r.texture.opaque+3.14159012f);
    return v;
}

static float ParticleOpacity(const CurveBlock& c, float age) {
    const float ramp = 1.0f - std::max(Clamp01((c.ramp.x-age)*c.ramp.z),
        Clamp01((age-c.ramp.y)*c.ramp.w));
    return Clamp01(ramp*Mix(c.opacity.x,c.opacity.y,age));
}

static Color ToColor(Vec4 c, float alpha) {
    auto byte=[](float v){return (unsigned char)std::clamp(v*255.0f,0.0f,255.0f);};
    return Color{byte(c.x),byte(c.y),byte(c.z),byte(alpha)};
}

struct DrawParticle {
    Texture2D texture{};
    Rectangle source{};
    Rectangle source2{};
    float frameBlend = 0;
    Vector3 position{};
    Vector2 size{};
    float rotation = 0;
    Color color{};
    bool additive = false;
    bool oriented = false;
    Vector3 right{};
    Vector3 up{};
};

struct DrawLight { Vector3 position{}; Vector3 color{}; float radius=0; float distance=0; };

struct TrailAtom {
    Vector3 position{}, velocity{}, previousA{};
    float age=0, q=1;
    uint32_t seed=0;
};
struct TrailRuntime { std::vector<TrailAtom> atoms; float lastElapsed=0; bool initialized=false; };

struct CachedTemplate {
    bool attempted = false;
    bool loaded = false;
    EffectTemplate effect;
    std::unordered_map<std::string, Texture2D> textures;
};

static std::string Lower(std::string s) {
    for(char& c:s) if(c>='A'&&c<='Z') c=(char)(c-'A'+'a'); return s;
}
static std::string Join(const std::string& a,const std::string& b) {
    if(a.empty()) return b; char last=a.back(); return a+(last=='/'||last=='\\'?"":"/")+b;
}

static float RecordPhase(const EffectTemplate& effect, const EffectRecord& record,
    float elapsed) {
    const float parentDuration=std::max(effect.duration,0.0001f);
    const float cycle=std::fmod(std::max(elapsed,0.0f),parentDuration);
    const float local=cycle-record.base.delay;
    float duration=record.kind==RecordKind::PointLight?record.lightDuration:record.emitter.duration;
    duration=std::max(duration,0.0001f);
    if(local<=0) return -1;
    if((record.base.flags&1u)!=0) return std::fmod(elapsed,duration)/duration;
    return local<=duration?local/duration:-1;
}

} // namespace

struct EffectPreview::Impl {
    std::unordered_map<std::string,CachedTemplate> templates;
    std::vector<DrawParticle> particles;
    std::vector<DrawLight> lights;
    std::unordered_map<uint64_t,TrailRuntime> trails;
    size_t failed=0;
    float nearClipStart=1,nearClipEnd=2;

    Vector3 trailCenter(const EffectRecord& record,const LvlxElement& element,
        uint32_t seed,uint32_t atomIndex,uint64_t stateKey,float elapsed,float& age) {
        TrailRuntime& state=trails[stateKey];
        const float duration=std::max(record.emitter.duration,0.0001f);
        const Vec4& p0=record.parameters[0],&p1=record.parameters[1],
            &p2=record.parameters[2],&p3=record.parameters[3],&p6=record.parameters[6];
        const Vector3 acceleration=ToWorldDirection(V3(p6),record,element);
        const float damping=p1.w!=0?p6.w/p1.w:p6.w;
        auto reset=[&](TrailAtom& atom,bool initializeAge){
            Vec4 s{RandomSigned(atom.seed+1),RandomSigned(atom.seed+2),
                RandomSigned(atom.seed+3),Random01(atom.seed+4)};
            Vector3 raw{RandomSigned(atom.seed+8),RandomSigned(atom.seed+9),RandomSigned(atom.seed+10)};
            Vector3 d=SafeNormalize(Mul(raw,V3(p3)));
            d=Vector3Add(Mul(d,V3(p3)),V3(p1));
            const float speed=(1.0f+(std::floor(Random01(atom.seed+11)*256.0f)/255.0f-1.0f)*p3.w)*p1.w;
            Vector3 sv{s.x,s.y,s.z};
            const float sl=std::sqrt(s.x*s.x+s.y*s.y+s.z*s.z+s.w*s.w);
            Vector3 spawn=Mul(Vector3Add(sv,Vector3Scale(Vector3Subtract(
                sl>0.000001f?Vector3Scale(sv,1.0f/sl):Vector3{},sv),p2.w)),V3(p2));
            if(initializeAge){
                atom.age=s.w*p0.y;
                atom.q=1.0f+std::floor(Random01(atom.seed+12)*100.0f)*.01f*p0.x;
            }
            atom.velocity=Vector3Scale(ToWorldDirection(d,record,element),speed);
            const float t=atom.age*duration;
            atom.previousA=Vector3Scale(acceleration,t*t);
            atom.position=Vector3Add(ToWorld(spawn,record,element),
                Vector3Add(Vector3Scale(atom.velocity,t),atom.previousA));
        };
        if(!state.initialized||state.atoms.size()!=record.emitter.count||elapsed<state.lastElapsed){
            state.atoms.resize(record.emitter.count);
            for(uint32_t i=0;i<record.emitter.count;++i){state.atoms[i].seed=Hash(seed+i*0x85ebca6bu);reset(state.atoms[i],true);}
            state.lastElapsed=elapsed;state.initialized=true;
        }
        const float deltaNorm=(elapsed-state.lastElapsed)/duration;
        state.lastElapsed=elapsed;
        for(TrailAtom& atom:state.atoms){
            atom.age+=atom.q*deltaNorm;
            if(atom.age>=1.0f){
                if(record.base.flags&1u){atom.age=std::fmod(atom.age,1.0f);reset(atom,false);}
                else atom.age=1.0f;
            }
            const float t=atom.age*duration;
            const Vector3 nextA=Vector3Scale(acceleration,t*t);
            const float drag=Clamp01(1.0f-damping*t*t);
            atom.position=Vector3Add(atom.position,Vector3Add(
                Vector3Scale(atom.velocity,duration*deltaNorm*drag),
                Vector3Subtract(nextA,atom.previousA)));
            atom.previousA=nextA;
        }
        if(state.atoms.empty()){age=0;return ToWorld({},record,element);}
        atomIndex=std::min<uint32_t>(atomIndex,(uint32_t)state.atoms.size()-1);
        age=state.atoms[atomIndex].age;
        return state.atoms[atomIndex].position;
    }

    CachedTemplate* load(const std::string& filename,const std::string& effectsDir,
        const std::string& texturesDir,const std::string& fallbackEffects,const std::string& fallbackTextures) {
        const std::string key=Lower(filename);
        CachedTemplate& cached=templates[key];
        if(cached.attempted) return cached.loaded?&cached:nullptr;
        cached.attempted=true;
        std::string effectPath=Join(effectsDir,filename);
        if(!FileExists(effectPath.c_str())&&!fallbackEffects.empty())effectPath=Join(fallbackEffects,filename);
        cached.loaded=ParseTemplate(effectPath,cached.effect);
        if(!cached.loaded){++failed; TraceLog(LOG_WARNING,"Could not decode FXT: %s",filename.c_str()); return nullptr;}
        for(const EffectRecord& record:cached.effect.records) {
            if(record.kind==RecordKind::PointLight||record.texture.filename.empty()) continue;
            const std::string textureKey=Lower(record.texture.filename);
            if(cached.textures.count(textureKey)) continue;
            std::string path=Join(texturesDir,record.texture.filename);
            if(!FileExists(path.c_str())&&!fallbackTextures.empty())path=Join(fallbackTextures,record.texture.filename);
            Texture2D texture{};
            if(FileExists(path.c_str())) texture=LoadTexture(path.c_str());
            if(texture.id){SetTextureWrap(texture,TEXTURE_WRAP_CLAMP);SetTextureFilter(texture,TEXTURE_FILTER_BILINEAR);}
            else TraceLog(LOG_WARNING,"Effect texture not found: %s",path.c_str());
            cached.textures.emplace(textureKey,texture);
        }
        return &cached;
    }

    void addEffect(const std::string& filename,const LvlxElement& element,const Camera3D& camera,
        const std::string& effectsDir,const std::string& texturesDir,const std::string& fallbackEffects,
        const std::string& fallbackTextures,float elapsed,uint32_t salt) {
        CachedTemplate* cached=load(filename,effectsDir,texturesDir,fallbackEffects,fallbackTextures); if(!cached) return;
        for(size_t ri=0;ri<cached->effect.records.size();++ri){
            const EffectRecord& record=cached->effect.records[ri];
            const float phase=RecordPhase(cached->effect,record,elapsed); if(phase<0) continue;
            if(record.kind==RecordKind::PointLight){
                Vec4 rgb=EvalColors(record.lightColors,phase);
                const float radius=std::max(0.0f,EvalScalars(record.lightRadii,phase));
                const Vector3 position=ToWorld({},record,element);
                lights.push_back({position,{rgb.x,rgb.y,rgb.z},radius,Vector3Distance(position,camera.position)});
                continue;
            }
            auto textureIt=cached->textures.find(Lower(record.texture.filename));
            if(textureIt==cached->textures.end()||!textureIt->second.id||record.texture.keys.empty()) continue;
            if(Vector3Distance(ToWorld({},record,element),camera.position)>300.0f) continue;
            const uint32_t count=record.emitter.count;
            for(uint32_t i=0;i<count&&particles.size()<60000;++i){
                const uint32_t seed=Hash(salt^(uint32_t)ri*0x9e3779b9u^i*0x85ebca6bu);
                float age=0,angle=0;
                Vector3 localDirection{};
                const uint64_t stateKey=((uint64_t)salt<<32)^((uint64_t)ri<<16);
                const bool cpuTrail=record.kind==RecordKind::Particle&&(record.parameterFlags&8u)!=0;
                Vector3 center;
                if(cpuTrail) center=trailCenter(record,element,Hash(salt^(uint32_t)ri),i,stateKey,
                    std::fmod(std::max(elapsed,0.0f),std::max(cached->effect.duration,.0001f)),age);
                else {
                    center=record.kind==RecordKind::Orbiter?
                        OrbiterCenter(record,seed,phase,age,angle):
                        ParticleCenter(record,seed,phase,age,angle,&localDirection);
                    center=ToWorld(center,record,element);
                }
                if(!std::isfinite(center.x)||!std::isfinite(age)) continue;
                const float spinSeed=Random01(seed+7);
                angle=(spinSeed<.5f?-1.0f:1.0f)*
                    (1+record.texture.controls.w*(spinSeed-1))*
                    (record.texture.opaque*elapsed+3.14159012f);
                // The shader interpolates a 16-entry table sampled at j/16,
                // with explicit first/last authored endpoints.
                const float tablePosition=Clamp01(age)*15.0f;
                const int tableIndex=(int)std::floor(tablePosition);
                const auto sample=[&](int index){
                    index=std::min(index,15);
                    Vec4 value=EvalColors(record.curve.colors,index/16.0f);
                    value.w=EvalScalars(record.curve.sizes,index/16.0f);
                    if(index==0||index==15){
                        if(!record.curve.colors.empty()) value=index==0?
                            record.curve.colors.front().value:record.curve.colors.back().value;
                        if(!record.curve.sizes.empty()) value.w=index==0?
                            record.curve.sizes.front().value:record.curve.sizes.back().value;
                    }
                    return value;
                };
                Vec4 color=Mix(sample(tableIndex),sample(tableIndex+1),Frac(tablePosition));
                // K04 is a separate size-randomization factor, especially
                // significant for sparkles; the old preview omitted it.
                float size=color.w*(1.0f+record.texture.controls.y*(Random01(seed+19)-1.0f))*
                    (1.0f+record.texture.controls.z*(Random01(seed+20)-1.0f));
                float alpha=ParticleOpacity(record.curve,age);
                if(!record.curve.disableNearFade){
                    const Vector3 forward=SafeNormalize(Vector3Subtract(camera.target,camera.position));
                    const float depth=Vector3DotProduct(Vector3Subtract(center,camera.position),forward);
                    alpha*=Clamp01((depth-nearClipStart)/(nearClipEnd-nearClipStart));
                }
                if(alpha<=0.001f||size<=0.0001f) continue;
                float keyPosition;
                if(record.texture.interpolate) keyPosition=Frac(age*record.texture.animationRate)*
                    std::max(0.0f,(float)record.texture.keys.size()-1.0001f);
                else keyPosition=Random01(seed+21)*((float)record.texture.keys.size()-0.0001f);
                size_t key0=std::min((size_t)std::floor(keyPosition),record.texture.keys.size()-1);
                size_t key1=std::min(key0+1,record.texture.keys.size()-1);
                const float blend=record.texture.interpolate?Frac(keyPosition):0;
                const Vec4 k0=record.texture.keys[key0],k1=record.texture.keys[key1];
                size*=Mix(k0.w,k1.w,blend);
                const Texture2D texture=textureIt->second;
                auto source=[&](Vec4 k){return Rectangle{k.x*texture.width,k.y*texture.height,
                    k.z*texture.width,k.z*texture.height};};
                DrawParticle draw{texture,source(k0),source(k1),blend,center,
                    {size,size},angle*RAD2DEG,ToColor(color,alpha),record.texture.blend==2};
                if(record.kind==RecordKind::Particle&&record.texture.animation==3&&!cpuTrail){
                    const Vector3 n=SafeNormalize(localDirection);
                    Vector3 u{n.x-n.y,n.z-n.x,-n.y+n.z};
                    Vector3 v{n.x+n.z,n.z-n.y,-n.y-n.x};
                    const Vector3 q{-n.z,n.y,n.x};
                    const bool gx=std::fabs(n.y)<std::fabs(n.x),
                        gy=std::fabs(n.z)<std::fabs(n.x),gz=std::fabs(n.z)<std::fabs(n.y);
                    u=gy?Vector3Add(u,Vector3{q.y,q.z,q.x}):Vector3{q.y,q.z,q.x};
                    v=gz?Vector3Add(v,q):q;
                    const Vector3 w=gx?u:v;
                    const Vector3 second{n.y*w.y-w.x*n.z,n.z*w.z-w.y*n.x,
                        n.x*w.x-w.z*n.y};
                    draw.oriented=true;
                    draw.right=ToWorldDirection(Vector3{w.z,w.x,w.y},record,element);
                    draw.up=ToWorldDirection(second,record,element);
                }
                particles.push_back(draw);
            }
        }
    }
};

EffectPreview::EffectPreview():impl_(std::make_unique<Impl>()){}
EffectPreview::~EffectPreview(){Unload();}

void EffectPreview::BuildFrame(const LvlxLevel& level,const LvlxDefinitionTable& definitions,
    const Camera3D& camera,const std::string& effectsDirectory,const std::string& texturesDirectory,
    const std::string& fallbackEffectsDirectory,const std::string& fallbackTexturesDirectory,
    float elapsedSeconds,float nearClipStart,float nearClipEnd){
    impl_->particles.clear();impl_->lights.clear();
    impl_->nearClipStart=nearClipStart;impl_->nearClipEnd=nearClipEnd;
    for(uint32_t i=0;i<level.element_count;++i){
        const LvlxElement& e=level.elements[i]; if(!e.present) continue;
        const LvlxElementDefinition* d=lvlx_find_definition(&definitions,e.definition_id); if(!d) continue;
        LvlxElement attached=e;
        attached.vector1.y+=d->vertical_offset;
        for(size_t n=0;n<d->steady_effect_count;++n)
            impl_->addEffect(d->steady_effects[n],attached,camera,effectsDirectory,texturesDirectory,
                fallbackEffectsDirectory,fallbackTexturesDirectory,
                elapsedSeconds,Hash(i*131u+(uint32_t)n));
        // Preserve triggered references, but only gameplay activation should play them.
    }
    std::sort(impl_->particles.begin(),impl_->particles.end(),[&](const DrawParticle&a,const DrawParticle&b){
        return Vector3DistanceSqr(a.position,camera.position)>Vector3DistanceSqr(b.position,camera.position);});
}

size_t EffectPreview::ApplyPointLights(Shader shader,const BoundingBox& receiver) const{
    // CONFIRMED full-angle FXT admission: radius-expanded world AABB, followed
    // by distance to its center (rendering.md, 0049b770/00498310). Stable ties
    // are preview policy beyond the game's small-vector insertion-sort branch.
    const Vector3 center=Vector3Scale(Vector3Add(receiver.min,receiver.max),.5f);
    std::vector<const DrawLight*> candidates;
    for(const auto& light:impl_->lights) {
        const auto p=light.position; const float r=light.radius;
        if(!std::isfinite(r)||r<=0 || !std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)) continue;
        if(p.x<receiver.min.x-r || p.x>receiver.max.x+r ||
            p.y<receiver.min.y-r || p.y>receiver.max.y+r ||
            p.z<receiver.min.z-r || p.z>receiver.max.z+r) continue;
        candidates.push_back(&light);
    }
    std::stable_sort(candidates.begin(),candidates.end(),[&](const DrawLight* a,const DrawLight* b){
        return Vector3DistanceSqr(a->position,center)<Vector3DistanceSqr(b->position,center);
    });
    const int count=(int)std::min<size_t>(candidates.size(),7);
    std::array<float,24> positions{},colors{};std::array<float,8> radii{};
    for(int i=0;i<count;++i){const DrawLight& l=*candidates[i];positions[i*3]=l.position.x;
        positions[i*3+1]=l.position.y;positions[i*3+2]=l.position.z;colors[i*3]=l.color.x;
        colors[i*3+1]=l.color.y;colors[i*3+2]=l.color.z;radii[i]=l.radius;}
    SetShaderValue(shader,GetShaderLocation(shader,"effectLightCount"),&count,SHADER_UNIFORM_INT);
    if(count){SetShaderValueV(shader,GetShaderLocation(shader,"effectLightPositions"),positions.data(),SHADER_UNIFORM_VEC3,count);
        SetShaderValueV(shader,GetShaderLocation(shader,"effectLightColors"),colors.data(),SHADER_UNIFORM_VEC3,count);
        SetShaderValueV(shader,GetShaderLocation(shader,"effectLightRadii"),radii.data(),SHADER_UNIFORM_FLOAT,count);}
    return count;
}

void EffectPreview::Draw(const Camera3D& camera) const{
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();rlDisableDepthMask();
    const auto drawParticle=[&](const DrawParticle& p,Rectangle source,Color color){
        Vector3 axisRight=p.right,axisUp=p.up;
        if(!p.oriented){
            const Vector3 forward=SafeNormalize(Vector3Subtract(camera.target,camera.position));
            const Vector3 cameraRight=SafeNormalize(Vector3CrossProduct(forward,camera.up));
            const Vector3 cameraUp=Vector3CrossProduct(cameraRight,forward);
            const float angle=p.rotation*DEG2RAD,c=std::cos(angle),s=std::sin(angle);
            axisRight=Vector3Subtract(Vector3Scale(cameraRight,c),Vector3Scale(cameraUp,s));
            axisUp=Vector3Add(Vector3Scale(cameraRight,s),Vector3Scale(cameraUp,c));
        }
        // Opposing corners remain centered on the simulated position at every
        // size and rotation. Rotation acts in an orthonormal camera plane.
        const Vector3 right=Vector3Scale(axisRight,p.size.x*.5f);
        const Vector3 up=Vector3Scale(axisUp,p.size.y*.5f);
        const Vector3 a=Vector3Subtract(Vector3Subtract(p.position,right),up);
        const Vector3 b=Vector3Add(Vector3Subtract(p.position,up),right);
        const Vector3 c=Vector3Add(Vector3Add(p.position,right),up);
        const Vector3 d=Vector3Add(Vector3Subtract(p.position,right),up);
        const float u0=source.x/p.texture.width,v0=source.y/p.texture.height;
        const float u1=(source.x+source.width)/p.texture.width,
            v1=(source.y+source.height)/p.texture.height;
        rlSetTexture(p.texture.id);rlBegin(RL_TRIANGLES);rlColor4ub(color.r,color.g,color.b,color.a);
        rlTexCoord2f(u0,v0);rlVertex3f(a.x,a.y,a.z);rlTexCoord2f(u1,v0);rlVertex3f(b.x,b.y,b.z);
        rlTexCoord2f(u1,v1);rlVertex3f(c.x,c.y,c.z);rlTexCoord2f(u0,v0);rlVertex3f(a.x,a.y,a.z);
        rlTexCoord2f(u1,v1);rlVertex3f(c.x,c.y,c.z);rlTexCoord2f(u0,v1);rlVertex3f(d.x,d.y,d.z);
        rlEnd();rlSetTexture(0);
    };
    for(int pass=0;pass<2;++pass){BeginBlendMode(pass?BLEND_ADDITIVE:BLEND_ALPHA);
        for(const DrawParticle&p:impl_->particles){if((int)p.additive!=pass)continue;
            Color c=p.color;if(p.frameBlend<0.999f){Color a=c;a.a=(unsigned char)(a.a*(1-p.frameBlend));
                drawParticle(p,p.source,a);}
            if(p.frameBlend>0.001f){Color b=c;b.a=(unsigned char)(b.a*p.frameBlend);
                drawParticle(p,p.source2,b);}
        }EndBlendMode();}
    rlDrawRenderBatchActive();
    rlEnableDepthMask();rlEnableBackfaceCulling();
}

void EffectPreview::Unload(){if(!impl_)return;for(auto& pair:impl_->templates)
    for(auto& texture:pair.second.textures)if(texture.second.id)UnloadTexture(texture.second);
    impl_->templates.clear();impl_->particles.clear();impl_->lights.clear();impl_->trails.clear();impl_->failed=0;}
size_t EffectPreview::LoadedTemplateCount()const{size_t n=0;for(const auto&p:impl_->templates)if(p.second.loaded)++n;return n;}
size_t EffectPreview::FailedTemplateCount()const{return impl_->failed;}
size_t EffectPreview::VisibleParticleCount()const{return impl_->particles.size();}
size_t EffectPreview::VisibleLightCount()const{return impl_->lights.size();}
bool EffectPreview::ValidateTemplateFile(const std::string& path){EffectTemplate effect;return ParseTemplate(path,effect);}
