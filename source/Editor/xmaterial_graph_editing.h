#ifndef XMATERIAL_GRAPH_EDITING_H
#define XMATERIAL_GRAPH_EDITING_H
#pragma once

// The material's document (its node graph on disk) and the commands that edit it: create and delete nodes, connect and disconnect pins, move nodes,
// set a node's properties, pick a node's shader file. The Material editor's window calls these and so does an AI or a script through the console.
// Every structural command snapshots the whole graph first, so undo puts back everything it touched; moving and property edits undo by value.
#include "source/Tools/Editor/xeditor_document_editor.h"
#include "plugins/xmaterial.plugin/source/Graph/xmaterial_graph.h"
#include "plugins/xmaterial.plugin/source/Graph/xmaterial_stack_adapter.h"

#include <charconv>
#include <functional>
#include <unordered_set>

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
            m_Graph.m_Stack = {};
            m_Graph.m_Stack.m_Sections.push_back({ .m_Guid = xmaterial_graph::section_guid{ xresource::guid_generator::Type64() }, .m_Name = "Output", .m_Kind = xmaterial_graph::section::kind::OUTPUT });
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

        //---- Symbols: wherever a node or pin is taken, hex or @StepName / @StepName.Pin (an output, a sub-element "x" or "Output.x", or an input). Sections: hex or @Name.
        inline std::string Lower(std::string S) noexcept { for (auto& c : S) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return S; }
        inline std::string NoSpace(std::string S) noexcept { std::erase(S, ' '); return Lower(std::move(S)); }
        inline std::string StepLabel(const xmaterial_graph::node& N) noexcept { return N.m_StepName.empty() ? N.m_Name : N.m_StepName; }

        inline xmaterial_graph::node* FindStep(const graph_document& Doc, const std::string& Name) noexcept
        {
            xmaterial_graph::node* pAny = nullptr;
            for (auto& [Id, pNode] : Doc.m_Graph.m_InstanceNodes)
            {
                if (pNode->m_StepName == Name) return pNode.get();
                if (!pNode->m_StepName.empty() && Lower(pNode->m_StepName) == Lower(Name)) pAny = pNode.get();
            }
            return pAny;
        }

        inline bool ParseHex(const std::string& Text, std::uint64_t& Out) noexcept
        {
            const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Out, 16);
            return !Text.empty() && Result.ec == std::errc() && Result.ptr == Text.data() + Text.size();
        }

        inline bool ResolveNodeText(const graph_document& Doc, const std::string& Text, std::uint64_t& Out, std::string* pErr = nullptr) noexcept
        {
            if (!Text.empty() && Text[0] == '@')
            {
                if (auto* p = FindStep(Doc, Text.substr(1))) { Out = p->m_Guid.m_Value; return true; }
                if (pErr) *pErr = std::format("no step named '{}'", Text.substr(1));
                return false;
            }
            if (ParseHex(Text, Out)) return true;
            if (pErr) *pErr = std::format("'{}' is not a node guid or @StepName", Text);
            return false;
        }

        inline bool ResolvePinText(const graph_document& Doc, const std::string& Text, bool bOutput, std::uint64_t& Out, std::string* pErr = nullptr) noexcept
        {
            auto Fail = [&](std::string Why) { if (pErr) *pErr = std::move(Why); return false; };
            if (Text.empty() || Text[0] != '@') return ParseHex(Text, Out) ? true : Fail(std::format("'{}' is not a pin guid or @StepName.Pin", Text));

            const auto Dot       = Text.find('.');
            const std::string StepName = Text.substr(1, Dot == std::string::npos ? std::string::npos : Dot - 1);
            auto* pNode = FindStep(Doc, StepName);
            if (!pNode) return Fail(std::format("no step named '{}'", StepName));
            const std::string Part = Dot == std::string::npos ? std::string{} : Text.substr(Dot + 1);

            std::vector<std::pair<std::string, std::uint64_t>> Names;
            if (bOutput)
                for (auto& op : pNode->m_OutputPins)
                {
                    Names.push_back({ op.m_Name, op.m_PinGUID.m_Value });
                    for (auto& sub : op.m_SubElements) { Names.push_back({ sub.m_Name, sub.m_PinGUID.m_Value }); Names.push_back({ op.m_Name + "." + sub.m_Name, sub.m_PinGUID.m_Value }); }
                }
            else for (auto& ip : pNode->m_InputPins) Names.push_back({ ip.m_Name, ip.m_PinGUID.m_Value });

            if (Part.empty())
            {
                if (bOutput && pNode->m_OutputPins.size() == 1) { Out = pNode->m_OutputPins[0].m_PinGUID.m_Value; return true; }
                return Fail(std::format("'{}' needs a pin: @{}.Name", Text, StepLabel(*pNode)));
            }
            int nFound = 0;
            for (auto& [Name, Guid] : Names) if (NoSpace(Name) == NoSpace(Part) || NoSpace(Name.substr(0, Name.find('('))) == NoSpace(Part)) { Out = Guid; ++nFound; }       // "A(4)" is also "A"
            if (nFound == 0) return Fail(std::format("'{}' has no {} pin '{}'", StepLabel(*pNode), bOutput ? "output" : "input", Part));
            if (nFound > 1)  return Fail(std::format("'{}' is ambiguous on '{}'", Part, StepLabel(*pNode)));
            return true;
        }

        inline int FindSection(const xmaterial_graph::stack& Stack, const std::string& Text, std::string* pErr = nullptr) noexcept
        {
            std::uint64_t Guid = 0;
            const bool bName = !Text.empty() && Text[0] == '@';
            if (!bName && !ParseHex(Text, Guid)) { if (pErr) *pErr = std::format("'{}' is not a section guid or @Name", Text); return -1; }
            for (int i = 0; i < static_cast<int>(Stack.m_Sections.size()); ++i)
                if (bName ? Stack.m_Sections[i].m_Name == Text.substr(1) : Stack.m_Sections[i].m_Guid.m_Value == Guid) return i;
            if (pErr) *pErr = std::format("no section '{}'", Text);
            return -1;
        }

        inline bool ResolveNodeArg(const graph_document& Doc, const xcmdline::parser& Parser, xcmdline::parser::handle H, std::uint64_t& Out, std::string* pErr = nullptr) noexcept
        {
            std::string Text;
            if (!GetArg(Parser, H, Text)) { if (pErr) *pErr = "bad arguments"; return false; }
            return ResolveNodeText(Doc, Text, Out, pErr);
        }

        inline bool ResolvePinArg(const graph_document& Doc, const xcmdline::parser& Parser, xcmdline::parser::handle H, bool bOutput, std::uint64_t& Out, std::string* pErr = nullptr) noexcept
        {
            std::string Text;
            if (!GetArg(Parser, H, Text)) { if (pErr) *pErr = "bad arguments"; return false; }
            return ResolvePinText(Doc, Text, bOutput, Out, pErr);
        }

        //---- Step names: letters, digits and '_', unique ignoring case (they become GLSL names)
        inline std::string SanitizeStepName(const std::string& In) noexcept
        {
            std::string S;
            bool bAny = false;
            for (char c : In) { const bool bOk = std::isalnum(static_cast<unsigned char>(c)) != 0; bAny |= bOk; S += bOk || c == '_' ? c : '_'; }
            if (!bAny) return {};
            if (std::isdigit(static_cast<unsigned char>(S[0]))) S.insert(S.begin(), '_');
            return S;
        }

        inline bool StepNameUsed(const xmaterial_graph::graph& G, const std::string& Name, const xmaterial_graph::node* pExcept = nullptr) noexcept
        {
            for (auto& [Id, pNode] : G.m_InstanceNodes) if (pNode.get() != pExcept && Lower(pNode->m_StepName) == Lower(Name)) return true;
            return false;
        }

        inline std::string UniqueStepName(const xmaterial_graph::graph& G, const std::string& Prefab) noexcept
        {
            std::string Base = SanitizeStepName(Prefab);
            if (Base.empty()) Base = "Step";
            std::string Name = Base;
            for (int k = 2; StepNameUsed(G, Name); ++k) Name = Base + std::to_string(k);
            return Name;
        }

        //---- The order: the stack as dense ints, for the pure functions of stack_order (Graph/xmaterial_stack_adapter.h)
        struct order_model
        {
            std::vector<xmaterial_graph::node_guid>             m_Ids;          // dense id -> node
            std::unordered_map<xmaterial_graph::node_guid, int> m_Id;
            xmaterial_graph::stack_order::sections              m_S;
            xmaterial_graph::stack_order::edges                 m_E;

            explicit order_model(const xmaterial_graph::graph& G) noexcept
            {
                for (auto& Sec : G.m_Stack.m_Sections)
                {
                    m_S.emplace_back();
                    for (auto Guid : Sec.m_Steps) { m_Id[Guid] = static_cast<int>(m_Ids.size()); m_S.back().push_back(static_cast<int>(m_Ids.size())); m_Ids.push_back(Guid); }
                }
                for (auto& [Cid, pConn] : G.m_Connections)
                {
                    auto* p = G.findNodeByPin(pConn->m_OutputPinGuid);
                    auto* q = G.findNodeByPin(pConn->m_InputPinGuid);
                    if (p && q && p != q && m_Id.count(p->m_Guid) && m_Id.count(q->m_Guid)) m_E.push_back({ m_Id[p->m_Guid], m_Id[q->m_Guid] });
                }
            }

            // "" when S (the sections of Meta, reordered) keeps every producer before its consumer and the output stack holds exactly the outputs; else the reasons.
            std::string Check(const xmaterial_graph::graph& G, const xmaterial_graph::stack_order::sections& S, const std::vector<xmaterial_graph::section>& Meta) const noexcept
            {
                auto Name = [&](int n) { return StepLabel(*G.m_InstanceNodes.at(m_Ids[n])); };
                const auto W = xmaterial_graph::stack_order::Locate(S, static_cast<int>(m_Ids.size()));
                std::string Out;
                for (auto [p, c] : m_E) if (W.m_Global[p] >= W.m_Global[c]) Out += std::format("{}{} is after {}", Out.empty() ? "" : "; ", Name(p), Name(c));
                for (int s = 0; s < static_cast<int>(S.size()); ++s)
                    for (int n : S[s])
                        if (G.m_InstanceNodes.at(m_Ids[n])->isOutputNode() != (Meta[s].m_Kind == xmaterial_graph::section::kind::OUTPUT))
                            return Out + (Out.empty() ? "" : "; ") + "the output stack accepts only outputs";
                return Out;
            }

            // Makes S, with Meta's sections, the stack.
            void Apply(xmaterial_graph::graph& G, const xmaterial_graph::stack_order::sections& S, std::vector<xmaterial_graph::section> Meta) const noexcept
            {
                for (int s = 0; s < static_cast<int>(S.size()); ++s)
                {
                    Meta[s].m_Steps.clear();
                    for (int n : S[s]) Meta[s].m_Steps.push_back(m_Ids[n]);
                }
                G.m_Stack.m_Sections = std::move(Meta);
                G.m_Stack.Reindex();
            }
        };

        inline int OutputSection(const xmaterial_graph::stack& Stack) noexcept
        {
            for (int i = static_cast<int>(Stack.m_Sections.size()) - 1; i >= 0; --i) if (Stack.m_Sections[i].m_Kind == xmaterial_graph::section::kind::OUTPUT) return i;
            return -1;
        }

        // Where a step goes: -Section (guid or @Name) with -After <node> | -Before <node> | -First 1 | -Last 1 (an anchor alone names the section).
        struct placement_options
        {
            xcmdline::parser::handle m_hSection, m_hAfter, m_hBefore, m_hFirst, m_hLast;
            void Register(xcmdline::parser& P) noexcept
            {
                m_hSection = P.addOption("Section", "The section: guid hex or @Name",     false, 1);
                m_hAfter   = P.addOption("After",   "Right after this step",              false, 1);
                m_hBefore  = P.addOption("Before",  "Right before this step",             false, 1);
                m_hFirst   = P.addOption("First",   "1: first in the section",            false, 1);
                m_hLast    = P.addOption("Last",    "1: last in the section (default)",   false, 1);
            }

            // Sec = section index, Slot = index among the section's steps without Skip. bGiven false: nothing was asked (the caller decides).
            std::string Resolve(const graph_document& Doc, const xcmdline::parser& P, const std::vector<xmaterial_graph::node_guid>& Skip, int& Sec, int& Slot, bool& bGiven) const noexcept
            {
                auto& Stack = Doc.m_Graph.m_Stack;
                std::string SectionText, AfterText, BeforeText, Err;
                const bool bSection = GetArg(P, m_hSection, SectionText), bAfter = GetArg(P, m_hAfter, AfterText), bBefore = GetArg(P, m_hBefore, BeforeText);
                const bool bFirst = P.hasOption(m_hFirst), bLast = P.hasOption(m_hLast);
                bGiven = bSection || bAfter || bBefore || bFirst || bLast;
                if (!bGiven) return {};
                if (int(bAfter) + int(bBefore) + int(bFirst) + int(bLast) > 1) return "give only one of -After, -Before, -First, -Last";

                std::uint64_t Anchor = 0;
                if (bAfter || bBefore) if (!ResolveNodeText(Doc, bAfter ? AfterText : BeforeText, Anchor, &Err)) return Err;
                if (bSection) { if (Sec = FindSection(Stack, SectionText, &Err); Sec < 0) return Err; }
                else if (bAfter || bBefore)
                {
                    auto It = Stack.m_Where.find(xmaterial_graph::node_guid{ Anchor });
                    if (It == Stack.m_Where.end()) return "the anchor is not a step";
                    Sec = It->second.first;
                }
                else return "-First and -Last need -Section";

                std::vector<xmaterial_graph::node_guid> Steps;
                for (auto Guid : Stack.m_Sections[Sec].m_Steps) if (std::find(Skip.begin(), Skip.end(), Guid) == Skip.end()) Steps.push_back(Guid);
                Slot = bFirst ? 0 : static_cast<int>(Steps.size());
                if (bAfter || bBefore)
                {
                    auto It = std::find(Steps.begin(), Steps.end(), xmaterial_graph::node_guid{ Anchor });
                    if (It == Steps.end()) return "the anchor is not in that section";
                    Slot = static_cast<int>(It - Steps.begin()) + (bAfter ? 1 : 0);
                }
                return {};
            }
        };

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
        // The node is also a step: it goes where -Section/-After/-Before/-First/-Last say, else at the end of the last user section (outputs: the output stack).
        struct create_node_cmd : graph_cmd
        {
            create_node_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "CreateNode") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Creates a node (undoable). Usage: CreateNode -Prefab hexguid -Node hexguid [-Section guid|@Name [-After node | -Before node | -First 1 | -Last 1]] [-Name text]. Prefab: a type from ListNodeTypes; Node: a new, unused id; node symbols: @StepName. -X -Y are accepted and ignored."; }
            void RegisterArguments() noexcept override
            {
                m_hPrefab = m_Parser.addOption("Prefab", "The node type's guid, hex",         true,  1);
                m_hNode   = m_Parser.addOption("Node",   "The new node's guid, hex",          true,  1);
                m_hName   = m_Parser.addOption("Name",   "The step's name (default: the type's name, made unique)", false, 1);
                m_hX      = m_Parser.addOption("X",      "Ignored (positions are derived)",   false, 1);
                m_hY      = m_Parser.addOption("Y",      "Ignored (positions are derived)",   false, 1);
                m_Place.Register(m_Parser);
            }
            std::string Apply() noexcept override
            {
                std::uint64_t Prefab = 0, Node = 0;
                if (!HexArg(m_Parser, m_hPrefab, Prefab) || !HexArg(m_Parser, m_hNode, Node) || Node == 0) return "bad arguments";
                auto& G = m_Doc.m_Graph;
                auto It = G.m_PrefabNodes.find(xmaterial_graph::node_guid{ Prefab });
                if (It == G.m_PrefabNodes.end()) return "no such node type";
                if (m_Doc.FindNode(Node)) return "a node with that id exists";
                G.m_Stack.Reindex();
                auto& Sections = G.m_Stack.m_Sections;

                // everything is checked before anything changes
                const bool bComment = It->second->isCommentNode(), bOutput = It->second->isOutputNode();
                std::string Name;
                if (!bComment)
                {
                    if (GetArg(m_Parser, m_hName, Name))
                    {
                        const std::string Given = Name;
                        Name = SanitizeStepName(Given);
                        if (Name.empty()) return "the name needs a letter or digit";
                        if (StepNameUsed(G, Name)) return "name already used";
                    }
                    else Name = UniqueStepName(G, It->second->m_Name);
                }

                int Sec = -1, Slot = 0; bool bGiven = false, bNewSection = false;
                if (auto Err = m_Place.Resolve(m_Doc, m_Parser, {}, Sec, Slot, bGiven); !Err.empty()) return Err;
                const int Out = OutputSection(G.m_Stack);
                if (!bComment)
                {
                    if (Out < 0) return "the stack has no output section";
                    if (!bGiven && bOutput) { Sec = Out; Slot = static_cast<int>(Sections[Out].m_Steps.size()); }
                    else if (!bGiven)
                    {
                        for (Sec = Out - 1; Sec >= 0 && Sections[Sec].m_Kind != xmaterial_graph::section::kind::USER; --Sec) {}
                        if (Sec < 0) { Sec = Out; bNewSection = true; }                     // no user section: "Steps" is made before the output stack
                        Slot = bNewSection ? 0 : static_cast<int>(Sections[Sec].m_Steps.size());
                    }
                    else if (bOutput != (Sections[Sec].m_Kind == xmaterial_graph::section::kind::OUTPUT)) return "the output stack accepts only outputs";
                }

                auto& N = G.CreateNode(xmaterial_graph::node_guid{ Prefab }, xmaterial_graph::node_guid{ Node });
                float X = 0, Y = 0;
                FloatArg(m_Parser, m_hX, X); FloatArg(m_Parser, m_hY, Y);
                N.m_Pos = { X, Y };
                m_Doc.m_bPositionsChanged = true;
                if (bComment) return {};

                N.m_StepName = Name;
                if (bNewSection)                                                            // its id comes from the node's, so redo makes the same section
                    Sections.insert(Sections.begin() + Out, { .m_Guid = xmaterial_graph::section_guid{ Node ^ 0x5EC710A5EC710A5Eull }, .m_Name = "Steps" }), Sec = Out;
                Sections[Sec].m_Steps.insert(Sections[Sec].m_Steps.begin() + Slot, N.m_Guid);
                G.m_Stack.Reindex();
                return {};
            }
            xcmdline::parser::handle m_hPrefab, m_hNode, m_hName, m_hX, m_hY;
            placement_options        m_Place;
        };

        //---- DeleteNode: with every connection that touches it
        struct delete_node_cmd : graph_cmd
        {
            delete_node_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "DeleteNode") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Deletes a node, its connections and its place in the stack (undoable; the last output node is refused). Usage: DeleteNode -Node hexguid|@StepName"; }
            void RegisterArguments() noexcept override { m_hNode = m_Parser.addOption("Node", "The node's guid, hex", true, 1); }
            std::string Apply() noexcept override
            {
                std::uint64_t Node = 0; std::string Err;
                if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node, &Err)) return Err;
                auto* pNode = m_Doc.FindNode(Node);
                if (!pNode) return "no such node";
                auto& G = m_Doc.m_Graph;
                if (pNode->isOutputNode())
                {
                    int nOutputs = 0;
                    for (auto& [Id, p] : G.m_InstanceNodes) nOutputs += p->isOutputNode();
                    if (nOutputs < 2) return "the last output node cannot be deleted";
                }
                G.RemoveNode(xmaterial_graph::node_guid{ Node });
                for (auto& Sec : G.m_Stack.m_Sections) std::erase(Sec.m_Steps, xmaterial_graph::node_guid{ Node });
                G.m_Stack.Reindex();
                return {};
            }
            xcmdline::parser::handle m_hNode;
        };

        //---- Connect: an output pin to an input pin. An input takes one connection: an older one is replaced.
        struct connect_cmd : graph_cmd
        {
            connect_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "Connect") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Connects an output pin to an input pin of the same type (undoable; replaces the input's connection). Usage: Connect -Output hexguid|@Step[.Pin] -Input hexguid|@Step.Pin -Connection hexguid. Refused when it would make a cycle. When the producer is after the consumer, the consumer and its dependents move to right after the producer."; }
            void RegisterArguments() noexcept override
            {
                m_hOutput     = m_Parser.addOption("Output",     "The output pin's guid, hex",        true, 1);
                m_hInput      = m_Parser.addOption("Input",      "The input pin's guid, hex",         true, 1);
                m_hConnection = m_Parser.addOption("Connection", "The new connection's guid, hex",    true, 1);
            }
            std::string Apply() noexcept override
            {
                std::uint64_t Out = 0, In = 0, Connection = 0; std::string Err;
                if (!ResolvePinArg(m_Doc, m_Parser, m_hOutput, true, Out, &Err) || !ResolvePinArg(m_Doc, m_Parser, m_hInput, false, In, &Err)) return Err;
                if (!HexArg(m_Parser, m_hConnection, Connection) || Connection == 0) return "bad arguments";
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

                std::function<void()> Reorder;
                // the new link may not close a loop (the consumer must not already lead to the producer), and the producer must come first in the stack
                {
                    std::vector<const xmaterial_graph::node*> Todo{ pInNode };
                    std::unordered_set<const xmaterial_graph::node*> Seen{ pInNode };
                    while (!Todo.empty())
                    {
                        auto* pFrom = Todo.back(); Todo.pop_back();
                        if (pFrom == pOutNode) return std::format("would make a cycle through {}", StepLabel(*pInNode));
                        for (auto& [Id, C] : G.m_Connections)
                            if (G.findNodeByPin(C->m_OutputPinGuid) == pFrom)
                                if (auto* pNext = G.findNodeByPin(C->m_InputPinGuid); pNext && Seen.insert(pNext).second) Todo.push_back(pNext);
                    }
                    G.m_Stack.Reindex();
                    auto P = G.m_Stack.m_Global.find(pOutNode->m_Guid), C = G.m_Stack.m_Global.find(pInNode->m_Guid);
                    if (P != G.m_Stack.m_Global.end() && C != G.m_Stack.m_Global.end() && P->second > C->second)
                    {
                        // the producer is after the consumer: the consumer and its dependents move to right after the producer (the 'dependents' resolution)
                        order_model M(G);
                        const int iP = M.m_Id[pOutNode->m_Guid], iC = M.m_Id[pInNode->m_Guid];
                        M.m_E.push_back({ iP, iC });
                        auto R = M.m_S;
                        for (auto& Sec : R) std::erase(Sec, iC);
                        int Sec = 0, Slot = 0;
                        for (int s = 0; s < static_cast<int>(R.size()); ++s)
                            if (auto It = std::find(R[s].begin(), R[s].end(), iP); It != R[s].end()) { Sec = s; Slot = static_cast<int>(It - R[s].begin()) + 1; }
                        auto Resolved = xmaterial_graph::stack_order::ResolveMove(M.m_S, M.m_E, { iC }, Sec, Slot).m_Dependents;
                        if (auto Bad = M.Check(G, Resolved, G.m_Stack.m_Sections); !Bad.empty()) return Bad;
                        Reorder = [M, Resolved, &G] { M.Apply(G, Resolved, G.m_Stack.m_Sections); };
                    }
                }

                for (auto& [Id, C] : G.m_Connections)
                    if (C->m_InputPinGuid.m_Value == In) { G.RemoveConnection(Id); break; }

                auto& Link = G.createConnection(xmaterial_graph::connection_guid{ Connection });
                Link.m_OutputPinGuid = xmaterial_graph::pin_guid{ Out };
                Link.m_InputPinGuid  = xmaterial_graph::pin_guid{ In };
                if (const int i = pInNode->getInputPinIndex(Link.m_InputPinGuid); i >= 0) pInNode->m_InputPins[i].m_ConnectionGUID = Link.m_Guid;
                if (Reorder) Reorder();
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
            const char* getCommandHelp() const noexcept override { return "Picks the shader file of a shader-file output node (undoable); its texture inputs become the node's pins. Usage: SetShaderFile -Node hexguid -File text (a path, absolute or relative to the project)"; }
            void RegisterArguments() noexcept override
            {
                m_hNode = m_Parser.addOption("Node", "The node's guid, hex",                           true, 1);
                m_hFile = m_Parser.addOption("File", "The shader file (absolute or project-relative)", true, 1);
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
                if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node) || !GetArg(m_Parser, m_hFile, Text)) return "bad arguments";
                auto* pNode = m_Doc.FindNode(Node);
                if (!pNode) return "no such node";
                if (pNode->m_Code != "[FULL_SHADER]" || pNode->m_Params.empty() || !pNode->m_Params[0].m_Value.is<std::wstring>()) return "that node has no shader file";

                std::wstring Path = xstrtool::To(Text);
                if (!std::filesystem::exists(Path)) Path = std::format(L"{}/{}", xresource_editor::g_LibMgr.m_ProjectPath, Path);          // relative to the project
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
            const char* getCommandHelp() const noexcept override { return "Moves a node on the old canvas (undoable); only its position, the stack ignores it. Usage: MoveNode -Node hexguid -X float -Y float [-BeforeX float -BeforeY float]"; }
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
                if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node) || !FloatArg(m_Parser, m_hX, X) || !FloatArg(m_Parser, m_hY, Y)) return "MoveNode: bad arguments";
                auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr;
                if (!pNode) return "MoveNode: no such node";
                pNode->m_Pos = { X, Y };
                m_Doc.m_bPositionsChanged = true;
                m_Doc.m_bDirty = true;
                return {};
            }
            void BackupCurrenState(xundo::undo_file& File) noexcept override
            {
                std::uint64_t Node = 0; ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node);
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
            const char* getCommandHelp() const noexcept override { return "Sets one property of a node (undoable): see NodeProperties for the paths. Usage: SetNodeProperty -Node hexguid -Path text -Value text [-Before text]"; }
            void RegisterArguments() noexcept override
            {
                m_hNode   = m_Parser.addOption("Node",   "The node's guid, hex",          true,  1);
                m_hPath   = m_Parser.addOption("Path",   "Property path",         true,  1);
                m_hValue  = m_Parser.addOption("Value",  "New value",             true,  1);
                m_hBefore = m_Parser.addOption("Before", "Previous value",        false, 1);
            }
            static property_target TargetOf(xmaterial_graph::node& N) noexcept { return { xproperty::getObject(N), &N }; }

            std::string Redo() noexcept override
            {
                std::uint64_t Node = 0; std::string Path, Value;
                if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node) || !GetArg(m_Parser, m_hPath, Path) || !GetArg(m_Parser, m_hValue, Value)) return "SetNodeProperty: bad arguments";
                auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr;
                if (!pNode) return "SetNodeProperty: no such node";

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
                ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node);
                if (GetArg(m_Parser, m_hPath, Path))
                {
                    xproperty::any Current;
                    if (auto* pNode = m_Doc.isLoaded() ? m_Doc.FindNode(Node) : nullptr; pNode && FindProperty(TargetOf(*pNode), Path, Current))
                    {
                        TypeGuid = Current.getTypeGuid();
                        Before   = FormatValue(Current);
                    }
                }
                if (std::string Given; GetArg(m_Parser, m_hBefore, Given)) Before = Given;
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
                G.m_Stack.Reindex();
                auto TypeName = [&](xmaterial_graph::type_guid T) { auto* p = G.findType(T); return p ? p->m_Name : std::string("?"); };

                std::string Out;
                for (auto& [Guid, N] : G.m_InstanceNodes)
                {
                    std::string Where;
                    if (auto W = G.m_Stack.m_Where.find(Guid); W != G.m_Stack.m_Where.end()) Where = std::format("  step={}  section={}", N->m_StepName, G.m_Stack.m_Sections[W->second.first].m_Name);
                    Out += std::format("node {:016X}  {}  type={:016X}  pos=({:.1f}, {:.1f}){}{}\n", Guid.m_Value, N->m_Name, N->m_PrefabGuid.m_Value, N->m_Pos.m_X, N->m_Pos.m_Y, Where, N->m_HasErrMsg ? "  [error]" : "");
                    for (auto& P : N->m_InputPins)  Out += std::format("  in  {:016X}  {}  {}{}\n", P.m_PinGUID.m_Value, P.m_Name, TypeName(P.m_TypeGUID), P.m_ConnectionGUID.m_Value ? std::format("  <- connection {:016X}", P.m_ConnectionGUID.m_Value) : std::string{});
                    for (auto& P : N->m_OutputPins)
                    {
                        Out += std::format("  out {:016X}  {}  {}\n", P.m_PinGUID.m_Value, P.m_Name, TypeName(P.m_TypeGUID));
                        for (auto& S : P.m_SubElements) Out += std::format("    sub {:016X}  {}  {}\n", S.m_PinGUID.m_Value, S.m_Name, TypeName(S.m_TypeGUID));
                    }
                }
                for (auto& Sec : G.m_Stack.m_Sections)
                {
                    Out += std::format("section {:016X}  {}{}{}:", Sec.m_Guid.m_Value, Sec.m_Name, Sec.m_Kind == xmaterial_graph::section::kind::OUTPUT ? "  [output]" : "", Sec.m_bCollapsed ? "  [collapsed]" : "");
                    for (auto Step : Sec.m_Steps) if (auto* p = m_Doc.FindNode(Step.m_Value)) Out += " " + StepLabel(*p);
                    Out += '\n';
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
                if (!ResolveNodeArg(m_Doc, m_Parser, m_hNode, Node)) return "NodeProperties: bad arguments";
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
