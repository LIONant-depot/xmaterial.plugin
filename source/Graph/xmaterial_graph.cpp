
#include "dependencies/xproperty/source/xcore/my_properties.h"
#include <iostream>
#include <fstream>
#include <numeric>  // std::iota
#include <filesystem>
#include <thread>
#include <chrono>
#include <dependencies/xstrtool/source/xstrtool.h>

#ifdef EDITOR
    //#include "dependencies/imgui/imgui.h"
    #include "dependencies/imgui-node-editor/imgui_node_editor.h"
    #include "dependencies/xeditor/include/xeditor/hint.h"                // editor-only (needs imgui); the compiler builds without EDITOR
    #include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_asset_mgr.h"
    
    #include <commdlg.h>

    // Function found in E19
    void RemapGUIDToString(std::string& Name, const xresource::full_guid& PreFullGuid);
    void ResourceBrowserPopup(const void* pUID, bool& Open, xresource::full_guid& Output, std::span<const xresource::type_guid > Filter);

#endif

#include "xmaterial_graph.h"   
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
        assert(it != m_PrefabNodes.end());

        auto Node = std::make_unique<node>();
        assert(m_InstanceNodes.find(Guid) == m_InstanceNodes.end());

        *Node = *it->second;
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
        }
        //
        //for reading and writing of nodes
        //
        int nNodeCount = static_cast<int>(graph.m_InstanceNodes.size());
        int nConnectionsCount = static_cast<int>(graph.m_Connections.size());
        int  Version = 3, nParameters = int(graph.m_Params.size());
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
                const auto& Prefab = *graph.m_PrefabNodes[PrefabGuid];
                for (std::size_t o = 0; o < std::min(Prefab.m_OutputPins.size(), Entry.m_OutputPins.size()); ++o)
                    Entry.m_OutputPins[o].m_TypeGUID = Prefab.m_OutputPins[o].m_TypeGUID;
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

#ifdef EDITOR
    static void HandleResources(node& Node, xproperty::any& Value, xresource_editor::library_mgr& LibraryMgr, xproperty::settings::context& Context)
    {
        auto& prop = Node.m_Params[0];

        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 32, 32, 200));

        xresource::full_guid& FullGuid = Value.get<xresource::full_guid>();

        bool bOpen = false;
        xresource::full_guid NewFullGuid = {};

        std::string texname;
        RemapGUIDToString(texname, FullGuid);
        if (ImGui::Button(std::format("{}##{}", (texname.empty() || texname == "None") ? "textures" : texname, (void*) & Value).c_str(), ImVec2(100, 14)))
        {
            //ImVec2 button_pos  = ax::NodeEditor::CanvasToScreen(ImGui::GetItemRectMin());
            //ImVec2 button_size = ax::NodeEditor::CanvasToScreen(ImGui::GetItemRectSize());
            //ImGui::SetNextWindowPos(ImVec2(button_pos.x, button_pos.y + 3.f));
            bOpen = true;
        }
        static constexpr auto filters = std::array{ xrsc::texture_type_guid_v };
        ResourceBrowserPopup(&FullGuid, bOpen, NewFullGuid, filters);
        ax::NodeEditor::Suspend();
        if (ImGui::IsItemHovered())
            xeditor::hint::Text("%s", texname.c_str());

        std::string popupId = "textures selection##" + std::to_string(Node.m_Guid.m_Value);


        if (NewFullGuid.empty() == false && NewFullGuid.m_Type == xrsc::texture_type_guid_v)
        {
            FullGuid = NewFullGuid;
        }

        ax::NodeEditor::Resume();
        ImGui::PopStyleColor();
    };

    //--------------------------------------------------------------------------------------------------

    void HandleFilesAndResource(node& Node, xproperty::any& Value, xresource_editor::library_mgr& LibraryMgr, xproperty::settings::context& Context)
        {
            if (Value.m_pType->m_GUID != xproperty::settings::var_type<std::wstring>::guid_v)
            {
                HandleResources(Node, Value, LibraryMgr, Context);
                return;
            }

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 60.f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.f);
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 32, 32, 200));

            std::wstring FileName = Value.get<std::wstring>();

            std::string texname;

            auto ButtonName = std::format("{}##{}", xstrtool::To(xstrtool::PathFileName(FileName)), (void*) & Value);

            if (ImGui::Button(ButtonName.c_str(), ImVec2(102, 14)))
            {
                wchar_t fileBuffer[MAX_PATH] = { 0 };
                OPENFILENAMEW ofn = { 0 };
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = nullptr; // or a valid HWND
                ofn.lpstrFilter = node_param::file_filter_v;
                ofn.lpstrFile = fileBuffer;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
                ofn.lpstrDefExt = L"txt";
                ofn.lpstrInitialDir = xproperty::member_ui<std::wstring>::g_CurrentPath.c_str();

                if (GetOpenFileNameW(&ofn))
                {
                    //
                    // Set the name
                    //
                    {
                        std::size_t pos = xstrtool::findI(fileBuffer, L".lionprj");
                        std::size_t sz = sizeof(".lionprj");
                        if (pos == std::wstring::npos)
                        {
                            pos = xstrtool::findI(fileBuffer, L".lionlib");
                            sz = sizeof(".lionlib");
                        }

                        if (pos != std::wstring::npos)
                        {
                            Value.get<std::wstring>() = std::wstring(fileBuffer).substr(pos + sz);
                            int a = 3;
                        }
                    }

                    //
                    // Add new inputs
                    //
                    {
                        std::string Shader;
                        if (auto Err = RefreshShaderOnlyNode(fileBuffer, Node, Shader); Err)
                        {
                            std::cerr << "Error: " << Err.getMessage() << "\n";
                        }
                    }
                }
            }
            ax::NodeEditor::Suspend();

            if (ImGui::IsItemHovered())
                xeditor::hint::Text("%s", xstrtool::To(FileName).c_str());

            std::string popupId = "textures selection##" + std::to_string(Node.m_Guid.m_Value);


            ax::NodeEditor::Resume();
            ImGui::PopStyleColor();
        };


