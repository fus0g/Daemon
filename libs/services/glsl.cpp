// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <unistd.h>
module daemon.services.glsl;
import std;
import daemon.base.util;
import daemon.base.process;
import daemon.services.lsp;

namespace fs = std::filesystem;

namespace ide::glsl {

// ====================================================================================================== file types

const std::vector<std::string>& extensions() {
    static const std::vector<std::string> e = {
        "glsl", "vert", "frag", "geom", "tesc", "tese", "comp", "mesh", "task",              // Khronos / glslang names
        "rgen", "rint", "rahit", "rchit", "rmiss", "rcall",                                   // ray tracing
        "vs", "fs", "tcs", "tes", "vsh", "fsh", "gsh",                            // common short names
        "glslv", "glslf", "glslg", "glslc", "glslh", "glslinc", "vertex", "fragment", "geometry", "compute",
        "vp", "fp", "frag.glsl", "vert.glsl",
    };
    return e;
}

namespace {

std::string lowerExt(std::string_view path) {
    const auto slash = path.find_last_of('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string e(name.substr(dot + 1));
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string stemExt(std::string_view path) {   // "shader.vert.glsl" -> "vert"
    const auto slash = path.find_last_of('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto d2 = name.find_last_of('.');
    if (d2 == std::string_view::npos || d2 == 0) return {};
    const auto d1 = name.find_last_of('.', d2 - 1);
    if (d1 == std::string_view::npos) return {};
    std::string e(name.substr(d1 + 1, d2 - d1 - 1));
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string stageFromExt(const std::string& e) {
    static const std::map<std::string, std::string> m = {
        {"vert", "vert"}, {"vs", "vert"}, {"vsh", "vert"}, {"glslv", "vert"}, {"vertex", "vert"}, {"vp", "vert"},
        {"frag", "frag"}, {"fs", "frag"}, {"fsh", "frag"}, {"glslf", "frag"}, {"fragment", "frag"}, {"fp", "frag"},
        {"geom", "geom"}, {"gsh", "geom"}, {"glslg", "geom"}, {"geometry", "geom"},
        {"tesc", "tesc"}, {"tcs", "tesc"}, {"tese", "tese"}, {"tes", "tese"},
        {"comp", "comp"}, {"compute", "comp"},
        {"mesh", "mesh"}, {"task", "task"},
        {"rgen", "rgen"}, {"rint", "rint"}, {"rahit", "rahit"}, {"rchit", "rchit"}, {"rmiss", "rmiss"}, {"rcall", "rcall"},
    };
    auto it = m.find(e);
    return it == m.end() ? std::string() : it->second;
}

}  // namespace

bool isGlslPath(std::string_view path) {
    const std::string e = lowerExt(path);
    if (e.empty()) return false;
    return std::ranges::find(extensions(), e) != extensions().end();
}

std::string stageOf(std::string_view path, std::string_view text) {
    if (auto s = stageFromExt(lowerExt(path)); !s.empty()) return s;
    if (auto s = stageFromExt(stemExt(path)); !s.empty()) return s;
    const auto at = text.find("#pragma shader_stage(");
    if (at != std::string_view::npos) {
        const auto b = at + 21, e = text.find(')', b);
        if (e != std::string_view::npos) {
            std::string v(text.substr(b, e - b));
            if (auto s = stageFromExt(v); !s.empty()) return s;
            if (v == "vertex") return "vert";
            if (v == "fragment") return "frag";
            if (v == "compute") return "comp";
            if (v == "geometry") return "geom";
            if (v == "tesscontrol") return "tesc";
            if (v == "tesseval") return "tese";
        }
    }
    return {};
}

// ====================================================================================================== language data

namespace {

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

const std::unordered_set<std::string>& keywords() {
    static const std::unordered_set<std::string> k = {
        "const", "uniform", "buffer", "shared", "attribute", "varying", "in", "out", "inout", "centroid", "flat", "smooth", "noperspective",
        "patch", "sample", "layout", "invariant", "precise", "precision", "highp", "mediump", "lowp", "coherent", "volatile", "restrict",
        "readonly", "writeonly", "subroutine", "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
        "return", "discard", "struct", "true", "false", "rayPayloadEXT", "rayPayloadInEXT", "hitAttributeEXT", "callableDataEXT",
        "callableDataInEXT", "shaderRecordEXT", "taskPayloadSharedEXT", "perprimitiveEXT", "terminateRayEXT", "ignoreIntersectionEXT",
    };
    return k;
}

// Qualifiers that can precede a declaration (not the control-flow keywords).
const std::unordered_set<std::string>& qualifierWords() {
    static const std::unordered_set<std::string> k = {
        "const", "uniform", "buffer", "shared", "attribute", "varying", "in", "out", "inout", "centroid", "flat", "smooth", "noperspective",
        "patch", "sample", "invariant", "precise", "highp", "mediump", "lowp", "coherent", "volatile", "restrict", "readonly", "writeonly",
        "rayPayloadEXT", "rayPayloadInEXT", "hitAttributeEXT", "callableDataEXT", "callableDataInEXT", "shaderRecordEXT", "taskPayloadSharedEXT",
        "perprimitiveEXT",
    };
    return k;
}

struct TypeInfo { int comps = 0; std::string element; std::string doc; };

const std::map<std::string, TypeInfo>& builtinTypes() {
    static const std::map<std::string, TypeInfo> t = [] {
        std::map<std::string, TypeInfo> m;
        m["void"] = {0, "", "no value"};
        m["bool"] = {0, "bool", "true or false"};
        m["int"] = {0, "int", "32-bit signed integer"};
        m["uint"] = {0, "uint", "32-bit unsigned integer"};
        m["float"] = {0, "float", "32-bit floating point number"};
        m["double"] = {0, "double", "64-bit floating point number"};
        const std::pair<const char*, std::pair<const char*, const char*>> vecs[] = {
            {"", {"float", "float"}}, {"i", {"int", "signed integer"}}, {"u", {"uint", "unsigned integer"}}, {"b", {"bool", "boolean"}}, {"d", {"double", "double precision"}}};
        for (const auto& [pre, el] : vecs)
            for (int n = 2; n <= 4; ++n)
                m[std::string(pre) + "vec" + std::to_string(n)] = {n, el.first, std::format("{}-component {} vector", n, el.second)};
        for (int c = 2; c <= 4; ++c) {
            m["mat" + std::to_string(c)] = {0, "float", std::format("{0}x{0} float matrix", c)};
            m["dmat" + std::to_string(c)] = {0, "double", std::format("{0}x{0} double matrix", c)};
            for (int r = 2; r <= 4; ++r) {
                m[std::format("mat{}x{}", c, r)] = {0, "float", std::format("float matrix with {} columns and {} rows", c, r)};
                m[std::format("dmat{}x{}", c, r)] = {0, "double", std::format("double matrix with {} columns and {} rows", c, r)};
            }
        }
        for (const char* pre : {"", "i", "u"}) {
            for (const char* d : {"1D", "2D", "3D", "Cube", "2DRect", "1DArray", "2DArray", "CubeArray", "Buffer", "2DMS", "2DMSArray"}) {
                m[std::string(pre) + "sampler" + d] = {0, "", "texture sampler"};
                m[std::string(pre) + "image" + d] = {0, "", "image variable (imageLoad / imageStore)"};
            }
            m[std::string(pre) + "texture2D"] = {0, "", "texture (Vulkan: combine with a sampler)"};
            m[std::string(pre) + "texture3D"] = {0, "", "texture (Vulkan: combine with a sampler)"};
            m[std::string(pre) + "textureCube"] = {0, "", "texture (Vulkan: combine with a sampler)"};
            m[std::string(pre) + "texture2DArray"] = {0, "", "texture (Vulkan: combine with a sampler)"};
            m[std::string(pre) + "subpassInput"] = {0, "", "input attachment"};
            m[std::string(pre) + "subpassInputMS"] = {0, "", "multisampled input attachment"};
        }
        for (const char* d : {"1D", "2D", "Cube", "2DRect", "1DArray", "2DArray", "CubeArray"}) m[std::string("sampler") + d + "Shadow"] = {0, "", "depth-comparison sampler"};
        m["sampler"] = {0, "", "Vulkan sampler object"};
        m["samplerShadow"] = {0, "", "Vulkan depth-comparison sampler object"};
        m["atomic_uint"] = {0, "", "atomic counter"};
        m["accelerationStructureEXT"] = {0, "", "ray tracing acceleration structure"};
        m["rayQueryEXT"] = {0, "", "ray query object"};
        return m;
    }();
    return t;
}

struct VarDef { const char* name; const char* type; const char* stages; const char* doc; };

const std::vector<VarDef>& builtinVars() {
    static const std::vector<VarDef> v = {
        {"gl_Position", "vec4", "vert tesc tese geom mesh", "Clip-space position of the vertex (written by the vertex stage)."},
        {"gl_PointSize", "float", "vert tesc tese geom", "Size of the point being rasterised, in pixels."},
        {"gl_ClipDistance", "float[]", "vert tesc tese geom frag", "Distances to user clip planes."},
        {"gl_CullDistance", "float[]", "vert tesc tese geom frag", "Distances for culling."},
        {"gl_VertexID", "int", "vert", "Index of the vertex (OpenGL)."},
        {"gl_InstanceID", "int", "vert", "Index of the instance (OpenGL)."},
        {"gl_VertexIndex", "int", "vert", "Index of the vertex including the base vertex (Vulkan)."},
        {"gl_InstanceIndex", "int", "vert", "Index of the instance including the base instance (Vulkan)."},
        {"gl_DrawID", "int", "vert", "Index of the draw in a multi-draw."},
        {"gl_BaseVertex", "int", "vert", "First vertex of the draw."},
        {"gl_BaseInstance", "int", "vert", "First instance of the draw."},
        {"gl_FragCoord", "vec4", "frag", "Window-space position of the fragment: xy in pixels, z depth, w 1/w."},
        {"gl_FrontFacing", "bool", "frag", "True when the fragment belongs to a front-facing primitive."},
        {"gl_PointCoord", "vec2", "frag", "Position inside the point being drawn, 0..1."},
        {"gl_FragDepth", "float", "frag", "Depth of the fragment (write it to replace the interpolated depth)."},
        {"gl_SampleID", "int", "frag", "Index of the sample being shaded."},
        {"gl_SamplePosition", "vec2", "frag", "Position of the sample inside the pixel."},
        {"gl_SampleMask", "int[]", "frag", "Coverage mask of the fragment."},
        {"gl_SampleMaskIn", "int[]", "frag", "Coverage mask of the primitive."},
        {"gl_Layer", "int", "vert geom frag mesh", "Layer of the framebuffer the primitive goes to."},
        {"gl_ViewportIndex", "int", "geom frag mesh", "Index of the viewport."},
        {"gl_PrimitiveID", "int", "tesc tese geom frag mesh rchit rahit rint", "Index of the primitive."},
        {"gl_HelperInvocation", "bool", "frag", "True when this invocation only exists to compute derivatives."},
        {"gl_FragColor", "vec4", "frag", "Colour output (compatibility profile)."},
        {"gl_FragData", "vec4[]", "frag", "Colour outputs (compatibility profile)."},
        {"gl_NumWorkGroups", "uvec3", "comp", "Number of work groups of the dispatch."},
        {"gl_WorkGroupSize", "uvec3", "comp", "Size of a work group (local_size_x/y/z)."},
        {"gl_WorkGroupID", "uvec3", "comp mesh task", "Index of the current work group."},
        {"gl_LocalInvocationID", "uvec3", "comp mesh task", "Index of the invocation inside its work group."},
        {"gl_GlobalInvocationID", "uvec3", "comp mesh task", "Index of the invocation in the whole dispatch."},
        {"gl_LocalInvocationIndex", "uint", "comp mesh task", "Flattened index of the invocation inside its work group."},
        {"gl_in", "gl_PerVertex[]", "tesc tese geom", "Per-vertex inputs of the primitive / patch."},
        {"gl_out", "gl_PerVertex[]", "tesc mesh", "Per-vertex outputs."},
        {"gl_InvocationID", "int", "tesc geom", "Index of the invocation."},
        {"gl_PrimitiveIDIn", "int", "geom", "Index of the incoming primitive."},
        {"gl_PatchVerticesIn", "int", "tesc tese", "Number of vertices of the input patch."},
        {"gl_TessLevelOuter", "float[4]", "tesc tese", "Outer tessellation levels."},
        {"gl_TessLevelInner", "float[2]", "tesc tese", "Inner tessellation levels."},
        {"gl_TessCoord", "vec3", "tese", "Position inside the tessellated domain."},
        {"gl_LaunchIDEXT", "uvec3", "rgen rint rahit rchit rmiss rcall", "Index of this ray generation invocation."},
        {"gl_LaunchSizeEXT", "uvec3", "rgen rint rahit rchit rmiss rcall", "Size of the ray tracing launch."},
        {"gl_WorldRayOriginEXT", "vec3", "rint rahit rchit rmiss", "Origin of the ray in world space."},
        {"gl_WorldRayDirectionEXT", "vec3", "rint rahit rchit rmiss", "Direction of the ray in world space."},
        {"gl_RayTminEXT", "float", "rint rahit rchit rmiss", "Minimum distance of the ray."},
        {"gl_RayTmaxEXT", "float", "rint rahit rchit rmiss", "Maximum distance of the ray."},
        {"gl_HitTEXT", "float", "rahit rchit", "Distance of the hit."},
        {"gl_InstanceCustomIndexEXT", "int", "rint rahit rchit", "Custom index of the instance that was hit."},
        {"gl_ObjectToWorldEXT", "mat4x3", "rint rahit rchit", "Object-to-world transform of the instance."},
        {"gl_WorldToObjectEXT", "mat4x3", "rint rahit rchit", "World-to-object transform of the instance."},
        {"gl_MeshVerticesEXT", "gl_MeshPerVertexEXT[]", "mesh", "Vertices written by the mesh shader."},
        {"gl_PrimitiveTriangleIndicesEXT", "uvec3[]", "mesh", "Triangles written by the mesh shader."},
        {"gl_SubgroupSize", "uint", "vert tesc tese geom frag comp mesh task", "Size of the subgroup."},
        {"gl_SubgroupInvocationID", "uint", "vert tesc tese geom frag comp mesh task", "Index of the invocation inside its subgroup."},
    };
    return v;
}

struct FuncDef { std::string name, label, ret, params, doc; };

// One signature per line: "ret name(params) | description". Generic types (genType = float / vecN, genIType, genUType, genBType,
// gvec4 = vec4 / ivec4 / uvec4, gsampler2D = any 2D sampler) are written as in the GLSL specification.
const char* kFuncTable = R"GLSL(
genType radians(genType degrees) | Converts degrees to radians.
genType degrees(genType radians) | Converts radians to degrees.
genType sin(genType angle) | Sine.
genType cos(genType angle) | Cosine.
genType tan(genType angle) | Tangent.
genType asin(genType x) | Arc sine, in -pi/2..pi/2.
genType acos(genType x) | Arc cosine, in 0..pi.
genType atan(genType y, genType x) | Arc tangent of y/x, using the signs to pick the quadrant.
genType atan(genType y_over_x) | Arc tangent, in -pi/2..pi/2.
genType sinh(genType x) | Hyperbolic sine.
genType cosh(genType x) | Hyperbolic cosine.
genType tanh(genType x) | Hyperbolic tangent.
genType asinh(genType x) | Inverse hyperbolic sine.
genType acosh(genType x) | Inverse hyperbolic cosine.
genType atanh(genType x) | Inverse hyperbolic tangent.
genType pow(genType x, genType y) | x raised to the power y.
genType exp(genType x) | e raised to x.
genType log(genType x) | Natural logarithm.
genType exp2(genType x) | 2 raised to x.
genType log2(genType x) | Base 2 logarithm.
genType sqrt(genType x) | Square root.
genType inversesqrt(genType x) | 1 / sqrt(x).
genType abs(genType x) | Absolute value.
genType sign(genType x) | -1, 0 or 1 by the sign of x.
genType floor(genType x) | Largest integer not greater than x.
genType trunc(genType x) | Nearest integer towards zero.
genType round(genType x) | Nearest integer (halves unspecified).
genType roundEven(genType x) | Nearest integer, halves go to the even one.
genType ceil(genType x) | Smallest integer not less than x.
genType fract(genType x) | x - floor(x).
genType mod(genType x, genType y) | x - y * floor(x / y).
genType modf(genType x, out genType i) | Splits x into integer part (in i) and fractional part (returned).
genType min(genType x, genType y) | Smaller of x and y.
genType max(genType x, genType y) | Larger of x and y.
genType clamp(genType x, genType minVal, genType maxVal) | min(max(x, minVal), maxVal).
genType mix(genType x, genType y, genType a) | Linear blend: x * (1 - a) + y * a.
genType mix(genType x, genType y, genBType a) | Selects y where a is true, otherwise x.
genType step(genType edge, genType x) | 0 where x < edge, otherwise 1.
genType smoothstep(genType edge0, genType edge1, genType x) | Smooth Hermite interpolation between 0 and 1.
genBType isnan(genType x) | True where x is not a number.
genBType isinf(genType x) | True where x is infinite.
genType fma(genType a, genType b, genType c) | a * b + c, fused.
genType frexp(genType x, out genIType exp) | Splits x into significand and exponent.
genType ldexp(genType x, genIType exp) | x * 2^exp.
genIType floatBitsToInt(genType value) | Reinterprets the bits of a float as an int.
genUType floatBitsToUint(genType value) | Reinterprets the bits of a float as a uint.
genType intBitsToFloat(genIType value) | Reinterprets the bits of an int as a float.
genType uintBitsToFloat(genUType value) | Reinterprets the bits of a uint as a float.
uint packUnorm2x16(vec2 v) | Packs two floats in 0..1 into 16-bit unsigned normalised values.
uint packSnorm2x16(vec2 v) | Packs two floats in -1..1 into 16-bit signed normalised values.
uint packUnorm4x8(vec4 v) | Packs four floats in 0..1 into 8-bit unsigned normalised values.
uint packSnorm4x8(vec4 v) | Packs four floats in -1..1 into 8-bit signed normalised values.
uint packHalf2x16(vec2 v) | Packs two floats as 16-bit halves.
vec2 unpackUnorm2x16(uint p) | Unpacks two 16-bit unsigned normalised values.
vec2 unpackSnorm2x16(uint p) | Unpacks two 16-bit signed normalised values.
vec4 unpackUnorm4x8(uint p) | Unpacks four 8-bit unsigned normalised values.
vec4 unpackSnorm4x8(uint p) | Unpacks four 8-bit signed normalised values.
vec2 unpackHalf2x16(uint v) | Unpacks two 16-bit halves.
float length(genType x) | Length of the vector.
float distance(genType p0, genType p1) | Distance between two points.
float dot(genType x, genType y) | Dot product.
vec3 cross(vec3 x, vec3 y) | Cross product.
genType normalize(genType x) | Vector of length 1 in the direction of x.
genType faceforward(genType N, genType I, genType Nref) | N, flipped if dot(Nref, I) is not negative.
genType reflect(genType I, genType N) | Reflection of I about the normal N.
genType refract(genType I, genType N, float eta) | Refraction of I through the surface with normal N and ratio of indices eta.
mat matrixCompMult(mat x, mat y) | Component-wise matrix product.
mat outerProduct(vec c, vec r) | Outer product: a matrix with c as columns scaled by r.
mat transpose(mat m) | Transposed matrix.
float determinant(mat m) | Determinant of a square matrix.
mat inverse(mat m) | Inverse of a square matrix.
bvec lessThan(vec x, vec y) | Component-wise x < y.
bvec lessThanEqual(vec x, vec y) | Component-wise x <= y.
bvec greaterThan(vec x, vec y) | Component-wise x > y.
bvec greaterThanEqual(vec x, vec y) | Component-wise x >= y.
bvec equal(vec x, vec y) | Component-wise x == y.
bvec notEqual(vec x, vec y) | Component-wise x != y.
bool any(bvec x) | True if any component is true.
bool all(bvec x) | True if all components are true.
bvec not(bvec x) | Component-wise logical complement.
genUType uaddCarry(genUType x, genUType y, out genUType carry) | Adds with carry out.
genUType usubBorrow(genUType x, genUType y, out genUType borrow) | Subtracts with borrow out.
genIType bitfieldExtract(genIType value, int offset, int bits) | Extracts a range of bits.
genIType bitfieldInsert(genIType base, genIType insert, int offset, int bits) | Replaces a range of bits.
genIType bitfieldReverse(genIType value) | Reverses the order of the bits.
genIType bitCount(genIType value) | Number of bits set to 1.
genIType findLSB(genIType value) | Index of the least significant set bit, -1 if none.
genIType findMSB(genIType value) | Index of the most significant set bit, -1 if none.
ivec textureSize(gsampler sampler, int lod) | Size of a mip level of the texture.
vec2 textureQueryLod(gsampler sampler, vec P) | Mip levels that a lookup would use.
int textureQueryLevels(gsampler sampler) | Number of mip levels.
int textureSamples(gsampler sampler) | Number of samples of a multisampled texture.
gvec4 texture(gsampler sampler, vec P) | Samples the texture at P.
gvec4 texture(gsampler sampler, vec P, float bias) | Samples the texture at P with a mip level bias.
gvec4 textureProj(gsampler sampler, vec P) | Samples with a projective divide of P.
gvec4 textureLod(gsampler sampler, vec P, float lod) | Samples a given mip level.
gvec4 textureOffset(gsampler sampler, vec P, ivec offset) | Samples with a texel offset.
gvec4 texelFetch(gsampler sampler, ivec P, int lod) | Reads one texel, no filtering.
gvec4 texelFetchOffset(gsampler sampler, ivec P, int lod, ivec offset) | Reads one texel with an offset.
gvec4 textureGrad(gsampler sampler, vec P, vec dPdx, vec dPdy) | Samples with explicit derivatives.
gvec4 textureGather(gsampler sampler, vec P) | Gathers the red component of the four texels that would be filtered.
gvec4 textureGather(gsampler sampler, vec P, int comp) | Gathers one component of the four texels that would be filtered.
gvec4 textureLodOffset(gsampler sampler, vec P, float lod, ivec offset) | Samples a mip level with an offset.
gvec4 textureProjLod(gsampler sampler, vec P, float lod) | Projective sample of a mip level.
ivec imageSize(gimage image) | Size of the image.
gvec4 imageLoad(gimage image, ivec P) | Reads a texel of the image.
void imageStore(gimage image, ivec P, gvec4 data) | Writes a texel of the image.
uint imageAtomicAdd(gimage image, ivec P, uint data) | Atomically adds to a texel.
uint imageAtomicMin(gimage image, ivec P, uint data) | Atomic minimum on a texel.
uint imageAtomicMax(gimage image, ivec P, uint data) | Atomic maximum on a texel.
uint imageAtomicAnd(gimage image, ivec P, uint data) | Atomic AND on a texel.
uint imageAtomicOr(gimage image, ivec P, uint data) | Atomic OR on a texel.
uint imageAtomicXor(gimage image, ivec P, uint data) | Atomic XOR on a texel.
uint imageAtomicExchange(gimage image, ivec P, uint data) | Atomically replaces a texel, returns the old value.
uint imageAtomicCompSwap(gimage image, ivec P, uint compare, uint data) | Atomic compare and swap on a texel.
uint atomicAdd(inout uint mem, uint data) | Atomically adds to a buffer / shared variable, returns the old value.
uint atomicMin(inout uint mem, uint data) | Atomic minimum, returns the old value.
uint atomicMax(inout uint mem, uint data) | Atomic maximum, returns the old value.
uint atomicAnd(inout uint mem, uint data) | Atomic AND, returns the old value.
uint atomicOr(inout uint mem, uint data) | Atomic OR, returns the old value.
uint atomicXor(inout uint mem, uint data) | Atomic XOR, returns the old value.
uint atomicExchange(inout uint mem, uint data) | Atomically replaces the value, returns the old one.
uint atomicCompSwap(inout uint mem, uint compare, uint data) | Atomic compare and swap, returns the old value.
genType dFdx(genType p) | Derivative of p along the screen x axis.
genType dFdy(genType p) | Derivative of p along the screen y axis.
genType dFdxFine(genType p) | Per-pixel derivative along x.
genType dFdyFine(genType p) | Per-pixel derivative along y.
genType dFdxCoarse(genType p) | Per-quad derivative along x.
genType dFdyCoarse(genType p) | Per-quad derivative along y.
genType fwidth(genType p) | abs(dFdx(p)) + abs(dFdy(p)).
genType fwidthFine(genType p) | fwidth with per-pixel derivatives.
genType fwidthCoarse(genType p) | fwidth with per-quad derivatives.
genType interpolateAtCentroid(genType interpolant) | Value of an input at the centroid of the pixel.
genType interpolateAtSample(genType interpolant, int sample) | Value of an input at a sample position.
genType interpolateAtOffset(genType interpolant, vec2 offset) | Value of an input at an offset from the pixel centre.
float noise1(genType x) | Noise value (deprecated).
vec2 noise2(genType x) | Noise value (deprecated).
vec3 noise3(genType x) | Noise value (deprecated).
vec4 noise4(genType x) | Noise value (deprecated).
void EmitVertex() | Emits the current output vertex (geometry shader).
void EndPrimitive() | Ends the current output primitive (geometry shader).
void EmitStreamVertex(int stream) | Emits the vertex to a stream (geometry shader).
void EndStreamPrimitive(int stream) | Ends the primitive on a stream (geometry shader).
void barrier() | Waits until every invocation of the work group reaches this point.
void memoryBarrier() | Orders all memory accesses of this invocation.
void memoryBarrierBuffer() | Orders buffer accesses.
void memoryBarrierShared() | Orders shared variable accesses.
void memoryBarrierImage() | Orders image accesses.
void groupMemoryBarrier() | Orders memory accesses within the work group.
void subgroupBarrier() | Barrier for all invocations of the subgroup.
bool subgroupElect() | True for exactly one active invocation of the subgroup.
bool subgroupAll(bool value) | True if value is true in every active invocation.
bool subgroupAny(bool value) | True if value is true in any active invocation.
genType subgroupBroadcast(genType value, uint id) | Value of the invocation with the given id.
genType subgroupAdd(genType value) | Sum over the subgroup.
genType subgroupMin(genType value) | Minimum over the subgroup.
genType subgroupMax(genType value) | Maximum over the subgroup.
void traceRayEXT(accelerationStructureEXT topLevel, uint rayFlags, uint cullMask, uint sbtRecordOffset, uint sbtRecordStride, uint missIndex, vec3 origin, float tMin, vec3 direction, float tMax, int payload) | Traces a ray into the scene.
bool reportIntersectionEXT(float hitT, uint hitKind) | Reports a hit found by an intersection shader.
void executeCallableEXT(uint sbtRecordIndex, int callable) | Calls a callable shader.
void SetMeshOutputsEXT(uint vertexCount, uint primitiveCount) | Sets how many vertices and primitives the mesh shader outputs.
void EmitMeshTasksEXT(uint x, uint y, uint z) | Launches mesh shader work groups from a task shader.
void debugPrintfEXT(string format, ...) | Prints from the shader (needs GL_EXT_debug_printf).
)GLSL";

const std::vector<FuncDef>& builtinFuncs() {
    static const std::vector<FuncDef> f = [] {
        std::vector<FuncDef> out;
        std::istringstream in(kFuncTable);
        std::string line;
        while (std::getline(in, line)) {
            const auto bar = line.find(" | ");
            if (bar == std::string::npos) continue;
            const std::string sig = line.substr(0, bar), doc = line.substr(bar + 3);
            const auto open = sig.find('('), sp = sig.rfind(' ', open);
            if (open == std::string::npos || sp == std::string::npos) continue;
            FuncDef d;
            d.ret = sig.substr(0, sp);
            d.name = sig.substr(sp + 1, open - sp - 1);
            d.params = sig.substr(open + 1, sig.size() - open - 2);
            d.label = d.name + "(" + d.params + ")";
            d.doc = doc;
            out.push_back(std::move(d));
        }
        return out;
    }();
    return f;
}

const std::vector<std::pair<const char*, const char*>>& directives() {
    static const std::vector<std::pair<const char*, const char*>> d = {
        {"version", "#version 450 core - the GLSL version (first line)"}, {"define", "#define NAME value"}, {"undef", "#undef NAME"},
        {"if", "#if expression"}, {"ifdef", "#ifdef NAME"}, {"ifndef", "#ifndef NAME"}, {"else", "#else"}, {"elif", "#elif expression"},
        {"endif", "#endif"}, {"error", "#error message"}, {"pragma", "#pragma ..."}, {"extension", "#extension NAME : enable"},
        {"line", "#line number"}, {"include", "#include \"file\" (needs GL_GOOGLE_include_directive)"},
    };
    return d;
}

const std::vector<std::pair<const char*, const char*>>& layoutQualifiers() {
    static const std::vector<std::pair<const char*, const char*>> q = {
        {"location", "location = N - attribute / varying / output slot"}, {"binding", "binding = N - descriptor binding"},
        {"set", "set = N - descriptor set (Vulkan)"}, {"push_constant", "push constant block (Vulkan)"},
        {"std140", "uniform block memory layout"}, {"std430", "buffer block memory layout"}, {"packed", "implementation packed layout"},
        {"row_major", "matrices stored by rows"}, {"column_major", "matrices stored by columns"}, {"offset", "offset = N - byte offset of a member"},
        {"align", "align = N - alignment of a member"}, {"component", "component = N - first component of a location"},
        {"index", "index = N - dual-source blending output"}, {"early_fragment_tests", "run depth / stencil tests before the shader"},
        {"local_size_x", "local_size_x = N - work group size"}, {"local_size_y", "local_size_y = N - work group size"},
        {"local_size_z", "local_size_z = N - work group size"}, {"constant_id", "constant_id = N - specialisation constant"},
        {"input_attachment_index", "input_attachment_index = N - subpass input"}, {"triangles", "primitive type"}, {"lines", "primitive type"},
        {"points", "primitive type"}, {"line_strip", "output primitive"}, {"triangle_strip", "output primitive"},
        {"max_vertices", "max_vertices = N - geometry shader output"}, {"vertices", "vertices = N - patch size"},
        {"rgba8", "image format"}, {"rgba16f", "image format"}, {"rgba32f", "image format"}, {"r32f", "image format"}, {"r32ui", "image format"},
        {"r32i", "image format"}, {"rg32f", "image format"}, {"readonly", "image access"}, {"writeonly", "image access"},
        {"depth_any", "depth output layout"}, {"depth_greater", "depth output layout"}, {"depth_less", "depth output layout"},
        {"origin_upper_left", "gl_FragCoord origin"}, {"pixel_center_integer", "gl_FragCoord pixel centre"},
    };
    return q;
}

bool stageMatches(const char* stages, std::string_view stage) {
    if (stage.empty()) return true;
    std::string_view s(stages);
    while (!s.empty()) {
        const auto sp = s.find(' ');
        if (s.substr(0, sp) == stage) return true;
        if (sp == std::string_view::npos) break;
        s.remove_prefix(sp + 1);
    }
    return false;
}

}  // namespace

// ====================================================================================================== tokens

std::vector<Token> tokenize(std::string_view t) {
    std::vector<Token> out;
    const std::size_t n = t.size();
    int line = 0;
    std::size_t lineStart = 0;
    bool atLineStart = true, pp = false;
    for (std::size_t i = 0; i < n;) {
        const char c = t[i];
        if (c == '\n') {
            if (pp && i > 0 && t[i - 1] != '\\' && !(i > 1 && t[i - 1] == '\r' && t[i - 2] == '\\')) pp = false;
            ++i; ++line; lineStart = i; atLineStart = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (c == '/' && i + 1 < n && t[i + 1] == '/') { while (i < n && t[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < n && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(t[i] == '*' && t[i + 1] == '/')) {
                if (t[i] == '\n') { ++line; lineStart = i + 1; }
                ++i;
            }
            i = std::min(n, i + 2);
            continue;
        }
        Token tok;
        tok.line = line;
        tok.col = static_cast<int>(i - lineStart);
        if (c == '#' && atLineStart) pp = true;
        atLineStart = false;
        tok.pp = pp;
        if (identStart(c)) {
            std::size_t j = i;
            while (j < n && identChar(t[j])) ++j;
            tok.text = std::string(t.substr(i, j - i));
            tok.ident = true;
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(t[i + 1])))) {
            std::size_t j = i;
            while (j < n && (identChar(t[j]) || t[j] == '.' || ((t[j] == '+' || t[j] == '-') && (t[j - 1] == 'e' || t[j - 1] == 'E') && !(t[i] == '0' && i + 1 < n && (t[i + 1] == 'x' || t[i + 1] == 'X'))))) ++j;
            tok.text = std::string(t.substr(i, j - i));
            tok.number = true;
            i = j;
        } else if (c == '"') {
            std::size_t j = i + 1;
            while (j < n && t[j] != '"' && t[j] != '\n') ++j;
            tok.text = std::string(t.substr(i, std::min(n, j + 1) - i));
            i = std::min(n, j + 1);
        } else {
            tok.text = std::string(1, c);
            ++i;
        }
        out.push_back(std::move(tok));
    }
    return out;
}

// ====================================================================================================== analysis

namespace {

struct Param { std::string type, name, qual; };

struct Decl {
    enum class Kind { Function, Struct, Variable, Block, Macro } kind = Kind::Variable;
    std::string name, type, qual, ret;        // qual: "uniform", "in", "const", ...
    int line = 0, col = 0, endLine = 0;
    std::vector<Param> params;                // functions
    std::vector<Param> members;               // structs and blocks: the fields
    std::vector<int> memberLines;
    bool prototype = false;
    std::string instance;                     // block: instance name ("" when the members are global)
    std::string macroBody;
};

struct Local { std::string name, type; int line = 0, col = 0, fn = -1; bool param = false; };

struct Analysis {
    std::vector<Decl> decls;
    std::vector<Local> locals;
    std::vector<Token> toks;                  // without preprocessor lines
};

bool isTypeName(const std::string& s, const std::unordered_set<std::string>& structs) {
    return builtinTypes().contains(s) || structs.contains(s);
}

// Joins tokens back into text for display ("vec3 pos[4]").
std::string typeText(const std::vector<Token>& t, std::size_t a, std::size_t b) {
    std::string s;
    for (std::size_t i = a; i < b && i < t.size(); ++i) {
        if (!s.empty() && (t[i].ident || t[i].number) && (std::isalnum(static_cast<unsigned char>(s.back())) || s.back() == '_' || s.back() == ']')) s += ' ';
        s += t[i].text;
    }
    return s;
}

Analysis analyze(std::string_view text) {
    Analysis a;
    const std::vector<Token> all = tokenize(text);
    // macros first: `#define NAME ...`
    for (std::size_t i = 0; i + 2 < all.size(); ++i) {
        if (all[i].pp && all[i].text == "#" && all[i + 1].text == "define" && all[i + 2].ident && all[i + 2].line == all[i].line) {
            Decl d;
            d.kind = Decl::Kind::Macro;
            d.name = all[i + 2].text;
            d.line = all[i + 2].line; d.col = all[i + 2].col; d.endLine = d.line;
            std::string body;
            for (std::size_t j = i + 3; j < all.size() && all[j].pp && all[j].line == all[i].line; ++j) body += (body.empty() ? "" : " ") + all[j].text;
            d.macroBody = body;
            a.decls.push_back(std::move(d));
        }
    }
    for (const auto& t : all) if (!t.pp) a.toks.push_back(t);
    const std::vector<Token>& T = a.toks;
    const std::size_t n = T.size();
    std::unordered_set<std::string> structs;

    auto matching = [&](std::size_t i, const char* open, const char* close) {   // index of the token that closes the group opened at i
        int depth = 0;
        for (std::size_t j = i; j < n; ++j) {
            if (T[j].text == open) ++depth;
            else if (T[j].text == close && --depth == 0) return j;
        }
        return n;
    };
    // fields of `{ ... }` starting at the '{' at i: "type name[...] , name2;"
    auto parseMembers = [&](std::size_t open, std::size_t close, Decl& d) {
        std::size_t j = open + 1;
        while (j < close) {
            std::size_t s = j;
            std::string qual;
            while (j < close && T[j].ident && (qualifierWords().contains(T[j].text) || T[j].text == "layout")) {
                if (T[j].text == "layout" && j + 1 < close && T[j + 1].text == "(") { j = matching(j + 1, "(", ")") + 1; continue; }
                qual += (qual.empty() ? "" : " ") + T[j].text;
                ++j;
            }
            if (j >= close || !T[j].ident) { ++j; continue; }
            const std::size_t typeStart = j;
            ++j;
            while (j < close && T[j].text == "[") j = matching(j, "[", "]") + 1;   // int[3] name
            const std::string type = typeText(T, typeStart, j);
            while (j < close) {
                if (!T[j].ident) break;
                Param m; m.type = type; m.name = T[j].text; m.qual = qual;
                d.members.push_back(m); d.memberLines.push_back(T[j].line);
                ++j;
                while (j < close && T[j].text == "[") { m.type += "[]"; d.members.back().type += "[]"; j = matching(j, "[", "]") + 1; }
                if (j < close && T[j].text == ",") { ++j; continue; }
                break;
            }
            while (j < close && T[j].text != ";") ++j;
            ++j;
            (void)s;
        }
    };

    std::size_t i = 0;
    while (i < n) {
        if (T[i].text == ";" || T[i].text == "}" || T[i].text == ")") { ++i; continue; }
        // qualifiers (including layout(...))
        std::string qual;
        std::size_t j = i;
        while (j < n && T[j].ident) {
            if (T[j].text == "layout" && j + 1 < n && T[j + 1].text == "(") { qual += (qual.empty() ? "" : " ") + std::string("layout"); j = matching(j + 1, "(", ")") + 1; continue; }
            if (qualifierWords().contains(T[j].text) || T[j].text == "precision") { qual += (qual.empty() ? "" : " ") + T[j].text; ++j; continue; }
            break;
        }
        if (j >= n) break;
        if (qual.starts_with("precision") || (j > i && T[i].text == "precision")) {   // precision highp float;
            while (j < n && T[j].text != ";") ++j;
            i = j + 1;
            continue;
        }
        if (T[j].text == "struct" && j + 1 < n && T[j + 1].ident) {
            Decl d;
            d.kind = Decl::Kind::Struct;
            d.name = T[j + 1].text; d.line = T[j + 1].line; d.col = T[j + 1].col;
            std::size_t k = j + 2;
            if (k < n && T[k].text == "{") {
                const std::size_t close = matching(k, "{", "}");
                parseMembers(k, close, d);
                d.endLine = close < n ? T[close].line : d.line;
                k = close + 1;
            }
            structs.insert(d.name);
            a.decls.push_back(d);
            while (k < n && T[k].text != ";") ++k;   // `} instance;`
            i = k + 1;
            continue;
        }
        if (T[j].ident && !keywords().contains(T[j].text)) {
            const std::size_t typeStart = j;
            std::size_t k = j + 1;
            while (k < n && T[k].text == "[") k = matching(k, "[", "]") + 1;
            const std::string type = typeText(T, typeStart, k);
            if (k < n && T[k].ident && !keywords().contains(T[k].text) && k + 1 < n && T[k + 1].text == "(") {   // function
                Decl d;
                d.kind = Decl::Kind::Function;
                d.name = T[k].text; d.ret = type; d.qual = qual; d.line = T[k].line; d.col = T[k].col;
                const std::size_t close = matching(k + 1, "(", ")");
                // parameters
                std::size_t p = k + 2;
                while (p < close) {
                    std::string pq;
                    while (p < close && T[p].ident && (T[p].text == "in" || T[p].text == "out" || T[p].text == "inout" || T[p].text == "const" || T[p].text == "highp" || T[p].text == "mediump" || T[p].text == "lowp" || T[p].text == "restrict" || T[p].text == "readonly" || T[p].text == "writeonly" || T[p].text == "coherent" || T[p].text == "volatile")) { pq += (pq.empty() ? "" : " ") + T[p].text; ++p; }
                    if (p >= close || !T[p].ident) { ++p; continue; }
                    const std::size_t ts = p;
                    ++p;
                    while (p < close && T[p].text == "[") p = matching(p, "[", "]") + 1;
                    Param pr; pr.qual = pq; pr.type = typeText(T, ts, p);
                    if (p < close && T[p].ident) { pr.name = T[p].text; ++p; }
                    while (p < close && T[p].text == "[") { pr.type += "[]"; p = matching(p, "[", "]") + 1; }
                    d.params.push_back(pr);
                    while (p < close && T[p].text != ",") ++p;
                    ++p;
                }
                std::size_t after = close + 1;
                if (after < n && T[after].text == "{") {
                    const std::size_t end = matching(after, "{", "}");
                    d.endLine = end < n ? T[end].line : T[n - 1].line;
                    const int fnIndex = static_cast<int>(a.decls.size());
                    for (const auto& pr : d.params)
                        if (!pr.name.empty()) a.locals.push_back({pr.name, pr.type, d.line, d.col, fnIndex, true});
                    // locals: `type name` followed by = ; , [ or ) inside the body
                    for (std::size_t q = after + 1; q < end && q + 1 < n; ++q) {
                        if (!T[q].ident || !isTypeName(T[q].text, structs)) continue;
                        std::size_t r = q + 1;
                        while (r < end && T[r].text == "[") r = matching(r, "[", "]") + 1;
                        if (r < end && T[r].ident && !keywords().contains(T[r].text) && r + 1 <= end) {
                            const std::string nx = r + 1 < n ? T[r + 1].text : "";
                            if (nx == "=" || nx == ";" || nx == "," || nx == "[" || nx == ")") {
                                a.locals.push_back({T[r].text, typeText(T, q, r), T[r].line, T[r].col, fnIndex, false});
                                // `vec3 a, b;` and `int i = 0, j = 1;`
                                std::size_t u = r + 1;
                                int depth = 0;
                                while (u < end && T[u].text != ";") {
                                    if (T[u].text == "(" || T[u].text == "[" || T[u].text == "{") ++depth;
                                    else if (T[u].text == ")" || T[u].text == "]" || T[u].text == "}") { if (depth == 0) break; --depth; }
                                    else if (depth == 0 && T[u].text == "," && u + 1 < end && T[u + 1].ident && u + 2 <= end &&
                                             (T[u + 2].text == "=" || T[u + 2].text == "," || T[u + 2].text == ";" || T[u + 2].text == "["))
                                        a.locals.push_back({T[u + 1].text, typeText(T, q, r), T[u + 1].line, T[u + 1].col, fnIndex, false});
                                    ++u;
                                }
                            }
                        }
                    }
                    after = end + 1;
                } else {
                    d.prototype = true;
                    d.endLine = d.line;
                    while (after < n && T[after].text != ";") ++after;
                    ++after;
                }
                a.decls.push_back(std::move(d));
                i = after;
                continue;
            }
            if (k < n && T[k].text == "{" && (qual.find("uniform") != std::string::npos || qual.find("buffer") != std::string::npos || qual.find("in") != std::string::npos ||
                                               qual.find("out") != std::string::npos || qual.find("layout") != std::string::npos)) {   // interface block
                Decl d;
                d.kind = Decl::Kind::Block;
                d.name = T[typeStart].text; d.qual = qual; d.line = T[typeStart].line; d.col = T[typeStart].col;
                const std::size_t close = matching(k, "{", "}");
                parseMembers(k, close, d);
                d.endLine = close < n ? T[close].line : d.line;
                std::size_t e = close + 1;
                if (e < n && T[e].ident) { d.instance = T[e].text; ++e; while (e < n && T[e].text == "[") e = matching(e, "[", "]") + 1; }
                a.decls.push_back(std::move(d));
                while (e < n && T[e].text != ";") ++e;
                i = e + 1;
                continue;
            }
            if (k < n && T[k].ident && !keywords().contains(T[k].text)) {   // global variable(s)
                std::size_t e = k;
                while (e < n) {
                    if (!T[e].ident) break;
                    Decl d;
                    d.kind = Decl::Kind::Variable;
                    d.name = T[e].text; d.qual = qual; d.line = T[e].line; d.col = T[e].col; d.endLine = d.line;
                    d.type = type;
                    ++e;
                    while (e < n && T[e].text == "[") { d.type += "[]"; e = matching(e, "[", "]") + 1; }
                    a.decls.push_back(std::move(d));
                    int depth = 0;
                    while (e < n && !(depth == 0 && (T[e].text == "," || T[e].text == ";"))) {
                        if (T[e].text == "(" || T[e].text == "{" || T[e].text == "[") ++depth;
                        else if (T[e].text == ")" || T[e].text == "}" || T[e].text == "]") --depth;
                        ++e;
                    }
                    if (e < n && T[e].text == ",") { ++e; continue; }
                    break;
                }
                while (e < n && T[e].text != ";") ++e;
                i = e + 1;
                continue;
            }
        }
        i = std::max(j, i) + (j == i ? 1 : 0);
    }
    return a;
}

std::string lineAt(std::string_view text, int line) {
    std::size_t pos = 0;
    for (int l = 0; l < line; ++l) {
        pos = text.find('\n', pos);
        if (pos == std::string_view::npos) return {};
        ++pos;
    }
    const auto e = text.find('\n', pos);
    std::string s(text.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos));
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

std::size_t offsetOf(std::string_view text, int line, int col) {
    std::size_t pos = 0;
    for (int l = 0; l < line; ++l) {
        pos = text.find('\n', pos);
        if (pos == std::string_view::npos) return text.size();
        ++pos;
    }
    return std::min(text.size(), pos + static_cast<std::size_t>(std::max(0, col)));
}

// The function whose body contains `line` (index into decls), or -1.
int functionAt(const Analysis& a, int line) {
    for (int i = 0; i < static_cast<int>(a.decls.size()); ++i)
        if (a.decls[static_cast<std::size_t>(i)].kind == Decl::Kind::Function && !a.decls[static_cast<std::size_t>(i)].prototype &&
            a.decls[static_cast<std::size_t>(i)].line <= line && line <= a.decls[static_cast<std::size_t>(i)].endLine) return i;
    return -1;
}

const Local* findLocal(const Analysis& a, const std::string& name, int line) {
    const int fn = functionAt(a, line);
    const Local* best = nullptr;
    for (const auto& l : a.locals)
        if (l.fn == fn && fn >= 0 && l.name == name && (l.param || l.line <= line)) best = &l;
    return best;
}

const Decl* findDecl(const Analysis& a, const std::string& name) {
    for (const auto& d : a.decls)
        if (d.name == name) return &d;
    for (const auto& d : a.decls)   // members of an instance-less block are globals
        if (d.kind == Decl::Kind::Block && d.instance.empty())
            for (const auto& m : d.members) if (m.name == name) return &d;
    return nullptr;
}

std::string stripArray(std::string t) {
    const auto b = t.find('[');
    return b == std::string::npos ? t : t.substr(0, b);
}

// The type of `name` in the scope of `line`: a local or parameter, a global variable, or a block instance / member.
std::string typeOfName(const Analysis& a, const std::string& name, int line) {
    if (const Local* l = findLocal(a, name, line)) return l->type;
    for (const auto& d : a.decls) {
        if (d.name == name && d.kind == Decl::Kind::Variable) return d.type;
        if (d.kind == Decl::Kind::Block && d.instance == name) return d.name;
        if (d.kind == Decl::Kind::Block && d.instance.empty())
            for (const auto& m : d.members) if (m.name == name) return m.type;
    }
    if (name.starts_with("gl_"))
        for (const auto& v : builtinVars()) if (name == v.name) return v.type;
    return {};
}

const Decl* structOrBlock(const Analysis& a, const std::string& type) {
    const std::string t = stripArray(type);
    for (const auto& d : a.decls)
        if ((d.kind == Decl::Kind::Struct || d.kind == Decl::Kind::Block) && d.name == t) return &d;
    return nullptr;
}

// Resolves `a.b[2].c` (the identifiers before the dot, outermost first) to a type name; empty when unknown.
std::string resolveChain(const Analysis& a, const std::vector<std::string>& names, int line) {
    if (names.empty()) return {};
    std::string type = typeOfName(a, names[0], line);
    for (std::size_t i = 1; i < names.size() && !type.empty(); ++i) {
        const Decl* sd = structOrBlock(a, type);
        std::string next;
        if (sd) for (const auto& m : sd->members) if (m.name == names[i]) next = m.type;
        type = next;
    }
    return type;
}

// Walks back from the '.' at `dot` collecting identifiers ("a.b[i].c" -> a, b, c).
std::vector<std::string> chainBefore(const std::string& ln, std::size_t dot) {
    std::vector<std::string> rev;
    std::size_t p = dot;
    while (true) {
        std::size_t q = p;
        while (q > 0 && (ln[q - 1] == ']' || ln[q - 1] == ')')) {   // skip [..] / (..) groups
            const char close = ln[q - 1], open = close == ']' ? '[' : '(';
            int depth = 0;
            std::size_t r = q;
            while (r > 0) {
                --r;
                if (ln[r] == close) ++depth;
                else if (ln[r] == open && --depth == 0) break;
            }
            q = r;
        }
        std::size_t e = q;
        while (q > 0 && identChar(ln[q - 1])) --q;
        if (q == e) return {};
        rev.push_back(ln.substr(q, e - q));
        if (q > 0 && ln[q - 1] == '.') { p = q - 1; continue; }
        break;
    }
    std::ranges::reverse(rev);
    return rev;
}

void addSwizzles(std::vector<LspCompletion>& out, int n, const std::string& element, const std::string& type) {
    static const char* sets[3] = {"xyzw", "rgba", "stpq"};
    for (const char* set : sets) {
        std::vector<std::string> names;
        std::string sub(set, static_cast<std::size_t>(n));
        for (char c : sub) names.emplace_back(1, c);
        // the full-width and half-width groups you actually write: xy, xyz, xyzw, ...
        for (int len = 2; len <= n; ++len) names.push_back(sub.substr(0, static_cast<std::size_t>(len)));
        for (const auto& nm : names) {
            LspCompletion c;
            c.label = nm; c.insertText = nm; c.filterText = nm; c.kind = 5;
            const std::size_t len = nm.size();
            const std::string pre = element == "int" ? "i" : element == "uint" ? "u" : element == "bool" ? "b" : element == "double" ? "d" : "";
            c.detail = len == 1 ? element : pre + "vec" + std::to_string(len);
            c.sortText = std::string("0") + nm;
            out.push_back(std::move(c));
        }
    }
    (void)type;
}

LspCompletion item(const std::string& label, int kind, const std::string& detail, const std::string& insert, const std::string& sort) {
    LspCompletion c;
    c.label = label; c.insertText = insert.empty() ? label : insert; c.filterText = insert.empty() ? label : insert; c.kind = kind; c.detail = detail; c.sortText = sort + c.filterText;
    return c;
}

std::string paramText(const std::vector<Param>& ps) {
    std::string s;
    for (const auto& p : ps) {
        if (!s.empty()) s += ", ";
        if (!p.qual.empty()) s += p.qual + " ";
        s += p.type;
        if (!p.name.empty()) s += " " + p.name;
    }
    return s;
}

}  // namespace

// ====================================================================================================== completion

std::vector<LspCompletion> complete(std::string_view text, int line, int col, std::string_view stage) {
    std::vector<LspCompletion> out;
    const std::string ln = lineAt(text, line);
    const std::size_t c = std::min<std::size_t>(static_cast<std::size_t>(col), ln.size());
    std::size_t ps = c;
    while (ps > 0 && identChar(ln[ps - 1])) --ps;
    // inside a comment or a string: nothing
    const std::size_t sl = ln.find("//");
    if (sl != std::string::npos && sl < ps) return out;

    // preprocessor directives
    {
        std::size_t k = 0;
        while (k < ps && (ln[k] == ' ' || ln[k] == '\t')) ++k;
        if (k < ln.size() && ln[k] == '#' && ps > k && ln.substr(k + 1, ps - k - 1).find_first_not_of(" \t") == std::string::npos) {
            for (const auto& [name, doc] : directives()) out.push_back(item(name, 14, doc, name, "0"));
            return out;
        }
        if (k < ln.size() && ln[k] == '#') return out;   // arguments of a directive
    }
    const Analysis a = analyze(text);

    // member access: swizzles, struct fields, block members
    if (ps > 0 && ln[ps - 1] == '.') {
        const auto chain = chainBefore(ln, ps - 1);
        const std::string type = resolveChain(a, chain, line);
        const std::string base = stripArray(type);
        auto bt = builtinTypes().find(base);
        if (bt != builtinTypes().end() && bt->second.comps >= 2 && type == base) addSwizzles(out, bt->second.comps, bt->second.element, base);
        if (const Decl* sd = structOrBlock(a, type); sd && type == base)
            for (std::size_t m = 0; m < sd->members.size(); ++m) out.push_back(item(sd->members[m].name, 5, sd->members[m].type, "", "0"));
        if (!type.empty() && (type.find('[') != std::string::npos || bt != builtinTypes().end())) out.push_back(item("length()", 2, "int", "length", "1"));
        return out;
    }

    // layout(...)
    {
        const std::string head = ln.substr(0, ps);
        const auto lp = head.rfind("layout");
        if (lp != std::string::npos) {
            const auto open = head.find('(', lp);
            if (open != std::string::npos && head.find(')', open) == std::string::npos) {
                for (const auto& [name, doc] : layoutQualifiers()) out.push_back(item(name, 14, doc, name, "0"));
                return out;
            }
        }
    }

    // keywords, types, built-ins
    for (const auto& k : keywords()) out.push_back(item(k, 14, "", "", "5"));
    for (const auto& [name, info] : builtinTypes()) out.push_back(item(name, 25, info.doc, "", "4"));
    for (const auto& f : builtinFuncs()) {
        bool dup = false;
        for (const auto& o : out) if (o.filterText == f.name && o.kind == 3) { dup = true; break; }
        if (!dup) out.push_back(item(f.label, 3, f.ret, f.name, "3"));
    }
    for (const auto& v : builtinVars()) if (stageMatches(v.stages, stage)) out.push_back(item(v.name, 6, v.type, "", "3"));
    for (const char* extra : {"gl_MaxDrawBuffers", "gl_MaxVertexAttribs", "gl_MaxTextureImageUnits"}) out.push_back(item(extra, 21, "const int", "", "6"));

    // the names declared in this file
    const int fn = functionAt(a, line);
    for (const auto& d : a.decls) {
        switch (d.kind) {
            case Decl::Kind::Function: {
                if (d.prototype && std::ranges::any_of(a.decls, [&](const Decl& o) { return &o != &d && o.kind == Decl::Kind::Function && o.name == d.name && !o.prototype; })) break;
                out.push_back(item(d.name + "(" + paramText(d.params) + ")", 3, d.ret, d.name, "1"));
                break;
            }
            case Decl::Kind::Struct: out.push_back(item(d.name, 22, "struct", "", "1")); break;
            case Decl::Kind::Macro: out.push_back(item(d.name, 21, "macro " + d.macroBody, "", "1")); break;
            case Decl::Kind::Variable: out.push_back(item(d.name, d.qual.find("const") != std::string::npos ? 21 : 6, (d.qual.empty() ? "" : d.qual + " ") + d.type, "", "1")); break;
            case Decl::Kind::Block:
                if (!d.instance.empty()) out.push_back(item(d.instance, 6, d.qual + " " + d.name, "", "1"));
                else for (const auto& m : d.members) out.push_back(item(m.name, 6, m.type, "", "1"));
                out.push_back(item(d.name, 22, "block", "", "2"));
                break;
        }
    }
    for (const auto& l : a.locals)
        if (l.fn == fn && fn >= 0 && (l.param || l.line <= line) && !(l.line == line && static_cast<std::size_t>(l.col) >= ps && static_cast<std::size_t>(l.col) <= c)) {
            bool dup = false;
            for (auto& o : out) if (o.filterText == l.name && (o.kind == 6) && o.sortText[0] == '0') { dup = true; break; }
            if (!dup) out.push_back(item(l.name, 6, l.type, "", "0"));
        }
    return out;
}

// ====================================================================================================== signature help

namespace {

std::vector<std::pair<int, int>> paramRanges(const std::string& label) {
    std::vector<std::pair<int, int>> r;
    const auto open = label.find('(');
    if (open == std::string::npos) return r;
    int depth = 0;
    std::size_t start = open + 1;
    for (std::size_t i = open + 1; i < label.size(); ++i) {
        const char ch = label[i];
        if (ch == '(' || ch == '[') ++depth;
        else if (ch == ')' || ch == ']') {
            if (depth == 0) { if (i > start) r.emplace_back(static_cast<int>(start), static_cast<int>(i)); break; }
            --depth;
        } else if (ch == ',' && depth == 0) {
            r.emplace_back(static_cast<int>(start), static_cast<int>(i));
            start = i + 1;
            while (start < label.size() && label[start] == ' ') ++start;
        }
    }
    return r;
}

}  // namespace

std::optional<LspSignatureHelp> signature(std::string_view text, int line, int col) {
    const std::size_t at = offsetOf(text, line, col);
    int depth = 0, commas = 0;
    std::size_t open = std::string_view::npos;
    const std::size_t limit = at > 3000 ? at - 3000 : 0;
    for (std::size_t i = at; i > limit; --i) {
        const char ch = text[i - 1];
        if (ch == ')' || ch == ']' || ch == '}') ++depth;
        else if (ch == '[' || ch == '{') { if (depth == 0) return std::nullopt; --depth; }
        else if (ch == '(') { if (depth == 0) { open = i - 1; break; } --depth; }
        else if (ch == ',' && depth == 0) ++commas;
        else if (ch == ';' && depth == 0) return std::nullopt;
    }
    if (open == std::string_view::npos) return std::nullopt;
    std::size_t e = open;
    while (e > 0 && (text[e - 1] == ' ' || text[e - 1] == '\t')) --e;
    std::size_t b = e;
    while (b > 0 && identChar(text[b - 1])) --b;
    if (b == e) return std::nullopt;
    const std::string name(text.substr(b, e - b));
    if (keywords().contains(name) || name == "layout") return std::nullopt;

    LspSignatureHelp help;
    auto add = [&](const std::string& label, const std::string& doc) {
        LspSignature sg;
        sg.label = label;
        sg.doc = doc;
        sg.params = paramRanges(label);
        sg.paramDocs.assign(sg.params.size(), "");
        sg.active = commas;
        help.sigs.push_back(std::move(sg));
    };
    const Analysis a = analyze(text);
    for (const auto& d : a.decls)
        if (d.kind == Decl::Kind::Function && d.name == name) add(d.ret + " " + d.name + "(" + paramText(d.params) + ")", "");
    for (const auto& d : a.decls)
        if (d.kind == Decl::Kind::Struct && d.name == name) add(d.name + "(" + paramText(d.members) + ")", "struct constructor");
    for (const auto& f : builtinFuncs())
        if (f.name == name) add(f.ret + " " + f.name + "(" + f.params + ")", f.doc);
    if (help.sigs.empty()) {
        auto bt = builtinTypes().find(name);
        if (bt != builtinTypes().end() && bt->second.comps >= 2) {   // vecN(...) constructors
            static const char* names[4] = {"x", "y", "z", "w"};
            const std::string el = bt->second.element;
            std::string full;
            for (int i = 0; i < bt->second.comps; ++i) full += (i ? ", " : "") + el + " " + names[i];
            add(name + "(" + full + ")", "one value per component");
            add(name + "(" + el + " v)", "the same value in every component");
            add(name + "(vec v, ...)", "components taken from other vectors");
        } else if (bt != builtinTypes().end() && bt->second.comps == 0 && !bt->second.element.empty()) {
            add(name + "(" + bt->second.element + " v)", "conversion");
        }
    }
    if (help.sigs.empty()) return std::nullopt;
    help.activeSig = 0;
    for (int i = 0; i < static_cast<int>(help.sigs.size()); ++i)   // the first overload that has a parameter at this position
        if (static_cast<int>(help.sigs[static_cast<std::size_t>(i)].params.size()) > commas) { help.activeSig = i; break; }
    return help;
}

// ====================================================================================================== hover, definition, outline

namespace {

struct Word { std::string text; int begin = 0, end = 0; };

std::optional<Word> wordAt(const std::string& ln, int col) {
    std::size_t c = std::min<std::size_t>(static_cast<std::size_t>(std::max(0, col)), ln.size());
    if (c < ln.size() && !identChar(ln[c]) && c > 0 && identChar(ln[c - 1])) --c;
    if (c >= ln.size() || !identChar(ln[c])) return std::nullopt;
    std::size_t b = c, e = c;
    while (b > 0 && identChar(ln[b - 1])) --b;
    while (e < ln.size() && identChar(ln[e])) ++e;
    return Word{ln.substr(b, e - b), static_cast<int>(b), static_cast<int>(e)};
}

}  // namespace

std::string hover(std::string_view text, int line, int col, std::string_view stage) {
    const std::string ln = lineAt(text, line);
    auto w = wordAt(ln, col);
    if (!w) return {};
    const Analysis a = analyze(text);
    const std::string& name = w->text;
    // member of something: `thing.name`
    if (w->begin > 0 && ln[static_cast<std::size_t>(w->begin) - 1] == '.') {
        const auto chain = chainBefore(ln, static_cast<std::size_t>(w->begin) - 1);
        const std::string type = resolveChain(a, chain, line);
        if (const Decl* sd = structOrBlock(a, type))
            for (const auto& m : sd->members) if (m.name == name) return m.type + " " + m.name + "\nmember of " + sd->name;
        auto bt = builtinTypes().find(stripArray(type));
        if (bt != builtinTypes().end() && bt->second.comps >= 2 && name.find_first_not_of("xyzwrgbastpq") == std::string::npos)
            return (name.size() == 1 ? bt->second.element : std::format("{}-component vector", name.size())) + " swizzle of " + type;
        return {};
    }
    if (const Local* l = findLocal(a, name, line)) return l->type + " " + l->name + (l->param ? "\nparameter" : "\nlocal variable");
    if (const Decl* d = findDecl(a, name)) {
        switch (d->kind) {
            case Decl::Kind::Function: return d->ret + " " + d->name + "(" + paramText(d->params) + ")";
            case Decl::Kind::Struct: {
                std::string s = "struct " + d->name + " {";
                for (const auto& m : d->members) s += "\n    " + m.type + " " + m.name + ";";
                return s + "\n}";
            }
            case Decl::Kind::Macro: return "#define " + d->name + (d->macroBody.empty() ? "" : " " + d->macroBody);
            case Decl::Kind::Block: {
                std::string s = d->qual + " " + d->name + " {";
                for (const auto& m : d->members) s += "\n    " + m.type + " " + m.name + ";";
                return s + "\n}" + (d->instance.empty() ? "" : " " + d->instance);
            }
            case Decl::Kind::Variable: return (d->qual.empty() ? "" : d->qual + " ") + d->type + " " + d->name;
        }
    }
    std::string out;
    for (const auto& f : builtinFuncs())
        if (f.name == name) { if (!out.empty()) out += "\n"; out += f.ret + " " + f.name + "(" + f.params + ")"; if (out.find(f.doc) == std::string::npos) out += "\n" + f.doc; }
    if (!out.empty()) return out;
    for (const auto& v : builtinVars())
        if (name == v.name) return std::string(v.type) + " " + v.name + (stageMatches(v.stages, stage) ? "" : "   (not available in this shader stage)") + "\n" + v.doc;
    if (auto bt = builtinTypes().find(name); bt != builtinTypes().end()) return name + "\n" + bt->second.doc;
    return {};
}

std::optional<std::pair<int, int>> definition(std::string_view text, int line, int col) {
    const std::string ln = lineAt(text, line);
    auto w = wordAt(ln, col);
    if (!w) return std::nullopt;
    const Analysis a = analyze(text);
    if (w->begin > 0 && ln[static_cast<std::size_t>(w->begin) - 1] == '.') {
        const auto chain = chainBefore(ln, static_cast<std::size_t>(w->begin) - 1);
        if (const Decl* sd = structOrBlock(a, resolveChain(a, chain, line)))
            for (std::size_t m = 0; m < sd->members.size(); ++m)
                if (sd->members[m].name == w->text) {
                    const std::string ml = lineAt(text, sd->memberLines[m]);
                    const auto at = ml.find(w->text);
                    return std::make_pair(sd->memberLines[m], static_cast<int>(at == std::string::npos ? 0 : at));
                }
        return std::nullopt;
    }
    if (const Local* l = findLocal(a, w->text, line)) return std::make_pair(l->line, l->col);
    const Decl* best = nullptr;
    for (const auto& d : a.decls)   // prefer the definition over a prototype
        if (d.name == w->text && (!best || (best->prototype && !d.prototype))) best = &d;
    if (!best) best = findDecl(a, w->text);
    if (best) return std::make_pair(best->line, best->col);
    return std::nullopt;
}

std::vector<LspSymbol> symbols(std::string_view text) {
    const Analysis a = analyze(text);
    std::vector<LspSymbol> out;
    for (const auto& d : a.decls) {
        LspSymbol s;
        s.name = d.name; s.line = d.line; s.col = d.col; s.endLine = d.endLine;
        switch (d.kind) {
            case Decl::Kind::Function:
                if (d.prototype && std::ranges::any_of(a.decls, [&](const Decl& o) { return o.kind == Decl::Kind::Function && o.name == d.name && !o.prototype; })) continue;
                s.kind = 12; s.detail = "(" + paramText(d.params) + ") -> " + d.ret;
                break;
            case Decl::Kind::Struct: s.kind = 23; s.detail = "struct"; break;
            case Decl::Kind::Block: s.kind = 23; s.detail = d.qual + " block"; s.name = d.instance.empty() ? d.name : d.name + " " + d.instance; break;
            case Decl::Kind::Macro: s.kind = 14; s.detail = "#define " + d.macroBody; break;
            case Decl::Kind::Variable: s.kind = d.qual.find("const") != std::string::npos ? 14 : 13; s.detail = (d.qual.empty() ? "" : d.qual + " ") + d.type; break;
        }
        if (d.kind == Decl::Kind::Struct || d.kind == Decl::Kind::Block)
            for (std::size_t m = 0; m < d.members.size(); ++m) {
                LspSymbol f;
                f.name = d.members[m].name; f.detail = d.members[m].type; f.kind = 8; f.line = d.memberLines[m]; f.endLine = f.line;
                const std::string ml = lineAt(text, f.line);
                const auto at = ml.find(f.name);
                f.col = at == std::string::npos ? 0 : static_cast<int>(at);
                s.kids.push_back(std::move(f));
            }
        out.push_back(std::move(s));
    }
    return out;
}

// ====================================================================================================== error checking

std::string compiler() {
    if (auto p = findExecutable("glslc")) return *p;
    if (auto p = findExecutable("glslangValidator")) return *p;
    return {};
}

namespace {

int glslVersion(std::string_view text) {
    const auto at = text.find("#version");
    if (at == std::string_view::npos) return 110;
    int v = 0;
    std::size_t i = at + 8;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) v = v * 10 + (text[i++] - '0');
    return v;
}

}  // namespace

std::vector<LspDiag> diagnose(std::string_view text, std::string_view stage, Target target, const std::string& includeDir, std::string& error) {
    std::vector<LspDiag> out;
    error.clear();
    const std::string exe = compiler();
    if (exe.empty()) { error = "no GLSL compiler (glslc or glslangValidator) installed"; return out; }
    if (stage.empty()) return out;
    const bool isGlslc = fs::path(exe).filename() == "glslc";

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / std::format("daemon-glsl-{}", static_cast<long>(::getpid()));
    fs::create_directories(dir, ec);
    const std::string base = "shader." + std::string(stage);
    const fs::path file = dir / base;
    if (!writeFile(file, text)) { error = "cannot write a temporary file"; return out; }

    bool vulkan = target == Target::Vulkan;
    if (target == Target::Auto) {
        const std::string t(text);
        vulkan = glslVersion(text) >= 450 && (t.find("push_constant") != std::string::npos || t.find("set =") != std::string::npos || t.find("set=") != std::string::npos ||
                                               t.find("gl_VertexIndex") != std::string::npos || t.find("gl_InstanceIndex") != std::string::npos ||
                                               t.find("subpassInput") != std::string::npos || t.find("EXT") != std::string::npos);
    }
    ProcessOptions o;
    o.cwd = dir.string();
    o.argv = {exe};
    if (isGlslc) {
        o.argv.push_back(vulkan ? "--target-env=vulkan1.3" : "--target-env=opengl");
        if (!includeDir.empty()) { o.argv.push_back("-I"); o.argv.push_back(includeDir); }
        o.argv.push_back("-o"); o.argv.push_back((dir / "out.spv").string());
        o.argv.push_back(base);
    } else {
        if (vulkan) { o.argv.push_back("-V"); o.argv.push_back("--target-env"); o.argv.push_back("vulkan1.3"); } else o.argv.push_back("-G");
        if (!includeDir.empty()) o.argv.push_back("-I" + includeDir);
        o.argv.push_back("-o"); o.argv.push_back((dir / "out.spv").string());
        o.argv.push_back(base);
    }
    const auto r = Process::runCapture(o, 15000);
    if (!r.error.empty()) { error = r.error; return out; }
    if (r.timedOut) { error = "the shader compiler timed out"; return out; }

    std::vector<std::string> lines;
    {
        std::istringstream in(r.output);
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
    }
    std::vector<std::string> srcLines;
    {
        std::istringstream in{std::string(text)};
        std::string l;
        while (std::getline(in, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); srcLines.push_back(l); }
    }
    auto place = [&](int lineNo, int severity, std::string msg) {
        LspDiag d;
        d.severity = severity;
        d.source = isGlslc ? "glslc" : "glslang";
        while (!msg.empty() && (msg.back() == ' ' || msg.back() == '\n')) msg.pop_back();
        d.message = msg;
        d.line = std::max(0, lineNo - 1);
        const std::string src = d.line < static_cast<int>(srcLines.size()) ? srcLines[static_cast<std::size_t>(d.line)] : std::string();
        std::size_t a = src.find_first_not_of(" \t"), b = src.size();
        if (a == std::string::npos) { a = 0; b = std::max<std::size_t>(1, src.size()); }
        const auto q1 = msg.find('\''), q2 = q1 == std::string::npos ? q1 : msg.find('\'', q1 + 1);
        if (q2 != std::string::npos && q2 > q1 + 1) {   // 'foo' : undeclared identifier -> underline foo
            const std::string tok = msg.substr(q1 + 1, q2 - q1 - 1);
            if (const auto at = src.find(tok); at != std::string::npos && !tok.empty()) { a = at; b = at + tok.size(); }
        }
        d.col = static_cast<int>(a);
        d.endLine = d.line;
        d.endCol = static_cast<int>(std::max(b, a + 1));
        out.push_back(std::move(d));
    };
    for (const auto& l : lines) {
        if (isGlslc) {   // "shader.frag:5: error: 'foo' : undeclared identifier"
            if (!l.starts_with(base + ":")) continue;
            std::size_t p = base.size() + 1;
            int num = 0;
            auto [ptr, ec2] = std::from_chars(l.data() + p, l.data() + l.size(), num);
            if (ec2 != std::errc()) continue;
            p = static_cast<std::size_t>(ptr - l.data());
            if (p >= l.size() || l[p] != ':') continue;
            std::string rest = l.substr(p + 1);
            while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            int sev = 1;
            if (rest.starts_with("error:")) rest.erase(0, 6);
            else if (rest.starts_with("warning:")) { rest.erase(0, 8); sev = 2; }
            else continue;
            while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            place(num, sev, rest);
        } else {         // "ERROR: shader.frag:5: 'foo' : undeclared identifier" or "ERROR: 0:5: ..."
            int sev = l.starts_with("ERROR:") ? 1 : l.starts_with("WARNING:") ? 2 : 0;
            if (!sev) continue;
            std::string rest = l.substr(l.find(':') + 1);
            while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            if (rest.starts_with(base + ":")) rest.erase(0, base.size() + 1);
            else { const auto colon = rest.find(':'); if (colon == std::string::npos || rest.substr(0, colon).find_first_not_of("0123456789") != std::string::npos) continue; rest.erase(0, colon + 1); }
            int num = 0;
            auto [ptr, ec2] = std::from_chars(rest.data(), rest.data() + rest.size(), num);
            if (ec2 != std::errc() || ptr == rest.data()) continue;
            std::string msg = rest.substr(static_cast<std::size_t>(ptr - rest.data()));
            if (!msg.empty() && msg.front() == ':') msg.erase(0, 1);
            while (!msg.empty() && msg.front() == ' ') msg.erase(0, 1);
            if (msg.starts_with("'' : compilation terminated")) continue;
            place(num, sev, msg);
        }
    }
    fs::remove_all(dir, ec);
    return out;
}

}  // namespace ide::glsl
