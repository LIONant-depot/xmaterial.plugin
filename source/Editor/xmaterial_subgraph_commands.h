#ifndef XMATERIAL_SUBGRAPH_COMMANDS_H
#define XMATERIAL_SUBGRAPH_COMMANDS_H
#pragma once

// A sub-graph's interface (documentation/subgraphs_plan.md 2.2): the pins of its Group Input (one output per sub-graph input) and of its Group
// Output (one input per sub-graph output). Pins are named and typed here; every edit is an undoable command, so an AI builds an interface exactly
// as a person does. A pin is addressed by its side and name: -Side In -Name UV. Then group nodes (SetSubgraph, UngroupStep, MakeSubgraph) and
// going in and out of them (EnterGroup, ExitGroup).
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_graph_editing.h"
#include "plugins/xmaterial.plugin/source/Graph/xmaterial_subgraph.h"
#include "source/Tools/Editor/xeditor_resource_editor.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_commands_assets.h"

namespace xmaterial_editor::cmds
{
    inline xmaterial_graph::node* FindGroupNode(xmaterial_graph::graph& G, bool bIn) noexcept
    {
        for (auto& [Id, p] : G.m_InstanceNodes) if (bIn ? p->isGroupInputNode() : p->isGroupOutputNode()) return p.get();
        return nullptr;
    }

    inline xmaterial_graph::sType* FindTypeByName(const xmaterial_graph::graph& G, const std::string& Name) noexcept
    {
        for (auto& [Id, pT] : G.m_type) if (Lower(pT->m_Name) == Lower(Name)) return pT.get();
        return nullptr;
    }

    // The pin of the interface on that side with that name (ignoring case): the Group Input's outputs are the inputs, the Group Output's inputs the outputs
    inline xmaterial_graph::pin* FindGroupPin(xmaterial_graph::node& N, bool bIn, const std::string& Name) noexcept
    {
        if (bIn) { for (auto& P : N.m_OutputPins) if (Lower(P.m_Name) == Lower(Name)) return &P; }
        else     { for (auto& P : N.m_InputPins)  if (Lower(P.m_Name) == Lower(Name)) return &P; }
        return nullptr;
    }

    // Every connection that touches the pin (or one of its sub-elements)
    inline void DropPinConnections(xmaterial_graph::graph& G, const xmaterial_graph::pin& P, bool bOutputPin) noexcept
    {
        std::vector<xmaterial_graph::pin_guid> Pins{ P.m_PinGUID };
        if (bOutputPin) for (auto& S : static_cast<const xmaterial_graph::output_pin&>(P).m_SubElements) Pins.push_back(S.m_PinGUID);
        std::vector<xmaterial_graph::connection_guid> Drop;
        for (auto& [Id, pC] : G.m_Connections)
            if (std::ranges::find(Pins, pC->m_InputPinGuid) != Pins.end() || std::ranges::find(Pins, pC->m_OutputPinGuid) != Pins.end()) Drop.push_back(Id);
        for (auto Id : Drop) G.RemoveConnection(Id);
    }

    // -Side In|Out: which side of the interface; every pin command takes it
    struct group_pin_cmd : graph_cmd
    {
        using graph_cmd::graph_cmd;
        void RegisterSide() noexcept
        {
            m_hSide = m_Parser.addOption("Side", "In (a sub-graph input: a Group Input pin) or Out (a sub-graph output: a Group Output pin)", true, 1);
            m_hName = m_Parser.addOption("Name", "The pin's name",                                                                            true, 1);
        }
        // The Group node of the side and the pin's name, or why not
        std::string Resolve(xmaterial_graph::node*& pNode, bool& bIn, std::string& Name) noexcept
        {
            std::string Side;
            if (!GetArg(m_Parser, m_hSide, Side) || !GetArg(m_Parser, m_hName, Name)) return "bad arguments";
            if (Lower(Side) != "in" && Lower(Side) != "out") return "-Side takes In or Out";
            if (!m_Doc.m_Graph.m_bSubgraph) return "only a sub-graph has an interface";
            bIn   = Lower(Side) == "in";
            pNode = FindGroupNode(m_Doc.m_Graph, bIn);
            if (!pNode) return bIn ? "the sub-graph has no Group Input" : "the sub-graph has no Group Output";
            return {};
        }
        void Touched(xmaterial_graph::node& N) noexcept
        {
            m_Doc.m_Graph.RebuildPinGuidForNode(N);
            N.ComputeMaxInputChars(); N.ComputeMaxOutputChars();
            m_Doc.m_bPositionsChanged = true;
        }
        xcmdline::parser::handle m_hSide, m_hName;
    };

