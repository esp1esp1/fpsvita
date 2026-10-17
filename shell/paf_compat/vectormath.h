/* vectormath.h - stub for Sony PSP2 Vectormath SDK */
#ifndef _SCE_VECTORMATH_H
#define _SCE_VECTORMATH_H
#include <stdint.h>

#ifndef SCE_VECTORMATH_ALWAYS_INLINE
# define SCE_VECTORMATH_ALWAYS_INLINE inline __attribute__((always_inline))
#endif

namespace sce {
namespace Vectormath {
namespace Simd {
namespace Aos {

class floatInVec {
public:
    floatInVec() : _val(0.0f) {}
    floatInVec(float v) : _val(v) {}
    operator float() const { return _val; }
    floatInVec& operator=(float v) { _val = v; return *this; }
private:
    float _val;
};

class Vector2 {
public:
    float x, y;
    Vector2() : x(0), y(0) {}
    Vector2(float x, float y) : x(x), y(y) {}
};

class Vector3 {
public:
    float x, y, z;
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}
};

class Vector4 {
public:
    float x, y, z, w;
    Vector4() : x(0), y(0), z(0), w(0) {}
    Vector4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
};

class Matrix4 {
public:
    float m[16];
    Matrix4() { for(int i=0;i<16;i++) m[i]=0; }
};

class Quat {
public:
    float x, y, z, w;
    Quat() : x(0), y(0), z(0), w(1) {}
};

} /* Aos */
} /* Simd */
} /* Vectormath */
} /* sce */

/* Also expose floatInVec in sce::Vectormath::Simd for v1.h */
namespace sce { namespace Vectormath { namespace Simd {
    using Aos::floatInVec;
    using Aos::Vector2;
    using Aos::Vector3;
    using Aos::Vector4;
    using Aos::Matrix4;
    using Aos::Quat;
}}}


#endif /* _SCE_VECTORMATH_H */
