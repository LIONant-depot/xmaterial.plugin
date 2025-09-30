
#include "xmaterial_xgpu_rsc_loader.h"
#include "xmaterial_runtime.h"
#include "dependencies/xserializer/source/xserializer.h"

//
// We will register the loader
//
inline static auto s_MaterialLoaderRegistration = xresource::loader_registration<xrsc::material_type_guid_v>{};

//------------------------------------------------------------------

xresource::loader< xrsc::material_type_guid_v >::data_type* xresource::loader< xrsc::material_type_guid_v >::Load( xresource::mgr& Mgr, const full_guid& GUID )
{
    auto&           UserData    = Mgr.getUserData<resource_mgr_user_data>();
    std::wstring    Path        = Mgr.getResourcePath(GUID, type_name_v);
    xmaterial::rt*  pMaterial   = nullptr;

    static_assert( sizeof(xmaterial::rt) == sizeof(xmaterial::data_file));

    // Load the xbitmap
    xserializer::stream Stream;
    if (auto Err = Stream.Load(Path, pMaterial); Err)
    {
        assert(false);
    }

    xgpu::shader::setup Setup
    { .m_Type   = xgpu::shader::type::bit::FRAGMENT
    , .m_Sharer = xgpu::shader::setup::raw_data{ std::span{reinterpret_cast<const int32_t*>(pMaterial->m_pShader), (std::size_t)pMaterial->m_ShaderSize } }
    };

    // Must clear the memory before we can recycle it....
    memset(&pMaterial->getShader(), 0, sizeof(pMaterial->getShader()) );

    // OK time to create the shader officially
    if (auto Err = UserData.m_Device.Create(pMaterial->getShader(), Setup); Err)
    {
        assert(false);
    }

    // Return the texture
    return pMaterial;
}

//------------------------------------------------------------------

void xresource::loader< xrsc::material_type_guid_v >::Destroy(xresource::mgr& Mgr, data_type&& Data, const full_guid& GUID)
{
    auto& UserData = Mgr.getUserData<resource_mgr_user_data>();

    // This function should be destorying the shader
  //  UserData.m_Device.Destroy( std::move(Data.getShader()) );

    // Free the bitmap
    xserializer::default_memory_handler_v.Free(xserializer::mem_type{ .m_bUnique = true }, &Data);
}

