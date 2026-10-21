// Standalone self-check of xmaterial_params.h (not in any CMakeLists). Build from the xLION root with /std:c++latest /I. so "dependencies/..." resolves.
#include "dependencies/xproperty/source/xcore/my_properties.cpp"
#include "dependencies/xtextfile/source/xtextfile.h"
#include "dependencies/xproperty/source/sprop/property_sprop_xtextfile_serializer.h"
#include "plugins/xmaterial.plugin/source/xmaterial_params.h"
#include <cstdio>
#include <filesystem>

int main()
{
    xmaterial::demo();

    // Text round trip of a manifest through xproperty
    xmaterial::param_decl D; D.m_ID = 0x6E; D.m_Name = "Tint"; D.m_Type = xmaterial::param_type::COLOR; D.m_EntityField = xmaterial::entity_field::TINT;
    auto M = xmaterial::Pack({ D }, { 0x6E });
    const auto Path = (std::filesystem::temp_directory_path() / L"MaterialLayout.txt").wstring();
    xproperty::settings::context C{};
    {
        xtextfile::stream F;
        assert(!F.Open(false, Path, xtextfile::file_type::TEXT));
        assert(!xproperty::sprop::serializer::Stream(F, M, C));
    }
    xmaterial::manifest R;
    {
        xtextfile::stream F;
        assert(!F.Open(true, Path, xtextfile::file_type::TEXT));
        assert(!xproperty::sprop::serializer::Stream(F, R, C));
    }
    assert(R.m_LayoutHash == M.m_LayoutHash && R.m_BlockBytes == 16 && R.m_Entries.size() == 1 && R.m_Decls[0].m_Name == "Tint" && xmaterial::Hash(R) == M.m_LayoutHash);

    std::puts("xmaterial_params: all asserts passed");
    return 0;
}