    //---- AddGroupPin: a new input or output of the sub-graph, at the end of its side
    struct add_group_pin_cmd : group_pin_cmd
    {
        add_group_pin_cmd(xundo::system& System, graph_document& Doc) noexcept : group_pin_cmd(System, Doc, "AddGroupPin") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Adds a pin to the sub-graph's interface (undoable). Usage: AddGroupPin -Side In|Out -Name text -Type float|Vector2|Vector3|Vector4|Color|SColor|Texture|Sampler2D [-Pin hexguid]. In: an input of the sub-graph (an output of Group Input); Out: an output (an input of Group Output). -Pin: the new pin's id (default: derived from the name)"; }
        void RegisterArguments() noexcept override
        {
            RegisterSide();
            m_hType = m_Parser.addOption("Type", "The pin's type (see the types of ListNodes)", true,  1);
            m_hPin  = m_Parser.addOption("Pin",  "The new pin's guid, hex",                    false, 1);
        }
        std::string Apply() noexcept override
        {
            xmaterial_graph::node* pNode = nullptr; bool bIn = true; std::string Name, Type;
            if (auto Err = Resolve(pNode, bIn, Name); !Err.empty()) return Err;
            if (!GetArg(m_Parser, m_hType, Type)) return "bad arguments";
            auto& G = m_Doc.m_Graph;
            auto* pType = FindTypeByName(G, Type);
            if (!pType) return std::format("no type '{}'", Type);
            if (Name.empty() || Name.find_first_not_of(' ') == std::string::npos) return "the name is empty";
            if (FindGroupPin(*pNode, bIn, Name)) return std::format("the sub-graph already has an {} named '{}'", bIn ? "input" : "output", Name);

            std::uint64_t Guid = 0;
            if (!HexArg(m_Parser, m_hPin, Guid))                                     // derived: the same on redo; mixed again while taken
                for (Guid = xmaterial_graph::MixGuid(pNode->m_Guid.m_Value, std::hash<std::string>{}(Lower(Name)) | 1); Guid == 0 || G.m_PinToNode.contains(xmaterial_graph::pin_guid{ Guid }); Guid = xmaterial_graph::MixGuid(Guid, 1)) {}
            else if (G.m_PinToNode.contains(xmaterial_graph::pin_guid{ Guid })) return "a pin with that id exists";

            if (bIn)
            {
                auto& P = pNode->m_OutputPins.emplace_back();
                P.m_Name = Name; P.m_PinGUID = xmaterial_graph::pin_guid{ Guid };
                xmaterial_graph::SetPinType(P, true, *pType);
            }
            else
            {
                auto& P = pNode->m_InputPins.emplace_back();
                P.m_Name = Name; P.m_PinGUID = xmaterial_graph::pin_guid{ Guid };
                xmaterial_graph::SetPinType(P, false, *pType);
            }
            Touched(*pNode);
            return {};
        }
        xcmdline::parser::handle m_hType, m_hPin;
    };

    //---- RemoveGroupPin: with its connections inside the sub-graph
    struct remove_group_pin_cmd : group_pin_cmd
    {
        remove_group_pin_cmd(xundo::system& System, graph_document& Doc) noexcept : group_pin_cmd(System, Doc, "RemoveGroupPin") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Removes a pin of the sub-graph's interface and its connections (undoable). Usage: RemoveGroupPin -Side In|Out -Name text"; }
        void RegisterArguments() noexcept override { RegisterSide(); }
        std::string Apply() noexcept override
        {
            xmaterial_graph::node* pNode = nullptr; bool bIn = true; std::string Name;
            if (auto Err = Resolve(pNode, bIn, Name); !Err.empty()) return Err;
            auto* pPin = FindGroupPin(*pNode, bIn, Name);
            if (!pPin) return std::format("no {} named '{}'", bIn ? "input" : "output", Name);
            DropPinConnections(m_Doc.m_Graph, *pPin, bIn);
            if (bIn) std::erase_if(pNode->m_OutputPins, [&](auto& P) { return &P == pPin; });
            else     std::erase_if(pNode->m_InputPins,  [&](auto& P) { return &P == pPin; });
            Touched(*pNode);
            return {};
        }
    };

