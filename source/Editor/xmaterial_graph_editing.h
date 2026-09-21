#ifndef XMATERIAL_GRAPH_EDITING_H
#define XMATERIAL_GRAPH_EDITING_H
#pragma once

// The material's document (its node graph on disk) and the commands that edit it: create and delete nodes, connect and disconnect pins, move nodes,
// set a node's properties, pick a node's shader file. The Material editor's window calls these and so does an AI or a script through the console.
// Every structural command snapshots the whole graph first, so undo puts back everything it touched; moving and property edits undo by value.
#include "source/Tools/Editor/xeditor_document_editor.h"
#include "plugins/xmaterial.plugin/source/Graph/xmaterial_graph.h"

#include <charconv>

namespace xmaterial_editor
{
    //--------------------------------------------------------------------------------------------
    // Document
    //--------------------------------------------------------------------------------------------
    struct graph_document : xeditor::file_document
    {
        xmaterial_graph::graph  m_Graph;
        bool                    m_bLoaded           = false;
        bool                    m_bPositionsChanged = true;         // the nodes moved behind the canvas's back (a load, an undo): it must move them

        bool isLoaded() const noexcept override { return m_bLoaded; }

        bool Reset() noexcept override
        {
            m_Graph.m_InstanceNodes.clear();
            m_Graph.m_Connections.clear();
            m_Graph.m_PinToNode.clear();
            if (m_Graph.m_PrefabNodes.empty()) m_Graph.CreateGraph(m_Graph);
            m_bLoaded = true;
            m_bPositionsChanged = true;
            return true;
        }

        bool ReplaceFromFile(const std::wstring& Path) noexcept override
        {
            if (!std::filesystem::exists(Path)) return false;
            m_Graph.serialize(m_Graph, true, Path);
            m_bLoaded = true;
            m_bPositionsChanged = true;
            return true;
        }

        bool WriteToFile(const std::wstring& Path) noexcept override
        {
            if (!m_bLoaded) return false;
            m_Graph.serialize(m_Graph, false, Path);
            return true;
        }

        void Validate(std::vector<std::string>& Errors) const noexcept override
        {
            if (m_Graph.m_InstanceNodes.empty()) Errors.push_back("The graph has no nodes");
        }

        xmaterial_graph::node* FindNode(std::uint64_t Guid) noexcept
        {
            auto It = m_Graph.m_InstanceNodes.find(xmaterial_graph::node_guid{ Guid });
            return It == m_Graph.m_InstanceNodes.end() ? nullptr : It->second.get();
        }
    };

    // Two pins can be connected when they are on different nodes and carry the same type.
    inline bool IsPinCompatible(const xmaterial_graph::pin& Out, const xmaterial_graph::pin& In, const xmaterial_graph::graph& G) noexcept
    {
        if (G.findNodeByPin(In.m_PinGUID) == G.findNodeByPin(Out.m_PinGUID)) return false;
        auto* pOut = G.findType(Out.m_TypeGUID);
        auto* pIn  = G.findType(In.m_TypeGUID);
        return pOut && pIn && pOut->m_GUID == pIn->m_GUID;
    }

    //--------------------------------------------------------------------------------------------
    // Commands
    //--------------------------------------------------------------------------------------------
    namespace cmds
    {
        using namespace xeditor::cmd_util;

        inline bool HexArg(const xcmdline::parser& Parser, xcmdline::parser::handle H, std::uint64_t& Out) noexcept
        {
            std::string Text;
            if (!GetArg(Parser, H, Text) || Text.empty()) return false;
            const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Out, 16);
            return Result.ec == std::errc() && Result.ptr == Text.data() + Text.size();
        }

