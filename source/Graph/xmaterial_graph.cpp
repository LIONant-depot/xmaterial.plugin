#ifdef EDITOR
    #include "dependencies/imgui/imgui.h"
    #include "dependencies/imgui-node-editor/imgui_node_editor.h"
    #include "source/Examples/E10_TextureResourcePipeline/E10_AssetMgr.h"
#endif

#include "source/graph/xmaterial_graph.h"
#include "dependencies/xproperty/source/examples/xcore_sprop_serializer/xcore_sprop_serializer.h"

namespace xmaterial_compiler
{
    sType& graph::CreateType(type_guid Guid)
    {
        auto Type = std::make_unique<sType>();
        Type->m_GUID = Guid;
        assert(m_type.find(Guid) == m_type.end());

        auto p = Type.get();
        m_type[Guid] = std::move(Type);
        return *p;
    }

    node& graph::CreatePrefabNode(node_guid Guid)
    {
        auto Node = std::make_unique<node>();
        Node->m_Guid = Guid;
        assert(m_PrefabNodes.find(Guid) == m_PrefabNodes.end());

        auto p = Node.get();
        m_PrefabNodes[Guid] = std::move(Node);
        return *p;
    }

    node& graph::CreateNode(node_guid PrefabGuid, node_guid Guid)
    {
        auto it = m_PrefabNodes.find(PrefabGuid);
        assert(it != m_PrefabNodes.end());

        auto Node = std::make_unique<node>();
        assert(m_InstanceNodes.find(Guid) == m_InstanceNodes.end());

        *Node = *it->second;
        Node->m_Guid = Guid;
        Node->m_PrefabGuid = PrefabGuid;

        //every new node created need to assign unique pin for each instance
        //input pin unique id
        for (auto& ip : Node->m_InputPins)
        {
            ip.m_PinGUID = pin_guid{ xresource::guid_generator::Type64() };
            m_PinToNode[ip.m_PinGUID] = Guid;
        }

        //output pin unique id
        for (auto& op : Node->m_OutputPins)
        {
            op.m_PinGUID = pin_guid{ xresource::guid_generator::Type64() };
            m_PinToNode[op.m_PinGUID] = Guid;
            if (!op.m_SubElements.empty())
            {
                for (auto& sub : op.m_SubElements)
                {
                    sub.m_PinGUID = pin_guid{ xresource::guid_generator::Type64() };
                    m_PinToNode[sub.m_PinGUID] = Guid; //-> node guid
                    if (!op.m_DefaultExpr.empty())
                        sub.m_DefaultExpr = op.m_DefaultExpr;
                }
            }
        }

        auto p = Node.get();
        m_InstanceNodes[Guid] = std::move(Node);
        return *p;
    }

    void graph::CompletePrefab(node& PrefabNode)
    {
        for (auto& Pin : PrefabNode.m_OutputPins)
        {
            auto itsType = m_type.find(Pin.m_TypeGUID);
            assert(itsType != m_type.end()); // type not found
            auto& Type = *itsType->second;

            if (Type.m_Sub.empty() == false) //there is sub var
            {
                Pin.m_SubElements.resize(Type.m_Sub.size());
                for (int i = 0, end = static_cast<int>(Type.m_Sub.size()); i < end; ++i)
                {
                    Pin.m_SubElements[i].m_Name = Type.m_Sub[i].m_Name;
                    Pin.m_SubElements[i].m_TypeGUID = Type.m_Sub[i].m_TypeGUID;
                }
            }
        }
    }

    connection& graph::createConnection(connection_guid connGuid)
    {
        assert(m_Connections.find(connGuid) == m_Connections.end());
        auto connector = std::make_unique<connection>();
        connector->m_Guid = connGuid;

        auto p = connector.get();
        m_Connections[connGuid] = std::move(connector);
        return *p;
    }

    void graph::RemoveConnection(connection_guid id)
    {
        auto it = m_Connections.find(id);

        if (it != m_Connections.end()) {
            // Clear the input pin’s m_ConnectionGUID
            const auto& conn = *it->second;
            if (auto* n = FindNodeByPin(conn.m_InputPinGuid)) {
                int idx = n->getInputPinIndex(conn.m_InputPinGuid);
                if (idx >= 0) {
                    n->m_InputPins[idx].m_ConnectionGUID = {};
                }
            }
            m_Connections.erase(it);
        }
    }

