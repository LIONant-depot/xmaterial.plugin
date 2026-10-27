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

        // Scene-linear HDR output, for xLIONRender (getShader() is the tone-mapped LDR one)
        xgpu::shader& getShaderHDR()
        {
            static_assert(sizeof(m_RawDataHDR) == sizeof(xgpu::shader));
            return reinterpret_cast<xgpu::shader&>(m_RawDataHDR);
        }

        // The G-buffer variant, for xLIONRender's thin G-buffer (an empty shader: none - see data_file::m_pShaderGBuffer)
        xgpu::shader& getShaderGBuffer()
        {
            static_assert(sizeof(m_RawDataGBuffer) == sizeof(xgpu::shader));
            return reinterpret_cast<xgpu::shader&>(m_RawDataGBuffer);
        }

        xgpu::pipeline& getPipeline(int MaterialBasisIndex)
        {
            static_assert(sizeof(material_basis_entry) == sizeof(xgpu::pipeline));
            return reinterpret_cast<xgpu::pipeline&>(m_MaterialBasisSlot[MaterialBasisIndex].m_Pipeline);
        }
    };
}

#endif