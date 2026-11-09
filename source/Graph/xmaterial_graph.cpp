
#include "dependencies/xproperty/source/xcore/my_properties.h"
#include <iostream>
#include <fstream>
#include <numeric>  // std::iota
#include <filesystem>
#include <thread>
#include <chrono>
#include <dependencies/xstrtool/source/xstrtool.h>


#include "xmaterial_graph.h"
#include "xmaterial_node_types.h"   
#include "xmaterial_stack_adapter.h"
#include "dependencies/xproperty/source/sprop/property_sprop_xtextfile_serializer.h"

namespace xmaterial_graph
{

    static constexpr auto texture_type_guid_v = type_guid{ xresource::type_guid{"texture"}.m_Value };


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

        auto Node = std::make_unique<node>();
        assert(m_InstanceNodes.find(Guid) == m_InstanceNodes.end());

        if (it != m_PrefabNodes.end()) *Node = *it->second;
        else            // a node type no NodeTypes file defines (deleted, or its file did not load): the step stays, said in red, and nothing compiles it
        {
            Node->m_Name      = std::format("Unknown node type #{:016X}", PrefabGuid.m_Value);
            Node->m_HasErrMsg = true;
            Node->m_ErrMsg    = std::format("No file in the plugin's NodeTypes folder defines this node type (#{:016X}): it was removed, or its file did not load (the log says why). The material does not compile until it is back or this step is replaced.", PrefabGuid.m_Value);
        }
        Node->m_Guid = Guid;
        Node->m_PrefabGuid = PrefabGuid;

        //every new node created need to assign unique pin for each instance. The pins' ids come from the node's id (the same node always gets the
        //same pins), so creating a node again - an undo followed by a redo - gives the connections that refer to its pins something to find
        std::uint64_t iPin = 0;
        auto NextPinGuid = [&]
        {
            std::uint64_t z = Guid.m_Value + (++iPin) * 0x9E3779B97F4A7C15ull;              // splitmix64
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return pin_guid{ z ^ (z >> 31) };
        };

        //input pin unique id
        for (auto& ip : Node->m_InputPins)
        {
            ip.m_PinGUID = NextPinGuid();
            m_PinToNode[ip.m_PinGUID] = Guid;
        }

        //output pin unique id
        for (auto& op : Node->m_OutputPins)
        {
            op.m_PinGUID = NextPinGuid();
            m_PinToNode[op.m_PinGUID] = Guid;
            if (!op.m_SubElements.empty())
            {
                for (auto& sub : op.m_SubElements)
                {
                    sub.m_PinGUID = NextPinGuid();
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
                    Pin.m_SubElements[i].m_Name     = Type.m_Sub[i].m_Name;
                    Pin.m_SubElements[i].m_TypeGUID = Type.m_Sub[i].m_TypeGUID;
                }
            }
        }