    void graph::RemoveNode(node_guid id)
    {
        auto it = m_InstanceNodes.find(id);
        if (it == m_InstanceNodes.end()) return;

        auto& node = *it->second;

        // --- Remove all connections referencing this node’s pins ---
        std::vector<connection_guid> toRemove;
        for (auto& [cid, conn] : m_Connections)
        {
            if (FindNodeByPin(conn->m_InputPinGuid) == &node ||
                FindNodeByPin(conn->m_OutputPinGuid) == &node)
            {
                toRemove.push_back(cid);
            }
        }
        for (auto& cid : toRemove)
            RemoveConnection(cid);

        // --- Erase all pin mappings ---
        for (auto& ip : node.m_InputPins)
            m_PinToNode.erase(ip.m_PinGUID);
        for (auto& op : node.m_OutputPins)
        {
            m_PinToNode.erase(op.m_PinGUID);
            for (auto& sub : op.m_SubElements)
                m_PinToNode.erase(sub.m_PinGUID);
        }

        m_InstanceNodes.erase(it);
    }

    node* graph::FindNodeByPin(pin_guid pin) const
    {
        auto it = m_PinToNode.find(pin);
        if (it == m_PinToNode.end()) return nullptr;

        auto itN = m_InstanceNodes.find(it->second);
        if (itN == m_InstanceNodes.end()) return nullptr;

        return itN->second.get();
    }

    sType* graph::GetType(type_guid id) const
    {
        auto it = m_type.find(id);
        return it == m_type.end() ? nullptr : it->second.get();
    }

    void graph::RebuildPinGuidForNode(const node& n)
    {
        for (auto it = m_PinToNode.begin(); it != m_PinToNode.end(); )
        {
            if (it->second == n.m_Guid) it = m_PinToNode.erase(it);
            else                        ++it;
        }

        // Re-index current pins
        for (const auto& ip : n.m_InputPins)
            m_PinToNode[ip.m_PinGUID] = n.m_Guid;
        for (const auto& op : n.m_OutputPins)
        {
            m_PinToNode[op.m_PinGUID] = n.m_Guid;
            for (const auto& sub : op.m_SubElements)
                m_PinToNode[sub.m_PinGUID] = n.m_Guid;
        }
    }

    void graph::serialize(graph& graph, bool isReading, const std::wstring_view filepath)
    {
        xtextfile::stream TextFile;
        xproperty::settings::context Context;
        
        if (auto Err = TextFile.Open(isReading, filepath
            , xtextfile::file_type::TEXT, xtextfile::flags{ .m_isWriteFloats = true }); Err)
        {
            assert(false);
        }

        if (TextFile.isReading())
        {
            graph.m_InstanceNodes.clear();//make sure the node container is empty
            graph.m_PinToNode.clear();
            graph.m_Connections.clear();  //make sure the connection container is empty
        }
        //
        //for reading and writing of nodes
        //
        int nNodeCount = static_cast<int>(graph.m_InstanceNodes.size());
        int nConnectionsCount = static_cast<int>(graph.m_Connections.size());
        xerr Error;

        TextFile.Record("Info"
            , [&](xerr& Error)
            {
                false
                || (Error = TextFile.Field("nNodes", nNodeCount))
                || (Error = TextFile.Field("nConnections", nConnectionsCount));
            });

        auto Itr = graph.m_InstanceNodes.begin();
        for (int i = 0; i < nNodeCount; ++i)
        {
            node_guid Guid;
            node_guid PrefabGuid;

            if (false == TextFile.isReading()) //writing
            {
                Guid = Itr->second->m_Guid;
                PrefabGuid = Itr->second->m_PrefabGuid;
                ++Itr;
            }

            //Object
            TextFile.Record("Node"
                , [&](xerr& Error)
                {
                    false
                    || (Error = TextFile.Field("Guid", Guid.m_Value))
                    || (Error = TextFile.Field("PrefabGuid", PrefabGuid.m_Value));
                });

            if (TextFile.isReading()) //reading from text file
            {
                auto& newNode = graph.CreateNode(PrefabGuid, Guid);
            }

            //properties of the object
            //properties will be set from the text file
            auto& Entry = *graph.m_InstanceNodes[Guid];
            if (auto Err = xproperty::sprop::serializer::Stream(TextFile, Entry, Context); Err)
            {
                assert(false);
            }

            //apply position to editor
            if (TextFile.isReading()) //reading from text file
            {
                graph.RebuildPinGuidForNode(Entry);
            }
        }

        //
        //for reading and writing of connections
        //
        auto ItrConn = graph.m_Connections.begin();
        for (int c = 0; c < nConnectionsCount; ++c)
        {
            connection_guid connGuid;

            if (false == TextFile.isReading()) //is writing
            {
                connGuid = ItrConn->second->m_Guid;
                ++ItrConn;
            }

            TextFile.Record("Connection",
                [&](xerr& Error)
                {
                    Error = TextFile.Field("Guid", connGuid.m_Value);
                }
            );

            if (TextFile.isReading()) //reading
            {
                auto& conn = graph.createConnection(connGuid);
            }

            auto& Entry = *graph.m_Connections[connGuid];
            if (auto Err = xproperty::sprop::serializer::Stream(TextFile, Entry, Context); Err)
            {
                assert(false);
            }

        }
    }