    //---- RenameGroupPin: connections keep (they hold pin ids, not names)
    struct rename_group_pin_cmd : group_pin_cmd
    {
        rename_group_pin_cmd(xundo::system& System, graph_document& Doc) noexcept : group_pin_cmd(System, Doc, "RenameGroupPin") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Renames a pin of the sub-graph's interface (undoable; its connections stay). Usage: RenameGroupPin -Side In|Out -Name text -NewName text"; }
        void RegisterArguments() noexcept override { RegisterSide(); m_hNewName = m_Parser.addOption("NewName", "The new name", true, 1); }
        std::string Apply() noexcept override
        {
            xmaterial_graph::node* pNode = nullptr; bool bIn = true; std::string Name, NewName;
            if (auto Err = Resolve(pNode, bIn, Name); !Err.empty()) return Err;
            if (!GetArg(m_Parser, m_hNewName, NewName) || NewName.find_first_not_of(' ') == std::string::npos) return "the new name is empty";
            auto* pPin = FindGroupPin(*pNode, bIn, Name);
            if (!pPin) return std::format("no {} named '{}'", bIn ? "input" : "output", Name);
            if (auto* pOther = FindGroupPin(*pNode, bIn, NewName); pOther && pOther != pPin) return std::format("the sub-graph already has an {} named '{}'", bIn ? "input" : "output", NewName);
            pPin->m_Name = NewName;
            Touched(*pNode);
            return {};
        }
        xcmdline::parser::handle m_hNewName;
    };

    //---- RetypeGroupPin: its connections are dropped (they were made for the old type)
    struct retype_group_pin_cmd : group_pin_cmd
    {
        retype_group_pin_cmd(xundo::system& System, graph_document& Doc) noexcept : group_pin_cmd(System, Doc, "RetypeGroupPin") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Changes the type of a pin of the sub-graph's interface (undoable; its connections are dropped). Usage: RetypeGroupPin -Side In|Out -Name text -Type typename"; }
        void RegisterArguments() noexcept override { RegisterSide(); m_hType = m_Parser.addOption("Type", "The new type", true, 1); }
        std::string Apply() noexcept override
        {
            xmaterial_graph::node* pNode = nullptr; bool bIn = true; std::string Name, Type;
            if (auto Err = Resolve(pNode, bIn, Name); !Err.empty()) return Err;
            if (!GetArg(m_Parser, m_hType, Type)) return "bad arguments";
            auto* pType = FindTypeByName(m_Doc.m_Graph, Type);
            if (!pType) return std::format("no type '{}'", Type);
            auto* pPin = FindGroupPin(*pNode, bIn, Name);
            if (!pPin) return std::format("no {} named '{}'", bIn ? "input" : "output", Name);
            if (pPin->m_TypeGUID == pType->m_GUID) return {};
            DropPinConnections(m_Doc.m_Graph, *pPin, bIn);
            xmaterial_graph::SetPinType(*pPin, bIn, *pType);
            Touched(*pNode);
            return {};
        }
        xcmdline::parser::handle m_hType;
    };

    //---- ListGroupPins: the interface, in order
    struct list_group_pins_cmd : xundo::query_command_base
    {
        graph_document& m_Doc;
        list_group_pins_cmd(xundo::system& System, graph_document& Doc) noexcept : query_command_base(System, "ListGroupPins", nullptr), m_Doc(Doc) {}
        const char* getCommandHelp() const noexcept override { return "Lists the sub-graph's interface: its inputs (Group Input) and outputs (Group Output) with their types and pin guids. Usage: ListGroupPins"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            auto& G = m_Doc.m_Graph;
            if (!G.m_bSubgraph) return "only a sub-graph has an interface";
            auto TypeName = [&](xmaterial_graph::type_guid T) { auto* p = G.findType(T); return p ? p->m_Name : std::string("?"); };
            std::string Out;
            for (bool bIn : { true, false })
            {
                Out += bIn ? "inputs:\n" : "outputs:\n";
                auto* pNode = FindGroupNode(G, bIn);
                if (!pNode) { Out += "  (no Group node)\n"; continue; }
                if (bIn) for (auto& P : pNode->m_OutputPins) Out += std::format("  {:016X}  {}  {}\n", P.m_PinGUID.m_Value, P.m_Name, TypeName(P.m_TypeGUID));
                else     for (auto& P : pNode->m_InputPins)  Out += std::format("  {:016X}  {}  {}\n", P.m_PinGUID.m_Value, P.m_Name, TypeName(P.m_TypeGUID));
            }
            return Out;
        }
    };

