#ifndef XMATERIAL_STEP_NAMES_H
#define XMATERIAL_STEP_NAMES_H
#pragma once

// GLSL identifiers of steps (stack_canvas.md 10.3), shared by the compiler (codegen) and the editor (maps compile errors back to nodes).
#include "xmaterial_graph_node.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace xmaterial_graph
{
    // Case-sensitive, as GLSL is. GLSL keywords, types and builtin names, plus the identifiers the compiler and the node prefabs declare.
    inline bool IsReservedGlslName(std::string_view Name) noexcept
    {
        static constexpr std::array Reserved = std::to_array<std::string_view>(
        { "active", "asm", "atomic_uint", "attribute", "bool", "break", "buffer", "bvec2", "bvec3", "bvec4", "case", "cast", "centroid", "class", "coherent"
        , "common", "const", "continue", "default", "discard", "dmat2", "dmat3", "dmat4", "do", "double", "dvec2", "dvec3", "dvec4", "else", "enum", "extern"
        , "external", "false", "filter", "fixed", "flat", "float", "for", "fvec2", "fvec3", "fvec4", "goto", "half", "highp", "hvec2", "hvec3", "hvec4", "if"
        , "iimage2D", "image2D", "in", "inline", "inout", "input", "int", "interface", "invariant", "isampler2D", "ivec2", "ivec3", "ivec4", "layout"
        , "long", "lowp", "main", "mat2", "mat3", "mat4", "mediump", "namespace", "noinline", "noperspective", "out", "output", "packed", "partition"
        , "patch", "precise", "precision", "public", "readonly", "resource", "restrict", "return", "sample", "sampler", "sampler2D", "samplerCube"
        , "shared", "short", "sizeof", "smooth", "static", "struct", "subroutine", "superp", "switch", "template", "this", "true", "typedef", "uint"
        , "uniform", "union", "unsigned", "usampler2D", "using", "uvec2", "uvec3", "uvec4", "varying", "vec2", "vec3", "vec4", "void", "volatile", "while"
        , "writeonly"
        // builtin functions
        , "abs", "acos", "all", "any", "asin", "atan", "ceil", "clamp", "cos", "cross", "degrees", "determinant", "distance", "dot", "dFdx", "dFdy", "exp"
        , "exp2", "faceforward", "floor", "fract", "fwidth", "inverse", "inversesqrt", "length", "log", "log2", "matrixCompMult", "max", "min", "mix", "mod"
        , "normalize", "not", "outerProduct", "pow", "radians", "reflect", "refract", "round", "sign", "sin", "smoothstep", "sqrt", "step", "tan"
        , "texelFetch", "texture", "textureLod", "textureProj", "textureOffset", "textureSize", "transpose", "trunc"
        // the compiler's and the prefabs' own identifiers (g_Mat, g_Ent and g_XMatPush are caught by "g_")
        , "In", "P", "E", "outFragColor" });

        const auto Starts = [&](std::string_view P) { return Name.starts_with(P); };
        return std::ranges::find(Reserved, Name) != Reserved.end()
            || Starts("gl_") || Starts("g_") || Starts("XMAT") || Starts("XMat") || Starts("N_") || Starts("s_") || Starts("o_") || Name.find("__") != std::string_view::npos
            || Starts("InInstance") || Starts("MaterialParamsBuf") || Starts("EntityParamsBuf");
    }

    // Letters, digits and _ only; empty if nothing is left.
    inline std::string SanitizeIdent(std::string_view In) noexcept
    {
        std::string S(In);
        for (auto& c : S) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
        return S;
    }

    // The variable of output OutIdx of N: the step name made an identifier. Unnamed => N_<guid>, as before the step names existed.
    // A name that starts with a digit, is reserved or compiler-owned gets "s_" in front (names starting "s_" do too, so two steps never meet).
    // Outputs beyond the first of a multi-output step are "o_<step>_<output>"; "o_" is reserved above, so they cannot meet a step's name either.
    inline std::string StepVarName(const node& N, int OutIdx = 0) noexcept
    {
        std::string S = N.m_StepName.empty() ? "N_" + std::to_string(N.m_Guid.m_Value) : SanitizeIdent(N.m_StepName);
        if (!N.m_StepName.empty() && (std::isdigit(static_cast<unsigned char>(S[0])) || IsReservedGlslName(S))) S = "s_" + S;
        if (N.m_OutputPins.size() > 1 && OutIdx > 0 && OutIdx < static_cast<int>(N.m_OutputPins.size()))
            S = "o_" + S + "_" + SanitizeIdent(N.m_OutputPins[OutIdx].m_Name);
        return S;
    }
}

#endif
