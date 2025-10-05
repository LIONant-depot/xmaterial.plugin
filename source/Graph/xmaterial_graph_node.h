#ifndef XMATERIAL_GRAPH_NODE_H
#define XMATERIAL_GRAPH_NODE_H
#pragma once

#ifdef EDITOR
    #include "source/Examples/E10_TextureResourcePipeline/E10_AssetMgr.h"
    #include "plugins/xtexture.plugin/source/xtexture_xgpu_rsc_loader.h"
#endif

#include "xmaterial_graph_node_guid.h"
#include "dependencies/xresource_guid/source/xresource_guid.h"
#include "plugins/xtexture.plugin/source/xtexture_xgpu_rsc_loader.h"

struct node_prop
{
    enum class type :std::uint8_t
    { NONE
    , FLOAT
    , INT
    , STRING
    , TEXTURE_RESOURCE
    , UNKNOWN 
    };

    node_prop() = default;
    node_prop( std::string_view Name, type Type ) : m_Name{ Name }
    {
        setupType(Type);
    }

    node_prop(std::string_view Name, int x)                             : m_Name{ Name }, m_Value{x},               m_Type{ type::INT }{}
    node_prop(std::string_view Name, float x)                           : m_Name{ Name }, m_Value{ x },             m_Type{ type::FLOAT } {}
    node_prop(std::string_view Name, type Type, xresource::full_guid x) : m_Name{ Name }, m_Value{ x },             m_Type{ Type } {}
    node_prop(std::string_view Name, std::string&& S)                   : m_Name{ Name }, m_Value{ std::move(S) },  m_Type{type::STRING} {}
    node_prop(std::string_view Name, std::string_view S)                : m_Name{ Name }, m_Value{ std::string(S) }, m_Type{ type::STRING } {}

    std::string     m_Name;
    xproperty::any  m_Value;
    type            m_Type;

    inline static constexpr auto type_enum_v = std::array
    { xproperty::settings::enum_item{ "NONE",               type::NONE      }
    , xproperty::settings::enum_item{ "FLOAT",              type::FLOAT     }
    , xproperty::settings::enum_item{ "INT",                type::INT       }
    , xproperty::settings::enum_item{ "STRING",             type::STRING    }
    , xproperty::settings::enum_item{ "TEXTURE RESOURCE",   type::TEXTURE_RESOURCE }
    , xproperty::settings::enum_item{ "UNKNOWN",            type::UNKNOWN   }
    };

    inline static constexpr auto type_guid_filters_v = std::array{ xrsc::texture_type_guid_v };

    void setupType(type Enum)
    {
        m_Type = Enum;
        switch (m_Type)
        {
        default:
        case type::NONE:                m_Value = {};                             break;
        case type::FLOAT:               m_Value.set<float>({});                   break;
        case type::INT:                 m_Value.set<int>({});                     break;
        case type::STRING:              m_Value.set<std::string>({});             break;
        case type::TEXTURE_RESOURCE:    m_Value.set<xresource::full_guid>({});    break;
        }
    }

    XPROPERTY_DEF
    ("Props", node_prop
        , obj_member<"Name", &node_prop::m_Name
            , member_flags<flags::SHOW_READONLY>>
        , obj_member<"Type", +[](node_prop& O, bool bRead, type& Enum)
            {
                if (bRead) Enum = O.m_Type;
                else       O.setupType(Enum);
            }
            , member_enum_span<type_enum_v >
            , member_flags<flags::DONT_SHOW>>
        , obj_member< "Float", +[](node_prop& O, bool bRead, float& Value)
            {
                if ( O.m_Type != type::FLOAT ) return;
                assert( O.m_Value.m_pType->m_GUID == xproperty::settings::var_type<float>::guid_v );

                if (bRead) Value = O.m_Value.get<float>();
                else       O.m_Value.set<float>(Value);
            }
            , member_dynamic_flags < +[](const node_prop& O)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly   = false;
                Flags.m_bDontShow       = O.m_Type != type::FLOAT;
                Flags.m_bDontSave       = O.m_Type != type::FLOAT;
                return Flags;
            } >>
        , obj_member< "Int", +[](node_prop& O, bool bRead, int& Value)
            {
                if (O.m_Type != type::INT) return;
                assert(O.m_Value.m_pType->m_GUID == xproperty::settings::var_type<int>::guid_v);

                if (bRead) Value = O.m_Value.get<int>();
                else      O.m_Value.set<int>(Value);
            }
            , member_dynamic_flags < +[](const node_prop& O)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly   = false;
                Flags.m_bDontShow       = O.m_Type != type::INT;
                Flags.m_bDontSave       = O.m_Type != type::INT;
                return Flags;
            } >>
        , obj_member< "String", +[](node_prop& O, bool bRead, std::string& Value)
            {
                if (O.m_Type != type::STRING) return;
                assert(O.m_Value.m_pType->m_GUID == xproperty::settings::var_type<std::string>::guid_v);

                if (bRead) Value = O.m_Value.get<std::string>();
                else      O.m_Value.set<std::string>(Value);
            }
            , member_dynamic_flags < +[](const node_prop& O)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly   = false;
                Flags.m_bDontShow       = O.m_Type != type::STRING;
                Flags.m_bDontSave       = O.m_Type != type::STRING;
                return Flags;
            } >>
        , obj_member < "TextureRef", +[](node_prop& O, bool bRead, xresource::full_guid& FullGuid)
            {
                if (O.m_Type != type::TEXTURE_RESOURCE) return;
                assert(O.m_Value.m_pType->m_GUID == xproperty::settings::var_type<xresource::full_guid>::guid_v);

                if (bRead) FullGuid = O.m_Value.get<xresource::full_guid>();
                else       O.m_Value.set<xresource::full_guid>(FullGuid);
            }
            , member_ui<xresource::full_guid>::type_filters<type_guid_filters_v>
            , member_dynamic_flags < +[](const node_prop& O)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly   = false;
                Flags.m_bDontShow       = O.m_Type != type::TEXTURE_RESOURCE;
                Flags.m_bDontSave       = O.m_Type != type::TEXTURE_RESOURCE;
                return Flags;
            } 
            >>
    )
};
XPROPERTY_REG(node_prop)

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
        std::vector<node_prop>          m_Params            = {};
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
        , obj_member<"Params",      &node::m_Params,        member_ui_open<true>>
        )

        int getInputPinIndex(pin_guid Guid);
    };
    XPROPERTY_REG(node)
}

#endif