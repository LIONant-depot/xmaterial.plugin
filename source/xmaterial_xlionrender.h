#ifndef XMATERIAL_XLIONRENDER_H
#define XMATERIAL_XLIONRENDER_H
#pragma once

// A material's render settings as xLIONRender's pipeline state (instance::CreateMaterial): the one place the two meet
#include "xmaterial_render_settings.h"
#include "dependencies/xLIONRender/src/scene/xlionrender_scene.h"

namespace xmaterial
{
    // DoubleSided: an instance's override (xmaterial_instance::data_file::m_DoubleSided), -1 for the material's own
    inline xlionrender::material_state RenderState(const render_settings& S, int DoubleSided = -1) noexcept
    {
        using blend = xlionrender::material_state::blend;
        constexpr blend Blend[] = { blend::OFF, blend::MASKED, blend::ALPHA, blend::ADD };     // render_settings::blend_mode order
        return
        { .m_Blend          = Blend[static_cast<int>(S.m_BlendMode)]
        , .m_bDoubleSided   = DoubleSided < 0 ? S.m_bDoubleSided : DoubleSided > 0
        , .m_bDepthWrite    = S.WritesDepth()
        , .m_bDepthTest     = S.m_bDepthTest
        , .m_bCastShadows   = S.m_bCastShadows
        , .m_bWireframe     = S.m_bWireframe
        , .m_SortPriority   = S.m_SortPriority
        };
    }
}

#endif
