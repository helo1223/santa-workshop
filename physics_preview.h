#pragma once
#include "raylib.h"
#include "raymath.h"
#include "lvlx_format.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// CONFIRMED initial authored-body geometry: reverse/knowledge/element_physics.md.
// Read-only wire overlay; no solver, mesh scaling, or later reset-pose simulation.
namespace physics_preview {
struct Shape { int kind = -1; Vector3 size{}, position{}, rotation{}; };
struct Body { std::vector<Shape> shapes; bool joints = false, kinematic = false; };
inline std::string normalize(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') out += c;
        else if (c == ' ' && !out.empty()) out += '_';
    }
    return out;
}
inline std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
    return s;
}
struct Tokens {
    std::vector<std::string> values; size_t at = 0;
    explicit Tokens(const std::string& text) {
        for (size_t i = 0; i < text.size();) {
            if (static_cast<unsigned char>(text[i]) <= 32) { ++i; continue; }
            if (text.compare(i,2,"//") == 0) { i = text.find('\n', i); if (i == std::string::npos) break; continue; }
            if (text.compare(i,2,"/*") == 0) { auto end=text.find("*/",i+2); if(end==std::string::npos) throw std::runtime_error("unfinished comment"); i=end+2; continue; }
            if (text[i] == '"') {
                size_t end = text.find('"', i+1);
                if (end == std::string::npos) throw std::runtime_error("unfinished string");
                values.push_back(text.substr(i+1,end-i-1)); i=end+1;
            } else if (std::string("(){}").find(text[i]) != std::string::npos) values.push_back(text.substr(i++,1));
            else { size_t start=i; while(i<text.size() && static_cast<unsigned char>(text[i])>32 && std::string("(){}\"").find(text[i])==std::string::npos) ++i; values.push_back(text.substr(start,i-start)); }
        }
    }
    std::string take() { if(at==values.size()) throw std::runtime_error("truncated physics body"); return values[at++]; }
    void expect(const char* s) { if(take()!=s) throw std::runtime_error("invalid physics grammar"); }
    float number() { auto s=take(); char* end=nullptr; float f=std::strtof(s.c_str(),&end); if(end==s.c_str() || *end || !std::isfinite(f)) throw std::runtime_error("invalid physics number"); return f; }
    Vector3 vector() { float x=number(),y=number(),z=number(); return {x,y,z}; }
};
struct Library {
    std::unordered_map<std::string,Body> bodies;
    std::string error;
    bool load(const std::string& path) {
        bodies.clear(); error.clear();
        try {
            std::ifstream f(path); if(!f) throw std::runtime_error("cannot open " + path);
            Tokens t(std::string((std::istreambuf_iterator<char>(f)),{}));
            while(t.at<t.values.size()) {
                if(lower(t.take())!="physicbody") throw std::runtime_error("unknown physics declaration");
                t.expect("("); auto name=normalize(t.take()); t.expect(")"); t.expect("{"); Body b;
                for(auto op=lower(t.take()); op!="}"; op=lower(t.take())) {
                    if(op=="shape") {
                        auto kind=lower(t.take()); t.take(); // material identity does not change geometry
                        Shape s;
                        if(kind=="box") { s.kind=0; s.size=t.vector(); }
                        else if(kind=="sphere") { s.kind=1; s.size.x=t.number(); }
                        else if(kind=="capsule") { s.kind=2; s.size.x=t.number(); s.size.y=t.number(); }
                        else if(kind=="heightfield") { t.take(); t.number(); } // confirmed no shape compiled
                        else throw std::runtime_error("unsupported shape " + kind);
                        if(s.size.x<0 || s.size.y<0 || s.size.z<0) throw std::runtime_error("negative shape dimension");
                        b.shapes.push_back(s);
                    } else if(op=="pos" || op=="rot") {
                        auto v=t.vector(); if(!b.shapes.empty()) {
                            if(op=="pos") b.shapes.back().position=v;
                            else b.shapes.back().rotation=Vector3Scale(v,DEG2RAD);
                        }
                    } else if(op=="joint") { auto k=lower(t.take()); if(k!="prismatic" && k!="revolute") throw std::runtime_error("unsupported joint"); t.vector(); t.number(); t.number(); b.joints=true; }
                    else if(op=="kinematic") b.kinematic=true;
                    else throw std::runtime_error("unsupported physics property " + op);
                }
                bodies.emplace(name,std::move(b)); // first normalized declaration wins
            }
            return true;
        } catch(const std::exception& e) { error=e.what(); bodies.clear(); return false; }
    }
    const Body* find(const char* name) const {
        if(!name) return nullptr;
        auto it=bodies.find(normalize(name)); return it==bodies.end()?nullptr:&it->second;
    }
};
inline Vector3 point(const Body& b, const Shape& s, const LvlxElement& e, Vector3 u) {
    // Game column-vector order: Rz(-z) Ry(-y) Rx(-x), then placement Ry(+90*index).
    if(s.kind!=1) {
        u=Vector3RotateByAxisAngle(u,{1,0,0},-s.rotation.x);
        u=Vector3RotateByAxisAngle(u,{0,1,0},-s.rotation.y);
        u=Vector3RotateByAxisAngle(u,{0,0,1},-s.rotation.z);
    }
    u=Vector3Add(u,s.position);
    if(!b.joints || b.kinematic) u=Vector3RotateByAxisAngle(u,{0,1,0},e.orientation*PI/2);
    return {u.x+e.vector1.x,u.y+e.vector1.y,-u.z-e.vector1.z};
}
inline void draw(const Library& lib, const LvlxLevel& level, const LvlxDefinitionTable& definitions) {
    for(uint32_t i=0;i<level.element_count;++i) {
        const auto& e=level.elements[i]; if(!e.present || e.orientation>3) continue;
        const auto* def=lvlx_find_definition(&definitions,e.definition_id);
        const Body* b=lib.find(def?def->physics_body:nullptr); if(!b) continue;
        const Color color={45,245,220,255};
        for(const auto& s:b->shapes) {
            if(s.kind==0) {
                std::array<Vector3,8> corners;
                for(int j=0;j<8;++j) corners[j]=point(*b,s,e,{(j&1)?s.size.x:-s.size.x,(j&2)?s.size.y:-s.size.y,(j&4)?s.size.z:-s.size.z});
                for(int j=0;j<8;++j) for(int bit=1;bit<=4;bit*=2) if(!(j&bit)) DrawLine3D(corners[j],corners[j|bit],color);
            } else if(s.kind==1) DrawSphereWires(point(*b,s,e,{}),s.size.x,12,16,color);
            else if(s.kind==2) DrawCapsuleWires(point(*b,s,e,{-s.size.y/2,0,0}),point(*b,s,e,{s.size.y/2,0,0}),s.size.x,12,8,color);
        }
    }
}
}
