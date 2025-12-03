#ifndef XMATERIAL_DATA_FILE_H
#define XMATERIAL_DATA_FILE_H

#include "dependencies/xserializer/source/xserializer.h"
#include "Plugins/xtexture.plugin/source/xtexture_xgpu_rsc_loader.h"

namespace xmaterial
{
    struct data_file
    {
        inline static constexpr auto xserializer_version_v = 1;

        struct flags
        {
            union
            {
                std::uint8_t m_Value;

                struct
                {
                    bool m_bAlpha:1;
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

        xrsc::texture_ref*  m_pDefaultTextures; 
        flags               m_Flags;
        std::uint8_t        m_nDefaultTextures;
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
