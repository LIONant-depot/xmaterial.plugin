#include "xmaterial_compiler.h"
#include <regex>
#include <unordered_set>
#include <filesystem>

#if defined(_WIN32)
    #include "dependencies/shaderc/include/shaderc/shaderc.hpp"
#else
    #include <shaderc/shaderc.hpp>     // Linux: the distribution's shaderc (see build/dependency/CMakeLists.txt)
#endif
#include "dependencies/xstrtool/source/xstrtool.h"
#include <fstream>
#include <cerrno>   // for errno
#include <iostream>
#include <bit>
#include <cmath>
#include <map>
#include <format>


#include "source/xmaterial_data_file.h"
#include "source/Graph/xmaterial_step_names.h"
#include "source/Graph/xmaterial_subgraph.h"
#include "dependencies/xtextfile/source/xtextfile.h"
#include "dependencies/xproperty/source/sprop/property_sprop_xtextfile_serializer.h"

#include "plugins/xmaterial_instance.plugin/source/xmaterial_intance_descriptor.h"

#if defined(_WIN32)
    #pragma comment(lib, "../../dependencies/shaderc/lib/shaderc_combined.lib")
    #include <process.h>
#endif

//
// force to create the property registrations for these types
//
#include "dependencies/xproperty/source/xcore/my_properties.cpp"
#include "dependencies/xmath/source/bridge/xmath_to_xproperty.h"

void xresource::loader< xrsc::material_type_guid_v >::Destroy(xresource::mgr& Mgr, data_type&& Data, const full_guid& GUID){}
void xresource::loader< xrsc::texture_type_guid_v >::Destroy(xresource::mgr& Mgr, data_type&& Data, const full_guid& GUID) {}

// The libraries a resource can use besides its own project or library: its members and dependencies, transitively, as the editor writes them in
// Project.config/Library.config.txt (xresource_editor::library; only the paths are read, the other properties are skipped)
namespace xmaterial_compiler::details
{
    struct library_paths
    {
        std::wstring                m_Path;
        std::vector<library_paths>  m_ParentLibraries, m_Libraries;

        XPROPERTY_DEF
        ( "Library", library_paths
        , obj_member<"Path",            &library_paths::m_Path>
        , obj_member<"ParentLibraries", &library_paths::m_ParentLibraries>
        , obj_member<"Libraries",       &library_paths::m_Libraries>
        )
    };
    XPROPERTY_REG(library_paths)

    inline std::vector<std::wstring> LibraryRoots(const std::wstring& Root) noexcept
    {
        std::vector<std::wstring> Roots{ Root };
        for (std::size_t i = 0; i < Roots.size(); ++i)
        {
            xtextfile::stream             Stream;
            library_paths                 Lib;
            xproperty::settings::context  Context;
            if (Stream.Open(true, std::format(L"{}/Project.config/Library.config.txt", Roots[i]), xtextfile::file_type::TEXT)) continue;
            if (xproperty::sprop::serializer::Stream(Stream, Lib, Context)) continue;
            for (auto* pList : { &Lib.m_Libraries, &Lib.m_ParentLibraries })
                for (auto& L : *pList) if (!L.m_Path.empty() && std::ranges::find(Roots, L.m_Path) == Roots.end()) Roots.push_back(L.m_Path);
        }
        return Roots;
    }
}

namespace xmaterial_compiler
{
#include <shaderc/shaderc.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <array>  // Or use struct for container

    // Resolves #include relative to the including file (or the working path: the project) and records every file it opens,
    // so the compiler can make them dependencies: a material recompiles when any shader it includes changes (the renderer's
    // shader library included).
    class CustomIncluder : public shaderc::CompileOptions::IncluderInterface
    {
    public:
        explicit CustomIncluder(std::vector<std::filesystem::path>* pOpened = nullptr) : m_pOpened(pOpened) {}

        shaderc_include_result* GetInclude(const char* requested_source,
            shaderc_include_type type,
            const char* requesting_source,
            size_t include_depth) override {
            auto result = new shaderc_include_result;
            auto container = new std::array<std::string, 2>;  // [0]: source_name, [1]: content or error
            result->user_data = container;

            try {
                std::filesystem::path requested_path(requested_source);
                if (type == shaderc_include_type_relative) {
                    std::filesystem::path requesting_path(requesting_source);
                    if (!requesting_path.empty()) {
                        requested_path = requesting_path.parent_path() / requested_path;
                    }
                }
                auto full_path = std::filesystem::absolute(requested_path);

                std::ifstream file(full_path, std::ios::in | std::ios::binary);
                if (!file) {
                    throw std::runtime_error("File not found: " + full_path.string());
                }

                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

                (*container)[0] = full_path.string();
                (*container)[1] = std::move(content);
                if (m_pOpened) m_pOpened->push_back(full_path);

                result->source_name = (*container)[0].data();
                result->source_name_length = (*container)[0].size();
                result->content = (*container)[1].data();
                result->content_length = (*container)[1].size();
            }
            catch (const std::exception& e) {
                (*container)[0] = "";
                (*container)[1] = e.what();

                result->source_name = (*container)[0].data();
                result->source_name_length = 0;
                result->content = (*container)[1].data();
                result->content_length = (*container)[1].size();
            }

            return result;
        }

        void ReleaseInclude(shaderc_include_result* data) override {
            delete static_cast<std::array<std::string, 2>*>(data->user_data);
            delete data;
        }

    private:
        std::vector<std::filesystem::path>* m_pOpened;
    };


    using namespace xmaterial_graph;
    static std::uint64_t Fnv(const std::vector<uint32_t>& V) { std::uint64_t H = 14695981039346656037ull; for (auto w : V) for (int i = 0; i < 4; ++i) { H ^= (w >> (8 * i)) & 0xff; H *= 1099511628211ull; } return H; }
    struct implementation final : xmaterial_compiler::instance
    {
        //-----------------------------------------------------------------
        // Parameters (documentation/material_contract.md sections 2-4, 10)
        //-----------------------------------------------------------------
        std::map<xmaterial::param_id, xmaterial::param_type>    m_Reads;                // every parameter a connected input reads, and as which type
        std::map<xmaterial::param_id, int>                      m_TexIndex;             // TEXTURE parameter -> index of the first texture that reads it
        xmaterial::manifest                                     m_Manifest;
        bool                                                    m_bManifest = false;
        bool                                                    m_bFailed   = false;
        bool                                                    m_bSamplers = false;    // a graph texture was emitted: the HDR variants read it through the texture table (TextureDecls)

        void Fail(const std::string& Message) { m_bFailed = true; LogMessage(xresource_pipeline::msg_type::ERROR, Message); }

