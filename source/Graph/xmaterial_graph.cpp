
#include "dependencies/xproperty/source/xcore/my_properties.h"
#include <iostream>
#include <fstream>
#include <numeric>  // std::iota
#include <dependencies/xstrtool/source/xstrtool.h>

#ifdef EDITOR
    //#include "dependencies/imgui/imgui.h"
    #include "dependencies/imgui-node-editor/imgui_node_editor.h"
    #include "source/Examples/E10_TextureResourcePipeline/E10_AssetMgr.h"
    
    #include <commdlg.h>

    // Function found in E19
    void RemapGUIDToString(std::string& Name, const xresource::full_guid& PreFullGuid);
    void ResourceBrowserPopup(const void* pUID, bool& Open, xresource::full_guid& Output, std::span<const xresource::type_guid > Filter);

#endif

#include "xmaterial_graph.h"   
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

        std::ifstream File(ShaderFileName);
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

                    auto result = std::from_chars(line.data() + binding_pos, &(*line.end()), BindingIndex);
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

                std::size_t FinalGuid   = Node.m_Guid.m_Value + 1000 + GUID;
                bool        bFound      = false;
                for (auto& E : Node.m_InputPins)
                {
                    if (E.m_PinGUID.m_Value == FinalGuid)
                    {
                        int Index = static_cast<int>(&E - Node.m_InputPins.data());
                        Found[Index] = 1;
                        bFound = true;
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
                TexturePos.erase(TexturePos.begin() + iParam);
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
            std::sort(idx.begin()+1, idx.end(), [&](std::size_t i, std::size_t j) 
            {
                return TexturePos[i] < TexturePos[j];
            });

            // Generic permutation apply (moves where possible, O(n) time + space)
            auto apply_perm = [](auto& vec, const std::vector<std::size_t>& indices)
            {
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
    static void HandleResources(node& Node, xproperty::any& Value, e10::library_mgr& LibraryMgr, xproperty::settings::context& Context)
    {
        auto& prop = Node.m_Params[0];

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 60.f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.f);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 32, 32, 200));

        xresource::full_guid& FullGuid = Value.get<xresource::full_guid>();

        bool bOpen = false;
        xresource::full_guid NewFullGuid = {};

        std::string texname;
        RemapGUIDToString(texname, FullGuid);
        if (ImGui::Button(std::format("{}##{}", (texname.empty() || texname == "None") ? "textures" : texname, std::to_string(Node.m_Guid.m_Value)).c_str(), ImVec2(100, 14)))
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
            ImGui::SetTooltip("%s", texname.c_str());

        std::string popupId = "textures selection##" + std::to_string(Node.m_Guid.m_Value);


        if (NewFullGuid.empty() == false && NewFullGuid.m_Type == xrsc::texture_type_guid_v)
        {
            FullGuid = NewFullGuid;
        }

        ax::NodeEditor::Resume();
        ImGui::PopStyleColor();
    };

    //--------------------------------------------------------------------------------------------------

    void HandleFilesAndResource(node& Node, xproperty::any& Value, e10::library_mgr& LibraryMgr, xproperty::settings::context& Context)
        {
            if (Value.m_pType->m_GUID != xproperty::settings::var_type<std::wstring>::guid_v)
            {
                HandleResources(Node, Value, LibraryMgr, Context);
                return;
            }

            auto& prop = Node.m_Params[0];

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 60.f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.f);
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 32, 32, 200));

            std::wstring FileName = Value.get<std::wstring>();

            std::string texname;
            if (ImGui::Button(std::format("{}##232331", xstrtool::To(xstrtool::PathFileName(FileName))).c_str(), ImVec2(100, 14)))
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
                ImGui::SetTooltip("%s", xstrtool::To(FileName).c_str());

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
            vertexInput.m_Code = "[TOP]<!layout(location = 0) in struct { vec4 Color; vec2 UV; } In;\n!>";

            vertexInput.m_OutputPins.resize(2);
            vertexInput.m_OutputPins[0].m_Name          = "Color(4)";
            vertexInput.m_OutputPins[0].m_TypeGUID      = typeVec4.m_GUID;
            vertexInput.m_OutputPins[0].m_DefaultExpr   = "In.Color";
            vertexInput.m_OutputPins[1].m_Name          = "UV(2)";
            vertexInput.m_OutputPins[1].m_TypeGUID      = typeVec2.m_GUID;
            vertexInput.m_OutputPins[1].m_DefaultExpr   = "In.UV";

            g.CompletePrefab(vertexInput);
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
    }
}
