#ifndef XMATERIAL_GRAPH_NODE_H
#define XMATERIAL_GRAPH_NODE_H
#pragma once

#ifdef EDITOR
    #include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_asset_mgr.h"
    #include "plugins/xtexture.plugin/source/xtexture_xgpu_rsc_loader.h"
#endif

#include "xmaterial_graph_node_guid.h"
#include "../xmaterial_params.h"
#include "dependencies/xresource_guid/source/xresource_guid.h"
#include "plugins/xtexture.plugin/source/xtexture_xgpu_rsc_loader.h"

namespace xmaterial_graph
{
    // The resource type of a sub-graph (plugins/xmaterial_subgraph.plugin): a graph file like a material's, with an interface (subgraphs_plan.md)
    inline constexpr auto subgraph_type_guid_v = xresource::type_guid(xresource::guid_generator::Instance64FromString("MaterialSubgraph"));

    struct node_param
    {
        static constexpr wchar_t        file_filter_v[] = L"ShaderFile\0 *.glsl;*.frag\0Any Thing\0 *.*\0";

        enum class type :std::uint8_t
        { NONE
        , FLOAT
        , INT
        , STRING
        , FILE
        , TEXTURE_RESOURCE
        , UNKNOWN
        , SUBGRAPH_RESOURCE         // a group node's MaterialSubgraph (subgraphs_plan.md 2.3); after UNKNOWN so the old numbers stay
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
        xmaterial::param_id m_ParamID = 0;      // the declared parameter this input reads instead of its literal (0 = none)

        inline static constexpr auto type_enum_v = std::array
        { xproperty::settings::enum_item{ "NONE",               type::NONE      }
        , xproperty::settings::enum_item{ "FLOAT",              type::FLOAT     }
        , xproperty::settings::enum_item{ "INT",                type::INT       }
        , xproperty::settings::enum_item{ "STRING",             type::STRING    }
        , xproperty::settings::enum_item{ "FILE",               type::FILE      }
        , xproperty::settings::enum_item{ "TEXTURE RESOURCE",   type::TEXTURE_RESOURCE }
        , xproperty::settings::enum_item{ "UNKNOWN",            type::UNKNOWN   }
        , xproperty::settings::enum_item{ "SUBGRAPH RESOURCE",  type::SUBGRAPH_RESOURCE }
        };

        inline static constexpr auto type_guid_filters_v     = std::array{ xrsc::texture_type_guid_v };
        inline static constexpr auto subgraph_guid_filters_v = std::array{ subgraph_type_guid_v };

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
            case type::SUBGRAPH_RESOURCE:   m_Value.set<xresource::full_guid>({});    break;
            }
        }

        // The value as the inspector shows it: a view of its type over this param (node_param_*_view below), not saved (the typed members are)
        static std::pair<const xproperty::type::object*, void*> ValueView(node_param& O) noexcept;