        // Shortest text that parses back to the same float, always a GLSL float literal (not "1")
        static std::string FloatLiteral(float V)
        {
            if (not std::isfinite(V)) return std::format("uintBitsToFloat(0x{:08x}u)", std::bit_cast<std::uint32_t>(V));
            auto S = std::format("{}", V);
            if (S.find_first_of(".eE") == std::string::npos) S += ".0";
            return S;
        }

        static int Components(xmaterial::param_type T)
        {
            switch (T) { case xmaterial::param_type::FLOAT: return 1; case xmaterial::param_type::FLOAT2: return 2; case xmaterial::param_type::FLOAT3: return 3; default: return 4; }
        }

        static bool EntityFieldFits(xmaterial::entity_field F, xmaterial::param_type T)
        {
            using namespace xmaterial;
            if (F == entity_field::TINT) return T == param_type::COLOR || T == param_type::FLOAT4 || T == param_type::FLOAT3;
            return T == param_type::FLOAT || T == param_type::FLOAT2 || T == param_type::FLOAT3 || T == param_type::FLOAT4;      // PARAM0/1: not COLOR
        }

        // Defaults as the instance resolver will clamp them (validRange, HDR, alpha)
        static xmaterial::float4v ResolveDefault(const xmaterial::param_decl& D)
        {
            float V[4] = { D.m_Default.m_X, D.m_Default.m_Y, D.m_Default.m_Z, D.m_Default.m_W };
            const float Lo[4] = { D.m_ValidMin.m_X, D.m_ValidMin.m_Y, D.m_ValidMin.m_Z, D.m_ValidMin.m_W }, Hi[4] = { D.m_ValidMax.m_X, D.m_ValidMax.m_Y, D.m_ValidMax.m_Z, D.m_ValidMax.m_W };
            for (int i = 0; i < 4; ++i) V[i] = std::clamp(V[i], Lo[i], Hi[i]);
            if (D.m_Type == xmaterial::param_type::COLOR)
            {
                if (not D.m_bHDR) for (int i = 0; i < 3; ++i) V[i] = std::clamp(V[i], 0.0f, 1.0f);
                if (not D.m_bAlpha) V[3] = 1.0f;
            }
            return { V[0], V[1], V[2], V[3] };
        }

        // Checks the reads against the declarations, packs the manifest. Everything it reports is logged with the contract's code.
        void BuildManifest(const graph& g)
        {
            using namespace xmaterial;
            m_bManifest = true;

            auto Decls = g.m_Params;
            if (m_Reads.contains(alpha_cutoff_param_id_v) && std::ranges::find(Decls, alpha_cutoff_param_id_v, &param_decl::m_ID) == Decls.end())
            {
                auto& D = Decls.emplace_back();         // Masked: the cutoff, an instance parameter (render_settings_plan.md section 3)
                D.m_ID       = alpha_cutoff_param_id_v;
                D.m_Name     = "Alpha Cutoff";
                D.m_Default  = { g.m_Render.m_AlphaCutoff, 0, 0, 0 };
                D.m_ValidMin = { 0, 0, 0, 0 };
                D.m_ValidMax = { 1, 1, 1, 1 };
                D.m_UiMax    = { 1, 1, 1, 1 };
                D.m_Order    = 1000000;                     // not the graph's: after its own parameters (a number xtextfile reads back)
            }
            if (std::ranges::find(Decls, double_sided_param_id_v, &param_decl::m_ID) == Decls.end())
            {
                auto& D = Decls.emplace_back();         // every material: an instance may make it double sided (render_settings_plan.md part three)
                D.m_ID       = double_sided_param_id_v;
                D.m_Name     = "Double Sided";
                D.m_Kind     = param_kind::STATIC;      // no storage: the pipeline's state, resolved by the instance compiler
                D.m_Default  = { g.m_Render.m_bDoubleSided ? 1.0f : 0.0f, 0, 0, 0 };
                D.m_ValidMin = { 0, 0, 0, 0 };
                D.m_ValidMax = { 1, 1, 1, 1 };
                D.m_UiMax    = { 1, 1, 1, 1 };
                D.m_Order    = 1000000;
            }
            std::vector<param_id> Used;
            std::array<param_id, 4> Owner = {};                                     // entity field -> the parameter that has it

            for (const auto& [ID, ReadType] : m_Reads)
            {
                auto pDecl = std::ranges::find(Decls, ID, &param_decl::m_ID);
                if (pDecl == Decls.end())               { Fail(std::format("E_PARAM_TYPE: an input reads parameter {:016X}, which the material does not declare", ID)); continue; }
                if (pDecl->m_Type != ReadType)          { Fail(std::format("E_PARAM_TYPE: parameter '{}' ({:016X}) is declared with another type than its input", pDecl->m_Name, ID)); continue; }
                if (pDecl->m_Kind != param_kind::DYNAMIC){ Fail(std::format("E_PARAM_TYPE: parameter '{}' ({:016X}) is not dynamic, it can not be read yet", pDecl->m_Name, ID)); continue; }
                Used.push_back(ID);

                // Entity override: the field must fit the type and be owned once (the later ID loses), otherwise it is dropped and the material value is read
                if (auto F = pDecl->m_EntityField; F != entity_field::NONE)
                {
                    auto& O = Owner[int(F)];
                    if (pDecl->m_Type == param_type::TEXTURE || not EntityFieldFits(F, pDecl->m_Type) || O != 0)
                    {
                        LogMessage(xresource_pipeline::msg_type::ERROR, std::format("E_ENTITY_FIELD: parameter '{}' ({:016X}) can not use the entity field {}, the entity override is dropped", pDecl->m_Name, ID, int(F)));
                        pDecl->m_EntityField = entity_field::NONE;
                    }
                    else O = ID;
                }
            }

            for (const auto& D : Decls)
                if (not m_Reads.contains(D.m_ID) && D.m_Kind == param_kind::DYNAMIC) LogMessage(xresource_pipeline::msg_type::WARNING, std::format("W_UNUSED: parameter '{}' ({:016X}) is declared but nothing reads it", D.m_Name, D.m_ID));

            m_Manifest = Pack(Decls, Used);

            // The sampler is the texture's slot (1.. in texture order), not the packer's guess. Same slot for a texture read by several inputs: the first wins [ponytail: one slot per parameter]
            for (auto& E : m_Manifest.m_Entries)
                if (E.m_Type == param_type::TEXTURE)
                {
                    if (auto It = m_TexIndex.find(E.m_ID); It != m_TexIndex.end()) E.m_Sampler = std::uint8_t(It->second + 1);
                }
            m_Manifest.m_LayoutHash = Hash(m_Manifest);
        }

