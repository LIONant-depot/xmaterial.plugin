#ifndef XMATERIAL_GRAPH_NODE_H
#define XMATERIAL_GRAPH_NODE_H
#pragma once

#ifdef EDITOR
    #include "source/Examples/E10_TextureResourcePipeline/E10_AssetMgr.h"
#endif

#include "xmaterial_graph_node_guid.h"
#include "source/xtexture_xgpu_rsc_loader.h"

struct any_friend : xproperty::any
{
    enum class type :std::uint8_t
    { NONE
    , FLOAT
    , INT
    , STRING
    , TEXTURE_RESOURCE
    , UNKNOWN 
    };

    inline static constexpr auto type_enum_v = std::array
    { xproperty::settings::enum_item{ "NONE",               type::NONE              }
    , xproperty::settings::enum_item{ "FLOAT",              type::FLOAT             }
    , xproperty::settings::enum_item{ "INT",                type::INT               }
    , xproperty::settings::enum_item{ "STRING",             type::STRING            }
    , xproperty::settings::enum_item{ "TEXTURE_RESOURCE",   type::TEXTURE_RESOURCE  }
    , xproperty::settings::enum_item{ "UNKNOWN",            type::UNKNOWN           }
    };

    public:
    inline static auto PropertiesDefinition()
    {
        assert(false);
        using namespace xproperty;
        using namespace xproperty::settings;
        return xproperty::def<"Any", xproperty::any, obj_member < "Type", +[](xproperty::any& O, bool bRead, type& Enum)
        {
            if (bRead)
            {
                if (O.m_pType == nullptr) Enum = type::NONE;
                else switch (O.m_pType->m_GUID)
                {
                case xproperty::settings::var_type<float>::guid_v:                  Enum = type::FLOAT; break;
                case xproperty::settings::var_type<int>::guid_v:                    Enum = type::INT; break;
                case xproperty::settings::var_type<std::string>::guid_v:            Enum = type::STRING; break;
                case xproperty::settings::var_type<xresource::full_guid>::guid_v:   Enum = type::TEXTURE_RESOURCE; break;
                default: Enum = type::UNKNOWN; break;
                }
            }
            else
            {
                switch (Enum)
                {
                default:
                case type::NONE:                O = {};                             break;
                case type::FLOAT:               O.set<float>(0);                    break;
                case type::INT:                 O.set<int>(0);                      break;
                case type::STRING:              O.set<std::string>("");             break;
                case type::TEXTURE_RESOURCE:    O.set<xresource::full_guid>({});    break;
                }
            }
        }
        , member_enum_span<type_enum_v >>
            , obj_member < "Float", +[](xproperty::any& Any, bool bRead, float& Value)
            {
                if (!Any.m_pType || Any.m_pType->m_GUID != xproperty::settings::var_type<float>::guid_v) return;
                if (bRead) Value = Any.get<float>();
                else      Any.set<float>(Value);
            }
            , member_dynamic_flags < +[](const xproperty::any& Any)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly = false;
                Flags.m_bDontShow = Any.m_pType == nullptr || Any.m_pType->m_GUID != xproperty::settings::var_type<float>::guid_v;
                Flags.m_bDontSave = Any.m_pType == nullptr || Any.m_pType->m_GUID != xproperty::settings::var_type<float>::guid_v;
                return Flags;
            } >>
        , obj_member < "Int", +[](xproperty::any& Any, bool bRead, int& Value)
            {
                if (!Any.m_pType || Any.m_pType->m_GUID != xproperty::settings::var_type<int>::guid_v) return;
                if (bRead) Value = Any.get<int>();
                else      Any.set<int>(Value);
            }
            , member_dynamic_flags < +[](const xproperty::any& Any)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly = false;
                Flags.m_bDontShow = Any.m_pType == nullptr || Any.m_pType->m_GUID != xproperty::settings::var_type<int>::guid_v;
                Flags.m_bDontSave = Any.m_pType == nullptr || Any.m_pType->m_GUID != xproperty::settings::var_type<int>::guid_v;
                return Flags;
            } >>
        , obj_member < "String", +[](xproperty::any& Any, bool bRead, std::string& Value)
            {
                if (!Any.m_pType || Any.m_pType->m_GUID != xproperty::settings::var_type<std::string>::guid_v) return;
                if (bRead) Value = Any.get<std::string>();
                else      Any.set<std::string>(Value);
            }
            , member_dynamic_flags < +[](const xproperty::any& Any)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly = false;
                Flags.m_bDontShow = Any.m_pType == nullptr || Any.m_pType->m_GUID != xproperty::settings::var_type<std::string>::guid_v;
                Flags.m_bDontSave = Any.m_pType == nullptr || Any.m_pType->m_GUID != xproperty::settings::var_type<std::string>::guid_v;
                return Flags;
            } >>
        , obj_member < "TextureResource", +[](xproperty::any& Any, bool bRead, xresource::full_guid& FullGuid)
            {
                if (!Any.m_pType || Any.m_pType->m_GUID != xproperty::settings::var_type<int>::guid_v) return;
                if (bRead) FullGuid = Any.get<xresource::full_guid>();
                else       Any.set<xresource::full_guid>(FullGuid);
            }
            , member_dynamic_flags < +[](const xproperty::any& Any)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly = false;
                Flags.m_bDontShow = Any.m_pType == nullptr || Any.m_pType->m_GUID != xproperty::settings::var_type<std::uint64_t>::guid_v;
                Flags.m_bDontSave = Any.m_pType == nullptr || Any.m_pType->m_GUID != xproperty::settings::var_type<std::uint64_t>::guid_v;
                return Flags;
            } >>
        > ();
    }
};
XPROPERTY_REG(any_friend)