#endif


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

        // Color: linear RGBA, a vec4 for math. SColor: sRGB-encoded RGBA; it feeds only SColor inputs and has no sub-elements, so its
        // channels never leak out undecoded ('SColor To Color' decodes it; the rule is IsPinCompatible's, xmaterial_graph_editing.h)
        auto& typeColor = g.CreateType(color_type_guid_v);
        typeColor.m_Name        = "Color";
        typeColor.m_CodeString  = "vec4";
        typeColor.m_Color       = FromRGB(235, 235, 235);
        typeColor.m_Sub         = typeVec4.m_Sub;

        auto& typeSColor = g.CreateType(scolor_type_guid_v);
        typeSColor.m_Name       = "SColor";
        typeSColor.m_CodeString = "vec4";
        typeSColor.m_Color      = FromRGB(255, 130, 30);

        // sampler2D
        auto& typeSampler2D = g.CreateType(type_guid{ xresource::type_guid{"Sampler2D"}.m_Value });
        typeSampler2D.m_Name        = "Sampler2D";
        typeSampler2D.m_CodeString  = "sampler2D";
        typeSampler2D.m_Color       = FromRGB(240, 80, 80);

        //string
        auto& typeTexture = g.CreateType(texture_type_guid_v);
        typeTexture.m_Name          = "Texture";
        typeTexture.m_CodeString    = "texture";
        typeTexture.m_Color         = FromRGB(200, 180, 80);

        //
        //-----Prefabs------
        //

        // vec3
        {
            auto& Vec3ConstantPrefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"Vec3 Const Node"}.m_Value });
            Vec3ConstantPrefabs.m_Name      = "Vec3";
            Vec3ConstantPrefabs.m_Code      = "vec3 $var = vec3($input[0]_prop[0],$input[1]_prop[1],$input[2]_prop[2]);\n";
            Vec3ConstantPrefabs.m_InputPins.resize(3);

            Vec3ConstantPrefabs.m_InputPins[0].m_Name       = "X(1)";
            Vec3ConstantPrefabs.m_InputPins[0].m_TypeGUID   = typeFloat.m_GUID;
            Vec3ConstantPrefabs.m_InputPins[0].m_ParamIndex = static_cast<int>(Vec3ConstantPrefabs.m_Params.size());
            Vec3ConstantPrefabs.m_Params.emplace_back( "x", 0.0f );

            Vec3ConstantPrefabs.m_InputPins[1].m_Name       = "Y(1)";
            Vec3ConstantPrefabs.m_InputPins[1].m_TypeGUID   = typeFloat.m_GUID;
            Vec3ConstantPrefabs.m_InputPins[1].m_ParamIndex = static_cast<int>(Vec3ConstantPrefabs.m_Params.size());
            Vec3ConstantPrefabs.m_Params.emplace_back( "y", 0.0f );

            Vec3ConstantPrefabs.m_InputPins[2].m_Name       = "Z(1)";
            Vec3ConstantPrefabs.m_InputPins[2].m_TypeGUID   = typeFloat.m_GUID;
            Vec3ConstantPrefabs.m_InputPins[2].m_ParamIndex = static_cast<int>(Vec3ConstantPrefabs.m_Params.size());
            Vec3ConstantPrefabs.m_Params.emplace_back( "z", 0.0f );

            Vec3ConstantPrefabs.m_OutputPins.resize(1);
            Vec3ConstantPrefabs.m_OutputPins[0].m_Name      = "Vec3";
            Vec3ConstantPrefabs.m_OutputPins[0].m_TypeGUID  = typeVec3.m_GUID;

            g.CompletePrefab(Vec3ConstantPrefabs);
        }


        // vec2
        {
            auto& Vec2ConstantPrefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"Vec2 Const Node"}.m_Value });
            Vec2ConstantPrefabs.m_Name = "Vec2";
            Vec2ConstantPrefabs.m_Code = "vec2 $var = vec2($input[0]_prop[0],$input[1]_prop[1]);\n";

            Vec2ConstantPrefabs.m_InputPins.resize(2);
            Vec2ConstantPrefabs.m_InputPins[0].m_Name       = "X(1)";
            Vec2ConstantPrefabs.m_InputPins[0].m_TypeGUID   = typeFloat.m_GUID;
            Vec2ConstantPrefabs.m_InputPins[0].m_ParamIndex = static_cast<int>(Vec2ConstantPrefabs.m_Params.size());
            Vec2ConstantPrefabs.m_Params.emplace_back( "x", 0.0f );

            Vec2ConstantPrefabs.m_InputPins[1].m_Name       = "Y(1)";
            Vec2ConstantPrefabs.m_InputPins[1].m_TypeGUID   = typeFloat.m_GUID;
            Vec2ConstantPrefabs.m_InputPins[1].m_ParamIndex = static_cast<int>(Vec2ConstantPrefabs.m_Params.size());
            Vec2ConstantPrefabs.m_Params.emplace_back( "y", 0.0f );

            Vec2ConstantPrefabs.m_OutputPins.resize(1);
            Vec2ConstantPrefabs.m_OutputPins[0].m_Name      = "Vec2";
            Vec2ConstantPrefabs.m_OutputPins[0].m_TypeGUID  = typeVec2.m_GUID;

            g.CompletePrefab(Vec2ConstantPrefabs);
        }

        // vec4
        {
            auto& Vec4ConstantPrefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"Vec4 Const Node"}.m_Value });
            Vec4ConstantPrefabs.m_Name      = "Vec4";
            Vec4ConstantPrefabs.m_Code      = "vec4 $var = vec4($input[0]_prop[0],$input[1]_prop[1],$input[2]_prop[2],$input[3]_prop[3]);\n";

            Vec4ConstantPrefabs.m_InputPins.resize(4);
            Vec4ConstantPrefabs.m_InputPins[0].m_Name       = "X(1)";
            Vec4ConstantPrefabs.m_InputPins[0].m_TypeGUID   = typeFloat.m_GUID;
            Vec4ConstantPrefabs.m_InputPins[0].m_ParamIndex = static_cast<int>(Vec4ConstantPrefabs.m_Params.size());
            Vec4ConstantPrefabs.m_Params.emplace_back( "x", 0.0f );

            Vec4ConstantPrefabs.m_InputPins[1].m_Name       = "Y(1)";
            Vec4ConstantPrefabs.m_InputPins[1].m_TypeGUID   = typeFloat.m_GUID;
            Vec4ConstantPrefabs.m_InputPins[1].m_ParamIndex = static_cast<int>(Vec4ConstantPrefabs.m_Params.size());
            Vec4ConstantPrefabs.m_Params.emplace_back( "y", 0.0f );

            Vec4ConstantPrefabs.m_InputPins[2].m_Name       = "Z(1)";
            Vec4ConstantPrefabs.m_InputPins[2].m_TypeGUID   = typeFloat.m_GUID;
            Vec4ConstantPrefabs.m_InputPins[2].m_ParamIndex = static_cast<int>(Vec4ConstantPrefabs.m_Params.size());
            Vec4ConstantPrefabs.m_Params.emplace_back( "z", 0.0f );

            Vec4ConstantPrefabs.m_InputPins[3].m_Name       = "W(1)";
            Vec4ConstantPrefabs.m_InputPins[3].m_TypeGUID   = typeFloat.m_GUID;
            Vec4ConstantPrefabs.m_InputPins[3].m_ParamIndex = static_cast<int>(Vec4ConstantPrefabs.m_Params.size());
            Vec4ConstantPrefabs.m_Params.emplace_back( "w", 0.0f );

            Vec4ConstantPrefabs.m_OutputPins.resize(1);
            Vec4ConstantPrefabs.m_OutputPins[0].m_Name      = "Vec4";
            Vec4ConstantPrefabs.m_OutputPins[0].m_TypeGUID  = typeVec4.m_GUID;

            g.CompletePrefab(Vec4ConstantPrefabs);
        }

        // Output: Target Color
        {
            auto& finaloutputprefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"fragoutprefabs"}.m_Value });
            finaloutputprefabs.m_Name = "Output: Target Color";
            finaloutputprefabs.m_Code = "[TOP]<!layout (location = 0) out  vec4 outFragColor;\n!> [BOT]<!outFragColor = vec4($input[0],$input[1]_prop[0]);\n!>";

            finaloutputprefabs.m_InputPins.resize(2);
            finaloutputprefabs.m_InputPins[0].m_Name        = "Color(3)";
            finaloutputprefabs.m_InputPins[0].m_TypeGUID    = typeVec3.m_GUID;
            finaloutputprefabs.m_InputPins[1].m_Name        = "A(1)";
            finaloutputprefabs.m_InputPins[1].m_TypeGUID    = typeFloat.m_GUID;
            finaloutputprefabs.m_InputPins[1].m_ParamIndex  = static_cast<int>(finaloutputprefabs.m_Params.size());
            finaloutputprefabs.m_Params.emplace_back( "a", 1.0f );

            g.CompletePrefab(finaloutputprefabs);
        }

        // Output: PBR - the renderer's lit shading (xLIONRender mb_standard_pbr.frag PBRLighting: the scene's lights, shadows, GI and probes
        // in the HDR variant; the LDR one tone maps for the editors). Albedo and Emissive linear; Normal tangent space (Normal Map decodes one).
        // An unconnected Normal or Emissive takes its m_DefaultExpr; the floats are parameters (Roughness can be exposed like any float).
        {
            auto& PBR = g.CreatePrefabNode(node_guid{ xresource::type_guid{"pbroutprefabs"}.m_Value });
            PBR.m_Name = "Output: PBR";
            PBR.m_Code = "[TOP]<!#ifdef XLION_LDR_OUTPUT\n#define XLION_NO_SHADOW_MAP\n#endif\n"
                         "#include \"Cache/dependencies/xLIONRender/src/shaders/mb_standard_pbr.frag\"\n"
                         "#include \"Cache/dependencies/xLIONRender/src/shaders/mb_tone_mapper_lion.frag\"\n"
                         "#include \"Cache/dependencies/xLIONRender/src/shaders/mb_lineartogamma.frag\"\n"
                         "layout (location = 0) out vec4 outFragColor;\n!> "
                         "[BOT]<!vec3 $var = PBRLighting($input[3], $input[0], $input[4]_prop[2], 0.04, $input[1]_prop[0], $input[2]_prop[1], $input[5]);\n"
                         "#ifdef XLION_LDR_OUTPUT\n$var = linearToSrgb(ToneMapper_lion($var));\n#endif\n"
                         "outFragColor = vec4($var, $input[6]_prop[3]);\n!>";

            PBR.m_InputPins.resize(7);
            PBR.m_InputPins[0].m_Name           = "Albedo(3)";
            PBR.m_InputPins[0].m_TypeGUID       = typeVec3.m_GUID;
            PBR.m_InputPins[1].m_Name           = "Roughness(1)";
            PBR.m_InputPins[1].m_TypeGUID       = typeFloat.m_GUID;
            PBR.m_InputPins[1].m_ParamIndex     = static_cast<int>(PBR.m_Params.size());
            PBR.m_Params.emplace_back( "roughness", 0.5f );
            PBR.m_InputPins[2].m_Name           = "Metallic(1)";
            PBR.m_InputPins[2].m_TypeGUID       = typeFloat.m_GUID;
            PBR.m_InputPins[2].m_ParamIndex     = static_cast<int>(PBR.m_Params.size());
            PBR.m_Params.emplace_back( "metallic", 0.0f );
            PBR.m_InputPins[3].m_Name           = "Normal(3)";
            PBR.m_InputPins[3].m_TypeGUID       = typeVec3.m_GUID;
            PBR.m_InputPins[3].m_DefaultExpr    = "vec3(0.0, 0.0, 1.0)";
            PBR.m_InputPins[4].m_Name           = "AO(1)";
            PBR.m_InputPins[4].m_TypeGUID       = typeFloat.m_GUID;
            PBR.m_InputPins[4].m_ParamIndex     = static_cast<int>(PBR.m_Params.size());
            PBR.m_Params.emplace_back( "ao", 1.0f );
            PBR.m_InputPins[5].m_Name           = "Emissive(3)";
            PBR.m_InputPins[5].m_TypeGUID       = typeVec3.m_GUID;
            PBR.m_InputPins[5].m_DefaultExpr    = "vec3(0.0)";
            PBR.m_InputPins[6].m_Name           = "A(1)";
            PBR.m_InputPins[6].m_TypeGUID       = typeFloat.m_GUID;
            PBR.m_InputPins[6].m_ParamIndex     = static_cast<int>(PBR.m_Params.size());
            PBR.m_Params.emplace_back( "a", 1.0f );

            g.CompletePrefab(PBR);
        }

        // Normal Map: a BC5 tangent-space normal map's sample (xy in .gr, as mb_material_pbr.glsl reads it) for Output: PBR's Normal; Strength
        // exaggerates the bumps (1 = as authored)
        {
            auto& NormalMap = g.CreatePrefabNode(node_guid{ xresource::type_guid{"NormalMapPrefabs"}.m_Value });
            NormalMap.m_Name = "Normal Map";
            NormalMap.m_Code = "vec3 $var = vec3($input[0].gr * 2.0 - 1.0, 0.0);\n$var.z = sqrt(max(1.0 - dot($var.xy, $var.xy), 0.0));\n$var = normalize(vec3($var.xy * $input[1]_prop[0], $var.z));\n";
            NormalMap.m_InputPins.resize(2);
            NormalMap.m_InputPins[0].m_Name         = "A(4)";
            NormalMap.m_InputPins[0].m_TypeGUID     = typeVec4.m_GUID;
            NormalMap.m_InputPins[1].m_Name         = "Strength(1)";
            NormalMap.m_InputPins[1].m_TypeGUID     = typeFloat.m_GUID;
            NormalMap.m_InputPins[1].m_ParamIndex   = static_cast<int>(NormalMap.m_Params.size());
            NormalMap.m_Params.emplace_back( "strength", 1.0f );
            NormalMap.m_OutputPins.resize(1);
            NormalMap.m_OutputPins[0].m_Name        = "Normal";
            NormalMap.m_OutputPins[0].m_TypeGUID    = typeVec3.m_GUID;
            g.CompletePrefab(NormalMap);
        }

        // Output: Shader File
        {
            auto& finaloutputprefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"frag_shader"}.m_Value });
            finaloutputprefabs.m_Name = "Output: Shader File";
            finaloutputprefabs.m_Code = "[FULL_SHADER]";

            finaloutputprefabs.m_InputPins.resize(1);
            finaloutputprefabs.m_InputPins[0].m_Name        = "Shader File";
            finaloutputprefabs.m_InputPins[0].m_TypeGUID    = typeVec3.m_GUID;
            finaloutputprefabs.m_InputPins[0].m_ParamIndex  = static_cast<int>(finaloutputprefabs.m_Params.size());
            finaloutputprefabs.m_Params.emplace_back("Shader File", std::wstring(L"") );
            finaloutputprefabs.m_pCustomInput = nullptr;