    //------------------------------------------------------------------------------------------------
    // Group nodes (subgraphs_plan.md 2.3): a "Group" step names a MaterialSubgraph and takes its interface as its pins
    //------------------------------------------------------------------------------------------------

    // The sub-graphs of the open libraries: id, name, the file of their graph
    struct subgraph_info
    {
        std::uint64_t   m_Guid = 0;
        std::string     m_Name;
        std::wstring    m_Path;
    };

    inline std::vector<subgraph_info> ListSubgraphs() noexcept
    {
        std::vector<subgraph_info> Out;
        for (auto& L : xresource_editor::g_LibMgr.m_mLibraryDB)
            L.second->m_InfoByTypeDataBase.FindAsReadOnly(xmaterial_graph::subgraph_type_guid_v, [&](const std::unique_ptr<xresource_editor::library_db::info_db>& DB)
            {
                for (auto& I : DB->m_InfoDataBase)
                {
                    if (!I.second.m_Info.m_RscLinks.empty() && I.second.m_Info.m_RscLinks.front() == xresource_editor::folder::trash_guid_v) continue;     // in the trash
                    std::wstring Path = I.second.m_Path;
                    if (const auto Slash = Path.find_last_of(L'\\'); Slash != std::wstring::npos) Path = Path.substr(0, Slash + 1) + L"Descriptor.txt";
                    Out.push_back({ I.first.m_Value, I.second.m_Info.m_Name.empty() ? std::format("Subgraph {:X}", I.first.m_Value) : I.second.m_Info.m_Name, std::move(Path) });
                }
            });
        std::sort(Out.begin(), Out.end(), [](auto& a, auto& b) { return a.m_Name < b.m_Name; });
        return Out;
    }

    inline std::wstring SubgraphPath(std::uint64_t Guid) noexcept
    {
        for (auto& S : ListSubgraphs()) if (S.m_Guid == Guid) return S.m_Path;
        return {};
    }

    // Every group node takes its sub-graph's interface again (after a load or an undo: the sub-graph may have changed since)
    inline void RefreshGroupNodes(xmaterial_graph::graph& G) noexcept
    {
        for (auto& [Id, p] : G.m_InstanceNodes)
        {
            if (!p->isGroupCallNode()) continue;
            xmaterial_graph::graph Sub;
            if (const auto S = xmaterial_graph::CallSubgraph(*p); S && xmaterial_graph::LoadSubgraph(Sub, SubgraphPath(S))) xmaterial_graph::CallPins(G, *p, Sub);
        }
    }