struct prop_friend : xproperty::sprop::container::prop
{
    XPROPERTY_DEF
    ("Props", xproperty::sprop::container::prop
        , obj_member<"Name", &xproperty::sprop::container::prop::m_Path>
        , obj_member<"Value", &xproperty::sprop::container::prop::m_Value >
    )
};
XPROPERTY_REG(prop_friend)

struct container_friend : xproperty::sprop::container
{
    XPROPERTY_DEF
    ("Container", xproperty::sprop::container
        , obj_member<"Props", &xproperty::sprop::container::m_Properties>
    )
};
XPROPERTY_REG(container_friend)

namespace xmaterial_compiler
{
    struct node //serialize instance guid ,type guid and position putting graph -> shader
    {
        node_guid                       m_Guid              = {};  //instance guid
        node_guid                       m_PrefabGuid        = {};  //prefab id
        std::string                     m_Name              = {};
        std::string                     m_Code              = {};
        std::vector<input_pin>          m_InputPins         = {};
        std::vector<output_pin>         m_OutputPins        = {};
        xmath::fvec2                    m_Pos               = {};
        xproperty::sprop::container     m_Params            = {};
        bool                            m_HasErrMsg         = { false };
        std::string                     m_ErrMsg            = {};
        

        inline bool isCommentNode       (void) const { return m_OutputPins.empty() && m_InputPins.empty(); }
        inline bool isOutputNode        (void) const { return m_OutputPins.empty() && !isCommentNode(); }
        inline bool isInputNode         (void) const { return m_InputPins.empty() && !isCommentNode(); }
        inline bool isFunctionNode      (void) const { return !m_OutputPins.empty() && !m_InputPins.empty(); }

        using custom_input_callback = void(node&, int& i, e10::library_mgr& LibraryMgr, xproperty::settings::context& Context);
        custom_input_callback* m_pCustomInput = nullptr;

        XPROPERTY_DEF//primitive type
        ("node", node
        , obj_member<"Name",        &node::m_Name,          member_flags<flags::SHOW_READONLY> >
        , obj_member<"NodeGuid",    &node::m_Guid,          member_flags<flags::DONT_SHOW, flags::DONT_SAVE> >
        , obj_member<"Position",    &node::m_Pos,           member_flags<flags::DONT_SHOW>>
        , obj_member<"Input Pins",  &node::m_InputPins,     member_flags<flags::DONT_SHOW>>
        , obj_member<"Output Pins", &node::m_OutputPins,    member_flags<flags::DONT_SHOW>>
        , obj_member<"Value",       &node::m_Params>
        )

        int getInputPinIndex(pin_guid Guid);
    };
    XPROPERTY_REG(node)
}

#endif