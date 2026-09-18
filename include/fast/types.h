#pragma once

typedef int Mtx_t[4][4];
typedef union {
    Mtx_t m;
    struct {
        unsigned short int intPart[4][4];
        unsigned short int fracPart[4][4];
    };
    long long int forc_structure_alignment;
} MtxS;

typedef float MtxF_t[4][4];
typedef union {
    MtxF_t mf;
    struct {
        float xx, yx, zx, wx, xy, yy, zy, wy, xz, yz, zz, wz, xw, yw, zw, ww;
    };
} MtxF;

// GBI_FLOATS: the whole GBI is float (vertices included).
// GBI_FLOAT_MTX (CMake option): only matrices are float (MtxF); vertices stay s16. The s16.16 fixed-point Mtx
// wraps any translation >= 32768, which caps the world at +/-32767 units even when everything else is float.
// The game writes floats straight into Mtx (guMtxF2L is a copy) and the interpreter reads them back verbatim;
// Matrix resources stored fixed-point are unpacked at load (MatrixFactory).
// GBI_S32_VTX (CMake option): only Vtx.ob is s32; matrices are unaffected. s16 vertex positions cap one
// display list at 65535 units across, which caps a room mesh (drawn under the identity matrix, so its vertices
// are world coordinates). Vertex resources stored s16 widen at load (VertexFactory), and the OTR vertex opcode
// carries a vertex count rather than a byte length, so existing archives need no re-export. Vtx grows 16 -> 24
// bytes. s32 (not float) keeps the integer semantics the game assumes wherever it reads a vertex back.
// The two options are independent; SoH's root CMakeLists.txt turns both on.
#if defined(GBI_FLOATS) || defined(GBI_FLOAT_MTX)
typedef MtxF Mtx;
#else
typedef MtxS Mtx;
#endif