        // One row in the inspector (sprop_union_group): the name, then the value. The typed members after them are what is saved, not shown.
        XPROPERTY_DEF
        ( "Props", node_param, xproperty::settings::sprop_union_group
        , obj_member<"Name", &node_param::m_Name
            , member_flags<flags::SHOW_READONLY>>
        , obj_member<"Value", +[](node_param& O) -> std::pair<const xproperty::type::object*, void*> { return ValueView(O); }
            , member_flags<flags::DONT_SAVE>>
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
                Flags.m_bDontSave       = O.m_Type != type::FLOAT;
                Flags.m_bDontShow       = true;
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
                Flags.m_bDontSave       = O.m_Type != type::INT;
                Flags.m_bDontShow       = true;
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
                Flags.m_bDontSave       = O.m_Type != type::STRING;
                Flags.m_bDontShow       = true;
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
                Flags.m_bDontSave       = O.m_Type != type::FILE;
                Flags.m_bDontShow       = true;
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
                Flags.m_bDontSave       = O.m_Type != type::TEXTURE_RESOURCE;
                Flags.m_bDontShow       = true;
                return Flags;
            }
            >>
        , obj_member < "SubgraphRef", +[](node_param& O, bool bRead, xresource::full_guid& FullGuid)
            {
                if (O.m_Type != type::SUBGRAPH_RESOURCE) return;
                if (bRead) FullGuid = O.m_Value.get<xresource::full_guid>();
                else       O.m_Value.set<xresource::full_guid>(FullGuid);
            }
            , member_ui<xresource::full_guid>::type_filters<subgraph_guid_filters_v>
            , member_dynamic_flags < +[](const node_param& O)
            {
                xproperty::flags::type Flags={};
                Flags.m_bDontSave       = O.m_Type != type::SUBGRAPH_RESOURCE;
                Flags.m_bDontShow       = true;
                Flags.m_bShowReadOnly   = true;             // SetSubgraph changes it: the group node's pins must follow

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
        , obj_member< "ParamID", &node_param::m_ParamID
            , member_dynamic_flags < +[](const node_param& O)
            {
                xproperty::flags::type Flags={};
                Flags.m_bDontShow       = true;
                Flags.m_bDontSave       = O.m_ParamID == 0;     // unlinked inputs write nothing: old files stay byte-identical
                return Flags;
            } 
            >>
        )
    };
    XPROPERTY_REG(node_param)

    // node_param's value, one view per type (the union's sides): each is the param itself, seen with one member
    struct node_param_float_view : node_param
    {
        XPROPERTY_DEF("node_param: Float", node_param_float_view
        , obj_member<"Float", +[](node_param_float_view& O, bool bRead, float& V) { if (bRead) V = O.m_Value.get<float>(); else O.m_Value.set<float>(V); }>)
    };
    struct node_param_int_view : node_param
    {
        XPROPERTY_DEF("node_param: Int", node_param_int_view
        , obj_member<"Int", +[](node_param_int_view& O, bool bRead, int& V) { if (bRead) V = O.m_Value.get<int>(); else O.m_Value.set<int>(V); }>)
    };
    struct node_param_string_view : node_param
    {
        XPROPERTY_DEF("node_param: String", node_param_string_view
        , obj_member<"String", +[](node_param_string_view& O, bool bRead, std::string& V) { if (bRead) V = O.m_Value.get<std::string>(); else O.m_Value.set<std::string>(V); }>)
    };
    struct node_param_file_view : node_param
    {
        XPROPERTY_DEF("node_param: File", node_param_file_view
        , obj_member<"File", +[](node_param_file_view& O, bool bRead, std::wstring& V) { if (bRead) V = O.m_Value.get<std::wstring>(); else O.m_Value.set<std::wstring>(V); }
            , member_ui<std::wstring>::file_dialog<node_param::file_filter_v, true, 1>>)
    };
    struct node_param_texture_view : node_param
    {
        XPROPERTY_DEF("node_param: TextureRef", node_param_texture_view
        , obj_member<"TextureRef", +[](node_param_texture_view& O, bool bRead, xresource::full_guid& V) { if (bRead) V = O.m_Value.get<xresource::full_guid>(); else O.m_Value.set<xresource::full_guid>(V); }
            , member_ui<xresource::full_guid>::type_filters<node_param::type_guid_filters_v>>)
    };
    struct node_param_subgraph_view : node_param
    {
        XPROPERTY_DEF("node_param: SubgraphRef", node_param_subgraph_view
        , obj_member<"SubgraphRef", +[](node_param_subgraph_view& O, bool bRead, xresource::full_guid& V) { if (bRead) V = O.m_Value.get<xresource::full_guid>(); else O.m_Value.set<xresource::full_guid>(V); }
            , member_ui<xresource::full_guid>::type_filters<node_param::subgraph_guid_filters_v>
            , member_flags<flags::SHOW_READONLY>>)        // SetSubgraph changes it: the group node's pins must follow
    };
    XPROPERTY_REG(node_param_float_view)
    XPROPERTY_REG(node_param_int_view)
    XPROPERTY_REG(node_param_string_view)
    XPROPERTY_REG(node_param_file_view)
    XPROPERTY_REG(node_param_texture_view)
    XPROPERTY_REG(node_param_subgraph_view)

    inline std::pair<const xproperty::type::object*, void*> node_param::ValueView(node_param& O) noexcept
    {
        switch (O.m_Type)
        {
        case type::FLOAT:               return { xproperty::getObjectByType<node_param_float_view>(),    &O };
        case type::INT:                 return { xproperty::getObjectByType<node_param_int_view>(),      &O };
        case type::STRING:              return { xproperty::getObjectByType<node_param_string_view>(),   &O };
        case type::FILE:                return { xproperty::getObjectByType<node_param_file_view>(),     &O };
        case type::TEXTURE_RESOURCE:    return { xproperty::getObjectByType<node_param_texture_view>(),  &O };
        case type::SUBGRAPH_RESOURCE:   return { xproperty::getObjectByType<node_param_subgraph_view>(), &O };
        default:                        return { nullptr, nullptr };
        }
    }

    inline constexpr auto group_input_prefab_v  = node_guid{ xresource::type_guid{"GroupInputPrefabs"}.m_Value };
    inline constexpr auto group_output_prefab_v = node_guid{ xresource::type_guid{"GroupOutputPrefabs"}.m_Value };
    inline constexpr auto group_call_prefab_v   = node_guid{ xresource::type_guid{"SubgraphCallPrefabs"}.m_Value };     // a group node: calls a sub-graph

    struct node //serialize instance guid ,type guid and position putting graph -> shader
    {
        node_guid                       m_Guid              = {};  //instance guid
        node_guid                       m_PrefabGuid        = {};  //prefab id
        std::string                     m_Name              = {};
        std::string                     m_StepName          = {};  // unique in the graph; chips, console symbols, GLSL names
        std::string                     m_Code              = {};
        std::vector<input_pin>          m_InputPins         = {};
        std::vector<output_pin>         m_OutputPins        = {};
        xmath::fvec2                    m_Pos               = {};
        std::vector<node_param>         m_Params            = {};
        bool                            m_HasErrMsg         = { false };
        std::string                     m_ErrMsg            = {};
        int                             m_MaxInputChars     = 0;
        int                             m_MaxOutputChars    = 0;

        // A sub-graph's interface (subgraphs_plan.md 2.2): its pins are its own, so it can have none and still be no comment
        inline bool isGroupInputNode    (void) const { return m_PrefabGuid == group_input_prefab_v  || m_Guid == group_input_prefab_v; }
        inline bool isGroupOutputNode   (void) const { return m_PrefabGuid == group_output_prefab_v || m_Guid == group_output_prefab_v; }
        inline bool isGroupCallNode     (void) const { return m_PrefabGuid == group_call_prefab_v   || m_Guid == group_call_prefab_v; }      // its pins mirror its sub-graph's interface
        inline bool isCommentNode       (void) const { return m_OutputPins.empty() && m_InputPins.empty() && !isGroupInputNode() && !isGroupOutputNode() && !isGroupCallNode(); }
        inline bool isOutputNode        (void) const { return isGroupOutputNode() || (m_OutputPins.empty() && !isCommentNode() && !isGroupInputNode() && !isGroupCallNode()); }
        inline bool isInputNode         (void) const { return isGroupInputNode()  || (m_InputPins.empty()  && !isCommentNode() && !isGroupOutputNode()); }
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
        using custom_input_callback = void(node&, xproperty::any& Value, xresource_editor::library_mgr& LibraryMgr, xproperty::settings::context& Context);
#else
        using custom_input_callback = void(void);
#endif
        custom_input_callback* m_pCustomInput = nullptr;

        XPROPERTY_DEF//primitive type
        ( "node", node
        , obj_member<"Name",        &node::m_Name,          member_flags<flags::SHOW_READONLY> >
        , obj_member<"NodeGuid",    &node::m_Guid,          member_flags<flags::DONT_SHOW, flags::DONT_SAVE>>
        , obj_member<"StepName",    &node::m_StepName>
        , obj_member<"Position",    &node::m_Pos,           member_flags<flags::DONT_SHOW>>   // still saved: the old canvas needs it until the stack canvas replaces it (stack_canvas.md task 12), then DONT_SAVE
        , obj_member<"Input Pins",  &node::m_InputPins,     member_flags<flags::DONT_SHOW>>
        , obj_member<"Output Pins", &node::m_OutputPins,    member_flags<flags::DONT_SHOW>>
        , obj_member<"Params",      &node::m_Params,        member_ui_open<true>, member_array_size_readonly<true> >
        )

        int getInputPinIndex(pin_guid Guid);
    };
    XPROPERTY_REG(node)
}

#endif