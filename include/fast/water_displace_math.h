#pragma once
// M12: matematica independente do jogo; espelho de shaders/lib/water.hlsli.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace Fast::WaterDisplace {
constexpr float Cell = 8.0f, Radius = 256.0f, InnerRadius = 192.0f;
constexpr float ShoreWidth = 32.0f, PlaneTolerance = 0.5f, MaxHeight = 2.82f;
struct Point { float x, y, z; };
struct Box { float x0, z0, x1, z1, y; };
struct Frame {
    std::array<Box, 8> boxes{};
    std::size_t count = 0;
    float time = 0, centerX = 0, centerZ = 0;
};
inline float Smooth(float a, float b, float v) {
    const float t = std::clamp((v - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
inline float Height(float x, float z, float t) {
    return 1.45f * std::sin(0.071f*x + 0.043f*z + 1.42f*t) +
           0.91f * std::sin(-0.037f*x + 0.081f*z - 1.67f*t) +
           0.46f * std::sin(0.132f*x - 0.097f*z + 2.25f*t);
}
inline std::array<float, 2> Slope(float x, float z, float t) {
    const float a = 1.45f * std::cos(0.071f*x + 0.043f*z + 1.42f*t);
    const float b = 0.91f * std::cos(-0.037f*x + 0.081f*z - 1.67f*t);
    const float c = 0.46f * std::cos(0.132f*x - 0.097f*z + 2.25f*t);
    return {0.071f*a - 0.037f*b + 0.132f*c, 0.043f*a + 0.081f*b - 0.097f*c};
}
inline float Mask(const Frame& f, float x, float z, float plane) {
    float shore = 0;
    for (std::size_t i = 0; i < std::min(f.count, f.boxes.size()); ++i) {
        const auto& b = f.boxes[i];
        if (std::abs(plane - b.y) > PlaneTolerance) continue;
        const float margin = std::min({x-b.x0, b.x1-x, z-b.z0, b.z1-z});
        shore = std::max(shore, Smooth(0, ShoreWidth, margin));
    }
    const float dx = x-f.centerX, dz = z-f.centerZ;
    return shore * (1-Smooth(InnerRadius, Radius, std::sqrt(dx*dx+dz*dz)));
}
inline float Displacement(const Frame& f, float x, float z, float plane) {
    return Mask(f,x,z,plane) * Height(x,z,f.time);
}
inline bool Eligible(const Frame& f, Point a, Point b, Point c) {
    if (!std::isfinite(a.x+a.y+a.z+b.x+b.y+b.z+c.x+c.y+c.z)) return false;
    const float ux=b.x-a.x, uy=b.y-a.y, uz=b.z-a.z;
    const float vx=c.x-a.x, vy=c.y-a.y, vz=c.z-a.z;
    const float nx=uy*vz-uz*vy, ny=uz*vx-ux*vz, nz=ux*vy-uy*vx;
    // Rejeita degenerados e inclinacao > ~0,57 grau. Winding indiferente.
    if (std::abs(ny) < 0.001f || nx*nx+nz*nz > 0.0001f*ny*ny) return false;
    for (std::size_t i=0; i<std::min(f.count,f.boxes.size()); ++i) {
        const auto& box=f.boxes[i];
        if (std::abs(a.y-box.y)>PlaneTolerance || std::abs(b.y-box.y)>PlaneTolerance ||
            std::abs(c.y-box.y)>PlaneTolerance) continue;
        if (std::max({a.x,b.x,c.x}) < box.x0 || std::min({a.x,b.x,c.x}) > box.x1 ||
            std::max({a.z,b.z,c.z}) < box.z0 || std::min({a.z,b.z,c.z}) > box.z1) continue;
        return true; // AABB conservador; clip posterior confirma cobertura real.
    }
    return false;
}

// Clip em XZ conserva pesos baricentricos: UV, cores e fog interpolam sem
// escrever nos recursos N64 nem no cache de vertices carregados.
struct Vertex { Point p{}; std::array<float,3> weight{}; };
struct Polygon { std::array<Vertex, 12> v{}; unsigned count=0; };
inline Polygon Clip(const Polygon& p, int axis, float limit, bool greater) {
    Polygon out;
    if (!p.count) return out;
    auto coord=[axis](const Vertex& v){return axis==0 ? v.p.x : v.p.z;};
    auto same=[](const Vertex& a,const Vertex& b) {
        return a.p.x==b.p.x && a.p.y==b.p.y && a.p.z==b.p.z;
    };
    auto append=[&out,&same](const Vertex& v) {
        if(out.count && same(out.v[out.count-1],v)) return;
        out.v[out.count++]=v;
    };
    Vertex a=p.v[p.count-1];
    float da=(coord(a)-limit)*(greater?1.0f:-1.0f);
    for(unsigned i=0;i<p.count;++i) {
        Vertex b=p.v[i];
        const float db=(coord(b)-limit)*(greater?1.0f:-1.0f);
        if((da>=0)!=(db>=0)) {
            // Ordena a aresta para o mesmo cruzamento em triangulos adjacentes.
            const Vertex& lo=coord(a)<coord(b)?a:b;
            const Vertex& hi=coord(a)<coord(b)?b:a;
            const float t=(limit-coord(lo))/(coord(hi)-coord(lo));
            Vertex q;
            q.p={lo.p.x+(hi.p.x-lo.p.x)*t, lo.p.y+(hi.p.y-lo.p.y)*t,
                 lo.p.z+(hi.p.z-lo.p.z)*t};
            if(axis==0) q.p.x=limit; else q.p.z=limit;
            for(unsigned k=0;k<3;++k) q.weight[k]=lo.weight[k]+(hi.weight[k]-lo.weight[k])*t;
            // Endpoint no plano: usa o proprio ponto/pesos, evitando duplicata
            // por arredondamento e crescimento de poligonos degenerados.
            if(coord(a)==limit) q=a; else if(coord(b)==limit) q=b;
            append(q);
        }
        if(db>=0) append(b);
        a=b; da=db;
    }
    if(out.count>1 && same(out.v[0],out.v[out.count-1])) --out.count;
    return out;
}
template<class Emit> inline void Fan(const Polygon& p, Emit& emit) {
    for(unsigned i=1;i+1<p.count;++i) {
        const auto& a=p.v[0].p; const auto& b=p.v[i].p; const auto& c=p.v[i+1].p;
        if(std::abs((b.x-a.x)*(c.z-a.z)-(b.z-a.z)*(c.x-a.x))>0.00001f)
            emit(p.v[0],p.v[i],p.v[i+1]);
    }
}
template<class Emit> inline void Tessellate(Point a, Point b, Point c, const Frame& f, Emit emit) {
    Polygon p; p.count=3;
    p.v[0]={a,{1,0,0}}; p.v[1]={b,{0,1,0}}; p.v[2]={c,{0,0,1}};
    const float x0=f.centerX-Radius, x1=f.centerX+Radius;
    const float z0=f.centerZ-Radius, z1=f.centerZ+Radius;
    // Quatro faixas exteriores, disjuntas, mantem TODA a geometria original.
    // H=0 fora do circulo: o limite do refinamento nao produz salto de altura.
    auto cut=[&](int axis,float limit,bool greater) {
        auto outside=Clip(p,axis,limit,!greater); Fan(outside,emit);
        p=Clip(p,axis,limit,greater);
    };
    cut(0,x0,true); cut(0,x1,false); cut(1,z0,true); cut(1,z1,false);
    if(p.count<3) return;
    float minX=x1,maxX=x0,minZ=z1,maxZ=z0;
    for(unsigned i=0;i<p.count;++i) {
        minX=std::min(minX,p.v[i].p.x); maxX=std::max(maxX,p.v[i].p.x);
        minZ=std::min(minZ,p.v[i].p.z); maxZ=std::max(maxZ,p.v[i].p.z);
    }
    const int ix0=std::max(0,int(std::floor((minX-x0)/Cell)));
    const int iz0=std::max(0,int(std::floor((minZ-z0)/Cell)));
    const int ix1=std::min(63,int(std::floor((maxX-x0)/Cell)));
    const int iz1=std::min(63,int(std::floor((maxZ-z0)/Cell)));
    for(int iz=iz0;iz<=iz1;++iz) for(int ix=ix0;ix<=ix1;++ix) {
        const float x=x0+ix*Cell,z=z0+iz*Cell;
        auto cell=Clip(Clip(Clip(Clip(p,0,x,true),0,x+Cell,false),1,z,true),1,z+Cell,false);
        Fan(cell,emit);
    }
}
} // namespace Fast::WaterDisplace
