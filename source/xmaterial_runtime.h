#ifndef XMATERIAL_RUNTIME_H
#define XMATERIAL_RUNTIME_H

#include "xmaterial_data_file.h"

namespace xmaterial
{
    struct rt : data_file
    {
        xgpu::shader& getShader()
        {
            static_assert(sizeof(m_RawData) == sizeof(xgpu::shader));
            return reinterpret_cast<xgpu::shader&>(m_RawData);
        }
    };
}

#endif