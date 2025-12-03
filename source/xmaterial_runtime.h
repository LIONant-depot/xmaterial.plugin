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

        xgpu::pipeline& getPipeline(int MaterialBasisIndex)
        {
            static_assert(sizeof(material_basis_entry) == sizeof(xgpu::pipeline));
            return reinterpret_cast<xgpu::pipeline&>(m_MaterialBasisSlot[MaterialBasisIndex].m_Pipeline);
        }
    };
}

#endif