#ifdef EDITOR
            finaloutputprefabs.m_pCustomInput = HandleFilesAndResource;
#endif
            g.CompletePrefab(finaloutputprefabs);
        }

        // Pow Function
        {
            auto& powerfuncprefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"powerprefabs"}.m_Value });
            powerfuncprefabs.m_Name = "Pow";
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

        // Vec4 To Vec2
        {
            auto& Vec4toVec2prefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"Vec4toVec2prefabs"}.m_Value });
            Vec4toVec2prefabs.m_Name = "Vec4 To Vec2";
            Vec4toVec2prefabs.m_Code = "vec2 $var = vec2($input[0].xy);\n";

            Vec4toVec2prefabs.m_InputPins.resize(1);
            Vec4toVec2prefabs.m_InputPins[0].m_Name         = "A(4)";
            Vec4toVec2prefabs.m_InputPins[0].m_TypeGUID     = typeVec4.m_GUID;

            Vec4toVec2prefabs.m_OutputPins.resize(1);
            Vec4toVec2prefabs.m_OutputPins[0].m_Name        = "Vec2";
            Vec4toVec2prefabs.m_OutputPins[0].m_TypeGUID    = typeVec2.m_GUID;

            g.CompletePrefab(Vec4toVec2prefabs);
        }

        // Vec4 To Vec3
        {
            auto& Vec4toVec3prefabs = g.CreatePrefabNode(node_guid{ xresource::type_guid{"Vec4toVec3prefabs"}.m_Value });
            Vec4toVec3prefabs.m_Name = "Vec4 To Vec3";
            Vec4toVec3prefabs.m_Code = "vec3 $var = vec3($input[0].xyz);\n";

            Vec4toVec3prefabs.m_InputPins.resize(1);
            Vec4toVec3prefabs.m_InputPins[0].m_Name         = "A(4)";
            Vec4toVec3prefabs.m_InputPins[0].m_TypeGUID     = typeVec4.m_GUID;

            Vec4toVec3prefabs.m_OutputPins.resize(1);
            Vec4toVec3prefabs.m_OutputPins[0].m_Name        = "Vec3";
            Vec4toVec3prefabs.m_OutputPins[0].m_TypeGUID    = typeVec3.m_GUID;

            g.CompletePrefab(Vec4toVec3prefabs);
        }

        //Vec4 Multiply
        {
            auto& multiplyVec4 = g.CreatePrefabNode(node_guid{ xresource::type_guid{"MultiplyVec4Prefabs"}.m_Value });
            multiplyVec4.m_Name = "Vec4 Multiply";
            multiplyVec4.m_Code = "vec4 $var = $input[0] * $input[1];\n";

            multiplyVec4.m_InputPins.resize(2);
            multiplyVec4.m_InputPins[0].m_Name          = "A(4)";
            multiplyVec4.m_InputPins[0].m_TypeGUID      = typeVec4.m_GUID;
            multiplyVec4.m_InputPins[1].m_Name          = "B(4)";
            multiplyVec4.m_InputPins[1].m_TypeGUID      = typeVec4.m_GUID;

            multiplyVec4.m_OutputPins.resize(1);
            multiplyVec4.m_OutputPins[0].m_Name         = "Vec4";
            multiplyVec4.m_OutputPins[0].m_TypeGUID     = typeVec4.m_GUID;

            g.CompletePrefab(multiplyVec4);
        }

        // Vec4 add
        {
            auto& addVec4 = g.CreatePrefabNode(node_guid{ xresource::type_guid{"addVec4Prefabs"}.m_Value });
            addVec4.m_Name = "Vec4 Add";
            addVec4.m_Code = "vec4 $var = $input[0] + $input[1];\n";

            addVec4.m_InputPins.resize(2);
            addVec4.m_InputPins[0].m_Name           = "A(4)";
            addVec4.m_InputPins[0].m_TypeGUID       = typeVec4.m_GUID;
            addVec4.m_InputPins[1].m_Name           = "B(4)";
            addVec4.m_InputPins[1].m_TypeGUID       = typeVec4.m_GUID;

            addVec4.m_OutputPins.resize(1);
            addVec4.m_OutputPins[0].m_Name          = "Vec4";
            addVec4.m_OutputPins[0].m_TypeGUID      = typeVec4.m_GUID;

            g.CompletePrefab(addVec4);
        }

        // Float Multiply
        {
            auto& multiplyFloat = g.CreatePrefabNode(node_guid{ xresource::type_guid{"MultiplyFloatPrefabs"}.m_Value });
            multiplyFloat.m_Name                        = "Float Multiply";
            multiplyFloat.m_Code                        = "float $var = $input[0] * $input[1];\n";

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


        // Texture
        {
            auto& texture = g.CreatePrefabNode(node_guid{ xresource::type_guid{"texturePrefabs"}.m_Value });
            texture.m_Name = "Texture";
            texture.m_Code = "vec4 $var = texture($input[0], $input[1]);\n";

            texture.m_InputPins.resize(2);
            texture.m_InputPins[0].m_Name       = "Texture(2D)";
            texture.m_InputPins[0].m_TypeGUID   = typeSampler2D.m_GUID;
            texture.m_InputPins[1].m_Name       = "UV(2)";
            texture.m_InputPins[1].m_TypeGUID   = typeVec2.m_GUID;

            texture.m_OutputPins.resize(1);
            texture.m_OutputPins[0].m_Name      = "Vec4";
            texture.m_OutputPins[0].m_TypeGUID  = typeVec4.m_GUID;

            g.CompletePrefab(texture);
        }

        // Sampler2D
        {
            auto& sampler = g.CreatePrefabNode(node_guid{ xresource::type_guid{"samplerPrefabs"}.m_Value });
            sampler.m_Name = "Sampler 2D";
            sampler.m_Code = "[TOP]<!layout(binding = $tex[0]) uniform  sampler2D  $var;\n!>";

            sampler.m_OutputPins.resize(1);
            sampler.m_OutputPins[0].m_Name          = "Texture(2D)";
            sampler.m_OutputPins[0].m_TypeGUID      = typeSampler2D.m_GUID;

            sampler.m_InputPins.resize(1);
            sampler.m_InputPins[0].m_Name           = "TextureRsc";
            sampler.m_InputPins[0].m_TypeGUID       = typeTexture.m_GUID;
            sampler.m_InputPins[0].m_ParamIndex     = static_cast<int>(sampler.m_Params.size());
            sampler.m_Params.emplace_back( "Default Texture", xmaterial_graph::node_param::type::TEXTURE_RESOURCE, xresource::full_guid(), true );
            sampler.m_pCustomInput = nullptr;

#ifdef EDITOR
            sampler.m_pCustomInput = &HandleResources;
#endif

            g.CompletePrefab(sampler);
        }

        // Input: Vertex Variants
        {
            auto& vertexInput = g.CreatePrefabNode(node_guid{ xresource::type_guid{"vertexPrefabs"}.m_Value });
            vertexInput.m_Name = "Input: Vertex Variants";
            vertexInput.m_Code = "[TOP]<!layout(location = 0) in struct { vec4 wSpacePosition; vec4 ShadowPosition; vec4 VertColor; vec4 Tangent; vec3 Normal; vec2 UV; } In;\n#define XMAT_VARYING_IN\n!>";      // (Output: PBR's lighting then reads this In)

            vertexInput.m_OutputPins.resize(2);
            vertexInput.m_OutputPins[0].m_Name          = "Color(4)";                // linear (no vertex format carries a colour yet: white)
            vertexInput.m_OutputPins[0].m_TypeGUID      = typeColor.m_GUID;
            vertexInput.m_OutputPins[0].m_DefaultExpr   = "In.VertColor";
            vertexInput.m_OutputPins[1].m_Name          = "UV(2)";
            vertexInput.m_OutputPins[1].m_TypeGUID      = typeVec2.m_GUID;
            vertexInput.m_OutputPins[1].m_DefaultExpr   = "In.UV";

            g.CompletePrefab(vertexInput);
        }

        // SColor To Color / Color To SColor: the exact piecewise sRGB transfer (IEC 61966-2-1), alpha untouched
        {
            auto& Decode = g.CreatePrefabNode(node_guid{ xresource::type_guid{"SColorToColorPrefabs"}.m_Value });
            Decode.m_Name = "SColor To Color";
            Decode.m_Code = "vec4 $var = vec4(mix($input[0].rgb / 12.92, pow(($input[0].rgb + 0.055) / 1.055, vec3(2.4)), greaterThan($input[0].rgb, vec3(0.04045))), $input[0].a);\n";
            Decode.m_InputPins.resize(1);
            Decode.m_InputPins[0].m_Name        = "A(4)";
            Decode.m_InputPins[0].m_TypeGUID    = typeSColor.m_GUID;
            Decode.m_OutputPins.resize(1);
            Decode.m_OutputPins[0].m_Name       = "Color";
            Decode.m_OutputPins[0].m_TypeGUID   = typeColor.m_GUID;
            g.CompletePrefab(Decode);

            auto& Encode = g.CreatePrefabNode(node_guid{ xresource::type_guid{"ColorToSColorPrefabs"}.m_Value });
            Encode.m_Name = "Color To SColor";
            Encode.m_Code = "vec4 $var = vec4(mix($input[0].rgb * 12.92, 1.055 * pow($input[0].rgb, vec3(1.0 / 2.4)) - 0.055, greaterThan($input[0].rgb, vec3(0.0031308))), $input[0].a);\n";
            Encode.m_InputPins.resize(1);
            Encode.m_InputPins[0].m_Name        = "A(4)";
            Encode.m_InputPins[0].m_TypeGUID    = typeColor.m_GUID;
            Encode.m_OutputPins.resize(1);
            Encode.m_OutputPins[0].m_Name       = "SColor";
            Encode.m_OutputPins[0].m_TypeGUID   = typeSColor.m_GUID;
            g.CompletePrefab(Encode);
        }

        // Vec3 Dot
        {
            auto& DotProduct = g.CreatePrefabNode(node_guid{ xresource::type_guid{"DotProductPrefabs"}.m_Value });
            DotProduct.m_Name = "Vec3 Dot";
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

        // Float
        {
            auto& FloatConstructor = g.CreatePrefabNode(node_guid{ xresource::type_guid{"FloatConstructPrefabs"}.m_Value });
            FloatConstructor.m_Name = "Float";
            FloatConstructor.m_Code = "float $var = $input[0]_prop[0];\n";
            FloatConstructor.m_InputPins.resize(1);
            FloatConstructor.m_InputPins[0].m_Name          = "A(1)";
            FloatConstructor.m_InputPins[0].m_TypeGUID      = typeFloat.m_GUID;
            FloatConstructor.m_InputPins[0].m_ParamIndex    = static_cast<int>(FloatConstructor.m_Params.size());
            FloatConstructor.m_Params.emplace_back( "x", 0.0f );

            FloatConstructor.m_OutputPins.resize(1);
            FloatConstructor.m_OutputPins[0].m_Name     = "A(1)";
            FloatConstructor.m_OutputPins[0].m_TypeGUID = typeFloat.m_GUID;

            g.CompletePrefab(FloatConstructor);
        }

        // Float Floor
        {
            auto& FloorFloat = g.CreatePrefabNode(node_guid{ xresource::type_guid{"FloorFloatPrefabs"}.m_Value });
            FloorFloat.m_Name = "Float Floor";
            FloorFloat.m_Code = "float $var = floor($input[0]);\n";
            FloorFloat.m_InputPins.resize(1);
            FloorFloat.m_InputPins[0].m_Name        = "A(1)";
            FloorFloat.m_InputPins[0].m_TypeGUID    = typeFloat.m_GUID;

            FloorFloat.m_OutputPins.resize(1);
            FloorFloat.m_OutputPins[0].m_Name       = "A(1)";
            FloorFloat.m_OutputPins[0].m_TypeGUID   = typeFloat.m_GUID;

            g.CompletePrefab(FloorFloat);
        }

        // Float Divider
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
