#ifndef XMATERIAL_GRAPH_H
#define XMATERIAL_GRAPH_H
#pragma once

#include "xmaterial_graph_node.h"
#include "dependencies/xresource_guid/source/xresource_guid.h"
#include "dependencies/xerr/source/xerr.h"
#include <unordered_map>
#include <Plugins/xtexture.plugin/source/xtexture_xgpu_rsc_loader.h>

namespace xmaterial_graph
{
    struct connection
    {
        connection_guid     m_Guid          = {};
        pin_guid            m_InputPinGuid  = {};
        pin_guid            m_OutputPinGuid = {}; // connects FROM this output pin or its subelement

        XPROPERTY_DEF
        ( "connection", connection
        , obj_member<"ConnectionGuid",  &connection::m_Guid,            member_flags<flags::DONT_SAVE>>
        , obj_member<"InputPinGuid",    &connection::m_InputPinGuid,    member_flags<flags::DONT_SHOW>>
        , obj_member<"OutputPinGuid",   &connection::m_OutputPinGuid,   member_flags<flags::DONT_SHOW>>
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
        node*               FindNodeByPin           (pin_guid pin) const;
        sType*              GetType                 (type_guid id) const;
        void                RebuildPinGuidForNode   (const node& n);
        void                serialize               (graph& graph, bool isReading, const std::wstring_view filepath);
        const pin*          FindPinConst            (const node& n, pin_guid pid, bool& isInput, int& idxOut, int& subOut) const;
        void                CreateGraph             (graph& g);

        std::unordered_map<node_guid, std::unique_ptr<node>>                m_PrefabNodes;      // right click option like sampler node etc
        std::map<node_guid, std::unique_ptr<node>>                          m_InstanceNodes;    // Keep certain maps as simple maps so it will save the information in the same order
        std::map<connection_guid, std::unique_ptr<connection>>              m_Connections;      // Keep certain maps as simple maps so it will save the information in the same order
        std::unordered_map<type_guid, std::unique_ptr<sType>>               m_type;
        std::unordered_map<pin_guid, node_guid>                             m_PinToNode;
        final_texture_nodes                                                 m_FinalTextureNodes;
    };
}

#endif