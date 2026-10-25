#ifndef XMATERIAL_PARAMS_H
#define XMATERIAL_PARAMS_H
#pragma once

// Material parameters (documentation/material_contract.md sections 1-3): the declarations, the layout manifest and the packer that makes it. Standalone (no graph, no
// resources, no editor) so a test can run it. The material compiler writes the manifest; the editor, the instance compiler and the runtime read it, nothing else computes offsets.
#include "dependencies/xproperty/source/xcore/my_properties.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cfloat>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xmaterial
{
    using param_id = std::uint64_t;                     // guid_generator::Instance64() at promotion; 0 = none. Never reused.

    enum class param_type  : std::uint8_t { FLOAT, FLOAT2, FLOAT3, FLOAT4, COLOR, TEXTURE };
    enum class param_kind  : std::uint8_t { DYNAMIC, STATIC /*reserved: switches*/ };
    enum class param_stage : std::uint8_t { FRAGMENT = 1, VERTEX = 2 /*reserved*/ };
    enum class entity_field: std::uint8_t { NONE, TINT, PARAM0, PARAM1 };

    inline static constexpr auto param_type_enum_v = std::array
    { xproperty::settings::enum_item{ "FLOAT",   param_type::FLOAT   }
    , xproperty::settings::enum_item{ "FLOAT2",  param_type::FLOAT2  }
    , xproperty::settings::enum_item{ "FLOAT3",  param_type::FLOAT3  }
    , xproperty::settings::enum_item{ "FLOAT4",  param_type::FLOAT4  }
    , xproperty::settings::enum_item{ "COLOR",   param_type::COLOR   }
    , xproperty::settings::enum_item{ "TEXTURE", param_type::TEXTURE }
    };
    inline static constexpr auto param_kind_enum_v = std::array
    { xproperty::settings::enum_item{ "DYNAMIC", param_kind::DYNAMIC }
    , xproperty::settings::enum_item{ "STATIC",  param_kind::STATIC  }
    };
    inline static constexpr auto entity_field_enum_v = std::array
    { xproperty::settings::enum_item{ "NONE",   entity_field::NONE   }
    , xproperty::settings::enum_item{ "TINT",   entity_field::TINT   }
    , xproperty::settings::enum_item{ "PARAM0", entity_field::PARAM0 }
    , xproperty::settings::enum_item{ "PARAM1", entity_field::PARAM1 }
    };

    struct float4v                                      // xproperty has no fvec4: four named floats
    {
        float m_X = 0, m_Y = 0, m_Z = 0, m_W = 0;

        XPROPERTY_DEF
        ( "float4v", float4v
        , obj_member<"X", &float4v::m_X>
        , obj_member<"Y", &float4v::m_Y>
        , obj_member<"Z", &float4v::m_Z>
        , obj_member<"W", &float4v::m_W>
        )
    };
    XPROPERTY_REG(float4v)

    struct param_decl                                   // lives in the graph file, one per parameter
    {
        param_id        m_ID            = 0;
        std::string     m_Name;                         // display + carry-over fallback; renaming never touches instances
        param_type      m_Type          = param_type::FLOAT;
        param_kind      m_Kind          = param_kind::DYNAMIC;
        std::uint8_t    m_Stages        = std::uint8_t(param_stage::FRAGMENT);
        float4v         m_Default;                      // linear. TEXTURE: unused
        xresource::full_guid m_DefaultTexture;          // TEXTURE only
        float4v         m_ValidMin  { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };    // clamped at resolve. Unbounded: +-FLT_MAX
        float4v         m_ValidMax  {  FLT_MAX,  FLT_MAX,  FLT_MAX,  FLT_MAX };
        float4v         m_UiMin, m_UiMax;               // slider range only; values outside it are legal
        bool            m_bHDR          = false;        // COLOR: rgb may exceed 1
        bool            m_bAlpha        = true;         // COLOR: w is meaningful
        entity_field    m_EntityField   = entity_field::NONE;   // "allow entity override": which schema field may replace it
        std::string     m_Group;                        // inspector grouping
        std::int32_t    m_Order         = 0;            // where an instance's inspector lists it: lower first, ties as declared (a value right under its texture)

        XPROPERTY_DEF
        ( "param_decl", param_decl
        , obj_member<"ID",              &param_decl::m_ID>
        , obj_member<"Name",            &param_decl::m_Name>
        , obj_member<"Type",            &param_decl::m_Type,        member_enum_span<param_type_enum_v>>
        , obj_member<"Kind",            &param_decl::m_Kind,        member_enum_span<param_kind_enum_v>>
        , obj_member<"Stages",          &param_decl::m_Stages>
        , obj_member<"Default",         &param_decl::m_Default>
        , obj_member<"DefaultTexture",  &param_decl::m_DefaultTexture>
        , obj_member<"ValidMin",        &param_decl::m_ValidMin>
        , obj_member<"ValidMax",        &param_decl::m_ValidMax>
        , obj_member<"UiMin",           &param_decl::m_UiMin>
        , obj_member<"UiMax",           &param_decl::m_UiMax>
        , obj_member<"bHDR",            &param_decl::m_bHDR>
        , obj_member<"bAlpha",          &param_decl::m_bAlpha>
        , obj_member<"EntityField",     &param_decl::m_EntityField, member_enum_span<entity_field_enum_v>>
        , obj_member<"Group",           &param_decl::m_Group>
        , obj_member<"Order",           &param_decl::m_Order>
        )
    };
    XPROPERTY_REG(param_decl)

    struct manifest_entry
    {
        param_id        m_ID            = 0;
        param_type      m_Type          = param_type::FLOAT;
        std::uint16_t   m_ByteOffset    = 0;            // into the block; TEXTURE: 0, the sampler slot is m_Sampler
        std::uint8_t    m_Sampler       = 0;            // TEXTURE only: 1..7 (0 is not a parameter sampler)
        entity_field    m_EntityField   = entity_field::NONE;
        std::uint8_t    m_Stages        = std::uint8_t(param_stage::FRAGMENT);

        XPROPERTY_DEF
        ( "manifest_entry", manifest_entry
        , obj_member<"ID",          &manifest_entry::m_ID>
        , obj_member<"Type",        &manifest_entry::m_Type,        member_enum_span<param_type_enum_v>>
        , obj_member<"ByteOffset",  &manifest_entry::m_ByteOffset>
        , obj_member<"Sampler",     &manifest_entry::m_Sampler>
        , obj_member<"EntityField", &manifest_entry::m_EntityField, member_enum_span<entity_field_enum_v>>
        , obj_member<"Stages",      &manifest_entry::m_Stages>
        )
    };
    XPROPERTY_REG(manifest_entry)

    struct manifest
    {
        int                         m_Version       = 1;
        std::uint64_t               m_LayoutHash    = 0;
        std::uint16_t               m_BlockBytes    = 0;
        std::vector<manifest_entry> m_Entries;          // packed block first (by size, then ID), then the textures (by ID)
        std::vector<param_decl>     m_Decls;            // defaults, ranges: every declaration, used or not

        XPROPERTY_DEF
        ( "manifest", manifest
        , obj_member<"Version",     &manifest::m_Version>
        , obj_member<"LayoutHash",  &manifest::m_LayoutHash>
        , obj_member<"BlockBytes",  &manifest::m_BlockBytes>
        , obj_member<"Entries",     &manifest::m_Entries>
        , obj_member<"Decls",       &manifest::m_Decls>
        )
    };

    // The order the inspectors list a material's parameters in (the Material Instance editor; a Mesh slot's rows, through the compiled
    // parameter list, which is written in it): the declaration's Order, lower first, ties as declared; a parameter without a declaration last
    inline std::pair<std::int32_t, std::ptrdiff_t> InspectorOrder( const param_decl* pDecl, std::span<const param_decl> Decls ) noexcept
    {
        return pDecl ? std::pair{ pDecl->m_Order, pDecl - Decls.data() } : std::pair{ std::int32_t(INT32_MAX), std::ptrdiff_t(Decls.size()) };
    }
    XPROPERTY_REG(manifest)

    // FNV-1a 64 over (ID, type, offset, entity field) of every entry in manifest order, plus the block size. A TEXTURE hashes its sampler where the offset would go (a
    // sampler swap breaks readers just as an offset does). Names, defaults and ranges are not in it: a rename or a default edit must not invalidate instances.
    inline std::uint64_t Hash(const manifest& M)
    {
        std::uint64_t H = 14695981039346656037ull;
        auto Mix = [&](std::uint64_t V, int nBytes) { for (int i = 0; i < nBytes; ++i) { H ^= (V >> (8 * i)) & 0xff; H *= 1099511628211ull; } };
        for (const auto& E : M.m_Entries)
        {
            Mix(E.m_ID, 8);
            Mix(std::uint8_t(E.m_Type), 1);
            Mix(E.m_Type == param_type::TEXTURE ? E.m_Sampler : E.m_ByteOffset, 2);
            Mix(std::uint8_t(E.m_EntityField), 1);
        }
        Mix(M.m_BlockBytes, 2);
        return H;
    }

    // Builds the manifest (section 2 packing). Used: the IDs some connected input reads; a declaration not in it gets no storage (the caller reports W_UNUSED), nor does
    // a STATIC one. Samplers are handed 1.. in ID order [ponytail: the graph decides sampler order today, the compiler overwrites m_Sampler if it must differ, then Hash() again].
    inline manifest Pack(const std::vector<param_decl>& Decls, const std::vector<param_id>& Used)
    {
        manifest M;
        M.m_Decls = Decls;

        std::vector<const param_decl*> Block, Tex;
        for (const auto& D : Decls)
        {
            if (D.m_Kind != param_kind::DYNAMIC || std::find(Used.begin(), Used.end(), D.m_ID) == Used.end()) continue;
            (D.m_Type == param_type::TEXTURE ? Tex : Block).push_back(&D);
        }

        auto Bytes = [](param_type T) { switch (T) { case param_type::FLOAT: return 4; case param_type::FLOAT2: return 8; case param_type::FLOAT3: return 12; default: return 16; } };
        auto Class = [](param_type T) { return T == param_type::COLOR ? param_type::FLOAT4 : T; };      // COLOR packs as FLOAT4
        std::sort(Block.begin(), Block.end(), [&](auto* A, auto* B) { return Class(A->m_Type) != Class(B->m_Type) ? Class(A->m_Type) > Class(B->m_Type) : A->m_ID < B->m_ID; });
        std::sort(Tex.begin(),   Tex.end(),   [](auto* A, auto* B) { return A->m_ID < B->m_ID; });

        // First fit on 4 B words: FLOAT3/FLOAT4 start on 16 B, FLOAT2 on 8 B, FLOAT on 4 B, never straddling a 16 B line (so a FLOAT fills a FLOAT3's .w)
        std::vector<bool> Used4;
        int               End = 0;
        for (const auto* D : Block)
        {
            const int Size = Bytes(D->m_Type), Align = Size == 12 ? 16 : Size;
            int       At   = 0;
            for (;; At += Align)
            {
                bool bFree = true;
                for (int w = At / 4; w < (At + Size) / 4 && bFree; ++w) bFree = w >= int(Used4.size()) || !Used4[w];
                if (bFree) break;
            }
            if (int(Used4.size()) < (At + Size) / 4) Used4.resize((At + Size) / 4, false);
            for (int w = At / 4; w < (At + Size) / 4; ++w) Used4[w] = true;
            End = std::max(End, At + Size);
            M.m_Entries.push_back({ D->m_ID, D->m_Type, std::uint16_t(At), 0, D->m_EntityField, D->m_Stages });
        }

        std::uint8_t iSampler = 1;
        for (const auto* D : Tex)
            M.m_Entries.push_back({ D->m_ID, D->m_Type, 0, iSampler++, D->m_EntityField, D->m_Stages });

        M.m_BlockBytes = std::uint16_t((End + 15) & ~15);
        M.m_LayoutHash = Hash(M);
        return M;
    }

    // The self-check of the packer (acceptance test 1). Run by plugins/xmaterial.plugin/source/xmaterial_params_test.cpp
    inline void demo()
    {
        auto D = [](param_id ID, const char* N, param_type T) { param_decl P; P.m_ID = ID; P.m_Name = N; P.m_Type = T; return P; };

        // COLOR at 0; FLOAT3 and FLOAT after it: the FLOAT fills the FLOAT3's .w (offset 28); sorted by size then ID, not by declaration or name
        auto M = Pack({ D(5,"Rough",param_type::FLOAT), D(3,"Pos",param_type::FLOAT3), D(9,"Tint",param_type::COLOR), D(7,"Unused",param_type::FLOAT4), D(2,"Albedo",param_type::TEXTURE) }
                     , { 5, 3, 9, 2 });
        assert(M.m_Entries.size() == 4);
        assert(M.m_Entries[0].m_ID == 9 && M.m_Entries[0].m_ByteOffset == 0);     // COLOR
        assert(M.m_Entries[1].m_ID == 3 && M.m_Entries[1].m_ByteOffset == 16);    // FLOAT3
        assert(M.m_Entries[2].m_ID == 5 && M.m_Entries[2].m_ByteOffset == 28);    // FLOAT in the FLOAT3's .w
        assert(M.m_Entries[3].m_ID == 2 && M.m_Entries[3].m_Sampler == 1);        // TEXTURE: no block storage
        assert(M.m_BlockBytes == 32 && M.m_Decls.size() == 5);                     // the unused FLOAT4 got nothing

        // The slice material: FLOAT3 + FLOAT -> FLOAT at 12
        auto S = Pack({ D(1,"Tint",param_type::FLOAT3), D(2,"Roughness",param_type::FLOAT) }, { 1, 2 });
        assert(S.m_Entries[1].m_ByteOffset == 12 && S.m_BlockBytes == 16);

        // FLOAT2s and FLOATs: 8 B alignment, a FLOAT never straddles a line
        auto F = Pack({ D(1,"A",param_type::FLOAT2), D(2,"B",param_type::FLOAT2), D(3,"C",param_type::FLOAT2), D(4,"D",param_type::FLOAT) }, { 1, 2, 3, 4 });
        assert(F.m_Entries[2].m_ByteOffset == 16 && F.m_Entries[3].m_ByteOffset == 24 && F.m_BlockBytes == 32);

        // Empty block is 0 bytes
        assert(Pack({ D(1,"A",param_type::TEXTURE) }, { 1 }).m_BlockBytes == 0 && Pack({ D(1,"A",param_type::FLOAT) }, {}).m_BlockBytes == 0);

        // Hash: stable across a rename (and a default edit), changes on retype, entity field, or a different packing
        auto Names = std::vector{ D(1,"Tint",param_type::FLOAT3), D(2,"Roughness",param_type::FLOAT) };
        const auto H0 = Pack(Names, { 1, 2 }).m_LayoutHash;
        Names[0].m_Name = "Renamed"; Names[0].m_Default.m_X = 0.7f;
        assert(Pack(Names, { 1, 2 }).m_LayoutHash == H0);
        Names[0].m_Type = param_type::COLOR;
        assert(Pack(Names, { 1, 2 }).m_LayoutHash != H0);
        Names[0].m_Type = param_type::FLOAT3; Names[0].m_EntityField = entity_field::TINT;
        assert(Pack(Names, { 1, 2 }).m_LayoutHash != H0);
    }
}

#endif // XMATERIAL_PARAMS_H