        // The GLSL that reads one parameter (an entity field, when it has one and the pass is not the LDR preview, replaces the material value)
        std::string ReadExpr(const xmaterial::manifest_entry& E) const
        {
            const int         n   = Components(E.m_Type);
            const int         c   = (E.m_ByteOffset % 16) / 4;
            const std::string Mat = std::format("g_Mat.P[XMAT_BASE + {}u].{}", E.m_ByteOffset / 16, std::string_view("xyzw").substr(c, n));
            if (E.m_EntityField == xmaterial::entity_field::NONE) return Mat;

            static constexpr const char* Field[] = { "", "Tint", "P0", "P1" };
            return std::format("XMAT_ENT({}u, {}, {}, {})", 1u << (int(E.m_EntityField) - 1), Field[int(E.m_EntityField)], std::string_view("xyzw").substr(0, n), Mat);
        }

        // The [TOP] declarations of a material with a block (section 2). Nothing at all when no parameter is read: those shaders do not change.
        std::string ParamTop(void) const
        {
            if (m_Manifest.m_BlockBytes == 0) return {};

            bool bEntity = false;
            for (const auto& E : m_Manifest.m_Entries) bEntity |= E.m_EntityField != xmaterial::entity_field::NONE;

            std::string S = "layout(std430, set = 1, binding = 9) readonly buffer MaterialParamsBuf { vec4 P[]; } g_Mat;\n#ifdef XLION_LDR_OUTPUT\n    #define XMAT_BASE 0u\n";
            if (bEntity) S += "    #define XMAT_ENT(Bit, Field, Swz, Mat) (Mat)\n";
            S += "#else\n    layout(push_constant) uniform XMatPush { layout(offset = 16) uint MaterialBase; } g_XMatPush;\n    #define XMAT_BASE g_XMatPush.MaterialBase\n";
            if (bEntity)
                S += "    struct XMatEntity { uvec4 H; vec4 Tint; vec4 P0; vec4 P1; };\n"
                     "    layout(std430, set = 1, binding = 10) readonly buffer EntityParamsBuf { XMatEntity E[]; } g_Ent;\n"
                     "    layout(location = 8) flat in uint InInstance;\n"
                     "    #define XMAT_ENT(Bit, Field, Swz, Mat) ((g_Ent.E[InInstance].H.x & Bit) != 0u ? g_Ent.E[InInstance].Field.Swz : (Mat))\n";
            S += "#endif\n";
            return S;
        }

        // The bindless variant's heaps and the texture ids: after the parameter block (its vec4s from m_BlockBytes / 16), two slots a vec4 -
        // .xy slot 2k (texture_id, sampler_id), .zw slot 2k + 1 - written there by xLIONRender (instance::CreateMaterial, table materials)
        std::string TextureTop(void) const
        {
            if (not m_bSamplers) return {};
            const auto Base = m_Manifest.m_BlockBytes / 16;
            // (no parameter block: ParamTop declared neither the block nor MaterialBase)
            const std::string Block = Base ? std::string{} : "    layout(std430, set = 1, binding = 9) readonly buffer MaterialParamsBuf { vec4 P[]; } g_Mat;\n"
                                                             "    layout(push_constant) uniform XMatPush { layout(offset = 16) uint MaterialBase; } g_XMatPush;\n"
                                                             "    #define XMAT_BASE g_XMatPush.MaterialBase\n";
            return std::format( "#if !defined(XLION_LDR_OUTPUT) && defined(XGPU_BINDLESS)\n{}"
                                "    layout(set = 3, binding = 0) uniform texture2D xgpu_Textures2D[];\n"
                                "    layout(set = 3, binding = 2) uniform sampler   xgpu_Samplers[];\n"
                                "    #define XGPU_TEXTURE(Id) xgpu_Textures2D[Id]\n"
                                "    #define XGPU_SAMPLER(Id) xgpu_Samplers[Id]\n"
                                "    #define XMAT_TEXID(i)  floatBitsToUint(g_Mat.P[XMAT_BASE + {}u + (i) / 2u][((i) % 2u) * 2u])\n"
                                "    #define XMAT_SAMPID(i) floatBitsToUint(g_Mat.P[XMAT_BASE + {}u + (i) / 2u][((i) % 2u) * 2u + 1u])\n"
                                "#endif\n", Block, Base, Base);
        }

        // A graph texture's declaration (the Sampler 2D node's) per variant: LDR sampler i; bindless its two heap entries; else binding i of set 3
        static std::string TextureDecls(const std::string& Code)
        {
            static const std::regex Decl(R"(layout\(binding = XMAT_SAMPLER\((\d+)\)\)\s+uniform\s+sampler2D\s+(\w+)\s*;)");
            std::string Out;
            auto        From = Code.cbegin();
            for (std::sregex_iterator It(Code.cbegin(), Code.cend(), Decl), End; It != End; ++It)
            {
                const auto& M = *It;
                Out.append(From, M[0].first);
                const auto i = M[1].str(), Name = M[2].str();
                Out += std::format( "\n#if defined(XLION_LDR_OUTPUT)\nlayout(binding = {0}) uniform sampler2D {1};\n"
                                    "#elif defined(XGPU_BINDLESS)\n#define {1} sampler2D(XGPU_TEXTURE(XMAT_TEXID({0}u)), XGPU_SAMPLER(XMAT_SAMPID({0}u)))\n"
                                    "#else\nlayout(set = 3, binding = {0}) uniform sampler2D {1};\n#endif\n", i, Name);
                From = M[0].second;
            }
            Out.append(From, Code.cend());
            return Out;
        }

        // Swaps the read placeholders (\x01 ID \x02) for the reads
        void ResolveReads(std::string& Code) const
        {
            for (size_t At; (At = Code.find('\x01')) != std::string::npos; )
            {
                const auto ID  = std::stoull(Code.substr(At + 1, 16), nullptr, 16);
                const auto pE  = std::ranges::find(m_Manifest.m_Entries, ID, &xmaterial::manifest_entry::m_ID);
                Code.replace(At, 18, pE == m_Manifest.m_Entries.end() ? "0.0" : ReadExpr(*pE));      // a failed parameter: the compile fails anyway
            }
        }

        std::string PinToVarName(const node& n, int OutIdx = 0) { return StepVarName(n, OutIdx); }

