#ifndef XMATERIAL_STACK_COMMANDS_H
#define XMATERIAL_STACK_COMMANDS_H
#pragma once

// The commands of the material stack (documentation/stack_canvas.md sections 7 and 9): where steps sit, step names, sections.
// All are snapshot-based graph_cmd: undo puts the file text back. Nodes are named by guid or @StepName, sections by guid or @Name.
#include "xmaterial_graph_editing.h"

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
            auto& G = m_Doc.m_Graph;
            G.m_Stack.Reindex();
            std::string Text, Err;
            GetArg(m_Parser, m_hNodes, Text);
            std::vector<xmaterial_graph::node_guid> Block;
            for (std::size_t Start = 0; Start <= Text.size();)
            {
                auto End = Text.find(',', Start);
                if (End == std::string::npos) End = Text.size();
                std::uint64_t Id = 0;
                if (!ResolveNodeText(m_Doc, Text.substr(Start, End - Start), Id, &Err)) return Err;
                if (!G.m_Stack.m_Where.contains(xmaterial_graph::node_guid{ Id })) return "not a step";
                if (std::find(Block.begin(), Block.end(), xmaterial_graph::node_guid{ Id }) == Block.end()) Block.push_back(xmaterial_graph::node_guid{ Id });
                Start = End + 1;
            }
            const int Mode = ResolveMode(m_Parser, m_hResolve);
            if (Mode < 0) return "-Resolve takes none, dependents or nearest";

            int Sec = -1, Slot = 0; bool bGiven = false;
            if (auto E = m_Place.Resolve(m_Doc, m_Parser, Block, Sec, Slot, bGiven); !E.empty()) return E;
            if (!bGiven) return "needs -Section (or -After/-Before)";

            order_model M(G);
            std::vector<int> Ids;
            for (auto Guid : Block) Ids.push_back(M.m_Id[Guid]);
            std::sort(Ids.begin(), Ids.end(), [&](int a, int b) { return G.m_Stack.m_Global[M.m_Ids[a]] < G.m_Stack.m_Global[M.m_Ids[b]]; });

            so::sections Plain = M.m_S;                                 // the block, in its own order, at the slot
            for (auto& s : Plain) std::erase_if(s, [&](int n) { return std::find(Ids.begin(), Ids.end(), n) != Ids.end(); });
            Plain[Sec].insert(Plain[Sec].begin() + Slot, Ids.begin(), Ids.end());

            const auto Res = so::ResolveMove(M.m_S, M.m_E, Ids, Sec, Slot);
            return Finish(m_Doc, M, Plain, Res, Mode, G.m_Stack.m_Sections);
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
}

#endif // XMATERIAL_STACK_COMMANDS_H
