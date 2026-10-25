#ifndef XMATERIAL_DATA_FILE_H
#define XMATERIAL_DATA_FILE_H

#include "dependencies/xserializer/source/xserializer.h"
#include "plugins/xtexture.plugin/source/xtexture_xgpu_rsc_loader.h"
#include "xmaterial_render_settings.h"

namespace xmaterial
{
    struct data_file
    {
        // v2: second shader, the scene-linear HDR variant for xLIONRender
        // v3: the parameter block: layout hash + byte size + the default block (documentation/material_contract.md section 2). A v2 file (older size and version) is refused by
        //     the serializer and loads as null, exactly like an uncompiled resource; the pipeline recompiles it because the compiler executable is newer.
        // v4: the render settings (xmaterial_render_settings.h) instead of the alpha flag
        // v5: graph materials with textures read them through xLIONRender's texture table (flags::m_bTextureTable): the HDR shader by its set 3,
        //     and its bindless twin (m_pShaderBindless) by the ids after the parameter block
        inline static constexpr auto xserializer_version_v = 5;

        struct flags
        {
            union
            {
                std::uint8_t m_Value;

                struct
                {
                    bool m_bSamplersFrom1:1;    // a graph material: the texture list has no slot-0 entry (xLIONRender's slot 0: the shadow atlas)
                    bool m_bTextureTable:1;     // its HDR shaders read the textures through xLIONRender's texture table (instance::CreateMaterial bTextureTable)
                };
            };
        };

        struct material_basis_entry
        {
            std::array<void*, 2> m_Pipeline;
        };

        //-------------------------------------------------------------------------

        inline          data_file       (void)                          noexcept = default;
        inline          data_file       (xserializer::stream& Steaming) noexcept;

        inline std::span<xrsc::texture_ref>         getTextures     (void) { return { m_pDefaultTextures, (std::size_t)m_nDefaultTextures  }; }

        std::array<material_basis_entry,1>  m_MaterialBasisSlot;

        union
        {
            std::array<void*,2> m_RawData;

            struct
            {
                std::uint32_t*  m_pShader;
                std::uint32_t   m_ShaderSize;
            };
        };

        // The same material compiled without XLION_LDR_OUTPUT: writes scene-linear HDR, the renderer tone maps.
        // ponytail: two variants while editors still draw materials straight to the screen; drop the LDR one after.
        union
        {
            std::array<void*,2> m_RawDataHDR;
            struct
            {
                std::uint32_t*  m_pShaderHDR;
                std::uint32_t   m_ShaderSizeHDR;
            };
        };
        // The HDR variant compiled with XGPU_BINDLESS (a texture-table material: the heaps by id); empty otherwise. The loader creates getShaderHDR()
        // from it on a bindless device
        std::uint32_t*      m_pShaderBindless;
        std::uint32_t       m_ShaderSizeBindless;
        xrsc::texture_ref*  m_pDefaultTextures;
        flags               m_Flags;
        std::uint8_t        m_nDefaultTextures;
        std::uint16_t       m_BlockBytes;               // the material's parameter block, a multiple of 16 (0: the shader reads no parameters)
        float*              m_pDefaultBlock;            // m_BlockBytes/4 floats: every parameter at its default (the material's own instance)
        std::uint64_t       m_LayoutHash;               // xmaterial::Hash of the manifest: a material instance must carry the same
        render_settings     m_Render;                   // its pipeline: blend, culling, depth, shadows (the renderer: xmaterial_xlionrender.h)
    };

    //-------------------------------------------------------------------------

    data_file::data_file(xserializer::stream& Steaming) noexcept
    {
        assert( Steaming.getResourceVersion() == data_file::xserializer_version_v );
    }
}

//-------------------------------------------------------------------------
// serializer
//-------------------------------------------------------------------------
namespace xserializer::io_functions
{
    //-------------------------------------------------------------------------
    template<> inline
    xerr SerializeIO<xmaterial::data_file>(xserializer::stream& Stream, const xmaterial::data_file& Material) noexcept
    {
        xerr Err;
        false
        || (Err = Stream.Serialize(Material.m_MaterialBasisSlot))
        || (Err = Stream.Serialize(Material.m_pShader, Material.m_ShaderSize))
        || (Err = Stream.Serialize(Material.m_ShaderSize))
        || (Err = Stream.Serialize(Material.m_Flags.m_Value))
        || (Err = Stream.Serialize(reinterpret_cast<std::uint64_t* const&>(Material.m_pDefaultTextures), Material.m_nDefaultTextures))
        || (Err = Stream.Serialize(Material.m_nDefaultTextures))
        || (Err = Stream.Serialize(Material.m_pShaderHDR, Material.m_ShaderSizeHDR))
        || (Err = Stream.Serialize(Material.m_ShaderSizeHDR))
        || (Err = Stream.Serialize(Material.m_pDefaultBlock, Material.m_BlockBytes / 4u))
        || (Err = Stream.Serialize(Material.m_BlockBytes))
        || (Err = Stream.Serialize(Material.m_LayoutHash))
        || (Err = Stream.Serialize(*(std::array<std::byte, sizeof(xmaterial::render_settings)>*)&Material.m_Render))
        || (Err = Stream.Serialize(Material.m_pShaderBindless, Material.m_ShaderSizeBindless))
        || (Err = Stream.Serialize(Material.m_ShaderSizeBindless))
        ;

        return Err;
    }

    //-------------------------------------------------------------------------
    template<> inline
    xerr SerializeIO<xmaterial::data_file::material_basis_entry>(xserializer::stream& Stream, const xmaterial::data_file::material_basis_entry& Entry) noexcept
    {
        xerr Err;
        false
        || (Err = Stream.Serialize( *(std::array<std::byte,sizeof(Entry)>*)&Entry))
        ;

        return Err;
    }

}

#endif
