/* paf/math/math.h - minimal stub: provides math types without full vectormath impl */
#ifndef _VDSUITE_USER_PAF_MATH_MATH_H
#define _VDSUITE_USER_PAF_MATH_MATH_H
#include <stdint.h>
#include <math.h>

namespace paf {
namespace math {

struct v1 {
    float x;
    v1() : x(0) {}
    explicit v1(float x) : x(x) {}
    operator float() const { return x; }
};

struct v2 {
    float x, y;
    v2() : x(0), y(0) {}
    v2(float x, float y) : x(x), y(y) {}
    v2(v1 a, v1 b) : x(a.x), y(b.x) {}
    bool operator==(const v2& o) const { return x==o.x && y==o.y; }
    bool operator!=(const v2& o) const { return !(*this==o); }
};

struct v3 {
    float x, y, z;
    v3() : x(0), y(0), z(0) {}
    v3(float x, float y, float z) : x(x), y(y), z(z) {}
    bool operator==(const v3& o) const { return x==o.x && y==o.y && z==o.z; }
    bool operator!=(const v3& o) const { return !(*this==o); }
};

struct v4 {
    float x, y, z, w;
    v4() : x(0), y(0), z(0), w(0) {}
    v4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    v4(v2 a, v2 b) : x(a.x), y(a.y), z(b.x), w(b.y) {}
    bool operator==(const v4& o) const { return x==o.x && y==o.y && z==o.z && w==o.w; }
    bool operator!=(const v4& o) const { return !(*this==o); }
    static v4 _0000() { return v4(0,0,0,0); }
    void set_x(float v) { x = v; }
    void set_y(float v) { y = v; }
    void set_z(float v) { z = v; }
    void set_w(float v) { w = v; }
};

struct v1i {
    int x;
    v1i() : x(0) {}
    explicit v1i(int x) : x(x) {}
};

struct v2i {
    int x, y;
    v2i() : x(0), y(0) {}
    v2i(int x, int y) : x(x), y(y) {}
};

struct quaternion {
    float x, y, z, w;
    quaternion() : x(0), y(0), z(0), w(1) {}
    quaternion(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
};

struct Matrix4 {
    float m[16];
    Matrix4() { for(int i=0;i<16;i++) m[i]=0; }
};

struct Matrix3x3 {
    float m[9];
    Matrix3x3() { for(int i=0;i<9;i++) m[i]=0; }
};

typedef Matrix4 matrix;

} /* math */
} /* paf */

#endif /* _VDSUITE_USER_PAF_MATH_MATH_H */
