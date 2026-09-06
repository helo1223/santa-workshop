#pragma once
#include "crf_format.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// CONFIRMED format/equations: reverse/knowledge/animations.md.
// Bounded single-clip CPU preview uses the scalar quaternion route. No gameplay,
// descriptor blending, event dispatch, or claim of SSE bit-exact equivalence.
namespace animation_preview {
using Vec = std::array<float,4>;
struct Matrix {
    float c[4][3]={{1,0,0},{0,1,0},{0,0,1},{0,0,0}};
    Vec transform(const Vec& p, bool direction=false) const {
        Vec out{};
        for(int r=0;r<3;++r) out[r]=c[0][r]*p[0]+c[1][r]*p[1]+c[2][r]*p[2]+(direction?0:c[3][r]);
        return out;
    }
};
inline Matrix multiply(const Matrix& a,const Matrix& b) {
    Matrix out;
    for(int j=0;j<4;++j) {
        auto v=a.transform({b.c[j][0],b.c[j][1],b.c[j][2],0},j!=3);
        for(int k=0;k<3;++k) out.c[j][k]=v[k];
    }
    return out;
}
inline void require(bool ok,const char* error) { if(!ok) throw std::runtime_error(error); }
inline std::vector<unsigned char> file(const std::string& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate); require(bool(f),"animation resource missing");
    auto n=f.tellg(); require(n>=0 && n<=128*1024*1024,"animation resource size");
    std::vector<unsigned char> b(static_cast<size_t>(n)); f.seekg(0);
    if(!b.empty()) require(bool(f.read(reinterpret_cast<char*>(b.data()),b.size())),"animation resource read");
    return b;
}
struct Reader {
    const std::vector<unsigned char>& b; size_t p=0,end;
    explicit Reader(const std::vector<unsigned char>& bytes):b(bytes),end(bytes.size()){}
    Reader(const std::vector<unsigned char>& bytes,size_t start,size_t n):b(bytes),p(start),end(start+n) { require(start<=b.size() && n<=b.size()-start,"chunk bounds"); }
    void skip(size_t n) { require(p<=end && n<=end-p,"truncated animation data"); p+=n; }
    uint8_t byte() { skip(1); return b[p-1]; }
    uint16_t shortValue() { uint16_t a=byte(); return a|(uint16_t(byte())<<8); }
    uint32_t word() { uint32_t a=shortValue(); return a|(uint32_t(shortValue())<<16); }
    float number() { uint32_t bits=word(); float f; std::memcpy(&f,&bits,4); require(std::isfinite(f),"nonfinite animation float"); return f; }
    Vec vector() { Vec v; for(float& f:v) f=number(); return v; }
    std::string string() { auto n=word(); size_t start=p; skip(n); return std::string(b.begin()+start,b.begin()+p); }
    void done() { require(p==end,"unsupported animation trailing data"); }
};
inline uint32_t crc(const std::string& s) {
    uint32_t c=0; for(unsigned char x:s) { c^=x; for(int i=0;i<8;++i) c=(c>>1)^((c&1)?0xedb88320u:0); } return c;
}
struct Track { uint32_t first,count,channel,name,joint=0; };
struct Key { Vec value,tangent; };
struct Clip {
    std::vector<Track> tracks; std::vector<float> times; std::vector<Key> keys;
    float duration=0; uint32_t mode=0;
    void load(const std::string& path,const std::unordered_map<uint32_t,uint32_t>& names,size_t joints) {
        auto bytes=file(path); Reader r(bytes); auto count=r.word();
        require(count>0 && count<=2*joints,"unsupported track count");
        tracks.resize(count); for(auto& t:tracks) { t.first=r.word(); t.count=r.word(); t.channel=r.word(); t.name=r.word(); }
        auto k=r.word(); require(k>0 && k<=1000000 && k<=(r.end-r.p)/36,"key count bounds");
        times.resize(k); for(float& t:times) t=r.number(); keys.resize(k);
        for(auto& key:keys) { key.value=r.vector(); key.tangent=r.vector(); }
        duration=r.number(); mode=r.word()&255u; require(duration>0 && mode<=1,"unsupported clip duration/mode");
        if(r.p<r.end) {
            auto markers=r.word(); require(markers<=65536,"clip marker count");
            for(uint32_t i=0;i<markers;++i) { r.string(); r.number(); } // consume without dispatching sound/gameplay
        }
        r.done();
        std::vector<bool> bound(2*joints); uint32_t cursor=0;
        for(auto& t:tracks) {
            require(t.first==cursor && t.count>0 && t.first<=k && t.count<=k-t.first && t.channel<=1,"invalid track span/channel");
            cursor+=t.count; auto found=names.find(t.name); require(found!=names.end(),"unmatched animation bone"); t.joint=found->second;
            auto dest=2*t.joint+t.channel; require(!bound[dest],"duplicate animation channel"); bound[dest]=true;
            for(uint32_t i=t.first+1;i<t.first+t.count;++i) require(times[i]>=times[i-1],"unsorted animation keys");
            require(times[t.first]>=0 && times[t.first+t.count-1]<=1,"animation keys outside normalized time");
        }
        require(cursor==k && std::all_of(bound.begin(),bound.end(),[](bool b){return b;}),"incomplete animation channel coverage");
    }
    Vec sample(const Track& t,float time) const {
        const auto begin=times.begin()+t.first,end=begin+t.count;
        auto upper=std::upper_bound(begin,end,time);
        uint32_t a,b; float ta,tb;
        if(upper==begin || upper==end) {
            a=upper==begin?t.first:t.first+t.count-1;
            if(mode==0) { Vec out; for(int j=0;j<4;++j) out[j]=keys[a].value[j]+(time-times[a])*keys[a].tangent[j]; return out; }
            a=t.first+t.count-1; b=t.first; ta=times[a]-(upper==begin?1.0f:0.0f); tb=times[b]+(upper==end?1.0f:0.0f);
        } else { b=uint32_t(upper-times.begin()); a=b-1; ta=times[a]; tb=times[b]; }
        const float dt=tb-ta;
        // Exact endpoint with duplicate 0/1 seam: use the endpoint, not 0/0.
        if(dt==0 && time==ta) return keys[a].value;
        require(dt>0,"invalid cyclic key interval");
        float u=(time-ta)/dt; Vec out;
        for(int j=0;j<4;++j) {
            float v0=keys[a].value[j],v1=keys[b].value[j],m0=dt*keys[a].tangent[j],m1=dt*keys[b].tangent[j];
            float A=2*(v0-v1)+m0+m1,B=3*(v1-v0)-2*m0-m1;
            out[j]=(A*u+B)*(u*u)+(m0*u+v0);
        }
        return out;
    }
};
struct Skin {
    std::vector<float> positions,normals;
    std::vector<uint8_t> weights,indices;
    std::vector<uint16_t> map;
};
struct Rig {
    std::vector<Matrix> assetMatrices;
    std::vector<int32_t> parents;
    std::vector<Skin> skins;
    Clip clip;
    bool ready=false;
    std::string error;
    bool load(const std::string& crf,const std::string& caf,const CrfModelData& mesh) {
        ready=false; error.clear();
        try {
            auto bytes=file(crf); Reader r(bytes);
            require(r.word()==0x636e6b66 && r.word()==1,"CRF container version");
            auto dir=r.word(); r.word(); auto n=r.word(); require(n>0 && n<=65536,"CRF directory count");
            Reader table(bytes,dir,size_t(n)*32);
            std::unordered_map<uint32_t,std::pair<uint32_t,uint32_t>> chunks;
            for(uint32_t i=0;i<n;++i) { auto id=table.word(); table.word(); auto at=table.word(),size=table.word(); table.skip(16); require(chunks.emplace(id,std::make_pair(at,size)).second,"duplicate CRF chunk"); }
            auto chunk=[&](const char* name) { auto it=chunks.find(crc(name)); require(it!=chunks.end(),"missing skeleton/jointmap chunk"); return Reader(bytes,it->second.first,it->second.second); };
            auto sk=chunk("skeleton"); require(sk.word()==1,"unsupported skeleton mode");
            auto joints=sk.word(); require(joints>0 && joints<=1024,"skeleton joint count");
            assetMatrices.resize(joints); parents.resize(joints); std::vector<uint32_t> ordinals(joints);
            for(uint32_t i=0;i<joints;++i) {
                for(int c=0;c<4;++c) { for(float& f:assetMatrices[i].c[c]) f=sk.number(); auto word=sk.word(); if(c==0) parents[i]=int32_t(word); if(c==1) ordinals[i]=word; }
                require(parents[i]>=-1 && parents[i]<int32_t(i),"invalid skeleton parent order");
            }
            require(sk.word()==joints,"skeleton name count"); std::unordered_map<uint32_t,uint32_t> names;
            for(uint32_t i=0;i<joints;++i) {
                auto nameCRC=sk.word(),joint=sk.word(); auto name=sk.string();
                require(joint<joints && ordinals[joint]==i && nameCRC==crc(name),"skeleton name/index mismatch");
                require(names.emplace(nameCRC,joint).second,"duplicate bone CRC");
                auto count=sk.word(); require(count<=joints,"skeleton children count");
                std::vector<uint32_t> expected; for(uint32_t j=0;j<joints;++j) if(parents[j]==int32_t(joint)) expected.push_back(j);
                require(count==expected.size(),"skeleton child count mismatch");
                for(auto j:expected) require(sk.word()==j,"skeleton child index mismatch");
            }
            // The established stock-pair contract uses an identity root/global matrix.
            for(int i=0;i<16;++i) require(sk.number()==(i%5==0?1.0f:0.0f),"nonidentity skeleton global matrix unsupported");
            require(sk.byte()==1,"disabled skeleton"); sk.done();
            auto jm=chunk("jointmap"); require(jm.word()==mesh.mesh_count,"jointmap/subset mismatch");
            skins.resize(mesh.mesh_count);
            for(uint32_t i=0;i<mesh.mesh_count;++i) {
                auto& s=skins[i]; const auto& source=mesh.meshes[i]; auto count=jm.word();
                require(count>0 && count<=76,"skin palette capacity"); s.map.resize(count);
                for(auto& j:s.map) { j=jm.shortValue(); require(j<joints,"jointmap index bounds"); }
                require(source.skin_indices!=nullptr,"unsupported vertex skin declaration");
                s.positions.assign(source.positions,source.positions+source.vertex_count*3);
                s.normals.assign(source.normals,source.normals+source.vertex_count*3);
                s.indices.assign(source.skin_indices,source.skin_indices+source.vertex_count*4);
                if(source.skin_weights) s.weights.assign(source.skin_weights,source.skin_weights+source.vertex_count*4);
                for(uint32_t v=0;v<source.vertex_count;++v) for(int k=0;k<(s.weights.empty()?1:4);++k)
                    require(s.indices[v*4+k]<count,"skin vertex palette index");
            }
            jm.done(); clip.load(caf,names,joints); ready=true; return true;
        } catch(const std::exception& e) { error=e.what(); skins.clear(); return false; }
    }
    std::vector<Matrix> pose(float normalizedTime) const {
        require(ready && std::isfinite(normalizedTime),"invalid animation pose request");
        float time=clip.mode==1?normalizedTime-std::floor(normalizedTime):std::clamp(normalizedTime,0.0f,1.0f);
        std::vector<Vec> channels(parents.size()*2);
        for(const auto& t:clip.tracks) channels[t.joint*2+t.channel]=clip.sample(t,time);
        std::vector<Matrix> globals(parents.size()),out(parents.size());
        for(size_t i=0;i<parents.size();++i) {
            Vec q=channels[2*i+1]; float norm=0; for(float f:q) norm+=f*f;
            require(std::isfinite(norm) && norm>1e-20f,"invalid sampled quaternion");
            for(float& f:q) f/=std::sqrt(norm);
            const float x=q[0],y=q[1],z=q[2],w=q[3],s=x*x+y*y+z*z+w*w;
            // These are COLUMNS: the recovered rotation is the conjugate of the
            // common active quaternion convention, not raymath QuaternionToMatrix.
            Matrix local{{{s-2*(y*y+z*z),2*(x*y-w*z),2*(x*z+w*y)},
                {2*(x*y+w*z),s-2*(x*x+z*z),2*(y*z-w*x)},
                {2*(x*z-w*y),2*(y*z+w*x),s-2*(x*x+y*y)},
                {channels[2*i][0],channels[2*i][1],channels[2*i][2]}}};
            globals[i]=parents[i]<0?local:multiply(globals[parents[i]],local);
            out[i]=multiply(globals[i],assetMatrices[i]);
            for(auto& c:out[i].c) for(float f:c) require(std::isfinite(f),"nonfinite sampled pose");
        }
        return out;
    }
    void deform(size_t index,const std::vector<Matrix>& pose,float* positions,float* normals) const {
        const auto& s=skins.at(index); require(pose.size()==parents.size(),"pose palette size");
        for(size_t v=0;v<s.positions.size()/3;++v) {
            Vec p={s.positions[v*3],s.positions[v*3+1],s.positions[v*3+2],1};
            Vec n={s.normals[v*3],s.normals[v*3+1],s.normals[v*3+2],0},a{},b{};
            const int order[4]={1,0,2,3};
            for(int k=0;k<(s.weights.empty()?1:4);++k) {
                int j=s.weights.empty()?0:order[k]; float w=s.weights.empty()?1.0f:s.weights[v*4+j]/255.0f;
                const auto& m=pose[s.map[s.indices[v*4+j]]]; auto pp=m.transform(p),nn=m.transform(n,true);
                for(int c=0;c<3;++c) { a[c]+=w*pp[c]; b[c]+=w*nn[c]; }
            }
            float length=std::sqrt(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]);
            require(std::isfinite(length),"nonfinite skinned normal");
            for(int c=0;c<3;++c) {
                require(std::isfinite(a[c]),"nonfinite skinned position");
                positions[v*3+c]=a[c]*(c==2?-1:1);
                normals[v*3+c]=(length>1e-20f?b[c]/length:n[c])*(c==2?-1:1);
            }
        }
    }
};
}
