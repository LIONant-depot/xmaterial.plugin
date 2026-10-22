#ifndef XMATERIAL_GRAPH_H
#define XMATERIAL_GRAPH_H
#pragma once

#include "xmaterial_graph_node.h"
#include "xmaterial_stack.h"
#include "../xmaterial_params.h"
#include "dependencies/xresource_guid/source/xresource_guid.h"
#include "dependencies/xerr/source/xerr.h"
#include <unordered_map>
#include <plugins/xtexture.plugin/source/xtexture_xgpu_rsc_loader.h>

namespace xmaterial_graph
{
    enum class state : std::uint8_t
    { OK
    , FAILURE
    };

    // The two colour pin types (material_contract.md 3): Color is linear RGBA, SColor is sRGB-encoded RGBA
    inline constexpr auto color_type_guid_v  = type_guid{ xresource::type_guid{"Color"}.m_Value };
    inline constexpr auto scolor_type_guid_v = type_guid{ xresource::type_guid{"SColor"}.m_Value };


    struct connection
    {
        connection_guid     m_Guid          = {};
        pin_guid            m_InputPinGuid  = {};
        pin_guid            m_OutputPinGuid = {}; // connects FROM this output pin or its subelement
        std::string         m_Swizzle       = {}; // the components the input takes from a whole vector output, in order ("zyx", "xyxy"; letters xyzw); "" = all of them,
                                                  // or the first ones when the input is shorter (truncation). One component is a sub-element connection, never a swizzle.

        XPROPERTY_DEF
        ( "connection", connection
        , obj_member<"ConnectionGuid",  &connection::m_Guid,            member_flags<flags::DONT_SAVE>>
        , obj_member<"InputPinGuid",    &connection::m_InputPinGuid,    member_flags<flags::DONT_SHOW>>
        , obj_member<"OutputPinGuid",   &connection::m_OutputPinGuid,   member_flags<flags::DONT_SHOW>>
        , obj_member<"Swizzle",         &connection::m_Swizzle
            , member_dynamic_flags < +[](const connection& O)
            {
                xproperty::flags::type Flags = {};
                Flags.m_bDontShow = true;
                Flags.m_bDontSave = O.m_Swizzle.empty();    // like node_param's ParamID: files without swizzles stay byte-identical, older files read it as ""
                return Flags;
            } >>
        )
    };
    XPROPERTY_REG(connection)

    struct final_texture_nodes
    {
        struct entry
        {
            const node* m_pNode;
            int         m_iParam;
        };
        mutable std::vector<entry> m_Textures;
    };

    xerr RefreshShaderOnlyNode(std::wstring ShaderFileName, node& Node, std::string& Shader);

/*
    struct xxx
    {
        xrsc::rendering_pipeline    m_RenderingPipeline;

        // BLEND options?

        // SORTED OPTIONS? (alpha (back to front), alpha (post-alpha blend) vs opaque?)

        // CULLING ORDER (CCW/CW/NONE)

        // WIRE-FRAME, SOLID, SOLID-AA?

        XPROPERTY_DEF
        ( "connection", connection
        , obj_member<"ConnectionGuid",  &connection::m_Guid,            member_flags<flags::DONT_SAVE>>
        )
    };
    XPROPERTY_REG(connection)
*/

    struct graph
    {
        sType&              CreateType              (type_guid Guid = type_guid{ xresource::guid_generator::Type64() });
        node&               CreatePrefabNode        (node_guid Guid = node_guid{ xresource::guid_generator::Type64() });
        node&               CreateNode              (node_guid PrefabGuid, node_guid Guid = node_guid{ xresource::guid_generator::Type64() });
        void                CompletePrefab          (node& PrefabNode);
        connection&         createConnection        (connection_guid connGuid = connection_guid{ xresource::guid_generator::Type64() });
        void                RemoveConnection        (connection_guid id);
        void                RemoveNode              (node_guid id);
        node*               findNodeByPin           (pin_guid pin) const;
        sType*              findType                (type_guid id) const;
        void                RebuildPinGuidForNode   (const node& n);
        void                serialize               (graph& graph, bool isReading, const std::wstring_view filepath);
        const pin*          findPinConst            (const node& n, pin_guid pid, bool& isInput, int& idxOut, int& subOut) const;
        node*               findFullShaderNode      (void);
        void                CreateGraph             (graph& g);

        std::unordered_map<node_guid, std::unique_ptr<node>>                m_PrefabNodes;      // right click option like sampler node etc
        std::map<node_guid, std::unique_ptr<node>>                          m_InstanceNodes;    // Keep certain maps as simple maps so it will save the information in the same order
        std::map<connection_guid, std::unique_ptr<connection>>              m_Connections;      // Keep certain maps as simple maps so it will save the information in the same order
        std::unordered_map<type_guid, std::unique_ptr<sType>>               m_type;
        std::unordered_map<pin_guid, node_guid>                             m_PinToNode;
        final_texture_nodes                                                 m_FinalTextureNodes;
        std::vector<xmaterial::param_decl>                                  m_Params;           // declared material parameters (file v3)
        stack                                                               m_Stack;            // organisation: sections and the order of their steps
        bool                                                                m_bSubgraph = false; // a MaterialSubgraph's graph (never saved: the resource type says it): Group Input/Output instead of material outputs
    };

    // The node types a graph may hold: a sub-graph has no material outputs, a material no Group Input/Output
    inline bool IsAllowedIn(const graph& G, const node& Prefab) noexcept
    {
        if (Prefab.isGroupInputNode() || Prefab.isGroupOutputNode()) return G.m_bSubgraph;
        return !(G.m_bSubgraph && Prefab.isOutputNode());
    }

    // The components of a type: 1 a float, N a vecN, 0 anything else (samplers, textures)
    inline int Components(const graph& G, type_guid T) noexcept
    {
        auto* p = G.findType(T);
        if (!p) return 0;
        if (p->m_CodeString == "float") return 1;
        return p->m_CodeString.size() == 4 && p->m_CodeString.starts_with("vec") ? p->m_CodeString[3] - '0' : 0;
    }

    // The letters a type's components are shown with: rgba for the colours, xyzw otherwise (a stored swizzle always uses xyzw)
    inline const char* ComponentLetters(type_guid T) noexcept
    {
        return T == color_type_guid_v || T == scolor_type_guid_v ? "rgba" : "xyzw";
    }
}

#endif