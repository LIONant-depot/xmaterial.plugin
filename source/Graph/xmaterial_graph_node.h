#ifndef XMATERIAL_GRAPH_NODE_H
#define XMATERIAL_GRAPH_NODE_H
#pragma once

#ifdef EDITOR
    #include "source/Examples/E10_TextureResourcePipeline/E10_AssetMgr.h"
    #include "plugins/xtexture.plugin/source/xtexture_xgpu_rsc_loader.h"
#endif

#include "xmaterial_graph_node_guid.h"
#include "dependencies/xresource_guid/source/xresource_guid.h"

struct prop_friend : xproperty::sprop::container::prop
{
    enum class type :std::uint8_t
    { NONE
    , FLOAT
    , INT
    , RESOURCE
    , UNKNOWN 
    };

    inline static constexpr auto type_enum_v = std::array
    { xproperty::settings::enum_item{ "NONE",       type::NONE      }
    , xproperty::settings::enum_item{ "FLOAT",      type::FLOAT     }
    , xproperty::settings::enum_item{ "INT",        type::INT       }
    , xproperty::settings::enum_item{ "RESOURCE",   type::RESOURCE  }
    , xproperty::settings::enum_item{ "UNKNOWN",    type::UNKNOWN   }
    };

    using prop_t = xproperty::sprop::container::prop;

    XPROPERTY_DEF
    ("Props", prop_t
        , obj_member<"Name", &prop_t::m_Path
            , member_flags<flags::SHOW_READONLY>>
        , obj_member<"Type", +[](prop_t& O, bool bRead, type& Enum)
            {
                if (bRead)
                {
                    if (O.m_Value.m_pType == nullptr) Enum = type::NONE;
                    else switch (O.m_Value.m_pType->m_GUID)
                    {
                    case xproperty::settings::var_type<float>::guid_v:                  Enum = type::FLOAT; break;
                    case xproperty::settings::var_type<int>::guid_v:                    Enum = type::INT; break;
                    case xproperty::settings::var_type<xresource::full_guid>::guid_v:   Enum = type::RESOURCE; break;
                    default: Enum = type::UNKNOWN; break;
                    }
                }
                else
                {
                    switch (Enum)
                    {
                    default:
                    case type::NONE:                O = {};                                     break;
                    case type::FLOAT:               O.m_Value.set<float>(0);                    break;
                    case type::INT:                 O.m_Value.set<int>(0);                      break;
                    case type::RESOURCE:            O.m_Value.set<xresource::full_guid>({});    break;
                    }
                }
            }
            , member_enum_span<type_enum_v >
            , member_flags<flags::DONT_SHOW>>
        , obj_member< "Float", +[](prop_t& O, bool bRead, float& Value)
            {
                if (!O.m_Value.m_pType || O.m_Value.m_pType->m_GUID != xproperty::settings::var_type<float>::guid_v) return;
                if (bRead) Value = O.m_Value.get<float>();
                else       O.m_Value.set<float>(Value);
            }
            , member_dynamic_flags < +[](const prop_t& O)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly = false;
                Flags.m_bDontShow = O.m_Value.m_pType == nullptr || O.m_Value.m_pType->m_GUID != xproperty::settings::var_type<float>::guid_v;
                Flags.m_bDontSave = O.m_Value.m_pType == nullptr || O.m_Value.m_pType->m_GUID != xproperty::settings::var_type<float>::guid_v;
                return Flags;
            } >>
        , obj_member< "Int", +[](prop_t& O, bool bRead, int& Value)
            {
                if (!O.m_Value.m_pType || O.m_Value.m_pType->m_GUID != xproperty::settings::var_type<int>::guid_v) return;
                if (bRead) Value = O.m_Value.get<int>();
                else      O.m_Value.set<int>(Value);
            }
            , member_dynamic_flags < +[](const prop_t& O)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly = false;
                Flags.m_bDontShow = O.m_Value.m_pType == nullptr || O.m_Value.m_pType->m_GUID != xproperty::settings::var_type<int>::guid_v;
                Flags.m_bDontSave = O.m_Value.m_pType == nullptr || O.m_Value.m_pType->m_GUID != xproperty::settings::var_type<int>::guid_v;
                return Flags;
            } >>
        , obj_member < "Resource", +[](prop_t& O, bool bRead, xresource::full_guid& FullGuid)
            {
                if (!O.m_Value.m_pType || O.m_Value.m_pType->m_GUID != xproperty::settings::var_type<xresource::full_guid>::guid_v) return;
                if (bRead) FullGuid = O.m_Value.get<xresource::full_guid>();
                else       O.m_Value.set<xresource::full_guid>(FullGuid);
            }
            , member_dynamic_flags < +[](const prop_t& O)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly = false;
                Flags.m_bDontShow = O.m_Value.m_pType == nullptr || O.m_Value.m_pType->m_GUID != xproperty::settings::var_type<xresource::full_guid>::guid_v;
                Flags.m_bDontSave = O.m_Value.m_pType == nullptr || O.m_Value.m_pType->m_GUID != xproperty::settings::var_type<xresource::full_guid>::guid_v;
                return Flags;
            } >>
    )
};
XPROPERTY_REG(prop_friend)

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

#ifdef EDITOR
        using custom_input_callback = void(node&, xresource::full_guid& FullGuid, e10::library_mgr& LibraryMgr, xproperty::settings::context& Context);
#else
        using custom_input_callback = void(void);
#endif
        custom_input_callback* m_pCustomInput = nullptr;

        XPROPERTY_DEF//primitive type
        ( "node", node
        , obj_member<"Name",        &node::m_Name,          member_flags<flags::SHOW_READONLY> >
        , obj_member<"NodeGuid",    &node::m_Guid,          member_flags<flags::DONT_SHOW, flags::DONT_SAVE>>
        , obj_member<"Position",    &node::m_Pos,           member_flags<flags::DONT_SHOW>>
        , obj_member<"Input Pins",  &node::m_InputPins,     member_flags<flags::DONT_SHOW>>
        , obj_member<"Output Pins", &node::m_OutputPins,    member_flags<flags::DONT_SHOW>>
        , obj_member<"Params",    +[](node& O)->auto& {return O.m_Params.m_Properties; }, member_ui_open<true>>
        )

        int getInputPinIndex(pin_guid Guid);
    };
    XPROPERTY_REG(node)
}

#endif