        std::string GetInputExpr(const graph& g, const node& n, int pinIndex, int paramIndex, bool requireConnectionOnly)
        {
            if (pinIndex < 0 || pinIndex >= static_cast<int>(n.m_InputPins.size()))
                return "/*bad pin index*/";

            const auto& ip = n.m_InputPins[pinIndex];

            //1. if connected == resolve upstream
            if (ip.m_ConnectionGUID.m_Value != 0) //will there be casing where the guid just = 0?
            {
                auto it = g.m_Connections.find(ip.m_ConnectionGUID);
                if (it != g.m_Connections.end())
                {
                    const auto& conn = *it->second;
                    if (auto* srcNode = g.findNodeByPin(conn.m_OutputPinGuid))
                    {
                        int idx, sub;
                        bool dummy;
                        const pin* outPin = g.findPinConst(*srcNode, conn.m_OutputPinGuid, dummy, idx, sub);
                        if (outPin)
                        {
                            // The source: a direct GLSL expr (like "In.Color" or "In.UV") or the step's variable
                            std::string E = outPin->m_DefaultExpr.empty() ? PinToVarName(*srcNode, idx) : outPin->m_DefaultExpr;

                            // The components it gives (1 float, N vecN), then the input takes: the connection's swizzle, or the first ones of a longer vector
                            // (truncation); a vector input spells every component out, whatever it takes (Vec4.xyzw, Vec2.xxxx, Albedo.xyz) inside a constructor of the
                            // input's type: vec4(Vec2.xxxx) (the rules: AreTypesCompatible, xmaterial_graph_editing.h). A float input keeps the plain expression
                            auto* pI = g.findType(ip.m_TypeGUID);
                            int nOut = xmaterial_graph::Components(g, outPin->m_TypeGUID);
                            const int nIn = xmaterial_graph::Components(g, ip.m_TypeGUID);
                            if (nIn >= 2)
                            {
                                std::string S;                                                      // the letters the input's components take
                                if (sub >= 0 && sub < 4) S = std::string(nIn, "xyzw"[sub]);
                                else if (!conn.m_Swizzle.empty())
                                {
                                    if (conn.m_Swizzle.size() > 4 || conn.m_Swizzle.find_first_not_of(std::string_view("xyzw", std::max(0, std::min(nOut, 4)))) != std::string::npos)
                                        Fail(std::format("E_SWIZZLE: node '{}' input '{}' takes .{} of a source without those components", n.m_Name, ip.m_Name, conn.m_Swizzle));
                                    S = conn.m_Swizzle;
                                }
                                else S = nOut >= nIn ? std::string("xyzw", nIn) : std::string(nIn, 'x');       // the whole (or its first ones), or a whole float splatted
                                return pI->m_CodeString + "(" + E + "." + S + ")";
                            }
                            if (sub >= 0 && sub < 4) E += std::string(".") + "xyzw"[sub];
                            if (!conn.m_Swizzle.empty())
                            {
                                if (sub >= 0 || conn.m_Swizzle.size() > 4 || conn.m_Swizzle.find_first_not_of(std::string_view("xyzw", std::max(0, std::min(nOut, 4)))) != std::string::npos)
                                    Fail(std::format("E_SWIZZLE: node '{}' input '{}' takes .{} of a source without those components", n.m_Name, ip.m_Name, conn.m_Swizzle));
                                E += "." + conn.m_Swizzle;
                            }
                            return E;
                        }
                    }
                }
            }

            // 2. Not connected: an optional input's value (Output: PBR's Normal, Emissive), else strict inputs fail
            if (!ip.m_DefaultExpr.empty()) return ip.m_DefaultExpr;
            if (requireConnectionOnly)
            {
                // strict mode → must be connected
                return "/* ERROR: Node '" + n.m_Name +
                    "' requires input[" + std::to_string(pinIndex) +
                    "] to be connected */";
            }

            // 3. use param fallback
            if (paramIndex >= 0 && paramIndex < static_cast<int>(n.m_Params.size()))
            {
                const auto& prop = n.m_Params[paramIndex];

                // A declared parameter: the input reads the material block instead of its literal (resolved to the packed offset once every reader is known)
                if (prop.m_ParamID != 0)
                {
                    // A vector parameter (COLOR, FLOAT2..4) is read by the float inputs of one Vec node, each as its own component (ExposeParam -Type)
                    const auto pVec = std::ranges::find(g.m_Params, prop.m_ParamID, &xmaterial::param_decl::m_ID);
                    const bool bComp = prop.m_Type == node_param::type::FLOAT && pVec != g.m_Params.end() && pVec->m_Type != xmaterial::param_type::FLOAT && pVec->m_Type != xmaterial::param_type::TEXTURE;
                    const auto Type = bComp ? pVec->m_Type : prop.m_Type == node_param::type::FLOAT ? xmaterial::param_type::FLOAT : xmaterial::param_type::TEXTURE;
                    if (prop.m_Type != node_param::type::FLOAT && prop.m_Type != node_param::type::TEXTURE_RESOURCE)
                        Fail(std::format("E_PARAM_TYPE: node '{}' input '{}' reads parameter {:016X} but only float and texture inputs can", n.m_Name, prop.m_Name, prop.m_ParamID));
                    else if (auto [It, bNew] = m_Reads.emplace(prop.m_ParamID, Type); not bNew && It->second != Type)
                        Fail(std::format("E_PARAM_TYPE: inputs sharing parameter {:016X} disagree on the type", prop.m_ParamID));

                    if (Type == xmaterial::param_type::FLOAT)
                        return std::format("\x01{:016X}\x02", prop.m_ParamID);
                    if (bComp)
                    {
                        int Comp = 0;
                        for (int i = 0; i < paramIndex; ++i) Comp += n.m_Params[i].m_ParamID == prop.m_ParamID;
                        return std::format("(\x01{:016X}\x02)[{}]", prop.m_ParamID, Comp);
                    }
                }

                if (prop.m_Value.m_pType)
                {
                    auto guid = prop.m_Value.m_pType->m_GUID;
                    if (guid == xproperty::settings::var_type<float>::guid_v)
                        return FloatLiteral(prop.m_Value.get<float>());
                    if (guid == xproperty::settings::var_type<int>::guid_v)
                        return std::to_string(prop.m_Value.get<int>());
                    if (guid == xproperty::settings::var_type<std::string>::guid_v)
                        return prop.m_Value.get<std::string>();
                    if (guid == xproperty::settings::var_type<xresource::full_guid>::guid_v)
                    {
                        g.m_FinalTextureNodes.m_Textures.emplace_back(&n, paramIndex);
                        if (prop.m_ParamID != 0) m_TexIndex.emplace(prop.m_ParamID, int(g.m_FinalTextureNodes.m_Textures.size() - 1));
                        m_bSamplers = true;
                        return std::format("XMAT_SAMPLER({})", g.m_FinalTextureNodes.m_Textures.size() - 1);
                    }


                    //extend with vec2/vec3/vec4 serialization here
                }
            }

            return "/*missing input*/";
        }

