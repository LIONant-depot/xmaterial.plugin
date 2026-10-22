#ifndef XMATERIAL_STACK_COMMANDS_H
#define XMATERIAL_STACK_COMMANDS_H
#pragma once

// The commands of the material stack (documentation/stack_canvas.md sections 7 and 9): where steps sit, step names, sections.
// All are snapshot-based graph_cmd: undo puts the file text back. Nodes are named by guid or @StepName, sections by guid or @Name.
#include "xmaterial_graph_editing.h"

#include <optional>

namespace xmaterial_editor::cmds
{
    namespace so = xmaterial_graph::stack_order;

    inline bool SectionNameUsed(const xmaterial_graph::stack& Stack, const std::string& Name, int Except = -1) noexcept
    {
        for (int i = 0; i < static_cast<int>(Stack.m_Sections.size()); ++i) if (i != Except && Lower(Stack.m_Sections[i].m_Name) == Lower(Name)) return true;
        return false;
    }

    // -Resolve none|dependents|nearest -> 0, 1, 2 (-1: bad text)
    inline int ResolveMode(const xcmdline::parser& P, xcmdline::parser::handle H) noexcept
    {
        std::string Text;
        if (!GetArg(P, H, Text) || Lower(Text) == "none") return 0;
        return Lower(Text) == "dependents" ? 1 : Lower(Text) == "nearest" ? 2 : -1;
    }

    // The result of a move: the plain order (Mode 0), or one of the two resolutions. "" when valid (and applied), else why not.
    inline std::string Finish(graph_document& Doc, const order_model& M, const so::sections& Plain, const so::move_result& Res, int Mode, const std::vector<xmaterial_graph::section>& Meta) noexcept
    {
        const auto& R = Mode == 0 ? Plain : Mode == 1 ? Res.m_Dependents : Res.m_Nearest;
        if (auto Err = M.Check(Doc.m_Graph, R, Meta); !Err.empty()) return Mode == 0 && Err.find("is after") != std::string::npos ? Err + " (use -Resolve dependents|nearest)" : Err;
        M.Apply(Doc.m_Graph, R, Meta);
        return {};
    }

    // What -Nodes -Section/-After/-Before/-First/-Last would do: the plain order and the two resolutions (not applied). "" or why not.
    struct place_plan
    {
        std::optional<order_model> m_M;
        so::sections               m_Plain;
        so::move_result            m_Res;
    };
    inline std::string PlanPlacement(graph_document& Doc, const xcmdline::parser& P, xcmdline::parser::handle hNodes, const placement_options& Place, place_plan& Plan) noexcept
    {
        auto& G = Doc.m_Graph;
        G.m_Stack.Reindex();
        std::string Text, Err;
        GetArg(P, hNodes, Text);
        std::vector<xmaterial_graph::node_guid> Block;
        for (std::size_t Start = 0; Start <= Text.size();)
        {
            auto End = Text.find(',', Start);
            if (End == std::string::npos) End = Text.size();
            std::uint64_t Id = 0;
            if (!ResolveNodeText(Doc, Text.substr(Start, End - Start), Id, &Err)) return Err;
            if (!G.m_Stack.m_Where.contains(xmaterial_graph::node_guid{ Id })) return "not a step";
            if (std::find(Block.begin(), Block.end(), xmaterial_graph::node_guid{ Id }) == Block.end()) Block.push_back(xmaterial_graph::node_guid{ Id });
            Start = End + 1;
        }

        int Sec = -1, Slot = 0; bool bGiven = false;
        if (auto E = Place.Resolve(Doc, P, Block, Sec, Slot, bGiven); !E.empty()) return E;
        if (!bGiven) return "needs -Section (or -After/-Before)";

        auto& M = Plan.m_M.emplace(G);
        std::vector<int> Ids;
        for (auto Guid : Block) Ids.push_back(M.m_Id[Guid]);
        std::sort(Ids.begin(), Ids.end(), [&](int a, int b) { return G.m_Stack.m_Global[M.m_Ids[a]] < G.m_Stack.m_Global[M.m_Ids[b]]; });

        Plan.m_Plain = M.m_S;                                       // the block, in its own order, at the slot
        for (auto& s : Plan.m_Plain) std::erase_if(s, [&](int n) { return std::find(Ids.begin(), Ids.end(), n) != Ids.end(); });
        Plan.m_Plain[Sec].insert(Plan.m_Plain[Sec].begin() + Slot, Ids.begin(), Ids.end());
        Plan.m_Res = so::ResolveMove(M.m_S, M.m_E, Ids, Sec, Slot);
        return {};
    }

