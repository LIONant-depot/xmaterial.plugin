#ifndef XMATERIAL_RENDER_SETTINGS_H
#define XMATERIAL_RENDER_SETTINGS_H
#pragma once

// A material's render settings (documentation/render_settings_plan.md): what makes its pipeline, so they belong to the material, never to an
// instance. Edited on the output node, stored once in the graph file (v4) and copied as they are into the compiled material (data_file v4).
// Standalone (no graph, no resources) like xmaterial_params.h. Trivially copyable: the data file stores its bytes.
#include "dependencies/xproperty/source/xcore/my_properties.h"
#include <array>
#include <cstdint>
#include <type_traits>

namespace xmaterial
{
    struct render_settings
    {
        enum class blend_mode  : std::uint8_t { SOLID,  /* "Opaque" (OPAQUE is a wingdi.h macro) */  MASKED, TRANSLUCENT, ADDITIVE };
        enum class back_normal : std::uint8_t { FLIP, NONE };          // a double sided material's back faces: their normal flipped, or lit as the front
        enum class depth_write : std::uint8_t { AUTO, ON, OFF };       // AUTO: on for SOLID and MASKED, off for TRANSLUCENT and ADDITIVE

        blend_mode      m_BlendMode     = blend_mode::SOLID;
        back_normal     m_BackNormal    = back_normal::FLIP;
        depth_write     m_DepthWrite    = depth_write::AUTO;
        bool            m_bDoubleSided  = false;
        bool            m_bDepthTest    = true;
        bool            m_bCastShadows  = true;
        bool            m_bWireframe    = false;                        // debug
        std::int8_t     m_SortPriority  = 0;                            // TRANSLUCENT, ADDITIVE: drawn back to front within one priority, lower first
        float           m_AlphaCutoff   = 0.5f;                         // MASKED: the default of its Alpha Cutoff parameter (instances may change it)

        constexpr bool  isBlended   (void) const noexcept { return m_BlendMode == blend_mode::TRANSLUCENT || m_BlendMode == blend_mode::ADDITIVE; }
        constexpr bool  WritesDepth (void) const noexcept { return m_DepthWrite == depth_write::AUTO ? !isBlended() : m_DepthWrite == depth_write::ON; }
        constexpr bool  operator == (const render_settings&) const noexcept = default;

        inline static constexpr auto blend_mode_enum_v = std::array
        { xproperty::settings::enum_item{ "Opaque",      blend_mode::SOLID      }
        , xproperty::settings::enum_item{ "Masked",      blend_mode::MASKED      }
        , xproperty::settings::enum_item{ "Translucent", blend_mode::TRANSLUCENT }
        , xproperty::settings::enum_item{ "Additive",    blend_mode::ADDITIVE    }
        };
        inline static constexpr auto back_normal_enum_v = std::array
        { xproperty::settings::enum_item{ "Flip",        back_normal::FLIP       }
        , xproperty::settings::enum_item{ "None",        back_normal::NONE       }
        };
        inline static constexpr auto depth_write_enum_v = std::array
        { xproperty::settings::enum_item{ "Auto",        depth_write::AUTO       }
        , xproperty::settings::enum_item{ "On",          depth_write::ON         }
        , xproperty::settings::enum_item{ "Off",         depth_write::OFF        }
        };

        XPROPERTY_DEF
        ( "render_settings", render_settings
        , obj_member<"Blend Mode",       &render_settings::m_BlendMode,    member_enum_span<blend_mode_enum_v>,  member_help<"Opaque, Masked (alpha below the cutoff is cut out), Translucent (alpha blended), Additive">>
        , obj_member<"Alpha Cutoff",     &render_settings::m_AlphaCutoff,  member_ui<float>::drag_bar<0.005f, 0.0f, 1.0f>, member_help<"Masked: alpha below this is cut out (a material parameter: instances may change it)">>
        , obj_member<"Double Sided",     &render_settings::m_bDoubleSided, member_help<"Back faces are drawn too (a pipeline without culling)">>
        , obj_member<"Back Face Normal", &render_settings::m_BackNormal,   member_enum_span<back_normal_enum_v>, member_help<"Double sided: Flip lights a back face by its own side, None lights it as the front (foliage cards)">>
        , obj_member<"Depth Write",      &render_settings::m_DepthWrite,   member_enum_span<depth_write_enum_v>, member_help<"Auto: on for Opaque and Masked, off for Translucent and Additive">>
        , obj_member<"Depth Test",       &render_settings::m_bDepthTest>
        , obj_member<"Cast Shadows",     &render_settings::m_bCastShadows>
        , obj_member<"Sort Priority",    &render_settings::m_SortPriority, member_help<"Translucent and Additive: lower priorities are drawn first, back to front within one">>
        , obj_member<"Wireframe",        &render_settings::m_bWireframe,   member_help<"Debug: draws the triangles' edges">>
        )
    };
    XPROPERTY_REG(render_settings)

    static_assert(std::is_trivially_copyable_v<render_settings> && sizeof(render_settings) == 12);
}

#endif