        void ExtractSection(std::string& src, const std::string& marker, std::string& out)
        {
            size_t start = src.find(marker);
            if (start != std::string::npos)
            {
                size_t open = src.find("<!", start);
                size_t close = src.find("!>", open);
                if (open != std::string::npos && close != std::string::npos && close > open)
                {
                    out += src.substr(open + 2, close - open - 2);
                    src.erase(start, close - start + 2);
                }
            }
        }

        void DFS(const graph& g, const node& n, std::unordered_set<node_guid>& visited, std::vector<const node*>& ordered)
        {
            if (visited.contains(n.m_Guid)) return;
            visited.insert(n.m_Guid);

            for (const auto& ip : n.m_InputPins)
            {
                if (ip.m_ConnectionGUID.m_Value != 0)
                {
                    auto it = g.m_Connections.find(ip.m_ConnectionGUID);
                    if (it != g.m_Connections.end())
                    {
                        if (auto* dep = g.findNodeByPin(it->second->m_OutputPinGuid))
                            DFS(g, *dep, visited, ordered);
                    }
                }
            }
            ordered.push_back(&n);
        }


        std::string HandleShaderOnlyNode(graph& g, node& n )
        {
            std::string         ss;
            std::wstring        ShaderFileName;
            std::string         Shader;

            ShaderFileName = std::format( L"{}/{}", m_ProjectPaths.m_Project, n.m_Params[0].m_Value.get<std::wstring>() );

            if ( auto Err = RefreshShaderOnlyNode(ShaderFileName, n, Shader); Err )
            {
                Err.ForEachInChain([&](xerr Error)
                {
                    xstrtool::print("ERROR: {}\n", Err.getMessage());
                    if (auto Hint = Err.getHint(); Hint.empty() == false)
                        xstrtool::print("- HINT: {}\n", Hint);
                });
                return{"ERROR"};
            }

            for( int i=1; i<n.m_InputPins.size(); ++i)
            {
                auto& E = n.m_InputPins[i];

                g.m_FinalTextureNodes.m_Textures.emplace_back( &n, i );
            }

            return Shader;
        }

        std::string HandleShaderOnlyGraphs( graph& g)
        {
            if ( auto pNode = g.findFullShaderNode(); pNode )
            {
                return HandleShaderOnlyNode(g, *pNode);
            }

            return {};
        }

        std::string GenerateGLSL( graph& g)
        {
            // handle shader only nodes
            if (auto str = HandleShaderOnlyGraphs(g); not str.empty())
                return str;

            const bool bMasked = g.m_Render.m_BlendMode == xmaterial::render_settings::blend_mode::MASKED;
            if (bMasked) m_Reads.emplace(xmaterial::alpha_cutoff_param_id_v, xmaterial::param_type::FLOAT);


            // Header (always)
            std::ostringstream ss;
            ss << "#version 450\n";
            ss << "#extension GL_ARB_separate_shader_objects  : enable\n";
            ss << "#extension GL_ARB_shading_language_420pack : enable\n\n";


            // Validate final output
            std::vector<const node*> fragOutNodes;
            for (auto& [id, nPtr] : g.m_InstanceNodes)
                if (nPtr->isOutputNode())
                    fragOutNodes.push_back(nPtr.get());

            if (fragOutNodes.empty())
                return "/* ERROR: Graph must have a Final FragOut node */\n";

            std::string topCode, botCode;

            // Traverse only from Final FragOut nodes
            std::unordered_set<node_guid> visited;
            std::vector<const node*> ordered;
            for (auto* fragNode : fragOutNodes)
                DFS(g, *fragNode, visited, ordered);

            // simple traversal for now
            for (auto* np : ordered)
            {
                const node& n = *np;
                std::string code = n.m_Code;

                // replace $var with node variable name
                code = std::regex_replace(code, std::regex("\\$var"), PinToVarName(n));


                // replace $input[pin]_prop[param] with expression
                std::regex reProp(R"(\$input\[(\d+)\]_prop\[(\d+)\])");
                std::smatch m;

                while (std::regex_search(code, m, reProp))
                {
                    int pinIdx = std::stoi(m[1].str());
                    int paramIdx = std::stoi(m[2].str());
                    std::string repl = GetInputExpr(g, n, pinIdx, paramIdx, false);
                    code.replace(m.position(0), m.length(0), repl);
                }

                // --- Replace plain $input[pin] (fallback paramIndex = same as pin) ---
                std::regex rePlain(R"(\$input\[(\d+)\])");//texture($input[0], $input[1])
                while (std::regex_search(code, m, rePlain))
                {
                    int pinIdx = std::stoi(m[1].str());
                    std::string repl = GetInputExpr(g, n, pinIdx, -1, true);
                    code.replace(m.position(0), m.length(0), repl);
                }


                // --- Replace plain $input[pin] (fallback paramIndex = same as pin) ---
                std::regex reTex(R"(\$tex\[(\d+)\])");
                while (std::regex_search(code, m, reTex))
                {
                    int pinIdx = std::stoi(m[1].str());
                    std::string repl = GetInputExpr(g, n, pinIdx, n.m_InputPins[pinIdx].m_ParamIndex, false);
                    code.replace(m.position(0), m.length(0), repl);
                }


                // extract [TOP]/[BOT]
                ExtractSection(code, "[TOP]", topCode);
                ExtractSection(code, "[BOT]", botCode);

                // whatever remains goes to botCode (main)
                if (!code.empty())
                    botCode += code;
            }

            // Now that every reader is known: pack the parameters, then turn the read placeholders into reads
            BuildManifest(g);
            ResolveReads(topCode);
            ResolveReads(botCode);

            // The graph's textures: the editors' LDR variant binds them from sampler 0; the HDR ones read xLIONRender's texture table (set 3,
            // xgpu::texture_table): bindless (XGPU_BINDLESS), the heaps by the ids in the material's block; otherwise binding i of the table's set
            if (m_bSamplers)
            {
                ss << "#if !defined(XLION_LDR_OUTPUT) && defined(XGPU_BINDLESS)\n#extension GL_EXT_nonuniform_qualifier : require\n#endif\n";
                topCode = TextureDecls(topCode);
            }
            if (bMasked)            // the output nodes' cut (XMAT_MASKED, a compile option)
            {
                auto Cutoff = std::format("#define XMAT_ALPHA_CUTOFF (\x01{:016X}\x02)\n", xmaterial::alpha_cutoff_param_id_v);
                ResolveReads(Cutoff);
                ss << Cutoff;
            }
            ss << ParamTop() << TextureTop() << topCode << "\n";
            ss << "void main() \n{\n";
            ss << botCode;
            ss << "}\n";

            return ss.str();
        }