    //---- PlaceSteps: move steps (presentation only, bindings are kept)
    struct place_steps_cmd : graph_cmd
    {
        place_steps_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "PlaceSteps") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Moves steps to a place in the stack; bindings are kept (undoable). Usage: PlaceSteps -Nodes node,node,... -Section guid|@Name [-After node | -Before node | -First 1 | -Last 1] [-Resolve none|dependents|nearest]. A move that puts a consumer before its producer fails (none), or also moves the dependents, or goes to the nearest valid spot."; }
        void RegisterArguments() noexcept override
        {
            m_hNodes   = m_Parser.addOption("Nodes",   "The steps, comma separated",        true,  1);
            m_hResolve = m_Parser.addOption("Resolve", "none (default), dependents, nearest", false, 1);
            m_Place.Register(m_Parser);
        }
        std::string Apply() noexcept override
        {
            const int Mode = ResolveMode(m_Parser, m_hResolve);
            if (Mode < 0) return "-Resolve takes none, dependents or nearest";
            place_plan Plan;
            if (auto Err = PlanPlacement(m_Doc, m_Parser, m_hNodes, m_Place, Plan); !Err.empty()) return Err;
            return Finish(m_Doc, *Plan.m_M, Plan.m_Plain, Plan.m_Res, Mode, m_Doc.m_Graph.m_Stack.m_Sections);
        }
        xcmdline::parser::handle m_hNodes, m_hResolve;
        placement_options        m_Place;
    };

    //---- RenameStep
    struct rename_step_cmd : graph_cmd
    {
        rename_step_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "RenameStep") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Renames a step (undoable). The name is made of letters, digits and _ (other characters become _), unique ignoring case. Usage: RenameStep -Node hexguid|@StepName -Name text"; }
        void RegisterArguments() noexcept override
        {
            m_hNode = m_Parser.addOption("Node", "The step",     true, 1);
            m_hName = m_Parser.addOption("Name", "The new name", true, 1);
        }
        std::string Apply() noexcept override
        {
            std::uint64_t Node = 0; std::string Given, Err;
            if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node, &Err)) return Err;
            GetArg(m_Parser, m_hName, Given);
            auto* pNode = m_Doc.FindNode(Node);
            if (!pNode || pNode->isCommentNode()) return "not a step";
            const std::string Name = SanitizeStepName(Given);
            if (Name.empty()) return "the name needs a letter or digit";
            if (StepNameUsed(m_Doc.m_Graph, Name, pNode)) return "name already used";
            pNode->m_StepName = Name;
            return {};
        }
        xcmdline::parser::handle m_hNode, m_hName;
    };

    //---- CreateSection: empty, never after the output section
    struct create_section_cmd : graph_cmd
    {
        create_section_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "CreateSection") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Creates an empty section (undoable). Usage: CreateSection -Section newhexguid -Name text [-After section | -Before section] (default: just before the output section)"; }
        void RegisterArguments() noexcept override
        {
            m_hSection = m_Parser.addOption("Section", "The new section's guid, hex", true,  1);
            m_hName    = m_Parser.addOption("Name",    "The section's name",          true,  1);
            m_hAfter   = m_Parser.addOption("After",   "Placed after this section",   false, 1);
            m_hBefore  = m_Parser.addOption("Before",  "Placed before this section",  false, 1);
        }
        std::string Apply() noexcept override
        {
            std::uint64_t Guid = 0; std::string Name, Anchor, Err;
            if (!HexArg(m_Parser, m_hSection, Guid) || Guid == 0 || !GetArg(m_Parser, m_hName, Name) || Name.empty()) return "bad arguments";
            auto& Stack = m_Doc.m_Graph.m_Stack;
            const int Out = OutputSection(Stack);
            if (Out < 0) return "the stack has no output section";
            for (auto& S : Stack.m_Sections) if (S.m_Guid.m_Value == Guid) return "a section with that id exists";
            if (SectionNameUsed(Stack, Name)) return "name already used";

            int At = Out;                                               // index the new section takes
            const bool bAfter = GetArg(m_Parser, m_hAfter, Anchor);
            if (bAfter || GetArg(m_Parser, m_hBefore, Anchor))
            {
                if (bAfter && GetArg(m_Parser, m_hBefore, Err)) return "give -After or -Before, not both";
                const int i = FindSection(Stack, Anchor, &Err);
                if (i < 0) return Err;
                At = i + (bAfter ? 1 : 0);
                if (At > Out) return "a section cannot go after the output section";
            }
            Stack.m_Sections.insert(Stack.m_Sections.begin() + At, { .m_Guid = xmaterial_graph::section_guid{ Guid }, .m_Name = Name });
            Stack.Reindex();
            return {};
        }
        xcmdline::parser::handle m_hSection, m_hName, m_hAfter, m_hBefore;
    };

    //---- RenameSection
    struct rename_section_cmd : graph_cmd
    {
        rename_section_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "RenameSection") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Renames a section (undoable). Usage: RenameSection -Section guid|@Name -Name text"; }
        void RegisterArguments() noexcept override
        {
            m_hSection = m_Parser.addOption("Section", "The section", true, 1);
            m_hName    = m_Parser.addOption("Name",    "The new name", true, 1);
        }
        std::string Apply() noexcept override
        {
            std::string Section, Name, Err;
            GetArg(m_Parser, m_hSection, Section); GetArg(m_Parser, m_hName, Name);
            auto& Stack = m_Doc.m_Graph.m_Stack;
            const int i = FindSection(Stack, Section, &Err);
            if (i < 0) return Err;
            if (Name.empty()) return "bad arguments";
            if (SectionNameUsed(Stack, Name, i)) return "name already used";
            Stack.m_Sections[i].m_Name = Name;
            return {};
        }
        xcmdline::parser::handle m_hSection, m_hName;
    };

    //---- MoveSection: the section with all its steps
    struct move_section_cmd : graph_cmd
    {
        move_section_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "MoveSection") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Moves a section in the section order (undoable); the output section stays last. Usage: MoveSection -Section guid|@Name (-After section | -Before section) [-Resolve none|dependents|nearest]"; }
        void RegisterArguments() noexcept override
        {
            m_hSection = m_Parser.addOption("Section", "The section to move",         true,  1);
            m_hAfter   = m_Parser.addOption("After",   "Placed after this section",   false, 1);
            m_hBefore  = m_Parser.addOption("Before",  "Placed before this section",  false, 1);
            m_hResolve = m_Parser.addOption("Resolve", "none (default), dependents, nearest", false, 1);
        }
        std::string Apply() noexcept override
        {
            std::string Section, Anchor, Err;
            GetArg(m_Parser, m_hSection, Section);
            auto& G = m_Doc.m_Graph;
            auto& Stack = G.m_Stack;
            const int From = FindSection(Stack, Section, &Err);
            if (From < 0) return Err;
            const bool bAfter = GetArg(m_Parser, m_hAfter, Anchor);
            if (!bAfter && !GetArg(m_Parser, m_hBefore, Anchor)) return "needs -After or -Before";
            const int To = FindSection(Stack, Anchor, &Err);
            if (To < 0) return Err;
            const int Mode = ResolveMode(m_Parser, m_hResolve);
            if (Mode < 0) return "-Resolve takes none, dependents or nearest";
            const int Out = OutputSection(Stack);
            if (From == Out || To == From) return From == Out ? "the output section cannot move" : "a section cannot be placed relative to itself";

            std::vector<int> Perm;                                      // new order of the old section indices
            for (int i = 0; i < static_cast<int>(Stack.m_Sections.size()); ++i) if (i != From) Perm.push_back(i);
            const int At = static_cast<int>(std::find(Perm.begin(), Perm.end(), To) - Perm.begin()) + (bAfter ? 1 : 0);
            Perm.insert(Perm.begin() + At, From);
            if (Perm.back() != Out) return "a section cannot go after the output section";

            order_model M(G);
            so::sections Plain;
            std::vector<xmaterial_graph::section> Meta;
            for (int i : Perm) { Plain.push_back(M.m_S[i]); Meta.push_back(Stack.m_Sections[i]); }
            const auto Res = Mode ? so::ResolveMove(Plain, M.m_E, Plain[At], At, 0) : so::move_result{};
            return Finish(m_Doc, M, Plain, Res, Mode, Meta);
        }
        xcmdline::parser::handle m_hSection, m_hAfter, m_hBefore, m_hResolve;
    };

    //---- DeleteSection: never deletes nodes
    struct delete_section_cmd : graph_cmd
    {
        delete_section_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "DeleteSection") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Deletes a section (undoable); a section with steps needs -MergeInto, which takes the steps (before the target's steps when it is later, else after them). The output section cannot be deleted. Usage: DeleteSection -Section guid|@Name [-MergeInto section]"; }
        void RegisterArguments() noexcept override
        {
            m_hSection = m_Parser.addOption("Section",   "The section to delete",   true,  1);
            m_hInto    = m_Parser.addOption("MergeInto", "Where its steps go",      false, 1);
        }
        std::string Apply() noexcept override
        {
            std::string Section, Into, Err;
            GetArg(m_Parser, m_hSection, Section);
            auto& G = m_Doc.m_Graph;
            auto& Stack = G.m_Stack;
            const int i = FindSection(Stack, Section, &Err);
            if (i < 0) return Err;
            if (Stack.m_Sections[i].m_Kind == xmaterial_graph::section::kind::OUTPUT) return "the output section cannot be deleted";

            order_model M(G);
            auto S = M.m_S;
            auto Meta = Stack.m_Sections;
            if (GetArg(m_Parser, m_hInto, Into))
            {
                const int To = FindSection(Stack, Into, &Err);
                if (To < 0) return Err;
                if (To == i) return "cannot merge a section into itself";
                S[To].insert(To > i ? S[To].begin() : S[To].end(), M.m_S[i].begin(), M.m_S[i].end());
            }
            else if (!S[i].empty()) return "the section has steps: give -MergeInto";
            S.erase(S.begin() + i);
            Meta.erase(Meta.begin() + i);
            if (auto Bad = M.Check(G, S, Meta); !Bad.empty()) return Bad + " (use PlaceSteps)";
            M.Apply(G, S, std::move(Meta));
            return {};
        }
        xcmdline::parser::handle m_hSection, m_hInto;
    };

    //---- SetSectionCollapsed
    struct set_section_collapsed_cmd : graph_cmd
    {
        set_section_collapsed_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "SetSectionCollapsed") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Collapses or expands a section (undoable). Usage: SetSectionCollapsed -Section guid|@Name -Collapsed 0|1"; }
        void RegisterArguments() noexcept override
        {
            m_hSection   = m_Parser.addOption("Section",   "The section",  true, 1);
            m_hCollapsed = m_Parser.addOption("Collapsed", "0 or 1",       true, 1);
        }
        std::string Apply() noexcept override
        {
            std::string Section, Value, Err;
            GetArg(m_Parser, m_hSection, Section); GetArg(m_Parser, m_hCollapsed, Value);
            const int i = FindSection(m_Doc.m_Graph.m_Stack, Section, &Err);
            if (i < 0) return Err;
            if (Value != "0" && Value != "1") return "-Collapsed takes 0 or 1";
            m_Doc.m_Graph.m_Stack.m_Sections[i].m_bCollapsed = Value == "1";
            return {};
        }
        xcmdline::parser::handle m_hSection, m_hCollapsed;
    };

    //---- Composite commands and queries (stack_canvas.md 9.2, 9.3)

    // An id made from a node's: never equal to another node's derived ids for nearby node ids
    inline std::uint64_t DerivedId(std::uint64_t Node, std::uint64_t k) noexcept { return Node + k * 0x9E3779B97F4A7C15ull; }

    // A connection of the given id from one output pin to one input pin (the input remembers it)
    inline void MakeLink(xmaterial_graph::graph& G, std::uint64_t Id, xmaterial_graph::pin_guid Out, xmaterial_graph::pin_guid In, std::string Swizzle = {}) noexcept
    {
        auto& Link = G.createConnection(xmaterial_graph::connection_guid{ Id });
        Link.m_OutputPinGuid = Out;
        Link.m_InputPinGuid  = In;
        Link.m_Swizzle       = std::move(Swizzle);
        if (auto* pNode = G.findNodeByPin(In)) if (const int i = pNode->getInputPinIndex(In); i >= 0) pNode->m_InputPins[i].m_ConnectionGUID = Link.m_Guid;
    }

    // -Name (made an identifier, unique), else Fallback made unique. "" or why not.
    inline std::string NewStepName(const xmaterial_graph::graph& G, const xcmdline::parser& P, xcmdline::parser::handle hName, const std::string& Fallback, std::string& Name) noexcept
    {
        if (!GetArg(P, hName, Name)) { Name = UniqueStepName(G, Fallback); return {}; }
        Name = SanitizeStepName(Name);
        return Name.empty() ? "the name needs a letter or digit" : StepNameUsed(G, Name) ? "name already used" : "";
    }

    // The chip a consumer shows for an output pin: Step, Step.Output (several outputs), Step.x (a sub-element)
    inline std::string ChipText(const xmaterial_graph::graph& G, xmaterial_graph::pin_guid Out) noexcept
    {
        auto* pNode = G.findNodeByPin(Out);
        if (!pNode) return "?";
        for (auto& op : pNode->m_OutputPins)
        {
            const std::string Base = StepLabel(*pNode) + (pNode->m_OutputPins.size() > 1 ? "." + op.m_Name : "");
            if (op.m_PinGUID == Out) return Base;
            for (auto& sub : op.m_SubElements) if (sub.m_PinGUID == Out) return Base + "." + sub.m_Name;
        }
        return "?";
    }

    // What an input reads: its source output's chip, then the components it takes (TakenComponents): Texture.xyz, Texture.a, Vec2.x, Albedo.bgr
    inline std::string InputChipText(const xmaterial_graph::graph& G, const xmaterial_graph::connection& C, const xmaterial_graph::pin& In) noexcept
    {
        auto* pMain = MainOutput(G, C.m_OutputPinGuid);
        if (!pMain) return "?";
        const auto T = TakenComponents(G, C, In);
        return ChipText(G, pMain->m_PinGUID) + (T.empty() ? "" : "." + T);
    }

    //---- InsertOnConnection: a new step on a connection; only that connection goes through it
    struct insert_on_connection_cmd : graph_cmd
    {
        insert_on_connection_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "InsertOnConnection") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Puts a new step on a connection (undoable): the source feeds -Input, -Output feeds the old consumer; the step goes right before the consumer (an output consumer: the end of the last user section). Usage: InsertOnConnection -Connection hexguid -Prefab hexguid -Node newhexguid -Input pin name -Output pin name [-Name text]. The two new connections get ids derived from -Node."; }
        void RegisterArguments() noexcept override
        {
            m_hConnection = m_Parser.addOption("Connection", "The connection, hex",              true,  1);
            m_hPrefab     = m_Parser.addOption("Prefab",     "The node type's guid, hex",        true,  1);
            m_hNode       = m_Parser.addOption("Node",       "The new node's guid, hex",         true,  1);
            m_hInput      = m_Parser.addOption("Input",      "The new step's input to feed",     true,  1);
            m_hOutput     = m_Parser.addOption("Output",     "The new step's output to use",     true,  1);
            m_hName       = m_Parser.addOption("Name",       "The step's name (default: the type's name, made unique)", false, 1);
        }
        std::string Apply() noexcept override
        {
            std::uint64_t Conn = 0, Prefab = 0, Node = 0;
            if (!HexArg(m_Parser, m_hConnection, Conn) || !HexArg(m_Parser, m_hPrefab, Prefab) || !HexArg(m_Parser, m_hNode, Node) || Node == 0) return "bad arguments";
            auto& G = m_Doc.m_Graph;
            auto It = G.m_Connections.find(xmaterial_graph::connection_guid{ Conn });
            if (It == G.m_Connections.end()) return "no such connection";
            auto Prefabs = G.m_PrefabNodes.find(xmaterial_graph::node_guid{ Prefab });
            if (Prefabs == G.m_PrefabNodes.end()) return "no such node type";
            if (m_Doc.FindNode(Node)) return "a node with that id exists";
            const std::uint64_t Id1 = DerivedId(Node, 1), Id2 = DerivedId(Node, 2);
            if (G.m_Connections.contains(xmaterial_graph::connection_guid{ Id1 }) || G.m_Connections.contains(xmaterial_graph::connection_guid{ Id2 })) return "the derived connection ids are taken";

            std::string Name, Err, InText, OutText;
            if (Err = NewStepName(G, m_Parser, m_hName, Prefabs->second->m_Name, Name); !Err.empty()) return Err;
            GetArg(m_Parser, m_hInput, InText); GetArg(m_Parser, m_hOutput, OutText);

            const auto SrcOut = It->second->m_OutputPinGuid, DstIn = It->second->m_InputPinGuid;
            const auto Swizzle = It->second->m_Swizzle;
            auto* pSrc = G.findNodeByPin(SrcOut);
            auto* pDst = G.findNodeByPin(DstIn);
            if (!pSrc || !pDst) return "the connection is broken";
            bool bIn = false; int Index = 0, Sub = 0;
            const auto& OldOut = *G.findPinConst(*pSrc, SrcOut, bIn, Index, Sub);
            const auto& OldIn  = *G.findPinConst(*pDst, DstIn,  bIn, Index, Sub);

            // everything is checked on the new node, which goes again if anything is wrong
            auto& N = G.CreateNode(xmaterial_graph::node_guid{ Prefab }, xmaterial_graph::node_guid{ Node });
            auto Fail = [&](std::string Why) { G.RemoveNode(N.m_Guid); return Why; };
            std::uint64_t NewIn = 0, NewOut = 0;
            if (N.isCommentNode() || N.isOutputNode() || N.isInputNode()) return Fail("that node type has no input and output to put on a connection");
            if (!PinByPart(N, false, InText, NewIn, &Err) || !PinByPart(N, true, OutText, NewOut, &Err)) return Fail(Err);
            const auto* pIn  = G.findPinConst(N, xmaterial_graph::pin_guid{ NewIn },  bIn, Index, Sub);
            const auto* pOut = G.findPinConst(N, xmaterial_graph::pin_guid{ NewOut }, bIn, Index, Sub);
            // the components the old input took go to the new step's input when they fit it, else the new input takes the whole source
            std::string Keep;
            if (auto* pMain = Swizzle.empty() ? nullptr : MainOutput(G, SrcOut); pMain && PickComponents(G, *pMain, Swizzle, *pIn).m_Error.empty()) Keep = PickComponents(G, *pMain, Swizzle, *pIn).m_Swizzle;
            if (Keep.empty() && !IsPinCompatible(OldOut, *pIn, G)) return Fail(TypesDiffer(G, OldOut, *pIn));
            if (!IsPinCompatible(*pOut, OldIn, G)) return Fail(TypesDiffer(G, *pOut, OldIn));

            G.m_Stack.Reindex();
            auto& Sections = G.m_Stack.m_Sections;
            auto [Sec, Slot] = G.m_Stack.m_Where.at(pDst->m_Guid);
            if (Sections[Sec].m_Kind == xmaterial_graph::section::kind::OUTPUT)                 // outputs only live in the output stack: the step goes at the end of the last user section
            {
                for (Sec = static_cast<int>(Sections.size()) - 1; Sec >= 0 && Sections[Sec].m_Kind != xmaterial_graph::section::kind::USER; --Sec) {}
                if (Sec < 0) return Fail("the stack has no user section");
                Slot = static_cast<int>(Sections[Sec].m_Steps.size());
            }

            N.m_StepName = Name;
            N.m_Pos = { (pSrc->m_Pos.m_X + pDst->m_Pos.m_X) / 2, (pSrc->m_Pos.m_Y + pDst->m_Pos.m_Y) / 2 };
            m_Doc.m_bPositionsChanged = true;
            Sections[Sec].m_Steps.insert(Sections[Sec].m_Steps.begin() + Slot, N.m_Guid);
            const xmaterial_graph::pin_guid NewInPin{ NewIn }, NewOutPin{ NewOut };
            G.RemoveConnection(xmaterial_graph::connection_guid{ Conn });
            MakeLink(G, Id1, SrcOut, NewInPin, Keep);
            MakeLink(G, Id2, NewOutPin, DstIn);
            G.m_Stack.Reindex();
            return {};
        }
        xcmdline::parser::handle m_hConnection, m_hPrefab, m_hNode, m_hInput, m_hOutput, m_hName;
    };

    //---- DuplicateStep: the same type and values, every input bound to the same sources, outputs unused; right after the original
    struct duplicate_step_cmd : graph_cmd
    {
        duplicate_step_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "DuplicateStep") { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Copies a step with its values and every input binding; its outputs feed nothing; it goes right after the original (undoable). Usage: DuplicateStep -Node hexguid|@StepName -NewNode newhexguid [-Name text]. The new connections get ids derived from -NewNode."; }
        void RegisterArguments() noexcept override
        {
            m_hNode    = m_Parser.addOption("Node",    "The step to copy",                  true,  1);
            m_hNewNode = m_Parser.addOption("NewNode", "The copy's guid, hex",              true,  1);
            m_hName    = m_Parser.addOption("Name",    "The copy's name (default: the original's, made unique)", false, 1);
        }
        std::string Apply() noexcept override
        {
            std::uint64_t Node = 0, New = 0; std::string Err;
            if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node, &Err)) return Err;
            if (!HexArg(m_Parser, m_hNewNode, New) || New == 0) return "bad arguments";
            auto& G = m_Doc.m_Graph;
            auto* pSrc = m_Doc.FindNode(Node);
            if (!pSrc || pSrc->isCommentNode()) return "not a step";
            if (m_Doc.FindNode(New)) return "a node with that id exists";
            if (!G.m_PrefabNodes.contains(pSrc->m_PrefabGuid)) return "its node type is gone";
            for (std::size_t i = 0; i < pSrc->m_InputPins.size(); ++i)
                if (G.m_Connections.contains(xmaterial_graph::connection_guid{ DerivedId(New, 3 + i) })) return "the derived connection ids are taken";
            std::string Name;
            if (Err = NewStepName(G, m_Parser, m_hName, StepLabel(*pSrc), Name); !Err.empty()) return Err;
            G.m_Stack.Reindex();
            const auto [Sec, Slot] = G.m_Stack.m_Where.at(pSrc->m_Guid);

            auto& N = G.CreateNode(pSrc->m_PrefabGuid, xmaterial_graph::node_guid{ New });
            if (N.m_InputPins.size() != pSrc->m_InputPins.size() || N.m_OutputPins.size() != pSrc->m_OutputPins.size()) { G.RemoveNode(N.m_Guid); return "that node's pins differ from its type's (a shader file?): not copied"; }
            N.m_Params   = pSrc->m_Params;
            N.m_StepName = Name;
            N.m_Pos      = { pSrc->m_Pos.m_X + 40, pSrc->m_Pos.m_Y + 40 };
            m_Doc.m_bPositionsChanged = true;
            for (std::size_t i = 0; i < pSrc->m_InputPins.size(); ++i)
                if (auto C = G.m_Connections.find(pSrc->m_InputPins[i].m_ConnectionGUID); C != G.m_Connections.end())
                    MakeLink(G, DerivedId(New, 3 + i), C->second->m_OutputPinGuid, N.m_InputPins[i].m_PinGUID, C->second->m_Swizzle);
            auto& Steps = G.m_Stack.m_Sections[Sec].m_Steps;
            Steps.insert(Steps.begin() + Slot + 1, N.m_Guid);
            G.m_Stack.Reindex();
            return {};
        }
        xcmdline::parser::handle m_hNode, m_hNewNode, m_hName;
    };

    //---- ListStack: the stack as text, each step with what feeds it
    struct list_stack_cmd : xundo::query_command_base
    {
        graph_document& m_Doc;
        list_stack_cmd(xundo::system& System, graph_document& Doc) noexcept : query_command_base(System, "ListStack", nullptr), m_Doc(Doc) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Lists the sections in order and each step: guid, name, type, its inputs as chips (Base <- Wetness.Darken) or their values. Usage: ListStack [-Section guid|@Name]"; }
        void RegisterArguments() noexcept override { m_hSection = m_Parser.addOption("Section", "Only this section", false, 1); }
        std::string Query() noexcept override
        {
            if (!m_Doc.isLoaded()) return "ListStack: nothing loaded";
            auto& G = m_Doc.m_Graph;
            std::string Only, Err;
            int One = -1;
            if (GetArg(m_Parser, m_hSection, Only) && (One = FindSection(G.m_Stack, Only, &Err)) < 0) return Err;

            std::string Out;
            for (int s = 0; s < static_cast<int>(G.m_Stack.m_Sections.size()); ++s)
            {
                auto& Sec = G.m_Stack.m_Sections[s];
                if (One >= 0 && s != One) continue;
                Out += std::format("section {:016X}  {}{}{}\n", Sec.m_Guid.m_Value, Sec.m_Name, Sec.m_Kind == xmaterial_graph::section::kind::OUTPUT ? "  [output]" : "", Sec.m_bCollapsed ? "  [collapsed]" : "");
                for (auto Step : Sec.m_Steps)
                {
                    auto* pNode = m_Doc.FindNode(Step.m_Value);
                    if (!pNode) continue;
                    Out += std::format("  {:016X}  {}  ({})", Step.m_Value, StepLabel(*pNode), pNode->m_Name);
                    bool bFirst = true;
                    for (auto& Pin : pNode->m_InputPins)
                    {
                        std::string What;
                        if (auto C = G.m_Connections.find(Pin.m_ConnectionGUID); C != G.m_Connections.end() && G.findNodeByPin(C->second->m_OutputPinGuid)) What = "<- " + InputChipText(G, *C->second, Pin);
                        else if (Pin.m_ParamIndex >= 0 && Pin.m_ParamIndex < static_cast<int>(pNode->m_Params.size()))
                        {
                            auto& Prop = pNode->m_Params[Pin.m_ParamIndex];
                            auto D = std::ranges::find(G.m_Params, Prop.m_ParamID, &xmaterial::param_decl::m_ID);
                            What = Prop.m_ParamID && D != G.m_Params.end() ? std::format("= parameter '{}'", D->m_Name) : Prop.m_Value.m_pType ? "= " + FormatValue(Prop.m_Value) : "(unconnected)";
                        }
                        else What = "(unconnected)";
                        Out += std::format("{} {} {}", bFirst ? "  " : ",", Pin.m_Name, What);
                        bFirst = false;
                    }
                    Out += '\n';
                }
            }
            return Out;
        }
        xcmdline::parser::handle m_hSection;
    };

    //---- Sources: what feeds each input of a step
    struct sources_cmd : xundo::query_command_base
    {
        graph_document& m_Doc;
        sources_cmd(xundo::system& System, graph_document& Doc) noexcept : query_command_base(System, "Sources", nullptr), m_Doc(Doc) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Lists each input of a step: the chip of its source, the connection and the source pin. Usage: Sources -Node hexguid|@StepName"; }
        void RegisterArguments() noexcept override { m_hNode = m_Parser.addOption("Node", "The step", true, 1); }
        std::string Query() noexcept override
        {
            std::uint64_t Node = 0; std::string Err;
            if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node, &Err)) return Err;
            auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr;
            if (!pNode) return "Sources: no such node";
            std::string Out;
            for (auto& Pin : pNode->m_InputPins)
                if (auto C = m_Doc.m_Graph.m_Connections.find(Pin.m_ConnectionGUID); C != m_Doc.m_Graph.m_Connections.end() && m_Doc.m_Graph.findNodeByPin(C->second->m_OutputPinGuid))
                    Out += std::format("{}  <- {}  connection {:016X}  source {:016X}\n", Pin.m_Name, InputChipText(m_Doc.m_Graph, *C->second, Pin), C->first.m_Value, C->second->m_OutputPinGuid.m_Value);
                else Out += std::format("{}  (not connected)\n", Pin.m_Name);
            return Out.empty() ? "(no inputs)" : Out;
        }
        xcmdline::parser::handle m_hNode;
    };

    //---- FindUses: every input fed by a step (or by one of its outputs)
    struct find_uses_cmd : xundo::query_command_base
    {
        graph_document& m_Doc;
        find_uses_cmd(xundo::system& System, graph_document& Doc) noexcept : query_command_base(System, "FindUses", nullptr), m_Doc(Doc) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Lists every input fed by a step, or by one of its outputs (a sub-element too): the consumer, its input, the connection. Usage: FindUses -Node hexguid|@StepName [-Output name]"; }
        void RegisterArguments() noexcept override
        {
            m_hNode   = m_Parser.addOption("Node",   "The step",              true,  1);
            m_hOutput = m_Parser.addOption("Output", "Only this output",      false, 1);
        }
        std::string Query() noexcept override
        {
            std::uint64_t Node = 0, Pin = 0; std::string Text, Err;
            if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node, &Err)) return Err;
            auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr;
            if (!pNode) return "FindUses: no such node";
            auto& G = m_Doc.m_Graph;
            std::unordered_set<std::uint64_t> Pins;                             // the output pins that count: all of the step's, or the named one and its sub-elements
            for (auto& op : pNode->m_OutputPins) { Pins.insert(op.m_PinGUID.m_Value); for (auto& sub : op.m_SubElements) Pins.insert(sub.m_PinGUID.m_Value); }
            if (GetArg(m_Parser, m_hOutput, Text))
            {
                if (!PinByPart(*pNode, true, Text, Pin, &Err)) return Err;
                Pins = { Pin };
                for (auto& op : pNode->m_OutputPins) if (op.m_PinGUID.m_Value == Pin) for (auto& sub : op.m_SubElements) Pins.insert(sub.m_PinGUID.m_Value);
            }
            std::string Out;
            for (auto& [Id, C] : G.m_Connections)
                if (Pins.contains(C->m_OutputPinGuid.m_Value))
                    if (auto* pTo = G.findNodeByPin(C->m_InputPinGuid))
                        Out += std::format("{}.{}  <- {}  connection {:016X}\n", StepLabel(*pTo), pTo->m_InputPins[pTo->getInputPinIndex(C->m_InputPinGuid)].m_Name, ChipText(G, C->m_OutputPinGuid), Id.m_Value);
            return Out.empty() ? "(no uses)" : Out;
        }
        xcmdline::parser::handle m_hNode, m_hOutput;
    };

    //---- ValidatePlacement: PlaceSteps as a dry run
    struct validate_placement_cmd : xundo::query_command_base
    {
        graph_document& m_Doc;
        validate_placement_cmd(xundo::system& System, graph_document& Doc) noexcept : query_command_base(System, "ValidatePlacement", nullptr), m_Doc(Doc) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Says whether PlaceSteps would work, changing nothing: ok, or the violations and the order each -Resolve would give. Usage: ValidatePlacement -Nodes node,node,... -Section guid|@Name [-After node | -Before node | -First 1 | -Last 1]"; }
        void RegisterArguments() noexcept override
        {
            m_hNodes = m_Parser.addOption("Nodes", "The steps, comma separated", true, 1);
            m_Place.Register(m_Parser);
        }
        std::string Query() noexcept override
        {
            if (!m_Doc.isLoaded()) return "ValidatePlacement: nothing loaded";
            place_plan Plan;
            if (auto Err = PlanPlacement(m_Doc, m_Parser, m_hNodes, m_Place, Plan); !Err.empty()) return Err;
            auto& G = m_Doc.m_Graph;
            auto& Meta = G.m_Stack.m_Sections;
            const auto& M = *Plan.m_M;
            if (auto Bad = M.Check(G, Plan.m_Plain, Meta); Bad.empty()) return "ok";
            else
            {
                std::string Out = "violations: " + Bad + "\n";
                for (auto [Mode, R] : { std::pair{ "dependents", &Plan.m_Res.m_Dependents }, std::pair{ "nearest", &Plan.m_Res.m_Nearest } })
                {
                    Out += std::format("-Resolve {}: ", Mode);
                    if (auto B = M.Check(G, *R, Meta); !B.empty()) { Out += "fails: " + B + "\n"; continue; }
                    for (int s = 0; s < static_cast<int>(R->size()); ++s)
                    {
                        Out += std::format("{}{}:", s ? " | " : "", Meta[s].m_Name);
                        for (int n : (*R)[s]) Out += " " + StepLabel(*G.m_InstanceNodes.at(M.m_Ids[n]));
                    }
                    Out += '\n';
                }
                return Out;
            }
        }
        xcmdline::parser::handle m_hNodes;
        placement_options        m_Place;
    };
}

#endif // XMATERIAL_STACK_COMMANDS_H