        inline bool FloatArg(const xcmdline::parser& Parser, xcmdline::parser::handle H, float& Out) noexcept
        {
            std::string Text;
            if (!GetArg(Parser, H, Text) || Text.empty()) return false;
            const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Out);
            return Result.ec == std::errc() && Result.ptr == Text.data() + Text.size();
        }

        // A structural edit: Apply changes the graph, undo restores the snapshot taken before.
        struct graph_cmd : xundo::command_base
        {
            graph_document& m_Doc;
            graph_cmd(xundo::system& System, graph_document& Doc, const char* pName) noexcept : command_base(System, pName, nullptr), m_Doc(Doc) {}

            virtual std::string Apply() noexcept = 0;

            std::string Redo() noexcept override
            {
                if (!m_Doc.isLoaded()) return std::format("{}: nothing loaded", m_pCommandName);
                auto Err = Apply();
                if (!Err.empty()) return std::format("{}: {}", m_pCommandName, Err);
                m_Doc.m_bDirty = true;
                return {};
            }
            void BackupCurrenState(xundo::undo_file& File) noexcept override { xeditor::WriteString(File, m_Doc.Snapshot()); }
            void Undo(xundo::undo_file& File) noexcept override               { m_Doc.Restore(xeditor::ReadString(File)); }
        };

        //---- CreateNode: a node of a type (see ListNodeTypes). The caller picks the new node's id, so redo makes the same node again.
        struct create_node_cmd : graph_cmd
        {
            create_node_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "CreateNode") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Creates a node (undoable). Usage: CreateNode -Prefab hexguid -Node hexguid [-X float -Y float]. Prefab: a type from ListNodeTypes; Node: a new, unused id."; }
            void RegisterArguments() noexcept override
            {
                m_hPrefab = m_Parser.addOption("Prefab", "The node type's guid, hex",         true,  1);
                m_hNode   = m_Parser.addOption("Node",   "The new node's guid, hex",          true,  1);
                m_hX      = m_Parser.addOption("X",      "Position on the canvas",            false, 1);
                m_hY      = m_Parser.addOption("Y",      "Position on the canvas",            false, 1);
            }
            std::string Apply() noexcept override
            {
                std::uint64_t Prefab = 0, Node = 0;
                if (!HexArg(m_Parser, m_hPrefab, Prefab) || !HexArg(m_Parser, m_hNode, Node) || Node == 0) return "bad arguments";
                if (!m_Doc.m_Graph.m_PrefabNodes.contains(xmaterial_graph::node_guid{ Prefab })) return "no such node type";
                if (m_Doc.FindNode(Node)) return "a node with that id exists";

                auto& N = m_Doc.m_Graph.CreateNode(xmaterial_graph::node_guid{ Prefab }, xmaterial_graph::node_guid{ Node });
                float X = 0, Y = 0;
                FloatArg(m_Parser, m_hX, X); FloatArg(m_Parser, m_hY, Y);
                N.m_Pos = { X, Y };
                m_Doc.m_bPositionsChanged = true;
                return {};
            }
            xcmdline::parser::handle m_hPrefab, m_hNode, m_hX, m_hY;
        };

        //---- DeleteNode: with every connection that touches it
        struct delete_node_cmd : graph_cmd
        {
            delete_node_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "DeleteNode") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Deletes a node and its connections (undoable). Usage: DeleteNode -Node hexguid"; }
            void RegisterArguments() noexcept override { m_hNode = m_Parser.addOption("Node", "The node's guid, hex", true, 1); }
            std::string Apply() noexcept override
            {
                std::uint64_t Node = 0;
                if (!HexArg(m_Parser, m_hNode, Node)) return "bad arguments";
                if (!m_Doc.FindNode(Node)) return "no such node";
                m_Doc.m_Graph.RemoveNode(xmaterial_graph::node_guid{ Node });
                return {};
            }
            xcmdline::parser::handle m_hNode;
        };

        //---- Connect: an output pin to an input pin. An input takes one connection: an older one is replaced.
        struct connect_cmd : graph_cmd
        {
            connect_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "Connect") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Connects an output pin to an input pin of the same type (undoable; replaces the input's connection). Usage: Connect -Output hexguid -Input hexguid -Connection hexguid"; }
            void RegisterArguments() noexcept override
            {
                m_hOutput     = m_Parser.addOption("Output",     "The output pin's guid, hex",        true, 1);
                m_hInput      = m_Parser.addOption("Input",      "The input pin's guid, hex",         true, 1);
                m_hConnection = m_Parser.addOption("Connection", "The new connection's guid, hex",    true, 1);
            }
            std::string Apply() noexcept override
            {
                std::uint64_t Out = 0, In = 0, Connection = 0;
                if (!HexArg(m_Parser, m_hOutput, Out) || !HexArg(m_Parser, m_hInput, In) || !HexArg(m_Parser, m_hConnection, Connection) || Connection == 0) return "bad arguments";
                auto& G = m_Doc.m_Graph;

                auto* pOutNode = G.findNodeByPin(xmaterial_graph::pin_guid{ Out });
                auto* pInNode  = G.findNodeByPin(xmaterial_graph::pin_guid{ In });
                if (!pOutNode || !pInNode) return "no such pin";
                bool bOutIsInput = true, bInIsInput = false;
                int  Index = 0, Sub = 0;
                auto* pOut = G.findPinConst(*pOutNode, xmaterial_graph::pin_guid{ Out }, bOutIsInput, Index, Sub);
                auto* pIn  = G.findPinConst(*pInNode,  xmaterial_graph::pin_guid{ In },  bInIsInput,  Index, Sub);
                if (!pOut || !pIn || bOutIsInput || !bInIsInput) return "that connects an output pin to an input pin";
                if (!IsPinCompatible(*pOut, *pIn, G)) return "the pins' types differ (or they are on the same node)";
                if (G.m_Connections.contains(xmaterial_graph::connection_guid{ Connection })) return "a connection with that id exists";

                for (auto& [Id, C] : G.m_Connections)
                    if (C->m_InputPinGuid.m_Value == In) { G.RemoveConnection(Id); break; }

                auto& Link = G.createConnection(xmaterial_graph::connection_guid{ Connection });
                Link.m_OutputPinGuid = xmaterial_graph::pin_guid{ Out };
                Link.m_InputPinGuid  = xmaterial_graph::pin_guid{ In };
                if (const int i = pInNode->getInputPinIndex(Link.m_InputPinGuid); i >= 0) pInNode->m_InputPins[i].m_ConnectionGUID = Link.m_Guid;
                return {};
            }
            xcmdline::parser::handle m_hOutput, m_hInput, m_hConnection;
        };

        //---- Disconnect
        struct disconnect_cmd : graph_cmd
        {
            disconnect_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "Disconnect") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Removes a connection (undoable). Usage: Disconnect -Connection hexguid"; }
            void RegisterArguments() noexcept override { m_hConnection = m_Parser.addOption("Connection", "The connection's guid, hex", true, 1); }
            std::string Apply() noexcept override
            {
                std::uint64_t Connection = 0;
                if (!HexArg(m_Parser, m_hConnection, Connection)) return "bad arguments";
                if (!m_Doc.m_Graph.m_Connections.contains(xmaterial_graph::connection_guid{ Connection })) return "no such connection";
                m_Doc.m_Graph.RemoveConnection(xmaterial_graph::connection_guid{ Connection });
                return {};
            }
            xcmdline::parser::handle m_hConnection;
        };

        //---- SetShaderFile: the shader file of an "Output: Shader File" node. The file's texture inputs become the node's inputs.
        struct set_shader_file_cmd : graph_cmd
        {
            set_shader_file_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "SetShaderFile") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Picks the shader file of a shader-file output node (undoable); its texture inputs become the node's pins. Usage: SetShaderFile -Node hexguid -File base64 (a path, absolute or relative to the project)"; }
            void RegisterArguments() noexcept override
            {
                m_hNode = m_Parser.addOption("Node", "The node's guid, hex",                           true, 1);
                m_hFile = m_Parser.addOption("File", "The shader file, base64 (absolute or project-relative)", true, 1);
            }

            // The path a node stores: relative to the project when the file is inside one (what the compiler expects), else as given.
            static std::wstring StoredPath(const std::wstring& Absolute) noexcept
            {
                for (const wchar_t* pMark : { L".lionprj", L".lionlib" })
                    if (auto Pos = xstrtool::findI(Absolute, pMark); Pos != std::wstring::npos) return Absolute.substr(Pos + std::wstring_view(pMark).size() + 1);
                return Absolute;
            }

            std::string Apply() noexcept override
            {
                std::uint64_t Node = 0;
                std::string Text;
                if (!HexArg(m_Parser, m_hNode, Node) || !GetArg(m_Parser, m_hFile, Text)) return "bad arguments";
                auto* pNode = m_Doc.FindNode(Node);
                if (!pNode) return "no such node";
                if (pNode->m_Code != "[FULL_SHADER]" || pNode->m_Params.empty() || !pNode->m_Params[0].m_Value.is<std::wstring>()) return "that node has no shader file";

                std::wstring Path = xstrtool::To(xeditor::Base64Decode(Text));
                if (!std::filesystem::exists(Path)) Path = std::format(L"{}/{}", e10::g_LibMgr.m_ProjectPath, Path);          // relative to the project
                if (!std::filesystem::exists(Path)) return "no such file";

                std::string Shader;
                if (auto Err = xmaterial_graph::RefreshShaderOnlyNode(Path, *pNode, Shader); Err) return std::string(Err.getMessage());
                pNode->m_Params[0].m_Value.get<std::wstring>() = StoredPath(std::filesystem::absolute(Path).wstring());
                m_Doc.m_Graph.RebuildPinGuidForNode(*pNode);
                return {};
            }
            xcmdline::parser::handle m_hNode, m_hFile;
        };

        //---- MoveNode: by value (dragging a node is many small moves: the editor sends one command when the drag ends)
        struct move_node_cmd : xundo::command_base
        {
            graph_document& m_Doc;
            move_node_cmd(xundo::system& System, graph_document& Doc) noexcept : command_base(System, "MoveNode", nullptr), m_Doc(Doc) { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Moves a node on the canvas (undoable). Usage: MoveNode -Node hexguid -X float -Y float [-BeforeX float -BeforeY float]"; }
            void RegisterArguments() noexcept override
            {
                m_hNode = m_Parser.addOption("Node", "The node's guid, hex",  true,  1);
                m_hX    = m_Parser.addOption("X",    "New position",          true,  1);
                m_hY    = m_Parser.addOption("Y",    "New position",          true,  1);
                m_hBX   = m_Parser.addOption("BeforeX", "Previous position, when the node has already moved", false, 1);
                m_hBY   = m_Parser.addOption("BeforeY", "Previous position, when the node has already moved", false, 1);
            }
            std::string Redo() noexcept override
            {
                std::uint64_t Node = 0; float X = 0, Y = 0;
                if (!HexArg(m_Parser, m_hNode, Node) || !FloatArg(m_Parser, m_hX, X) || !FloatArg(m_Parser, m_hY, Y)) return "MoveNode: bad arguments";
                auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr;
                if (!pNode) return "MoveNode: no such node";
                pNode->m_Pos = { X, Y };
                m_Doc.m_bPositionsChanged = true;
                m_Doc.m_bDirty = true;
                return {};
            }
            void BackupCurrenState(xundo::undo_file& File) noexcept override
            {
                std::uint64_t Node = 0; HexArg(m_Parser, m_hNode, Node);
                float X = 0, Y = 0;
                if (auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr) { X = pNode->m_Pos.m_X; Y = pNode->m_Pos.m_Y; }
                FloatArg(m_Parser, m_hBX, X); FloatArg(m_Parser, m_hBY, Y);
                File.Write(Node); File.Write(X); File.Write(Y);
            }
            void Undo(xundo::undo_file& File) noexcept override
            {
                std::uint64_t Node = 0; float X = 0, Y = 0;
                File.Read(Node); File.Read(X); File.Read(Y);
                if (auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr) { pNode->m_Pos = { X, Y }; m_Doc.m_bPositionsChanged = true; m_Doc.m_bDirty = true; }
            }
            xcmdline::parser::handle m_hNode, m_hX, m_hY, m_hBX, m_hBY;
        };

        //---- SetNodeProperty: one property of a node (a parameter's value, whether it is exposed, ...) by value
        struct set_node_property_cmd : xundo::command_base
        {
            graph_document& m_Doc;
            set_node_property_cmd(xundo::system& System, graph_document& Doc) noexcept : command_base(System, "SetNodeProperty", nullptr), m_Doc(Doc) { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Sets one property of a node (undoable): see NodeProperties for the paths. Usage: SetNodeProperty -Node hexguid -Path base64 -Value base64 [-Before base64]"; }
            void RegisterArguments() noexcept override
            {
                m_hNode   = m_Parser.addOption("Node",   "The node's guid, hex",          true,  1);
                m_hPath   = m_Parser.addOption("Path",   "Property path, base64",         true,  1);
                m_hValue  = m_Parser.addOption("Value",  "New value, base64",             true,  1);
                m_hBefore = m_Parser.addOption("Before", "Previous value, base64",        false, 1);
            }
            static property_target TargetOf(xmaterial_graph::node& N) noexcept { return { xproperty::getObject(N), &N }; }

            std::string Redo() noexcept override
            {
                std::uint64_t Node = 0; std::string Path, Value;
                if (!HexArg(m_Parser, m_hNode, Node) || !GetArg(m_Parser, m_hPath, Path) || !GetArg(m_Parser, m_hValue, Value)) return "SetNodeProperty: bad arguments";
                auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr;
                if (!pNode) return "SetNodeProperty: no such node";
                Path = xeditor::Base64Decode(Path); Value = xeditor::Base64Decode(Value);

                xproperty::any Current;
                if (!FindProperty(TargetOf(*pNode), Path, Current)) return std::format("SetNodeProperty: no property '{}'", Path);
                if (!SetValue(TargetOf(*pNode), Path, Current.getTypeGuid(), Value)) return std::format("SetNodeProperty: '{}' does not take '{}'", Path, Value);
                m_Doc.m_bDirty = true;
                return {};
            }
            void BackupCurrenState(xundo::undo_file& File) noexcept override
            {
                std::uint64_t Node = 0; std::string Path, Before;
                std::uint32_t TypeGuid = 0;
                HexArg(m_Parser, m_hNode, Node);
                if (GetArg(m_Parser, m_hPath, Path))
                {
                    Path = xeditor::Base64Decode(Path);
                    xproperty::any Current;
                    if (auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr; pNode && FindProperty(TargetOf(*pNode), Path, Current))
                    {
                        TypeGuid = Current.getTypeGuid();
                        Before   = FormatValue(Current);
                    }
                }
                if (std::string Given; GetArg(m_Parser, m_hBefore, Given)) Before = xeditor::Base64Decode(Given);
                File.Write(Node); File.Write(TypeGuid);
                xeditor::WriteString(File, Path);
                xeditor::WriteString(File, Before);
            }
            void Undo(xundo::undo_file& File) noexcept override
            {
                std::uint64_t Node = 0; std::uint32_t TypeGuid = 0;
                File.Read(Node); File.Read(TypeGuid);
                const std::string Path   = xeditor::ReadString(File);
                const std::string Before = xeditor::ReadString(File);
                if (auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr; pNode && TypeGuid)
                {
                    SetValue(TargetOf(*pNode), Path, TypeGuid, Before);
                    m_Doc.m_bDirty = true;
                }
            }
            xcmdline::parser::handle m_hNode, m_hPath, m_hValue, m_hBefore;
        };

        //---- Queries
        struct list_node_types_cmd : xundo::query_command_base
        {
            graph_document& m_Doc;
            list_node_types_cmd(xundo::system& System, graph_document& Doc) noexcept : query_command_base(System, "ListNodeTypes", nullptr), m_Doc(Doc) {}
            const char* getCommandHelp() const noexcept override { return "Lists the node types CreateNode takes: guid and name. Usage: ListNodeTypes"; }
            void RegisterArguments() noexcept override {}
            std::string Query() noexcept override
            {
                std::string Out;
                for (auto& [Guid, N] : m_Doc.m_Graph.m_PrefabNodes) Out += std::format("{:016X}  {}\n", Guid.m_Value, N->m_Name);
                return Out.empty() ? "(no node types)" : Out;
            }
        };

        // Nodes with their pins (the guids Connect takes) and the connections
        struct list_nodes_cmd : xundo::query_command_base
        {
            graph_document& m_Doc;
            list_nodes_cmd(xundo::system& System, graph_document& Doc) noexcept : query_command_base(System, "ListNodes", nullptr), m_Doc(Doc) {}
            const char* getCommandHelp() const noexcept override { return "Lists the nodes with their pins and the connections. Usage: ListNodes"; }
            void RegisterArguments() noexcept override {}
            std::string Query() noexcept override
            {
                if (!m_Doc.isLoaded()) return "ListNodes: nothing loaded";
                auto& G = m_Doc.m_Graph;
                auto TypeName = [&](xmaterial_graph::type_guid T) { auto* p = G.findType(T); return p ? p->m_Name : std::string("?"); };

                std::string Out;
                for (auto& [Guid, N] : G.m_InstanceNodes)
                {
                    Out += std::format("node {:016X}  {}  type={:016X}  pos=({:.1f}, {:.1f}){}\n", Guid.m_Value, N->m_Name, N->m_PrefabGuid.m_Value, N->m_Pos.m_X, N->m_Pos.m_Y, N->m_HasErrMsg ? "  [error]" : "");
                    for (auto& P : N->m_InputPins)  Out += std::format("  in  {:016X}  {}  {}{}\n", P.m_PinGUID.m_Value, P.m_Name, TypeName(P.m_TypeGUID), P.m_ConnectionGUID.m_Value ? std::format("  <- connection {:016X}", P.m_ConnectionGUID.m_Value) : std::string{});
                    for (auto& P : N->m_OutputPins)
                    {
                        Out += std::format("  out {:016X}  {}  {}\n", P.m_PinGUID.m_Value, P.m_Name, TypeName(P.m_TypeGUID));
                        for (auto& S : P.m_SubElements) Out += std::format("    sub {:016X}  {}  {}\n", S.m_PinGUID.m_Value, S.m_Name, TypeName(S.m_TypeGUID));
                    }
                }
                for (auto& [Guid, C] : G.m_Connections) Out += std::format("connection {:016X}  {:016X} -> {:016X}\n", Guid.m_Value, C->m_OutputPinGuid.m_Value, C->m_InputPinGuid.m_Value);
                return Out.empty() ? "(empty graph)" : Out;
            }
        };

        struct node_properties_cmd : xundo::query_command_base
        {
            graph_document& m_Doc;
            node_properties_cmd(xundo::system& System, graph_document& Doc) noexcept : query_command_base(System, "NodeProperties", nullptr), m_Doc(Doc) { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Lists a node's properties (path = value), the paths SetNodeProperty takes. Usage: NodeProperties -Node hexguid [-Filter text]"; }
            void RegisterArguments() noexcept override
            {
                m_hNode   = m_Parser.addOption("Node",   "The node's guid, hex",              true,  1);
                m_hFilter = m_Parser.addOption("Filter", "Only paths containing this text",   false, 1);
            }
            std::string Query() noexcept override
            {
                std::uint64_t Node = 0; std::string Filter;
                if (!HexArg(m_Parser, m_hNode, Node)) return "NodeProperties: bad arguments";
                GetArg(m_Parser, m_hFilter, Filter);
                auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr;
                if (!pNode) return "NodeProperties: no such node";
                return ListProperties(set_node_property_cmd::TargetOf(*pNode), Filter);
            }
            xcmdline::parser::handle m_hNode, m_hFilter;
        };
    }
}

#endif // XMATERIAL_GRAPH_EDITING_H