        xerr onCompile(void) override
        {
            //
            // Load the descriptor 
            //
            auto DescriptorFileName = std::format( L"{}/{}/Descriptor.txt", m_ProjectPaths.m_Project, m_InputSrcDescriptorPath);

            graph Graph;
            Graph.CreateGraph(Graph);
            Graph.serialize(Graph, true, DescriptorFileName);

            // Group nodes: each is replaced by a copy of its sub-graph's steps (nested ones too), before anything reads the graph. A sub-graph is
            // looked for in this project or library first, then in the libraries it uses. Their files are dependencies: editing one compiles every
            // material that uses it again (one of another library: by a path relative to this one, "../Other.lionlib/Descriptors/...").
            {
                std::vector<std::wstring> Used;
                const auto Roots  = details::LibraryRoots(m_ProjectPaths.m_Project);
                const auto PathOf = [&](std::uint64_t Guid)
                {
                    for (const auto& R : Roots)
                        if (auto P = std::format(L"{}/Descriptors/MaterialSubgraph/{:02X}/{:02X}/{:X}.desc/Descriptor.txt", R, Guid & 0xff, (Guid >> 8) & 0xff, Guid); std::filesystem::exists(P)) return P;
                    return std::wstring{};
                };
                const auto Why = FlattenCalls(Graph, PathOf, &Used);
                for (const auto& Path : Used)
                {
                    std::error_code Ec;
                    const auto Relative = std::filesystem::relative(Path, m_ProjectPaths.m_Project, Ec);
                    m_Dependencies.m_Assets.push_back(Ec ? Path : Relative.wstring());
                }
                if (!Why.empty())
                {
                    LogMessage(xresource_pipeline::msg_type::ERROR, "E_GROUP: " + Why);
                    return xerr::create_f<state, "A group node could not be expanded (see the log)">();
                }
            }

            // A sub-graph (plugins/xmaterial_subgraph.plugin uses this compiler): checked, and its interface written as its resource. It makes no
            // shader of its own: the materials that use it copy its steps in.
            if (std::wstring_view(m_InputSrcDescriptorPath).find(L"MaterialSubgraph") != std::wstring_view::npos)
            {
                Graph.m_bSubgraph = true;
                if (const auto Why = ValidateSubgraph(Graph); !Why.empty())
                {
                    LogMessage(xresource_pipeline::msg_type::ERROR, "E_SUBGRAPH: " + Why);
                    return xerr::create_f<state, "The sub-graph is not valid (see the log)">();
                }
                const auto Interface = InterfaceText(Graph);
                LogMessage(xresource_pipeline::msg_type::INFO, "Sub-graph interface:\n" + Interface);
                for (auto& T : m_Target)
                    if (T.m_bValid)
                    {
                        std::error_code Ec; std::filesystem::create_directories(std::filesystem::path(T.m_DataPath).parent_path(), Ec);
                        std::ofstream Out(std::filesystem::path(T.m_DataPath), std::ios::binary | std::ios::trunc);
                        if (!Out) return xerr::create_f<state, "Fail to write the sub-graph's interface">();
                        Out << Interface;
                    }
                return {};
            }

            const auto& shaderString = GenerateGLSL(Graph);
            if (not m_bManifest) BuildManifest(Graph);                  // a shader-file material reads no parameters: its manifest is only the declarations

            // A shader-file material binds its own samplers 1-7 (xLIONRender's set 0); a graph material's texture table has no limit of its own,
            // the editors' LDR variant binds them as pipeline samplers (16 at most)
            if (const auto n = Graph.m_FinalTextureNodes.m_Textures.size(); n > (Graph.findFullShaderNode() ? 7u : 16u))
                Fail(std::format("E_TOO_MANY_TEXTURES: {} material textures, {} at most", n, Graph.findFullShaderNode() ? 7 : 16));

            if (m_bFailed)
                return xerr::create_f<state, "The material parameters are not valid (see the log for the E_ codes)">();

            LogMessage(xresource_pipeline::msg_type::INFO, std::format("Setting the working path to : {}", xstrtool::To(m_ProjectPaths.m_Project)));
            std::filesystem::current_path(m_ProjectPaths.m_Project);

            //
            // Generate the sprv shader
            //
            displayProgressBar("Compiling Shader", 0.0f);
            // Two variants (xLIONRender): LDR = tone mapped in the material (XLION_LDR_OUTPUT defined, what the
            // editors draw straight to the screen), HDR = scene-linear output that the renderer tone maps.
            // Materials that never test XLION_LDR_OUTPUT produce the same code twice.
            shaderc::Compiler compiler;
            std::vector<std::filesystem::path> Includes;
            const auto CompileVariant = [&](bool bLDR, std::vector<uint32_t>& SPIRV, bool bBindless = false, bool bGBuffer = false) -> xerr
            {
                shaderc::CompileOptions options;
                options.SetIncluder(std::make_unique<CustomIncluder>(&Includes));
                options.SetOptimizationLevel(shaderc_optimization_level_performance);
                options.SetTargetSpirv(shaderc_spirv_version_1_0);
                if (bLDR) options.AddMacroDefinition("XLION_LDR_OUTPUT");
                if (bBindless) options.AddMacroDefinition("XGPU_BINDLESS");
                if (bGBuffer)  options.AddMacroDefinition("XLION_GBUFFER_OUTPUT");     // (xLIONRender's thin G-buffer: mb_standard_pbr.frag PBRLighting)
                if (Graph.m_Render.m_BlendMode == xmaterial::render_settings::blend_mode::MASKED) options.AddMacroDefinition("XMAT_MASKED");
                if (Graph.m_Render.m_BackNormal == xmaterial::render_settings::back_normal::NONE) options.AddMacroDefinition("XMAT_BACK_FACE_AS_FRONT");     // else back faces flip their normal: always compiled in, so an instance can make it double sided
                auto result = compiler.CompileGlslToSpv( shaderString
                                                       , shaderc_glsl_default_fragment_shader
                                                       , "shader.glsl"
                                                       , options
                                                       );
                if (const auto Err = result.GetCompilationStatus(); Err != shaderc_compilation_status_success )
                {
                    LogMessage(xresource_pipeline::msg_type::ERROR, result.GetErrorMessage().c_str());
                    switch (Err)
                    {
                    case shaderc_compilation_status_invalid_stage:          return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_invalid_stage">();
                    case shaderc_compilation_status_compilation_error:      return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_compilation_error">();
                    case shaderc_compilation_status_internal_error:         return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_internal_error">();
                    case shaderc_compilation_status_null_result_object:     return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_null_result_object">();
                    case shaderc_compilation_status_invalid_assembly:       return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_invalid_assembly">();
                    case shaderc_compilation_status_validation_error:       return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_validation_error">();
                    case shaderc_compilation_status_transformation_error:   return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_transformation_error">();
                    case shaderc_compilation_status_configuration_error:    return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_configuration_error">();
                    default:                                                return xerr::create_f<state, "Compilation for spirv fail">();
                    }
                }
                SPIRV.assign(result.cbegin(), result.cend());
                return {};
            };

            // A graph material with textures: the HDR variant twice, through the texture table's set (any device) and bindless (XGPU_BINDLESS)
            std::vector<uint32_t> spirv, spirvHDR, spirvBindless;
            if (auto Err = CompileVariant(true,  spirv);    Err) return Err;
            if (auto Err = CompileVariant(false, spirvHDR); Err) return Err;
            if (m_bSamplers) if (auto Err = CompileVariant(false, spirvBindless, true); Err) return Err;
            // The G-buffer variant (and its bindless twin): kept only where the define changed the shader - a material lit by
            // mb_standard_pbr.frag; an unlit one would write its color as the G-buffer's normal
            std::vector<uint32_t> spirvGBuffer, spirvGBufferBindless;
            if (auto Err = CompileVariant(false, spirvGBuffer, false, true); Err) return Err;
            if (spirvGBuffer == spirvHDR) spirvGBuffer.clear();
            else if (m_bSamplers) if (auto Err = CompileVariant(false, spirvGBufferBindless, true, true); Err) return Err;
            displayProgressBar("Compiling Shader", 1.0f);

            //
            // Print the generated shader
            //
            LogMessage(xresource_pipeline::msg_type::INFO, shaderString);
            LogMessage(xresource_pipeline::msg_type::INFO, std::format("Render settings: blend {}, double sided {}, depth write {}, cast shadows {}, wireframe {}", int(Graph.m_Render.m_BlendMode), Graph.m_Render.m_bDoubleSided, Graph.m_Render.WritesDepth(), Graph.m_Render.m_bCastShadows, Graph.m_Render.m_bWireframe));

            //
            // Add the dependencies
            //
            for (auto& E : Graph.m_FinalTextureNodes.m_Textures )
            {
                auto Ref = E.m_pNode->m_Params[ E.m_iParam ].m_Value.get<xresource::full_guid>();
                if (std::ranges::find(m_Dependencies.m_Resources, Ref) == m_Dependencies.m_Resources.end()) m_Dependencies.m_Resources.push_back(Ref);     // a texture used by several nodes: once
            }

            // Every shader file the material includes (both variants), relative to the project as the asset dependencies are
            for (const auto& Path : Includes)
            {
                std::error_code Ec;
                const auto Relative = std::filesystem::relative(Path, m_ProjectPaths.m_Project, Ec);
                m_Dependencies.m_Assets.push_back(Ec ? Path.wstring() : Relative.wstring());
            }

            // Check if it is a FullShaderNode if so add the shader as an asset
            if ( auto pNode = Graph.findFullShaderNode(); pNode )
            {
                m_Dependencies.m_Assets.push_back( pNode->m_Params[0].m_Value.get<std::wstring>() );
            }

            //
            // Export the compiled material
            //
            LogMessage(xresource_pipeline::msg_type::INFO, std::format("Shader compiled successfully! SPIR-V size: {} words (HDR variant {}). Hash {:016x}/{:016x}\n", spirv.size(), spirvHDR.size(), Fnv(spirv), Fnv(spirvHDR)) );


            //
            // Create the actual material
            //

            // Allocate and collect the textures
            auto Textures = std::make_unique<xrsc::texture_ref[]>(Graph.m_FinalTextureNodes.m_Textures.size());
            for (auto& E : Graph.m_FinalTextureNodes.m_Textures)
            {
                const int Index = static_cast<int>(&E - Graph.m_FinalTextureNodes.m_Textures.data());
                auto Ref = E.m_pNode->m_Params[E.m_iParam].m_Value.get<xresource::full_guid>();
                Textures[Index].m_Instance = Ref.m_Instance;
            }

            // Setup the structure
            xmaterial::data_file MaterialDataFile = {};

            MaterialDataFile.m_pShader          = spirv.data();
            MaterialDataFile.m_ShaderSize       = static_cast<std::uint32_t>(spirv.size());
            MaterialDataFile.m_pShaderHDR       = spirvHDR.data();
            MaterialDataFile.m_ShaderSizeHDR    = static_cast<std::uint32_t>(spirvHDR.size());
            MaterialDataFile.m_pShaderBindless  = spirvBindless.empty() ? nullptr : spirvBindless.data();
            MaterialDataFile.m_ShaderSizeBindless = static_cast<std::uint32_t>(spirvBindless.size());
            MaterialDataFile.m_pShaderGBuffer   = spirvGBuffer.empty() ? nullptr : spirvGBuffer.data();
            MaterialDataFile.m_ShaderSizeGBuffer = static_cast<std::uint32_t>(spirvGBuffer.size());
            MaterialDataFile.m_pShaderGBufferBindless   = spirvGBufferBindless.empty() ? nullptr : spirvGBufferBindless.data();
            MaterialDataFile.m_ShaderSizeGBufferBindless = static_cast<std::uint32_t>(spirvGBufferBindless.size());
            MaterialDataFile.m_Flags.m_bTextureTable = m_bSamplers;
            MaterialDataFile.m_Render           = Graph.m_Render;
            MaterialDataFile.m_Flags.m_bSamplersFrom1 = Graph.findFullShaderNode() == nullptr;   // every graph material: also one without textures (Output: PBR samples the shadow atlas, slot 0)
            MaterialDataFile.m_pDefaultTextures = Graph.m_FinalTextureNodes.m_Textures.empty()? nullptr : Textures.get();
            MaterialDataFile.m_nDefaultTextures = static_cast<std::uint8_t>(Graph.m_FinalTextureNodes.m_Textures.size());

            // The parameter block: every parameter at its default, in the manifest's layout
            std::vector<float> DefaultBlock(m_Manifest.m_BlockBytes / 4, 0.0f);
            for (const auto& E : m_Manifest.m_Entries)
            {
                if (E.m_Type == xmaterial::param_type::TEXTURE) continue;
                const auto  D = ResolveDefault(*std::ranges::find(m_Manifest.m_Decls, E.m_ID, &xmaterial::param_decl::m_ID));
                const float V[4] = { D.m_X, D.m_Y, D.m_Z, D.m_W };
                for (int i = 0; i < Components(E.m_Type); ++i) DefaultBlock[E.m_ByteOffset / 4 + i] = V[i];
            }
            MaterialDataFile.m_BlockBytes       = m_Manifest.m_BlockBytes;
            MaterialDataFile.m_pDefaultBlock    = DefaultBlock.empty() ? nullptr : DefaultBlock.data();
            MaterialDataFile.m_LayoutHash       = m_Manifest.m_LayoutHash;

            // The parameters with their names (the runtime addresses an entity's overrides by them), in the order the inspectors list them
            // (xmaterial::InspectorOrder: a Mesh slot's rows are this list)
            std::vector<xmaterial::data_file::param_info> ParamInfo;
            auto Entries = m_Manifest.m_Entries;
            const auto DeclOf = [&](std::uint64_t ID) { const auto D = std::ranges::find(m_Manifest.m_Decls, ID, &xmaterial::param_decl::m_ID); return D == m_Manifest.m_Decls.end() ? nullptr : &*D; };
            std::ranges::stable_sort(Entries, {}, [&](const xmaterial::manifest_entry& E) { return xmaterial::InspectorOrder(DeclOf(E.m_ID), m_Manifest.m_Decls); });
            for (const auto& E : Entries)
            {
                auto& P = ParamInfo.emplace_back();
                P.m_ID          = E.m_ID;
                P.m_ByteOffset  = E.m_ByteOffset;
                P.m_Type        = std::uint8_t(E.m_Type);
                P.m_Slot        = E.m_Type == xmaterial::param_type::TEXTURE && E.m_Sampler ? std::uint8_t(E.m_Sampler - 1) : 0;
                P.m_EntityField = std::uint8_t(E.m_EntityField);
                if (const auto D = std::ranges::find(m_Manifest.m_Decls, E.m_ID, &xmaterial::param_decl::m_ID); D != m_Manifest.m_Decls.end())
                    std::strncpy(P.m_Name, D->m_Name.c_str(), sizeof(P.m_Name) - 1);
            }
            MaterialDataFile.m_pParamInfo = ParamInfo.empty() ? nullptr : ParamInfo.data();
            MaterialDataFile.m_nParamInfo = std::uint16_t(ParamInfo.size());

            //
            // Save the layout manifest: the one place that says where each parameter lives (instance compiler, editor and runtime read it)
            //
            {
                xtextfile::stream Stream;
                if ( auto Err = Stream.Open(false, std::format(L"{}/{}.log/MaterialLayout.txt", m_ProjectPaths.m_ResourcesLogs, m_ResourcePartialPath), xtextfile::file_type::TEXT); Err )
                    return xerr::create_f<state, "Fail to open the file to save the material layout">(Err);

                xproperty::settings::context C;
                if ( auto Err = xproperty::sprop::serializer::Stream( Stream, m_Manifest, C); Err )
                    return xerr::create_f<state, "Fail to save the material layout">(Err);
            }

            //
            // Save the material instance template
            //
            {
                displayProgressBar("default material instance", 0.0f);

                xmaterial_instance::descriptor MaterialInstance;

                // Set the material reference
                MaterialInstance.m_MaterialRef.m_Instance = m_ResourceGuid;

                // Allocate all the memory for the final textures
                MaterialInstance.m_lFinalTextures.resize(Graph.m_FinalTextureNodes.m_Textures.size());

                // Set all the textures
                for (auto& E : Graph.m_FinalTextureNodes.m_Textures)
                {
                    const int       Index = static_cast<int>(&E - Graph.m_FinalTextureNodes.m_Textures.data());
                    const auto&     Param = E.m_pNode->m_Params[E.m_iParam];

                    if (Param.m_bCanExpose && Param.m_bExpose )
                    {
                        auto        Ref   = Param.m_Value.get<xresource::full_guid>();
                        auto&       Entry = MaterialInstance.m_lTextureDefaults.emplace_back();

                        // Double check that the file is unique
                        for (auto& O : MaterialInstance.m_lTextureDefaults)
                        {
                            if (O.m_Name == Param.m_ExposeName)
                            {
                                LogMessage(xresource_pipeline::msg_type::ERROR, std::format("Found an Material Texture Expose Parameter duplication [{}]", O.m_Name) );
                                return xerr::create_f<state, "Found an Material Texture Expose Parameter duplication">();
                            }
                        }

                        // Find the input pin ID
                        for ( auto& P : E.m_pNode->m_InputPins)
                        {
                            if ( P.m_ParamIndex == E.m_iParam )
                            {
                                Entry.m_GUID = P.m_PinGUID.m_Value;
                            }
                        }

                        // Set up the entry
                        Entry.m_Name        = Param.m_ExposeName;
                        Entry.m_Index       = Index;
                        

                        // Set the default... let the material instance override it
                        MaterialInstance.m_lFinalTextures[Index].m_TextureRef.m_Instance = Ref.m_Instance;
                    }
                    else
                    {
                        // In the future we need to differentiate between Hardcoded textures and system textures...
                        auto Ref = Param.m_Value.get<xresource::full_guid>();
                        MaterialInstance.m_lFinalTextures[Index].m_TextureRef.m_Instance = Ref.m_Instance;
                    }
                }

                xtextfile::stream Stream;
                if ( auto Err = Stream.Open(false, std::format(L"{}/{}.log/MaterialInstance.txt", m_ProjectPaths.m_ResourcesLogs, m_ResourcePartialPath), xtextfile::file_type::TEXT); Err )
                    return xerr::create_f<state, "Fail to open the file to save the material instance template">(Err);

                xproperty::settings::context C;
                if ( auto Err = xproperty::sprop::serializer::Stream( Stream, MaterialInstance, C); Err )
                    return xerr::create_f<state, "Fail to save the material instance template">(Err);

                displayProgressBar("default material instance", 1.0f);
            }

            //
            // Serialize Final xMaterial
            //
            int Count = 0;
            for (auto& T : m_Target)
            {
                displayProgressBar("Serializing", Count++ / static_cast<float>(m_Target.size()));

                if (T.m_bValid)
                {
                    xserializer::stream Stream;
                    if (auto Err = Stream.Save(T.m_DataPath, MaterialDataFile, xserializer::compression_level::HIGH); Err)
                        return Err;
                }
            }
            displayProgressBar("Serializing", 1);

            //
            // Serialize the actual shader (If it is not a full-shader...)
            //
            if (auto pNode = Graph.findFullShaderNode(); pNode == nullptr)
            {
                std::ofstream out(std::filesystem::path(std::format(L"{}/{}.log/Shader.txt", m_ProjectPaths.m_ResourcesLogs, m_ResourcePartialPath)));
                if (!out) { std::cerr << "Failed to open file for writing.\n";}
                else
                {
                    out << shaderString << "\n";
                    out.close();
                }
            }

            return {};
        }
    };
}

//
// The creation function
//
std::unique_ptr<xmaterial_compiler::instance> xmaterial_compiler::instance::Create(void)
{
    return std::make_unique<implementation>();
}