        // Compute the max
        PrefabNode.ComputeMaxInputChars();
        PrefabNode.ComputeMaxOutputChars();
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
            if (auto* n = findNodeByPin(conn.m_InputPinGuid)) {
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
            if (findNodeByPin(conn->m_InputPinGuid) == &node ||
                findNodeByPin(conn->m_OutputPinGuid) == &node)
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

    node* graph::findNodeByPin(pin_guid pin) const
    {
        auto it = m_PinToNode.find(pin);
        if (it == m_PinToNode.end()) return nullptr;

        auto itN = m_InstanceNodes.find(it->second);
        if (itN == m_InstanceNodes.end()) return nullptr;

        return itN->second.get();
    }

    sType* graph::findType(type_guid id) const
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

    void stack::Reindex(void) noexcept
    {
        m_Where.clear();
        m_Global.clear();
        for (int s = 0, g = 0; s < (int)m_Sections.size(); ++s)
            for (int i = 0; i < (int)m_Sections[s].m_Steps.size(); ++i, ++g)
            {
                m_Where [m_Sections[s].m_Steps[i]] = { s, i };
                m_Global[m_Sections[s].m_Steps[i]] = g;
            }
    }

    bool stack::IsOrderValid(const graph& G, std::vector<connection_guid>* pViolations) const noexcept
    {
        bool bValid = true;
        for (auto& [cid, conn] : G.m_Connections)
        {
            auto* pProducer = G.findNodeByPin(conn->m_OutputPinGuid);
            auto* pConsumer = G.findNodeByPin(conn->m_InputPinGuid);
            if (pProducer == nullptr || pConsumer == nullptr) continue;

            auto P = m_Global.find(pProducer->m_Guid);
            auto C = m_Global.find(pConsumer->m_Guid);
            if (P == m_Global.end() || C == m_Global.end() || P->second < C->second) continue;     // not steps (comments) or fine

            bValid = false;
            if (pViolations) pViolations->push_back(cid);
        }
        return bValid;
    }

    void graph::serialize(graph& graph, bool isReading, const std::wstring_view filepath)
    {
        xtextfile::stream TextFile;
        xproperty::settings::context Context;
        
        // The project's file watcher briefly opens what was just written (an undo snapshot, a saved descriptor): try again for a while
        xerr OpenErr;
        for (int Try = 0; Try < 40; ++Try)
        {
            if (!(OpenErr = TextFile.Open(isReading, filepath, xtextfile::file_type::TEXT, xtextfile::flags{ .m_isWriteFloats = true }))) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (OpenErr)
        {
            assert(false);
        }

        if (TextFile.isReading())
        {
            graph.m_InstanceNodes.clear();//make sure the node container is empty
            graph.m_PinToNode.clear();
            graph.m_Connections.clear();  //make sure the connection container is empty
            graph.m_Stack = {};
            graph.m_Params.clear();
            graph.m_Render = {};                  // v3 files: the defaults (Opaque, single sided: as they rendered)
        }
        //
        //for reading and writing of nodes
        //
        int nNodeCount = static_cast<int>(graph.m_InstanceNodes.size());
        int nConnectionsCount = static_cast<int>(graph.m_Connections.size());
        int  Version = 4, nParameters = int(graph.m_Params.size());
        xerr Error;

        TextFile.Record("Info"
            , [&](xerr& Error)
            {
                false
                || (Error = TextFile.Field("nNodes", nNodeCount))
                || (Error = TextFile.Field("nConnections", nConnectionsCount));

                // v1 files have no Version column, v2 no nParameters (and a failed read must not stop the record)
                if (TextFile.isReading())
                {
                    if (TextFile.Field("Version", Version)) Version = 1;
                    else if (Version >= 3) Error = TextFile.Field("nParameters", nParameters);
                }
                else
                {
                    false
                    || (Error = TextFile.Field("Version", Version))
                    || (Error = TextFile.Field("nParameters", nParameters));
                }
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

                // outputs take their types from the node's definition, not the file (old files' vertex colour: Vector4 -> Color)
                if (auto It = graph.m_PrefabNodes.find(PrefabGuid); It != graph.m_PrefabNodes.end())      // (an unknown node type keeps the file's: CreateNode)
                    for (std::size_t o = 0; o < std::min(It->second->m_OutputPins.size(), Entry.m_OutputPins.size()); ++o)
                        Entry.m_OutputPins[o].m_TypeGUID = It->second->m_OutputPins[o].m_TypeGUID;
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

        //
        // A connection whose pins belong to no node (hand edits, deleted nodes of older versions) is dropped, and so is any reference to a missing connection
        //
        if (TextFile.isReading())
        {
            for (auto It = graph.m_Connections.begin(); It != graph.m_Connections.end(); )
            {
                if (graph.findNodeByPin(It->second->m_InputPinGuid) && graph.findNodeByPin(It->second->m_OutputPinGuid)) { ++It; continue; }
                printf("Warning: dropping the connection %016llX: its pins belong to no node\n", static_cast<unsigned long long>(It->first.m_Value));
                It = graph.m_Connections.erase(It);
            }
            for (auto& [Id, pNode] : graph.m_InstanceNodes)
                for (auto& Pin : pNode->m_InputPins)
                    if (Pin.m_ConnectionGUID.m_Value != 0 && !graph.m_Connections.contains(Pin.m_ConnectionGUID)) Pin.m_ConnectionGUID = {};
        }

        //
        // The organisation (v2 only, v1 files get theirs from the adapter)
        //
        if (Version >= 2)
        {
            int nSections = static_cast<int>(graph.m_Stack.m_Sections.size());
            TextFile.Record("StackLayout", [&](xerr& Error){ Error = TextFile.Field("nSections", nSections); });

            if (auto Err = xproperty::sprop::serializer::Stream(TextFile, graph.m_Stack, Context); Err)
            {
                assert(false);
            }
        }

        //
        // The declared parameters (v3 only)
        //
        if (Version >= 3)
        {
            if (TextFile.isReading()) graph.m_Params.resize(nParameters);
            for (auto& Decl : graph.m_Params)
            {
                TextFile.Record("Parameter", [&](xerr& Error){ Error = TextFile.Field("Guid", Decl.m_ID); });
                if (auto Err = xproperty::sprop::serializer::Stream(TextFile, Decl, Context); Err)
                {
                    assert(false);
                }
            }
        }
        //
        // The material's render settings (v4 only, documentation/render_settings_plan.md)
        //
        if (Version >= 4)
        {
            if (auto Err = xproperty::sprop::serializer::Stream(TextFile, graph.m_Render, Context); Err)
            {
                assert(false);
            }
        }
        if (TextFile.isReading() && Version < 2) stack_adapter::Migrate(graph);   // v1: no stack yet, sort the graph into one
        if (TextFile.isReading()) graph.m_Stack.Reindex();
    }

    const pin* graph::findPinConst(const node& n, pin_guid pid, bool& isInput, int& idxOut, int& subOut) const
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

    xerr RefreshShaderOnlyNode( std::wstring ShaderFileName, node& Node, std::string& Shader )
    {
        xstrtool::print( L"Opening Shader: {} \n", ShaderFileName );

        std::ifstream File{ std::filesystem::path(ShaderFileName) };
        if (not File.is_open())
        {
            xerr::LogMessage<state::FAILURE>( std::format( "Failed opening the shader file {}", xstrtool::To(ShaderFileName)));
            return xerr::create_f<state,"Failed opening the shader file">();
        }

        node             Backup         = Node;
        std::vector<int> TexturePos     = {};

        //
        // Process the shader
        //
        std::string         line;
        int                 lineNumber = 0;
        std::vector<int>    Found(Node.m_InputPins.size(), 0);
        bool                bCanExpose = true;
        while( std::getline(File, line) )
        {
            Shader = std::format( "{}\n{}", Shader, line );

            ++lineNumber;
            if (std::size_t pos = line.find("INPUT_TEXTURE_"); pos != std::string::npos)
            {
                std::string TextureName;
                if (std::size_t name_pos = line.find("sampler2D"); name_pos != std::string::npos)
                {
                    name_pos += sizeof("sampler2D");

                    // Skip any spaces
                    while (line[name_pos] == ' ') name_pos++;

                    char buffer[256] = { 0 };

                    for (int i = 0; line[name_pos + i] != ' ' && line[name_pos + i] != ';'; ++i)
                    {
                        buffer[i] = line[name_pos + i];
                        buffer[i + 1] = 0;
                    }

                    TextureName = buffer;
                }
                else
                {
                    Node = Backup;
                    return xerr::create_f<state, "Unsupported sampler type (PLEASE REQUEST FOR SUPPORT)!">();
                }

                int BindingIndex = -1;
                if( std::size_t binding_pos = line.find("binding"); binding_pos != std::string::npos)
                {
                    binding_pos += sizeof("binding")-1;

                    // Skip any spaces
                    while (line[binding_pos] == ' ') binding_pos++;

                    if ( line[binding_pos++] != '=')
                    {
                        Node = Backup;
                        return xerr::create_f<state, "Fail reading the binding... `binding =` is missing...">();
                    }

                    // Skip any spaces
                    while (line[binding_pos] == ' ') binding_pos++;

                    auto result = std::from_chars(line.data() + binding_pos, &(*(line.end()-1)), BindingIndex);
                    if (result.ec != std::errc())
                    {
                        std::error_code ec = std::make_error_code(result.ec);
                        xerr::LogMessage<state::FAILURE>( std::format( "Error converting the 'binding = ##err##' error: [{}]", ec.message()) );
                        Node = Backup;
                        return xerr::create_f<state, "Error converting the 'binding = ##err##'">();
                    }
                }
                else
                {
                    Node = Backup;
                    return xerr::create_f<state, "Unable to find the binding for one of the textures">();
                }

                pos += sizeof("INPUT_TEXTURE_") - 1;
                line = line.substr(pos);

                std::size_t systemPos = line.find("SYSTEM");
                if (systemPos != std::string::npos)
                {
                    bCanExpose = false;
                    // Skip keyword system
                    if (systemPos == 0) line = line.substr(sizeof("SYSTEM"));
                }

                int GUID;
                auto result = std::from_chars(line.data(), line.data() + line.size(), GUID);
                if (result.ec != std::errc())
                {
                    std::error_code ec = std::make_error_code(result.ec);
                    xerr::LogMessage<state::FAILURE>(std::format("Error converting the texture GUID, (INPUT_TEXTURE_##err##), error: [{}]", ec.message()));
                    Node = Backup;
                    return xerr::create_f<state, "Error converting the texture GUID, (INPUT_TEXTURE_##err##)">();
                }

                std::size_t FinalGuid   = GUID;
                bool        bFound      = false;
                for (auto& E : Node.m_InputPins)
                {
                    if (E.m_PinGUID.m_Value == FinalGuid)
                    {
                        int Index = static_cast<int>(&E - Node.m_InputPins.data());
                        Found[Index] = 1;
                        bFound = true;

                        // Make sure we are up to date with the name
                        E.m_Name                             = TextureName;
                        Node.m_Params[E.m_ParamIndex].m_Name = TextureName;

                        if (Index >= TexturePos.size()) TexturePos.resize(Index+1);
                        TexturePos[Index] = BindingIndex;
                        break;
                    }
                }

                if (bFound == false)
                {
                    auto& Input             = Node.m_InputPins.emplace_back();

                    Input.m_Name            = TextureName;
                    Input.m_TypeGUID        = texture_type_guid_v;
                    Input.m_ParamIndex      = static_cast<int>(Node.m_Params.size());
                    Input.m_PinGUID.m_Value = FinalGuid;
                    auto& Param = Node.m_Params.emplace_back(TextureName, node_param::type::TEXTURE_RESOURCE, xresource::full_guid{}, bCanExpose);

                    // Set some useful defaults...
                    if (bCanExpose)
                    {
                        Param.m_bExpose     = true;
                        Param.m_ExposeName  = TextureName;
                    }

                    std::size_t Index = Node.m_InputPins.size()-1;
                    if (Index >= TexturePos.size()) TexturePos.resize(Index + 1);
                    TexturePos[Index] = BindingIndex;
                }
            }
        }
        File.close();

        //
        // Delete unused inputs
        //
        for (auto& E : std::views::reverse(Found))
        {
            int Index = static_cast<int>(&E - Found.data());
            if (E == 0 && Index != 0)
            {
                int iParam = Node.m_InputPins[Index].m_ParamIndex;
                for (auto& I : Node.m_InputPins)
                {
                    if (I.m_ParamIndex > iParam) I.m_ParamIndex--;
                }

                // Delete the param
                Node.m_Params.erase(Node.m_Params.begin() + iParam);

                // Delete pin
                Node.m_InputPins.erase(Node.m_InputPins.begin() + Index);

                // Delete texture index
                // TODO: If we remove an entry don't we have to reindex all the params?
                if (iParam < TexturePos.size()) TexturePos.erase(TexturePos.begin() + iParam);
            }
        }

        //
        // Sort params base on the binding from 0..n of the textures...
        //
        {
            // Assume TexturePos and Node.m_Params have the same size > 0
            std::vector<std::size_t> idx(TexturePos.size());
            std::iota(idx.begin(), idx.end(), 0);

            // Sort indices from 1 to end by TexturePos values (ascending), leave idx[0] == 0
            if (idx.size() > 1)
            {
                std::sort(idx.begin() + 1, idx.end(), [&](std::size_t i, std::size_t j)
                {
                    return TexturePos[i] < TexturePos[j];
                });
            }

            // Generic permutation apply (moves where possible, O(n) time + space)
            auto apply_perm = [](auto& vec, const std::vector<std::size_t>& indices)
            {
                if (indices.empty()) return;

                using T = typename std::remove_reference<decltype(vec)>::type::value_type;
                std::vector<T> temp(vec.size());
                for (std::size_t i = 0; i < vec.size(); ++i) 
                {
                    temp[i] = std::move(vec[indices[i]]);
                }
                vec = std::move(temp);
            };

            apply_perm(TexturePos,    idx);
            apply_perm(Node.m_Params, idx);
        }

        //
        // Validate the binding positions
        //
        for (int i = 1; i < TexturePos.size(); ++i )
        {
            int Expecting = i-1;
            if (TexturePos[i] != Expecting)
            {
                printf("Error: Non consecutive bindings... expecting %d but found %d\n", Expecting, TexturePos[i] );
                xerr::LogMessage<state::FAILURE>(std::format("Error: Non consecutive bindings... expecting {} but found {}", Expecting, TexturePos[i]));
                Node = Backup;
                return xerr::create_f<state, "Non consecutive bindings...">();
            }
        }

        //
        // Make sure to update the max length of our inputs
        //
        Node.ComputeMaxInputChars();

        return {};
    }

    node* graph::findFullShaderNode()
    {
        for (auto& E : m_InstanceNodes)
        {
            if (E.second->isOutputNode())
            {
                if (E.second->m_Code == "[FULL_SHADER]")
                {
                    return E.second.get();
                }
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
        // The pin types and the node types: data files of the plugin (NodeTypes/: Types.txt, a <Name>.txt + <Name>.glsl per node type; read
        // once a process, xmaterial_node_types.h). Only the structural nodes below stay in C++ (their pins come from elsewhere)
        {
            const auto& L = node_types::Cached(NodeTypesFolder());
            for (const auto& [Guid, T] : L.m_Types) g.CreateType(type_guid{ Guid }) = T;
            for (const auto& [Guid, N] : L.m_Nodes)
            {
                auto& P = g.CreatePrefabNode(node_guid{ Guid });
                P = N; P.m_Guid = node_guid{ Guid };
                g.CompletePrefab(P);
            }
        }

        // Comment
        {
            auto& groupWithComment = g.CreatePrefabNode(node_guid{ xresource::type_guid{"groupWithComment"}.m_Value });
            groupWithComment.m_Name = "Comment";
            groupWithComment.m_Params.emplace_back( "Comment", std::string() );
            groupWithComment.m_Params.emplace_back( "Size width", 10.f );
            groupWithComment.m_Params.emplace_back( "Size Height", 10.f );
            g.CompletePrefab(groupWithComment);
        }

        // A sub-graph's interface (subgraphs_plan.md 2.2): no pins of their own, every instance gets its pins from AddGroupPin
        {
            auto& GroupInput = g.CreatePrefabNode(group_input_prefab_v);
            GroupInput.m_Name = "Group Input";
            g.CompletePrefab(GroupInput);

            auto& GroupOutput = g.CreatePrefabNode(group_output_prefab_v);
            GroupOutput.m_Name = "Group Output";
            g.CompletePrefab(GroupOutput);

            auto& Call = g.CreatePrefabNode(group_call_prefab_v);              // a group node: its pins come from its sub-graph (SetSubgraph)
            Call.m_Name = "Group";
            Call.m_Params.emplace_back("Subgraph", node_param::type::SUBGRAPH_RESOURCE, xresource::full_guid{});
            g.CompletePrefab(Call);
        }
    }
}