    //---- SetSubgraph: which sub-graph a group node calls; its pins become that sub-graph's interface
    struct set_subgraph_cmd : graph_cmd
    {
        set_subgraph_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "SetSubgraph") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Sets the sub-graph a group node calls (undoable): its pins become the sub-graph's inputs and outputs; connections to pins it no longer has are dropped. A sub-graph that uses the one being edited is refused. Usage: SetSubgraph -Node hexguid|@StepName -Subgraph hexguid|name. ListSubgraphs lists them."; }
        void RegisterArguments() noexcept override
        {
            m_hNode     = m_Parser.addOption("Node",     "The group node",                        true, 1);
            m_hSubgraph = m_Parser.addOption("Subgraph", "The MaterialSubgraph: its guid, hex, or its name", true, 1);
        }
        std::string Apply() noexcept override
        {
            std::uint64_t Node = 0; std::string Err, Text;
            if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node, &Err)) return Err;
            if (!GetArg(m_Parser, m_hSubgraph, Text)) return "bad arguments";
            auto* pNode = m_Doc.FindNode(Node);
            if (!pNode || !pNode->isGroupCallNode()) return "not a group node";

            const auto All = ListSubgraphs();
            std::uint64_t Guid = 0;
            const subgraph_info* pInfo = nullptr;
            for (auto& S : All) if (Lower(S.m_Name) == Lower(Text)) pInfo = &S;
            if (!pInfo && ParseHex(Text, Guid)) for (auto& S : All) if (S.m_Guid == Guid) pInfo = &S;
            if (!pInfo) return std::format("no sub-graph '{}'", Text);

            auto& G = m_Doc.m_Graph;
            xmaterial_graph::graph Sub;
            if (!xmaterial_graph::LoadSubgraph(Sub, pInfo->m_Path)) return std::format("the sub-graph '{}' has no graph yet: open it and save it once", pInfo->m_Name);
            const auto Self = m_Doc.m_Guid.m_Instance.m_Value;
            if (G.m_bSubgraph && (pInfo->m_Guid == Self || xmaterial_graph::UsesSubgraph(Sub, Self, SubgraphPath)))
                return std::format("'{}' uses this sub-graph: a sub-graph cannot contain itself", pInfo->m_Name);

            xresource::full_guid Ref;
            Ref.m_Instance.m_Value = pInfo->m_Guid;
            Ref.m_Type             = xmaterial_graph::subgraph_type_guid_v;
            for (auto& P : pNode->m_Params) if (P.m_Type == xmaterial_graph::node_param::type::SUBGRAPH_RESOURCE) P.m_Value.set<xresource::full_guid>(Ref);
            xmaterial_graph::CallPins(G, *pNode, Sub);

            // A step still named after the type takes the sub-graph's name
            if (pNode->m_StepName.starts_with("Group") && pNode->m_StepName.find_first_not_of("0123456789", 5) == std::string::npos)
                if (auto Name = SanitizeStepName(pInfo->m_Name); !Name.empty()) { pNode->m_StepName.clear(); pNode->m_StepName = UniqueStepName(G, Name); }
            m_Doc.m_bPositionsChanged = true;
            return {};
        }
        xcmdline::parser::handle m_hNode, m_hSubgraph;
    };

    //---- ListSubgraphs: the sub-graphs a group node can call
    struct list_subgraphs_cmd : xundo::query_command_base
    {
        list_subgraphs_cmd(xundo::system& System) noexcept : query_command_base(System, "ListSubgraphs", nullptr) {}
        const char* getCommandHelp() const noexcept override { return "Lists the MaterialSubgraphs of the open libraries with their interfaces. Usage: ListSubgraphs"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            std::string Out;
            for (auto& S : ListSubgraphs())
            {
                xmaterial_graph::graph Sub;
                Out += std::format("{:016X}  {}\n", S.m_Guid, S.m_Name);
                if (xmaterial_graph::LoadSubgraph(Sub, S.m_Path)) { auto I = xmaterial_graph::InterfaceText(Sub); for (std::size_t a = 0, b; a < I.size(); a = b + 1) { b = I.find('\n', a); Out += "    " + I.substr(a, b - a) + "\n"; } }
                else Out += "    (no graph yet)\n";
            }
            return Out.empty() ? "(no sub-graphs)" : Out;
        }
    };
    //---- UngroupStep: a group node becomes the steps of its sub-graph, wired as it was (the sub-graph itself is untouched)
    struct ungroup_step_cmd : graph_cmd
    {
        ungroup_step_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "UngroupStep") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Replaces a group node by a copy of its sub-graph's steps, named <group>_<step>, wired as the group node was (undoable; the sub-graph is untouched). Usage: UngroupStep -Node hexguid|@StepName"; }
        void RegisterArguments() noexcept override { m_hNode = m_Parser.addOption("Node", "The group node", true, 1); }
        std::string Apply() noexcept override
        {
            std::uint64_t Node = 0; std::string Err;
            if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node, &Err)) return Err;
            auto* pNode = m_Doc.FindNode(Node);
            if (!pNode || !pNode->isGroupCallNode()) return "not a group node";
            const auto Before = m_Doc.Snapshot();
            std::vector<std::uint64_t> Chain;
            if (m_Doc.m_Graph.m_bSubgraph) Chain.push_back(m_Doc.m_Guid.m_Instance.m_Value);
            if (auto Why = xmaterial_graph::FlattenCalls(m_Doc.m_Graph, SubgraphPath, nullptr, xmaterial_graph::node_guid{ Node }, Chain); !Why.empty()) { m_Doc.Restore(Before); return Why; }
            m_Doc.m_bPositionsChanged = true;
            return {};
        }
        xcmdline::parser::handle m_hNode;
    };

    //---- MakeSubgraph: steps become a new sub-graph, and one group node calling it takes their place
    struct make_subgraph_cmd : graph_cmd
    {
        make_subgraph_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "MakeSubgraph") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Moves steps into a new MaterialSubgraph (next to this resource) and puts one group node calling it in their place, wired as they were: a sub-graph input per distinct source outside, an output per output used outside (undoable here; the new sub-graph stays). The steps must hold every step on a path between two of them. Usage: MakeSubgraph -Nodes hex,hex,... -Name text -Subgraph hexguid -Node hexguid. -Subgraph: the new sub-graph's id (odd); -Node: the group node's id"; }
        void RegisterArguments() noexcept override
        {
            m_hNodes    = m_Parser.addOption("Nodes",    "The steps, comma separated (guids or @StepName)", true, 1);
            m_hName     = m_Parser.addOption("Name",     "The new sub-graph's name",                         true, 1);
            m_hSubgraph = m_Parser.addOption("Subgraph", "The new sub-graph's guid, hex (odd)",              true, 1);
            m_hNode     = m_Parser.addOption("Node",     "The group node's guid, hex",                       true, 1);
        }
        std::string Apply() noexcept override
        {
            std::string List, Name, Err;
            std::uint64_t SubGuid = 0, Node = 0;
            if (!GetArg(m_Parser, m_hNodes, List) || !GetArg(m_Parser, m_hName, Name) || !HexArg(m_Parser, m_hSubgraph, SubGuid) || !HexArg(m_Parser, m_hNode, Node)) return "bad arguments";
            if ((SubGuid & 1) == 0) return "-Subgraph must be odd (a resource guid)";
            if (m_Doc.FindNode(Node)) return "a node with that id exists";
            auto& G = m_Doc.m_Graph;

            // the steps, in reading order
            std::unordered_set<std::uint64_t> Picked;
            for (std::size_t a = 0; a <= List.size();)
            {
                const auto b = std::min(List.find(',', a), List.size());
                std::uint64_t Id = 0;
                if (b > a && !ResolveNodeText(m_Doc, List.substr(a, b - a), Id, &Err)) return Err;
                if (b > a) Picked.insert(Id);
                a = b + 1;
            }
            std::vector<xmaterial_graph::node_guid> Steps;
            for (auto& Sec : G.m_Stack.m_Sections) for (auto S : Sec.m_Steps) if (Picked.contains(S.m_Value)) Steps.push_back(S);
            if (Steps.size() != Picked.size()) return "every step must be in the stack";

            // the new resource: next to this one, in the same library
            std::wstring Path;
            if (SubgraphPath(SubGuid).empty())
            {
                xresource::full_guid Parent{};
                xresource_editor::g_LibMgr.getInfo(m_Doc.m_LibraryGuid, m_Doc.m_Guid, [&](const xresource_pipeline::info& I) { if (!I.m_RscLinks.empty()) Parent = I.m_RscLinks.front(); });
                xresource::full_guid New; New.m_Instance.m_Value = SubGuid; New.m_Type = xmaterial_graph::subgraph_type_guid_v;
                xresource_editor::commands::CreateOrRestoreAsset(m_Doc.m_LibraryGuid, New, Parent, Name);
            }
            Path = SubgraphPath(SubGuid);
            if (Path.empty()) return "the sub-graph could not be made";

            const auto Before = m_Doc.Snapshot();
            xmaterial_graph::graph Sub;
            if (auto Why = xmaterial_graph::ExtractSubgraph(G, Steps, SubGuid, xmaterial_graph::node_guid{ Node }, Sub); !Why.empty()) { m_Doc.Restore(Before); return Why; }
            if (auto N = SanitizeStepName(Name); !N.empty()) m_Doc.FindNode(Node)->m_StepName = UniqueStepName(G, N);
            else                                             m_Doc.FindNode(Node)->m_StepName = UniqueStepName(G, "Group");

            Sub.serialize(Sub, false, Path);                                     // the sub-graph's file, then its compile
            xresource::full_guid New; New.m_Instance.m_Value = SubGuid; New.m_Type = xmaterial_graph::subgraph_type_guid_v;
            xresource_editor::g_LibMgr.MakeDescriptorDirty(m_Doc.m_LibraryGuid, New);
            m_Doc.m_bPositionsChanged = true;
            return {};
        }
        xcmdline::parser::handle m_hNodes, m_hName, m_hSubgraph, m_hNode;
    };

    //------------------------------------------------------------------------------------------------
    // Going in and out of groups (subgraphs_plan.md 2.8): a sub-graph opens in its own editor; each editor remembers the trail of graphs
    // it was entered from (the breadcrumb), so it can go back.
    //------------------------------------------------------------------------------------------------
    struct trail_step { xresource::full_guid m_Guid; xresource_editor::library::guid m_Library; std::string m_Name; };
    inline std::unordered_map<std::uint64_t, std::vector<trail_step>>& GroupTrails() noexcept { static std::unordered_map<std::uint64_t, std::vector<trail_step>> s; return s; }

    inline std::string OpenEditorOf(xresource::full_guid Guid, xresource_editor::library::guid Library) noexcept
    {
        auto* pHost = xeditor::host::current();
        auto* pEditors = pHost ? pHost->find<xeditor::open_resource_editors>() : nullptr;
        if (!pEditors) return "no editor host";
        if (Library.empty()) Library = xeditor::open_resource_editors::FindLibraryOf(Guid);
        if (Library.empty()) return "no open library has it";
        auto* p = pEditors->Open(Guid, Library);
        return p && p->isLoaded() ? "" : "its editor did not open";
    }

    // Opens the sub-graph of a group node; its breadcrumb is this graph's plus this graph
    inline std::string EnterGroup(graph_document& Doc, std::uint64_t Node) noexcept
    {
        auto* pNode = Doc.FindNode(Node);
        if (!pNode || !pNode->isGroupCallNode()) return "not a group node";
        const auto Sub = xmaterial_graph::CallSubgraph(*pNode);
        if (!Sub) return "the group names no sub-graph";
        auto Trail = GroupTrails()[Doc.m_Guid.m_Instance.m_Value];
        Trail.push_back({ Doc.m_Guid, Doc.m_LibraryGuid, Doc.getDisplayName() });
        GroupTrails()[Sub] = std::move(Trail);
        xresource::full_guid G; G.m_Instance.m_Value = Sub; G.m_Type = xmaterial_graph::subgraph_type_guid_v;
        return OpenEditorOf(G, {});
    }

    // Back to the graph this one was entered from ("" when there is one); Steps back: 1 = the parent
    inline std::string ExitGroup(graph_document& Doc, int Steps = 1) noexcept
    {
        auto It = GroupTrails().find(Doc.m_Guid.m_Instance.m_Value);
        if (It == GroupTrails().end() || It->second.empty()) return "this graph was not entered from a group";
        const auto& T = It->second[std::max<std::size_t>(0, It->second.size() - std::min<std::size_t>(It->second.size(), Steps))];
        return OpenEditorOf(T.m_Guid, T.m_Library);
    }

    //---- EnterGroup / ExitGroup: the same as a double click on a group node and the breadcrumb (view only, no undo)
    struct enter_group_cmd : xundo::query_command_base
    {
        graph_document& m_Doc;
        enter_group_cmd(xundo::system& System, graph_document& Doc) noexcept : query_command_base(System, "EnterGroup", nullptr), m_Doc(Doc) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "View only: opens the sub-graph of a group node in its editor (its breadcrumb leads back here). Usage: EnterGroup -Node hexguid|@StepName"; }
        void RegisterArguments() noexcept override { m_hNode = m_Parser.addOption("Node", "The group node", true, 1); }
        std::string Query() noexcept override
        {
            std::uint64_t Node = 0; std::string Err;
            if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node, &Err)) return "EnterGroup: " + Err;
            const auto Why = EnterGroup(m_Doc, Node);
            return Why.empty() ? std::string{} : "EnterGroup: " + Why;
        }
        xcmdline::parser::handle m_hNode;
    };

    struct exit_group_cmd : xundo::query_command_base
    {
        graph_document& m_Doc;
        exit_group_cmd(xundo::system& System, graph_document& Doc) noexcept : query_command_base(System, "ExitGroup", nullptr), m_Doc(Doc) {}
        const char* getCommandHelp() const noexcept override { return "View only: goes back to the graph this sub-graph was entered from. Usage: ExitGroup"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override { const auto Why = ExitGroup(m_Doc); return Why.empty() ? std::string{} : "ExitGroup: " + Why; }
    };
}

#endif
