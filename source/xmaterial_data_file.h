#ifndef XMATERIAL_DATA_FILE_H
#define XMATERIAL_DATA_FILE_H

#include "dependencies/xserializer/source/xserializer.h"

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

        //-------------------------------------------------------------------------

        inline          data_file       (void)                          noexcept = default;
        inline          data_file       (xserializer::stream& Steaming) noexcept;

        union
        {
            std::array<void*,2> m_RawData;

            struct
            {
                std::uint32_t*  m_pShader;
                std::uint32_t   m_ShaderSize;
            };
        };

        flags           m_Flags;
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
    template<>
    xerr SerializeIO<xmaterial::data_file>(xserializer::stream& Stream, const xmaterial::data_file& Material) noexcept
    {
        xerr Err;
        false
        || (Err = Stream.Serialize(Material.m_pShader, Material.m_ShaderSize))
        || (Err = Stream.Serialize(Material.m_ShaderSize))
        || (Err = Stream.Serialize(Material.m_Flags.m_Value))
        ;

        return Err;
    }
}

#endif