    const pin* graph::FindPinConst(const node& n, pin_guid pid, bool& isInput, int& idxOut, int& subOut) const
    {
        // input search
        for (int i = 0; i < (int)n.m_InputPins.size(); ++i)
        {
            if (n.m_InputPins[i].m_PinGUID == pid)
            {
                isInput = true; idxOut = i; subOut = -1; return &n.m_InputPins[i];
            }
        }
        // output search
        for (int i = 0; i < (int)n.m_OutputPins.size(); ++i)
        {
            if (n.m_OutputPins[i].m_PinGUID == pid)
            {
                isInput = false; idxOut = i; subOut = -1; return &n.m_OutputPins[i];
            }
            for (int s = 0; s < (int)n.m_OutputPins[i].m_SubElements.size(); ++s)
                if (n.m_OutputPins[i].m_SubElements[s].m_PinGUID == pid)
                {
                    isInput = false; idxOut = i; subOut = s; return &n.m_OutputPins[i].m_SubElements[s];
                }
        }
        return nullptr;
    }
    //
    //When add new types or nodes remember to recompile shader_compiler.sln too
    // 
    //
    void graph::CreateGraph(graph& g)
    {
        //
        //-----types-----
        // 
        //float
        auto& typeFloat = g.CreateType(type_guid{ xresource::type_guid{"float"}.m_Value });
        typeFloat.m_Name        = "float";
        typeFloat.m_CodeString  = "float";
        typeFloat.m_Color       = FromRGB(240, 200, 50);

        //vec2
        auto& typeVec2 = g.CreateType(type_guid{ xresource::type_guid{"Vector2"}.m_Value });
        typeVec2.m_Name         = "Vector2";
        typeVec2.m_CodeString   = "vec2";
        typeVec2.m_Color        = FromRGB(80, 180, 250);
        typeVec2.m_Sub.resize(2);
        typeVec2.m_Sub[0].m_Name        = "x";
        typeVec2.m_Sub[0].m_TypeGUID    = typeFloat.m_GUID;
        typeVec2.m_Sub[1].m_Name        = "y";
        typeVec2.m_Sub[1].m_TypeGUID    = typeFloat.m_GUID;

        //vec3
        auto& typeVec3 = g.CreateType(type_guid{ xresource::type_guid{"Vector3"}.m_Value });
        typeVec3.m_Name         = "Vector3";
        typeVec3.m_CodeString   = "vec3";
        typeVec3.m_Color        = FromRGB(80, 220, 120);
        typeVec3.m_Sub.resize(3);
        typeVec3.m_Sub[0].m_Name        = "x";
        typeVec3.m_Sub[0].m_TypeGUID    = typeFloat.m_GUID;
        typeVec3.m_Sub[1].m_Name        = "y";
        typeVec3.m_Sub[1].m_TypeGUID    = typeFloat.m_GUID;
        typeVec3.m_Sub[2].m_Name        = "z";
        typeVec3.m_Sub[2].m_TypeGUID    = typeFloat.m_GUID;

        //vec4
        auto& typeVec4 = g.CreateType(type_guid{ xresource::type_guid{"Vector4"}.m_Value });
        typeVec4.m_Name         = "Vector4";
        typeVec4.m_CodeString   = "vec4";
        typeVec4.m_Color        = FromRGB(180, 80, 220);
        typeVec4.m_Sub.resize(4);
        typeVec4.m_Sub[0].m_Name        = "x";
        typeVec4.m_Sub[0].m_TypeGUID    = typeFloat.m_GUID;
        typeVec4.m_Sub[1].m_Name        = "y";
        typeVec4.m_Sub[1].m_TypeGUID    = typeFloat.m_GUID;
        typeVec4.m_Sub[2].m_Name        = "z";
        typeVec4.m_Sub[2].m_TypeGUID    = typeFloat.m_GUID;
        typeVec4.m_Sub[3].m_Name        = "w";
        typeVec4.m_Sub[3].m_TypeGUID    = typeFloat.m_GUID;

        // sampler2D
        auto& typeSampler2D = g.CreateType(type_guid{ xresource::type_guid{"Sampler2D"}.m_Value });
        typeSampler2D.m_Name        = "Sampler2D";
        typeSampler2D.m_CodeString  = "sampler2D";
        typeSampler2D.m_Color       = FromRGB(240, 80, 80);

        //string
        auto& typeTexture = g.CreateType(type_guid{ xresource::type_guid{"texture"}.m_Value });
        typeTexture.m_Name          = "Texture";
        typeTexture.m_CodeString    = "texture";
        typeTexture.m_Color         = FromRGB(200, 180, 80);

        //
        //-----Prefabs------
        // 
        //const vec3
        {
            auto& Vec3ConstantPrefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"Vec3 Const Node"}.m_Value });
            Vec3ConstantPrefabs.m_Name      = "Vec3 Const";
            Vec3ConstantPrefabs.m_Code      = "vec3 $var = vec3($input[0]_prop[0],$input[1]_prop[1],$input[2]_prop[2]);\n";
            Vec3ConstantPrefabs.m_InputPins.resize(3);

            Vec3ConstantPrefabs.m_InputPins[0].m_Name       = "X(1)";
            Vec3ConstantPrefabs.m_InputPins[0].m_TypeGUID   = typeFloat.m_GUID;
            Vec3ConstantPrefabs.m_InputPins[0].m_ParamIndex = static_cast<int>(Vec3ConstantPrefabs.m_Params.m_Properties.size());
            Vec3ConstantPrefabs.m_Params.m_Properties.push_back({ .m_Path= "x", .m_Value= xproperty::any(0.0f) });

            Vec3ConstantPrefabs.m_InputPins[1].m_Name       = "Y(1)";
            Vec3ConstantPrefabs.m_InputPins[1].m_TypeGUID   = typeFloat.m_GUID;
            Vec3ConstantPrefabs.m_InputPins[1].m_ParamIndex = static_cast<int>(Vec3ConstantPrefabs.m_Params.m_Properties.size());
            Vec3ConstantPrefabs.m_Params.m_Properties.push_back({ .m_Path= "y", .m_Value= xproperty::any(0.0f) });

            Vec3ConstantPrefabs.m_InputPins[2].m_Name       = "Z(1)";
            Vec3ConstantPrefabs.m_InputPins[2].m_TypeGUID   = typeFloat.m_GUID;
            Vec3ConstantPrefabs.m_InputPins[2].m_ParamIndex = static_cast<int>(Vec3ConstantPrefabs.m_Params.m_Properties.size());
            Vec3ConstantPrefabs.m_Params.m_Properties.push_back({ .m_Path= "z", .m_Value= xproperty::any(0.0f) });

            Vec3ConstantPrefabs.m_OutputPins.resize(1);
            Vec3ConstantPrefabs.m_OutputPins[0].m_Name      = "Vec3";
            Vec3ConstantPrefabs.m_OutputPins[0].m_TypeGUID  = typeVec3.m_GUID;

            g.CompletePrefab(Vec3ConstantPrefabs);
        }


        //const vec2
        {
            auto& Vec2ConstantPrefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"Vec2 Const Node"}.m_Value });
            Vec2ConstantPrefabs.m_Name = "Vec2 Const";
            Vec2ConstantPrefabs.m_Code = "vec2 $var = vec2($input[0]_prop[0],$input[1]_prop[1]);\n";
            Vec2ConstantPrefabs.m_InputPins.resize(2);

            Vec2ConstantPrefabs.m_InputPins[0].m_Name       = "X(1)";
            Vec2ConstantPrefabs.m_InputPins[0].m_TypeGUID   = typeFloat.m_GUID;
            Vec2ConstantPrefabs.m_InputPins[0].m_ParamIndex = static_cast<int>(Vec2ConstantPrefabs.m_Params.m_Properties.size());
            Vec2ConstantPrefabs.m_Params.m_Properties.push_back({ .m_Path= "x", .m_Value= xproperty::any(0.0f) });

            Vec2ConstantPrefabs.m_InputPins[1].m_Name       = "Y(1)";
            Vec2ConstantPrefabs.m_InputPins[1].m_TypeGUID   = typeFloat.m_GUID;
            Vec2ConstantPrefabs.m_InputPins[1].m_ParamIndex = static_cast<int>(Vec2ConstantPrefabs.m_Params.m_Properties.size());
            Vec2ConstantPrefabs.m_Params.m_Properties.push_back({ .m_Path= "y", .m_Value= xproperty::any(0.0f) });

            Vec2ConstantPrefabs.m_OutputPins.resize(1);
            Vec2ConstantPrefabs.m_OutputPins[0].m_Name      = "Vec2";
            Vec2ConstantPrefabs.m_OutputPins[0].m_TypeGUID  = typeVec2.m_GUID;

            g.CompletePrefab(Vec2ConstantPrefabs);
        }


        //--- final fragout ---
        {
            auto& finaloutputprefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"fragoutprefabs"}.m_Value });
            finaloutputprefabs.m_Name = "Final FragOut";
            finaloutputprefabs.m_Code = "[TOP]<!layout (location = 0) out  vec4 outFragColor;\n!> [BOT]<!outFragColor = vec4($input[0],$input[1]_prop[0]);\n!>";

            finaloutputprefabs.m_InputPins.resize(2);
            finaloutputprefabs.m_InputPins[0].m_Name        = "Color(3)";
            finaloutputprefabs.m_InputPins[0].m_TypeGUID    = typeVec3.m_GUID;
            finaloutputprefabs.m_InputPins[1].m_Name        = "A(1)";
            finaloutputprefabs.m_InputPins[1].m_TypeGUID    = typeFloat.m_GUID;
            finaloutputprefabs.m_InputPins[1].m_ParamIndex  = static_cast<int>(finaloutputprefabs.m_Params.m_Properties.size());
            finaloutputprefabs.m_Params.m_Properties.push_back({ .m_Path= "a", .m_Value= xproperty::any(1.0f) });

            g.CompletePrefab(finaloutputprefabs);
        }

        //pow function node
        {
            auto& powerfuncprefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"powerprefabs"}.m_Value });
            powerfuncprefabs.m_Name = "Pow Function";
            powerfuncprefabs.m_Code = "vec3 $var = pow($input[0], $input[1]);\n";

            powerfuncprefabs.m_InputPins.resize(2);
            powerfuncprefabs.m_InputPins[0].m_Name      = "A(3)";
            powerfuncprefabs.m_InputPins[0].m_TypeGUID  = typeVec3.m_GUID;
            powerfuncprefabs.m_InputPins[1].m_Name      = "B(3)";
            powerfuncprefabs.m_InputPins[1].m_TypeGUID  = typeVec3.m_GUID;

            powerfuncprefabs.m_OutputPins.resize(1);
            powerfuncprefabs.m_OutputPins[0].m_Name     = "Vec3";
            powerfuncprefabs.m_OutputPins[0].m_TypeGUID = typeVec3.m_GUID;

            g.CompletePrefab(powerfuncprefabs);
        }
        //convertVec4 to vec3 prefabs
        {
            auto& Vec4toVec3prefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"Vec4toVec3prefabs"}.m_Value });
            Vec4toVec3prefabs.m_Name = "Vec4toVec3 Func";
            Vec4toVec3prefabs.m_Code = "vec3 $var = vec3($input[0].xyz);\n";

            Vec4toVec3prefabs.m_InputPins.resize(1);
            Vec4toVec3prefabs.m_InputPins[0].m_Name     = "A(4)";
            Vec4toVec3prefabs.m_InputPins[0].m_TypeGUID = typeVec4.m_GUID;

            Vec4toVec3prefabs.m_OutputPins.resize(1);
            Vec4toVec3prefabs.m_OutputPins[0].m_Name     = "Vec3";
            Vec4toVec3prefabs.m_OutputPins[0].m_TypeGUID = typeVec3.m_GUID;

            g.CompletePrefab(Vec4toVec3prefabs);
        }

        //Vec4 multiply
        {
            auto& multiplyVec4 = g.CreatePrefabNode(node_guid{ xresource::type_guid{"MultiplyVec4Prefabs"}.m_Value });
            multiplyVec4.m_Name = "MultiplyVec4";
            multiplyVec4.m_Code = "vec4 $var = $input[0] * $input[1];\n";

            multiplyVec4.m_InputPins.resize(2);
            multiplyVec4.m_InputPins[0].m_Name      = "A(4)";
            multiplyVec4.m_InputPins[0].m_TypeGUID  = typeVec4.m_GUID;
            multiplyVec4.m_InputPins[1].m_Name      = "B(4)";
            multiplyVec4.m_InputPins[1].m_TypeGUID  = typeVec4.m_GUID;

            multiplyVec4.m_OutputPins.resize(1);
            multiplyVec4.m_OutputPins[0].m_Name     = "Vec4";
            multiplyVec4.m_OutputPins[0].m_TypeGUID = typeVec4.m_GUID;

            g.CompletePrefab(multiplyVec4);
        }

        //float multiplier
        {
            auto& multiplyFloat = g.CreatePrefabNode(node_guid{ xresource::type_guid{"MultiplyFloatPrefabs"}.m_Value });
            multiplyFloat.m_Name = "MultiplyFloat";
            multiplyFloat.m_Code = "float $var = $input[0] * $input[1];\n";

            multiplyFloat.m_InputPins.resize(2);
            multiplyFloat.m_InputPins[0].m_Name         = "A(1)";
            multiplyFloat.m_InputPins[0].m_TypeGUID     = typeFloat.m_GUID;
            multiplyFloat.m_InputPins[1].m_Name         = "B(1)";
            multiplyFloat.m_InputPins[1].m_TypeGUID     = typeFloat.m_GUID;

            multiplyFloat.m_OutputPins.resize(1);
            multiplyFloat.m_OutputPins[0].m_Name        = "A(1)";
            multiplyFloat.m_OutputPins[0].m_TypeGUID    = typeFloat.m_GUID;

            g.CompletePrefab(multiplyFloat);
        }


        //Texture Function
        {
            auto& texture = g.CreatePrefabNode(node_guid{ xresource::type_guid{"texturePrefabs"}.m_Value });
            texture.m_Name = "Texture";
            texture.m_Code = "vec4 $var = texture($input[0], $input[1]);\n";

            texture.m_InputPins.resize(2);
            texture.m_InputPins[0].m_Name       = "T(2D)";
            texture.m_InputPins[0].m_TypeGUID   = typeSampler2D.m_GUID;
            texture.m_InputPins[1].m_Name       = "UV(2)";
            texture.m_InputPins[1].m_TypeGUID   = typeVec2.m_GUID;

            texture.m_OutputPins.resize(1);
            texture.m_OutputPins[0].m_Name      = "Vec4";
            texture.m_OutputPins[0].m_TypeGUID  = typeVec4.m_GUID;

            g.CompletePrefab(texture);
        }

        //SamplerNode
        {
            auto& sampler = g.CreatePrefabNode(node_guid{ xresource::type_guid{"samplerPrefabs"}.m_Value });
            sampler.m_Name = "Sampler2D";
            sampler.m_Code = "[TOP]<!layout(binding = $tex[0]) uniform  sampler2D  $var;\n!>";

            sampler.m_OutputPins.resize(1);
            sampler.m_OutputPins[0].m_Name      = "Texture(2D)";
            sampler.m_OutputPins[0].m_TypeGUID  = typeSampler2D.m_GUID;

            sampler.m_InputPins.resize(1);
            sampler.m_InputPins[0].m_Name       = "texture";
            sampler.m_InputPins[0].m_TypeGUID   = typeTexture.m_GUID;
            sampler.m_InputPins[0].m_ParamIndex = static_cast<int>(sampler.m_Params.m_Properties.size());
            sampler.m_Params.m_Properties.push_back({ .m_Path= "texture", .m_Value= xproperty::any(std::string())});

            sampler.m_pCustomInput = nullptr;

#ifdef EDITOR
            sampler.m_pCustomInput = [](node& Node, int& Index, e10::library_mgr& LibraryMgr, xproperty::settings::context& Context )
                {
                    auto& prop = Node.m_Params.m_Properties[0];
                    std::string& texname = prop.m_Value.get<std::string>();
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 20.f);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.f);
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 32, 32, 200));
                    
                    if (ImGui::Button(std::format("{}{}", (texname == "None" || texname == "") ? "textures" : texname , std::to_string(Node.m_Guid.m_Value)).c_str(), ImVec2(68,14)))
                    {
                        ImVec2 button_pos = ax::NodeEditor::CanvasToScreen(ImGui::GetItemRectMin());
                        ImVec2 button_size = ax::NodeEditor::CanvasToScreen(ImGui::GetItemRectSize());
                        ImGui::SetNextWindowPos(ImVec2(button_pos.x, button_pos.y + 3.f));

                        std::string popupId = "textures selection##" + std::to_string(Node.m_Guid.m_Value);
                        ImGui::OpenPopup(popupId.c_str());
                        
                    }
                    ax::NodeEditor::Suspend();

                    std::string popupId = "textures selection##" + std::to_string(Node.m_Guid.m_Value);
                    if (ImGui::BeginPopup(popupId.c_str()))
                    {
                        /*
                        for (int n = 0; n < textname.size(); n++)
                        {
                            bool is_selected = (Index == n);
                            if (ImGui::Selectable(textname[n].c_str(), is_selected))
                            {
                                Index = n;
                                texname = textname[n];
                            }
                            if (is_selected)
                                ImGui::SetItemDefaultFocus();
                        }
                        */
                        ImGui::EndPopup();
                    }
                    ax::NodeEditor::Resume();
                    ImGui::PopStyleColor();
                };
