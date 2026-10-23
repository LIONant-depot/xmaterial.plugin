#ifndef XMATERIAL_THUMBNAIL_H
#define XMATERIAL_THUMBNAIL_H
#pragma once

// Material's thumbnail renderer: the material (its default parameters and textures) on the material ball, drawn by the same mesh_preview the
// Material editor's preview panel uses, headless into a small colour+depth target of its own, then read back. Done the way
// xmaterial_instance_thumbnail.h does it (an instance is drawn on a sphere, so the two read apart).
#include "source/Tools/Editor/xeditor_thumbnail.h"
#include "source/Tools/Editor/xeditor_mesh_preview.h"
#include "plugins/xmaterial.plugin/source/xmaterial_xgpu_rsc_loader.h"
#include "plugins/xmaterial.plugin/source/xmaterial_runtime.h"

#include <list>
#include <unordered_map>

namespace xmaterial_editor
{
    class thumbnail_renderer final : public xeditor::thumbnail_renderer
    {
    public:
        static constexpr int s_CellPixels = 128;   // matches xeditor_thumbnail_cache::s_CellPixels

        bool Init(xgpu::device& Device) noexcept override
        {
            if (m_bReady) return true;
            if (!m_Preview.Init(Device)) return false;
            m_Preview.m_Model = e19::mesh_manager::model::MATERIAL_BALL;
            m_Preview.Camera().m_pAngles->m_Pitch = xmath::radian{ xmath::DegToRad(-25.0f) };      // a little from above (negative: above), the bowl facing the camera

            if (!Ok(Device.Create(m_ColorTarget, { .m_Format = xgpu::texture::format::R8G8B8A8_UNORM, .m_Width = s_CellPixels, .m_Height = s_CellPixels, .m_isGamma = false }))) return false;
            if (!Ok(Device.Create(m_DepthTarget, { .m_Format = xgpu::texture::format::DEPTH_F32, .m_Width = s_CellPixels, .m_Height = s_CellPixels, .m_isGamma = false }))) return false;
            std::array<xgpu::renderpass::attachment, 2> Attachments{ { { m_ColorTarget }, { m_DepthTarget } } };
            if (!Ok(Device.Create(m_Pass, { .m_Attachments = Attachments, .m_ClearColorR = 0, .m_ClearColorG = 0, .m_ClearColorB = 0, .m_ClearColorA = 0 }))) return false;

            m_bReady = true;
            return true;
        }

        bool Render(xgpu::device& Device, xgpu::window& Window, xresource::full_guid Guid, xbitmap& OutBitmap) noexcept override
        {
            if (!m_bReady) return false;

            if (m_bBusy)
            {
                if (m_BusyGuid != Guid) return false;              // busy with someone else - try again later
                if (!m_bReadbackDone) return false;                // still waiting for the GPU
                m_bBusy = false;
                if (m_ReadbackWidth != s_CellPixels || m_ReadbackHeight != s_CellPixels) return false;
                OutBitmap.CreateBitmap(s_CellPixels, s_CellPixels);
                auto Dst = OutBitmap.getMip<xcolori>(0);
                std::memcpy(Dst.data(), m_ReadbackPixels.data(), Dst.size() * sizeof(xcolori));
                return true;
            }

            // The material and its default textures (a texture not loaded yet draws as the preview's default texture)
            auto* pMaterial = Reference(Guid);
            if (!pMaterial) return false;

            std::vector<xgpu::pipeline_instance::sampler_binding> Bindings;
            Bindings.reserve(pMaterial->m_nDefaultTextures);
            for (auto& Entry : pMaterial->getTextures())
            {
                if (auto* pTexture = Entry.empty() ? nullptr : xresource::g_Mgr.getResource(Entry)) Bindings.emplace_back(*pTexture);
                else                                                                                 Bindings.emplace_back(m_Preview.DefaultTexture());
            }
            if (!m_Preview.SetMaterial(pMaterial->getShader(), Bindings, { pMaterial->m_pDefaultBlock, std::size_t(pMaterial->m_BlockBytes) / 4 })) return false;

            {
                auto CmdBuffer = Window.StartRenderPass(m_Pass);         // its destructor ends the pass, before the readback
                m_Preview.Draw(CmdBuffer, static_cast<float>(s_CellPixels), static_cast<float>(s_CellPixels), false);
            }

            (void)Window.ReadbackTexture(m_ColorTarget, m_ReadbackPixels, m_ReadbackWidth, m_ReadbackHeight, m_bReadbackDone);
            m_bBusy    = true;
            m_BusyGuid = Guid;
            return false;
        }

    private:
        static bool Ok(xgpu::device::error* pErr) noexcept
        {
            if (!pErr) return true;
            std::printf("Material thumbnail: %s\n", std::string(xgpu::getErrorMsg(pErr)).c_str());
            return false;
        }

        // Keeps the material loaded for a little while after its last request: what is drawn now is read back several frames later
        xmaterial::rt* Reference(const xresource::full_guid& Guid) noexcept
        {
            if (auto It = m_Refs.find(Guid); It != m_Refs.end())
            {
                m_Order.remove(Guid);
                m_Order.push_front(Guid);
                return xresource::g_Mgr.getResource(It->second);
            }

            xrsc::material_ref Ref;
            Ref.m_Instance = Guid.m_Instance;
            auto& Held = m_Refs.emplace(Guid, Ref).first->second;
            m_Order.push_front(Guid);
            while (m_Refs.size() > m_Capacity)
            {
                const auto Oldest = m_Order.back();
                if (auto It = m_Refs.find(Oldest); It != m_Refs.end()) { xresource::g_Mgr.ReleaseRef(It->second); m_Refs.erase(It); }
                m_Order.pop_back();
            }
            return xresource::g_Mgr.getResource(Held);
        }

        xeditor::mesh_preview                                       m_Preview;
        xgpu::texture                                               m_ColorTarget;
        xgpu::texture                                               m_DepthTarget;
        xgpu::renderpass                                            m_Pass;
        bool                                                        m_bReady = false;

        bool                                                        m_bBusy          = false;
        xresource::full_guid                                        m_BusyGuid       {};
        std::vector<std::uint32_t>                                  m_ReadbackPixels;
        int                                                         m_ReadbackWidth  = 0;
        int                                                         m_ReadbackHeight = 0;
        bool                                                        m_bReadbackDone  = false;

        std::size_t                                                 m_Capacity = 20;
        std::unordered_map<xresource::full_guid, xrsc::material_ref> m_Refs;
        std::list<xresource::full_guid>                             m_Order;
    };

    inline const xeditor::auto_register_thumbnail_renderer g_ThumbReg{ xrsc::material_type_guid_v, []{ return std::make_unique<thumbnail_renderer>(); } };
}

#endif // XMATERIAL_THUMBNAIL_H
