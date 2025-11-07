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

namespace xmaterial_graph
{
    struct node_param
    {
        static constexpr wchar_t        file_filter_v[] = L"ShaderFile\0 *.glsl\0Any Thing\0 *.*\0";

        enum class type :std::uint8_t
        { NONE
        , FLOAT
        , INT
        , STRING
        , FILE
        , TEXTURE_RESOURCE
        , UNKNOWN 
        };

        node_param() = default;
        template<typename T>
        node_param( std::string_view Name, type Type, T&& Value, bool bCanExpose = false ) : m_Name{ Name }, m_Value{ std::move(Value) }, m_bCanExpose(bCanExpose)
        {
            setupType(Type);
        }

        node_param(std::string_view Name, int x)                             : m_Name{ Name }, m_Value{x},                   m_Type{type::INT}{}
        node_param(std::string_view Name, float x)                           : m_Name{ Name }, m_Value{ x },                 m_Type{type::FLOAT} {}
        node_param(std::string_view Name, type Type, xresource::full_guid x) : m_Name{ Name }, m_Value{ x },                 m_Type{ Type } {}
        node_param(std::string_view Name, std::string&& S)                   : m_Name{ Name }, m_Value{ std::move(S) },      m_Type{type::STRING} {}
        node_param(std::string_view Name, std::string_view S)                : m_Name{ Name }, m_Value{ std::string(S) },    m_Type{type::STRING} {}
        node_param(std::string_view Name, std::wstring&& S)                  : m_Name{ Name }, m_Value{ std::move(S) },      m_Type{type::FILE} {}
        node_param(std::string_view Name, std::wstring_view S)               : m_Name{ Name }, m_Value{ std::wstring(S) },   m_Type{type::FILE} {}

        std::string     m_Name;
        xproperty::any  m_Value;
        type            m_Type;
        bool            m_bCanExpose = false;
        bool            m_bExpose    = false;
        std::string     m_ExposeName = {};

        inline static constexpr auto type_enum_v = std::array
        { xproperty::settings::enum_item{ "NONE",               type::NONE      }
        , xproperty::settings::enum_item{ "FLOAT",              type::FLOAT     }
        , xproperty::settings::enum_item{ "INT",                type::INT       }
        , xproperty::settings::enum_item{ "STRING",             type::STRING    }
        , xproperty::settings::enum_item{ "FILE",               type::FILE      }
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
            case type::FILE:                m_Value.set<std::wstring>({});            break;
            case type::TEXTURE_RESOURCE:    m_Value.set<xresource::full_guid>({});    break;
            }
        }