#endif

            g.CompletePrefab(sampler);
        }

        //vertex input node
        {
            auto& vertexInput = g.CreatePrefabNode(node_guid{ xresource::type_guid{"vertexPrefabs"}.m_Value });
            vertexInput.m_Name = "VtxInput";
            vertexInput.m_Code = "[TOP]<!layout(location = 0) in struct { vec4 Color; vec2 UV; } In;\n!>";

            vertexInput.m_OutputPins.resize(2);
            vertexInput.m_OutputPins[0].m_Name          = "Clr(4)";
            vertexInput.m_OutputPins[0].m_TypeGUID      = typeVec4.m_GUID;
            vertexInput.m_OutputPins[0].m_DefaultExpr   = "In.Color";
            vertexInput.m_OutputPins[1].m_Name          = "UV(2)";
            vertexInput.m_OutputPins[1].m_TypeGUID      = typeVec2.m_GUID;
            vertexInput.m_OutputPins[1].m_DefaultExpr   = "In.UV";

            g.CompletePrefab(vertexInput);
        }

        //vec3 Dot Product vec3
        {
            auto& DotProduct = g.CreatePrefabNode(node_guid{ xresource::type_guid{"DotProductPrefabs"}.m_Value });
            DotProduct.m_Name = "DotProductV3";
            DotProduct.m_Code = "float $var = dot($input[0], $input[1]);\n";
            DotProduct.m_InputPins.resize(2);
            DotProduct.m_InputPins[0].m_Name        = "A(3)";
            DotProduct.m_InputPins[0].m_TypeGUID    = typeVec3.m_GUID;
            DotProduct.m_InputPins[1].m_Name        = "B(3)";
            DotProduct.m_InputPins[1].m_TypeGUID    = typeVec3.m_GUID;

            DotProduct.m_OutputPins.resize(1);
            DotProduct.m_OutputPins[0].m_Name       = "A(1)";
            DotProduct.m_OutputPins[0].m_TypeGUID   = typeFloat.m_GUID;

            g.CompletePrefab(DotProduct);
        }

        //Float constructor
        {
            auto& FloatConstructor = g.CreatePrefabNode(node_guid{ xresource::type_guid{"FloatConstructPrefabs"}.m_Value });
            FloatConstructor.m_Name = "Float Const";
            FloatConstructor.m_Code = "float $var = $input[0]_prop[0];\n";
            FloatConstructor.m_InputPins.resize(1);
            FloatConstructor.m_InputPins[0].m_Name          = "A(1)";
            FloatConstructor.m_InputPins[0].m_TypeGUID      = typeFloat.m_GUID;
            FloatConstructor.m_InputPins[0].m_ParamIndex    = static_cast<int>(FloatConstructor.m_Params.m_Properties.size());
            FloatConstructor.m_Params.m_Properties.push_back({ .m_Path= "x", .m_Value= xproperty::any(0.0f) });

            FloatConstructor.m_OutputPins.resize(1);
            FloatConstructor.m_OutputPins[0].m_Name     = "A(1)";
            FloatConstructor.m_OutputPins[0].m_TypeGUID = typeFloat.m_GUID;

            g.CompletePrefab(FloatConstructor);
        }

        //Floor Func
        {
            auto& FloorFloat = g.CreatePrefabNode(node_guid{ xresource::type_guid{"FloorFloatPrefabs"}.m_Value });
            FloorFloat.m_Name = "Float Floor Func";
            FloorFloat.m_Code = "float $var = floor($input[0]);\n";
            FloorFloat.m_InputPins.resize(1);
            FloorFloat.m_InputPins[0].m_Name        = "A(1)";
            FloorFloat.m_InputPins[0].m_TypeGUID    = typeFloat.m_GUID;

            FloorFloat.m_OutputPins.resize(1);
            FloorFloat.m_OutputPins[0].m_Name       = "A(1)";
            FloorFloat.m_OutputPins[0].m_TypeGUID   = typeFloat.m_GUID;

            g.CompletePrefab(FloorFloat);
        }

        //float divider
        {
            auto& floatDivider = g.CreatePrefabNode(node_guid{ xresource::type_guid{"FloatDividerPrefabs"}.m_Value });
            floatDivider.m_Name                     = "Float Divider";
            floatDivider.m_Code                     = "float $var = $input[0]/$input[1];\n";
            floatDivider.m_InputPins.resize(2);
            floatDivider.m_InputPins[0].m_Name      = "A(1)";
            floatDivider.m_InputPins[0].m_TypeGUID  = typeFloat.m_GUID;
            floatDivider.m_InputPins[1].m_Name      = "B(1)";
            floatDivider.m_InputPins[1].m_TypeGUID  = typeFloat.m_GUID;

            floatDivider.m_OutputPins.resize(1);
            floatDivider.m_OutputPins[0].m_Name     = "A(1)";
            floatDivider.m_OutputPins[0].m_TypeGUID = typeFloat.m_GUID;

            g.CompletePrefab(floatDivider);
        }

        //grouping node with comment
        {
            auto& groupWithComment = g.CreatePrefabNode(node_guid{ xresource::type_guid{"groupWithComment"}.m_Value });
            groupWithComment.m_Name = "Comment";
            groupWithComment.m_Params.m_Properties.push_back({ "Comment", xproperty::any(std::string()) });
            groupWithComment.m_Params.m_Properties.push_back({ "Size width", xproperty::any(10.f)});
            groupWithComment.m_Params.m_Properties.push_back({ "Size Height", xproperty::any(10.f)});
            g.CompletePrefab(groupWithComment);
        }
    }

    xerr shader_details::serializeShaderDetails(bool reading, std::wstring_view sDetailspath) const
    {
        xtextfile::stream               TextFile;
        xproperty::settings::context    Context;

        if (auto Err = TextFile.Open(reading, sDetailspath, xtextfile::file_type::TEXT, xtextfile::flags{ .m_isWriteFloats = true }); Err)
            return Err;

        if (auto Err = xproperty::sprop::serializer::Stream(TextFile, const_cast<shader_details&>(*this), Context); Err)
            return Err;

        return {};
    }
}