        XPROPERTY_DEF
        ( "Props", node_param
        , obj_member<"Name", &node_param::m_Name
            , member_flags<flags::SHOW_READONLY>>
        , obj_member<"Type", +[](node_param& O, bool bRead, type& Enum)
            {
                if (bRead) Enum = O.m_Type;
                else       O.setupType(Enum);
            }
            , member_enum_span<type_enum_v >
            , member_flags<flags::DONT_SHOW>>
        , obj_member< "Float", +[](node_param& O, bool bRead, float& Value)
            {
                if ( O.m_Type != type::FLOAT ) return;
                assert( O.m_Value.m_pType->m_GUID == xproperty::settings::var_type<float>::guid_v );

                if (bRead) Value = O.m_Value.get<float>();
                else       O.m_Value.set<float>(Value);
            }
            , member_dynamic_flags < +[](const node_param& O)
            {
                xproperty::flags::type Flags = {};
                Flags.m_bDontShow       = O.m_Type != type::FLOAT;
                Flags.m_bDontSave       = Flags.m_bDontShow;
                return Flags;
            } >>
        , obj_member< "Int", +[](node_param& O, bool bRead, int& Value)
            {
                if (O.m_Type != type::INT) return;
                assert(O.m_Value.m_pType->m_GUID == xproperty::settings::var_type<int>::guid_v);

                if (bRead) Value = O.m_Value.get<int>();
                else      O.m_Value.set<int>(Value);
            }
            , member_dynamic_flags < +[](const node_param& O)
            {
                xproperty::flags::type Flags={};
                Flags.m_bDontShow       = O.m_Type != type::INT;
                Flags.m_bDontSave       = Flags.m_bDontShow;
                return Flags;
            } >>
        , obj_member< "String", +[](node_param& O, bool bRead, std::string& Value)
            {
                if (O.m_Type != type::STRING) return;
                assert(O.m_Value.m_pType->m_GUID == xproperty::settings::var_type<std::string>::guid_v);

                if (bRead) Value = O.m_Value.get<std::string>();
                else      O.m_Value.set<std::string>(Value);
            }
            , member_dynamic_flags < +[](const node_param& O)
            {
                xproperty::flags::type Flags={};
                Flags.m_bDontShow       = O.m_Type != type::STRING;
                Flags.m_bDontSave       = Flags.m_bDontShow;
                return Flags;
            } >>
        , obj_member< "File", +[](node_param& O, bool bRead, std::wstring& Value)
            {
                if (O.m_Type != type::FILE) return;
                assert(O.m_Value.m_pType->m_GUID == xproperty::settings::var_type<std::wstring>::guid_v);

                if (bRead) Value = O.m_Value.get<std::wstring>();
                else      O.m_Value.set<std::wstring>(Value);
            }
            , member_dynamic_flags < +[](const node_param& O)
            {
                xproperty::flags::type Flags;
                Flags.m_bShowReadOnly   = false;
                Flags.m_bDontShow       = O.m_Type != type::FILE;
                Flags.m_bDontSave       = Flags.m_bDontShow;
                return Flags;
            }> 
            , member_ui<std::wstring>::file_dialog<file_filter_v, true, 1
            >>
        , obj_member < "TextureRef", +[](node_param& O, bool bRead, xresource::full_guid& FullGuid)
            {
                if (O.m_Type != type::TEXTURE_RESOURCE) return;
                assert(O.m_Value.m_pType->m_GUID == xproperty::settings::var_type<xresource::full_guid>::guid_v);

                if (bRead) FullGuid = O.m_Value.get<xresource::full_guid>();
                else       O.m_Value.set<xresource::full_guid>(FullGuid);
            }
            , member_ui<xresource::full_guid>::type_filters<type_guid_filters_v>
            , member_dynamic_flags < +[](const node_param& O)
            {
                xproperty::flags::type Flags={};
                Flags.m_bDontShow       = O.m_Type != type::TEXTURE_RESOURCE;
                Flags.m_bDontSave       = Flags.m_bDontShow;
                return Flags;
            } 
            >>
        , obj_member< "bCanExpose", &node_param::m_bCanExpose, member_flags<flags::DONT_SHOW>>
        , obj_member< "bExpose", &node_param::m_bExpose 
            , member_dynamic_flags < +[](const node_param& O)
            {
                xproperty::flags::type Flags={};
                Flags.m_bDontShow       = !O.m_bCanExpose;
                Flags.m_bDontSave       = Flags.m_bDontShow;
                return Flags;
            } 
            >>
        , obj_member< "ExposeName", &node_param::m_ExposeName
            , member_dynamic_flags < +[](const node_param& O)
            {
                xproperty::flags::type Flags={};
                Flags.m_bDontShow       = !O.m_bCanExpose || !O.m_bExpose;
                Flags.m_bDontSave       = Flags.m_bDontShow;
                return Flags;
            } 
            >>
        )
    };
    XPROPERTY_REG(node_param)

    struct node //serialize instance guid ,type guid and position putting graph -> shader
    {
        node_guid                       m_Guid              = {};  //instance guid
        node_guid                       m_PrefabGuid        = {};  //prefab id
        std::string                     m_Name              = {};
        std::string                     m_Code              = {};
        std::vector<input_pin>          m_InputPins         = {};
        std::vector<output_pin>         m_OutputPins        = {};
        xmath::fvec2                    m_Pos               = {};
        std::vector<node_param>         m_Params            = {};
        bool                            m_HasErrMsg         = { false };
        std::string                     m_ErrMsg            = {};
        int                             m_MaxInputChars     = 0;
        int                             m_MaxOutputChars    = 0;

        inline bool isCommentNode       (void) const { return m_OutputPins.empty() && m_InputPins.empty(); }
        inline bool isOutputNode        (void) const { return m_OutputPins.empty() && !isCommentNode(); }
        inline bool isInputNode         (void) const { return m_InputPins.empty() && !isCommentNode(); }
        inline bool isFunctionNode      (void) const { return !m_OutputPins.empty() && !m_InputPins.empty(); }

        inline void ComputeMaxInputChars()
        {
            m_MaxInputChars = 0;
            for ( auto& E : m_InputPins)
            {
                m_MaxInputChars = std::max(m_MaxInputChars, static_cast<int>(E.m_Name.length()) );
            }
        }

        inline void ComputeMaxOutputChars()
        {
            m_MaxOutputChars = 0;
            for (auto& E : m_OutputPins)
            {
                m_MaxOutputChars = std::max(m_MaxOutputChars, static_cast<int>(E.m_Name.length()));
            }
        }

#ifdef EDITOR
        using custom_input_callback = void(node&, xproperty::any& Value, e10::library_mgr& LibraryMgr, xproperty::settings::context& Context);
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