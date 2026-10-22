#ifndef XMATERIAL_GRAPH_EDITING_H
#define XMATERIAL_GRAPH_EDITING_H
#pragma once

// The material's document (its node graph on disk) and the commands that edit it: create and delete nodes, connect and disconnect pins, move nodes,
// set a node's properties, pick a node's shader file. The Material editor's window calls these and so does an AI or a script through the console.
// Every structural command snapshots the whole graph first, so undo puts back everything it touched; moving and property edits undo by value.
#include "source/Tools/Editor/xeditor_document_editor.h"
#include "plugins/xmaterial.plugin/source/Graph/xmaterial_graph.h"
#include "plugins/xmaterial.plugin/source/Graph/xmaterial_stack_adapter.h"
#include "plugins/xmaterial_instance.plugin/source/xmaterial_instance_migrate.h"
#include "dependencies/xproperty/source/sprop/property_sprop_xtextfile_serializer.h"

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
        std::function<void(xmaterial_graph::graph&)> m_OnRead;      // after every read of the file (a load, an undo)

        bool isLoaded() const noexcept override { return m_bLoaded; }

        bool Reset() noexcept override
        {
            m_Graph.m_InstanceNodes.clear();
            m_Graph.m_Connections.clear();
            m_Graph.m_PinToNode.clear();
            m_Graph.m_Stack = {};
            m_Graph.m_Stack.m_Sections.push_back({ .m_Guid = xmaterial_graph::section_guid{ xresource::guid_generator::Type64() }, .m_Name = "Output", .m_Kind = xmaterial_graph::section::kind::OUTPUT });
            if (m_Graph.m_PrefabNodes.empty()) m_Graph.CreateGraph(m_Graph);
            m_Graph.m_bSubgraph = m_Guid.m_Type == xmaterial_graph::subgraph_type_guid_v;
            if (m_Graph.m_bSubgraph)                                        // a new sub-graph starts with its interface: Group Input first, Group Output in the output stack
            {
                auto& In  = m_Graph.CreateNode(xmaterial_graph::group_input_prefab_v,  xmaterial_graph::node_guid{ xresource::guid_generator::Type64() });
                auto& Out = m_Graph.CreateNode(xmaterial_graph::group_output_prefab_v, xmaterial_graph::node_guid{ xresource::guid_generator::Type64() });
                In.m_StepName = "Inputs"; Out.m_StepName = "Outputs";
                m_Graph.m_Stack.m_Sections.insert(m_Graph.m_Stack.m_Sections.begin(), xmaterial_graph::section{ .m_Guid = xmaterial_graph::section_guid{ xresource::guid_generator::Type64() }, .m_Name = "Inputs", .m_Steps = { In.m_Guid } });
                m_Graph.m_Stack.m_Sections.back().m_Steps.push_back(Out.m_Guid);
                m_Graph.m_Stack.Reindex();
            }
            m_bLoaded = true;
            m_bPositionsChanged = true;
            return true;
        }

        bool ReplaceFromFile(const std::wstring& Path) noexcept override
        {
            if (!std::filesystem::exists(Path)) return false;
            m_Graph.serialize(m_Graph, true, Path);
            if (m_OnRead) m_OnRead(m_Graph);                                // group nodes take their sub-graphs' interfaces again
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

    // Why the colour spaces forbid an Out value feeding an In input, "" when they do not (material_contract.md 3): an SColor (sRGB-encoded)
    // feeds only SColor inputs and a Color (linear) never feeds one; a plain Vector4 may, which declares raw data sRGB-encoded.
    inline const char* ColorSpaceClash(xmaterial_graph::type_guid Out, xmaterial_graph::type_guid In) noexcept
    {
        if (Out == In) return "";
        if (Out == xmaterial_graph::scolor_type_guid_v) return "SColor is sRGB-encoded; convert with 'SColor To Color'";
        if (Out == xmaterial_graph::color_type_guid_v && In == xmaterial_graph::scolor_type_guid_v) return "Color is linear; convert with 'Color To SColor'";
        return "";
    }

    using xmaterial_graph::Components;
    using xmaterial_graph::ComponentLetters;

    // The type rule for types alone: the same type, a float feeding a vec2/vec3/vec4 (a splat: the compiler emits vecN(x)), two types of one
    // GLSL type (Color and Vector4 are both vec4), or a longer vector feeding a shorter vector input (truncation: the input takes the first
    // components, the compiler emits .xyz and the chip shows it), unless the colour spaces clash. Widening is refused (a sub-element splats).
    inline bool AreTypesCompatible(const xmaterial_graph::graph& G, xmaterial_graph::type_guid Out, xmaterial_graph::type_guid In) noexcept
    {
        auto* pOut = G.findType(Out);
        auto* pIn  = G.findType(In);
        return pOut && pIn && !*ColorSpaceClash(Out, In)
            && (Out == In || (pOut->m_CodeString == "float" && pIn->m_CodeString.starts_with("vec")) || pOut->m_CodeString == pIn->m_CodeString
                || (Components(G, In) >= 2 && Components(G, Out) > Components(G, In)));
    }

    // The output a pin belongs to: itself, or the output of a sub-element
    inline const xmaterial_graph::output_pin* MainOutput(const xmaterial_graph::graph& G, xmaterial_graph::pin_guid Pin) noexcept
    {
        auto* pN = G.findNodeByPin(Pin);
        if (pN) for (auto& O : pN->m_OutputPins)
        {
            if (O.m_PinGUID == Pin) return &O;
            for (auto& S : O.m_SubElements) if (S.m_PinGUID == Pin) return &O;
        }
        return nullptr;
    }

    // Which components an input takes from an output (Main: a whole output) when asked for Ask ("", "w", "bgr", "xxxx"; xyzw or rgba): the pin
    // to connect and the swizzle to store, or why not. One form for each meaning, so both canvases and the compiler agree:
    //   ""                          the whole output (the type rule above: exact, splat or truncation)
    //   one letter, or one repeated  the sub-element (a float; splatted into a vector input, as the old canvas's sub-element pins always did)
    //   the first letters in order   the whole output again (no swizzle: truncation, or exact)
    //   anything else               a swizzle on the whole output, as long as the input (2 to 4 letters), e.g. ".bgr", ".xyxy"
    struct component_pick { xmaterial_graph::pin_guid m_Out = {}; std::string m_Swizzle, m_Error; };
    inline component_pick PickComponents(const xmaterial_graph::graph& G, const xmaterial_graph::output_pin& Main, std::string Ask, const xmaterial_graph::pin& In) noexcept
    {
        component_pick R{ Main.m_PinGUID };
        auto TypeName = [&](xmaterial_graph::type_guid T) { auto* p = G.findType(T); return p ? p->m_Name : std::string("?"); };
        const int nOut = Components(G, Main.m_TypeGUID), nIn = Components(G, In.m_TypeGUID);
        if (auto* pWhy = ColorSpaceClash(Main.m_TypeGUID, In.m_TypeGUID); *pWhy) { R.m_Error = pWhy; return R; }
        if (Ask.empty())
        {
            if (!AreTypesCompatible(G, Main.m_TypeGUID, In.m_TypeGUID))
                R.m_Error = nIn == 1 && nOut > 1 ? std::format("a {} feeds a float input one component: pick .x .y .z or .w", TypeName(Main.m_TypeGUID))
                                                 : std::format("types differ ({} -> {}); pick components (a sub-element or a swizzle) or convert", TypeName(Main.m_TypeGUID), TypeName(In.m_TypeGUID));
            return R;
        }
        for (auto& c : Ask)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (auto k = std::string_view("rgba").find(c); k != std::string_view::npos) c = "xyzw"[k];
        }
        const std::string_view Have("xyzw", std::clamp(nOut, 0, 4));
        if (nOut < 2 || static_cast<int>(Main.m_SubElements.size()) < nOut) { R.m_Error = std::format("a {} has no components to pick", TypeName(Main.m_TypeGUID)); return R; }
        if (Ask.size() > 4 || Ask.find_first_not_of(Have) != std::string::npos) { R.m_Error = std::format("a {} has the components .{} only", TypeName(Main.m_TypeGUID), std::string(ComponentLetters(Main.m_TypeGUID), nOut)); return R; }
        if (nIn == 0) { R.m_Error = std::format("a {} input takes no components", TypeName(In.m_TypeGUID)); return R; }
        if (Ask.find_first_not_of(Ask[0]) == std::string::npos && (Ask.size() == 1 || static_cast<int>(Ask.size()) == nIn))
        {
            R.m_Out = Main.m_SubElements[Have.find(Ask[0])].m_PinGUID;
            return R;
        }
        if (static_cast<int>(Ask.size()) != nIn) { R.m_Error = std::format("this input takes {} component{}: {} letter{}", nIn, nIn == 1 ? "" : "s", nIn, nIn == 1 ? "" : "s"); return R; }
        if (Ask != std::string("xyzw", nIn)) R.m_Swizzle = std::move(Ask);
        return R;
    }

    // The components an input takes from its source, as the chip shows them after the source, one letter per component of a vector input (never shorthand):
    // "xyzw" (the whole), "xxxx" (a splat), "xyz" (truncation), "zyx" (a swizzle); a float input shows its single component ("x", "a") or nothing (all of it)
    inline std::string TakenComponents(const xmaterial_graph::graph& G, const xmaterial_graph::connection& C, const xmaterial_graph::pin& In) noexcept
    {
        auto* pMain = MainOutput(G, C.m_OutputPinGuid);
        if (!pMain) return {};
        const char* L = ComponentLetters(pMain->m_TypeGUID);
        const int nOut = Components(G, pMain->m_TypeGUID), nIn = Components(G, In.m_TypeGUID);
        const std::size_t nVec = nIn >= 2 ? static_cast<std::size_t>(nIn) : 1;
        for (std::size_t k = 0; k < pMain->m_SubElements.size() && k < 4; ++k) if (pMain->m_SubElements[k].m_PinGUID == C.m_OutputPinGuid) return std::string(nVec, L[k]);
        if (!C.m_Swizzle.empty())
        {
            std::string S = C.m_Swizzle;
            for (auto& c : S) if (auto k = std::string_view("xyzw").find(c); k != std::string_view::npos) c = L[k];
            return S;
        }
        if (nIn < 2 || nOut < 1) return {};
        return nOut >= nIn ? std::string(L, nIn) : std::string(nIn, L[0]);          // the whole (or its first ones), or a whole float splatted
    }

    // Two pins can be connected when they are on different nodes and their types fit (AreTypesCompatible).
    inline bool IsPinCompatible(const xmaterial_graph::pin& Out, const xmaterial_graph::pin& In, const xmaterial_graph::graph& G) noexcept
    {
        if (G.findNodeByPin(In.m_PinGUID) == G.findNodeByPin(Out.m_PinGUID)) return false;
        return AreTypesCompatible(G, Out.m_TypeGUID, In.m_TypeGUID);
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

        // A pin of N by name: an output (a sub-element "x" or "Output.x" too) or an input; "A" also matches "A(4)". No name: the node's single output.
        inline bool PinByPart(const xmaterial_graph::node& N, bool bOutput, const std::string& Part, std::uint64_t& Out, std::string* pErr = nullptr) noexcept
        {
            auto Fail = [&](std::string Why) { if (pErr) *pErr = std::move(Why); return false; };
            std::vector<std::pair<std::string, std::uint64_t>> Names;
            if (bOutput)
                for (auto& op : N.m_OutputPins)
                {
                    Names.push_back({ op.m_Name, op.m_PinGUID.m_Value });
                    for (auto& sub : op.m_SubElements) { Names.push_back({ sub.m_Name, sub.m_PinGUID.m_Value }); Names.push_back({ op.m_Name + "." + sub.m_Name, sub.m_PinGUID.m_Value }); }
                }
            else for (auto& ip : N.m_InputPins) Names.push_back({ ip.m_Name, ip.m_PinGUID.m_Value });

            if (Part.empty())
            {
                if (bOutput && N.m_OutputPins.size() == 1) { Out = N.m_OutputPins[0].m_PinGUID.m_Value; return true; }
                return Fail(std::format("'{}' needs a pin: @{}.Name", StepLabel(N), StepLabel(N)));
            }
            int nFound = 0;
            for (auto& [Name, Guid] : Names) if (NoSpace(Name) == NoSpace(Part) || (Name.find('.') == std::string::npos && NoSpace(Name.substr(0, Name.find('('))) == NoSpace(Part))) { Out = Guid; ++nFound; }       // "A(4)" is also "A" (not "A(4).x")
            if (nFound == 0) return Fail(std::format("'{}' has no {} pin '{}'", StepLabel(N), bOutput ? "output" : "input", Part));
            if (nFound > 1)  return Fail(std::format("'{}' is ambiguous on '{}'", Part, StepLabel(N)));
            return true;
        }

        inline bool ResolvePinText(const graph_document& Doc, const std::string& Text, bool bOutput, std::uint64_t& Out, std::string* pErr = nullptr) noexcept
        {
            auto Fail = [&](std::string Why) { if (pErr) *pErr = std::move(Why); return false; };
            if (Text.empty() || Text[0] != '@') return ParseHex(Text, Out) ? true : Fail(std::format("'{}' is not a pin guid or @StepName.Pin", Text));

            const auto Dot       = Text.find('.');
            const std::string StepName = Text.substr(1, Dot == std::string::npos ? std::string::npos : Dot - 1);
            auto* pNode = FindStep(Doc, StepName);
            if (!pNode) return Fail(std::format("no step named '{}'", StepName));
            return PinByPart(*pNode, bOutput, Dot == std::string::npos ? std::string{} : Text.substr(Dot + 1), Out, pErr);
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

        inline std::string TypesDiffer(const xmaterial_graph::graph& G, const xmaterial_graph::pin& Out, const xmaterial_graph::pin& In) noexcept
        {
            if (auto* pWhy = ColorSpaceClash(Out.m_TypeGUID, In.m_TypeGUID); *pWhy) return pWhy;
            auto Name = [&](const xmaterial_graph::pin& P) { auto* p = G.findType(P.m_TypeGUID); return p ? p->m_Name : std::string("?"); };
            return std::format("types differ ({} -> {}); use a sub-element or a conversion", Name(Out), Name(In));
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
                if (!xmaterial_graph::IsAllowedIn(G, *It->second)) return G.m_bSubgraph ? "a sub-graph has no material outputs: its outputs are the Group Output's pins" : "Group Input and Group Output belong in a sub-graph";
                if (It->second->isGroupInputNode() || It->second->isGroupOutputNode())
                    for (auto& [Id, p] : G.m_InstanceNodes) if (p->m_PrefabGuid == It->first) return std::format("a sub-graph has one {}", It->second->m_Name);
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
            const char* getCommandHelp() const noexcept override { return "Connects an output pin to an input pin of the same type (undoable; replaces the input's connection). Usage: Connect -Output hexguid|@Step[.Pin] -Input hexguid|@Step.Pin -Connection hexguid [-Swizzle letters]. -Swizzle: the components the input takes from the output (xyzw or rgba: w, bgr, xyxy; see SetInputSwizzle). A longer vector feeding a shorter vector input gives its first components. Refused when it would make a cycle. When the producer is after the consumer, the consumer and its dependents move to right after the producer."; }
            void RegisterArguments() noexcept override
            {
                m_hOutput     = m_Parser.addOption("Output",     "The output pin's guid, hex",        true, 1);
                m_hInput      = m_Parser.addOption("Input",      "The input pin's guid, hex",         true, 1);
                m_hConnection = m_Parser.addOption("Connection", "The new connection's guid, hex",    true, 1);
                m_hSwizzle    = m_Parser.addOption("Swizzle",    "The components the input takes (xyzw or rgba)", false, 1);
            }
            std::string Apply() noexcept override
            {
                std::uint64_t Out = 0, In = 0, Connection = 0; std::string Err, Swizzle;
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
                if (pOutNode == pInNode) return "the pins are on the same node";
                GetArg(m_Parser, m_hSwizzle, Swizzle);
                if (Lower(Swizzle) == "all") Swizzle.clear();
                if (!Swizzle.empty())
                {
                    auto* pMain = MainOutput(G, xmaterial_graph::pin_guid{ Out });
                    if (!pMain || pMain->m_PinGUID.m_Value != Out) return "-Swizzle picks components of a whole output, not of a sub-element";
                    auto Pick = PickComponents(G, *pMain, Swizzle, *pIn);
                    if (!Pick.m_Error.empty()) return Pick.m_Error;
                    Out = Pick.m_Out.m_Value; Swizzle = Pick.m_Swizzle;
                }
                else if (!IsPinCompatible(*pOut, *pIn, G)) return TypesDiffer(G, *pOut, *pIn);
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
                Link.m_Swizzle       = Swizzle;
                if (const int i = pInNode->getInputPinIndex(Link.m_InputPinGuid); i >= 0) pInNode->m_InputPins[i].m_ConnectionGUID = Link.m_Guid;
                if (Reorder) Reorder();
                return {};
            }
            xcmdline::parser::handle m_hOutput, m_hInput, m_hConnection, m_hSwizzle;
        };

        //---- SetInputSwizzle: which components a connected input takes from its source output (the same connection, its id kept)
        struct set_input_swizzle_cmd : graph_cmd
        {
            set_input_swizzle_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "SetInputSwizzle") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Picks the components a connected input takes from its source output (undoable; the connection keeps its id). Usage: SetInputSwizzle -Input hexguid|@Step.Pin -Swizzle letters. Letters xyzw or rgba: one letter (w) takes that component (the output's sub-element; a vector input splats it), as many letters as the input has components reorder or repeat them (bgr, xyxy), -Swizzle all takes the whole output (a longer vector: its first components)."; }
            void RegisterArguments() noexcept override
            {
                m_hInput   = m_Parser.addOption("Input",   "The connected input",                      true, 1);
                m_hSwizzle = m_Parser.addOption("Swizzle", "The components, xyzw or rgba, or all", true, 1);
            }
            std::string Apply() noexcept override
            {
                std::uint64_t In = 0; std::string Err, Swizzle;
                if (!ResolvePinArg(m_Doc, m_Parser, m_hInput, false, In, &Err)) return Err;
                GetArg(m_Parser, m_hSwizzle, Swizzle);
                if (Lower(Swizzle) == "all") Swizzle.clear();
                auto& G = m_Doc.m_Graph;
                auto* pNode = G.findNodeByPin(xmaterial_graph::pin_guid{ In });
                const int i = pNode ? pNode->getInputPinIndex(xmaterial_graph::pin_guid{ In }) : -1;
                if (i < 0) return "no such input";
                auto C = G.m_Connections.find(pNode->m_InputPins[i].m_ConnectionGUID);
                if (C == G.m_Connections.end()) return "the input is not connected";
                auto* pMain = MainOutput(G, C->second->m_OutputPinGuid);
                if (!pMain) return "the connection is broken";
                auto Pick = PickComponents(G, *pMain, Swizzle, pNode->m_InputPins[i]);
                if (!Pick.m_Error.empty()) return Pick.m_Error;
                if (Pick.m_Out == C->second->m_OutputPinGuid && Pick.m_Swizzle == C->second->m_Swizzle) return "it takes those components already";
                C->second->m_OutputPinGuid = Pick.m_Out;
                C->second->m_Swizzle       = Pick.m_Swizzle;
                return {};
            }
            xcmdline::parser::handle m_hInput, m_hSwizzle;
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

        //---- Parameters (documentation/material_contract.md): an unconnected float or texture input can read a declared parameter instead of its literal.
        inline xmaterial::param_decl* FindParam(xmaterial_graph::graph& G, xmaterial::param_id ID) noexcept
        {
            auto It = std::ranges::find(G.m_Params, ID, &xmaterial::param_decl::m_ID);
            return It == G.m_Params.end() ? nullptr : &*It;
        }

        // A parameter: its id (hex) or @Name (its name, case-insensitive)
        inline bool ResolveParamText(xmaterial_graph::graph& G, const std::string& Text, xmaterial::param_id& Out, std::string* pErr = nullptr) noexcept
        {
            if (!Text.empty() && Text[0] == '@')
            {
                for (auto& D : G.m_Params) if (Lower(D.m_Name) == Lower(Text.substr(1))) { Out = D.m_ID; return true; }
                if (pErr) *pErr = std::format("no parameter named '{}'", Text.substr(1));
                return false;
            }
            if (ParseHex(Text, Out) && FindParam(G, Out)) return true;
            if (pErr) *pErr = std::format("'{}' is not a parameter id or @Name", Text);
            return false;
        }

        // The inputs that read a parameter: "Step.Input", in node order
        inline std::vector<std::string> ParamReaders(const xmaterial_graph::graph& G, xmaterial::param_id ID) noexcept
        {
            std::vector<std::string> Out;
            for (auto& [Guid, pNode] : G.m_InstanceNodes)
                for (auto& Pin : pNode->m_InputPins)
                    if (Pin.m_ParamIndex >= 0 && Pin.m_ParamIndex < static_cast<int>(pNode->m_Params.size()) && pNode->m_Params[Pin.m_ParamIndex].m_ParamID == ID) Out.push_back(StepLabel(*pNode) + "." + Pin.m_Name);
            return Out;
        }

        // The input pin and its node param of "-Node -Param <input name | pin guid>"
        inline std::string ParamInput(graph_document& Doc, const xcmdline::parser& P, xcmdline::parser::handle hNode, xcmdline::parser::handle hParam, xmaterial_graph::node*& pNode, xmaterial_graph::input_pin*& pPin) noexcept
        {
            std::uint64_t Node = 0, Pin = 0; std::string Text, Err;
            if (!ResolveNodeArg(Doc, P, hNode, Node, &Err)) return Err;
            if (pNode = Doc.FindNode(Node); !pNode) return "no such node";
            if (!GetArg(P, hParam, Text)) return "bad arguments";
            if (!PinByPart(*pNode, false, Text, Pin, &Err) && !(ParseHex(Text, Pin) && pNode->getInputPinIndex(xmaterial_graph::pin_guid{ Pin }) >= 0)) return Err;
            pPin = &pNode->m_InputPins[pNode->getInputPinIndex(xmaterial_graph::pin_guid{ Pin })];
            if (pPin->m_ParamIndex < 0 || pPin->m_ParamIndex >= static_cast<int>(pNode->m_Params.size())) return std::format("'{}' has no literal to expose", pPin->m_Name);
            return {};
        }

        //---- ExposeParam: an input becomes a parameter (default = its literal); -Id attaches it to an existing one
        struct expose_param_cmd : graph_cmd
        {
            expose_param_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "ExposeParam") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Makes an unconnected float or texture input read a material parameter instead of its literal (undoable). Usage: ExposeParam -Node hexguid|@StepName -Param input name|pin guid [-Id hexguid|@Name] [-Name text] [-Type COLOR|FLOAT2|FLOAT3|FLOAT4]. A new parameter takes the input's pin guid as its id, its literal as the default and -Name (default: the input's name); -Id attaches the input to an existing parameter of the same type. -Type exposes a Vec2/Vec3/Vec4 node's inputs together as one vector parameter (each input reads one component; a COLOR needs a Vec3 or Vec4)."; }
            void RegisterArguments() noexcept override
            {
                m_hNode  = m_Parser.addOption("Node",  "The node",                                        true,  1);
                m_hParam = m_Parser.addOption("Param", "The input: its name or pin guid",                 true,  1);
                m_hId    = m_Parser.addOption("Id",    "An existing parameter to read (id hex or @Name)", false, 1);
                m_hName  = m_Parser.addOption("Name",  "The new parameter's name",                        false, 1);
                m_hType  = m_Parser.addOption("Type",  "A vector parameter: COLOR, FLOAT2, FLOAT3 or FLOAT4", false, 1);
            }
            // -Type: the float inputs of a Vec node, in order, become the components of one parameter (the compiler reads input i as component i)
            std::string ExposeVector(xmaterial_graph::node& Node, const std::string& TypeText) noexcept
            {
                auto& G = m_Doc.m_Graph;
                const auto It = std::ranges::find_if(xmaterial::param_type_enum_v, [&](auto& E) { return Lower(E.m_pName) == Lower(TypeText); });
                if (It == xmaterial::param_type_enum_v.end() || It->m_Value == int(xmaterial::param_type::FLOAT) || It->m_Value == int(xmaterial::param_type::TEXTURE)) return "-Type takes COLOR, FLOAT2, FLOAT3 or FLOAT4";
                const auto Type = static_cast<xmaterial::param_type>(It->m_Value);
                const int  Size = Type == xmaterial::param_type::FLOAT2 ? 2 : Type == xmaterial::param_type::FLOAT3 ? 3 : 4;

                std::vector<xmaterial_graph::node_param*> Comps;
                for (auto& Pin : Node.m_InputPins)
                {
                    if (Pin.m_ConnectionGUID.m_Value != 0 || Pin.m_ParamIndex < 0 || Pin.m_ParamIndex >= static_cast<int>(Node.m_Params.size())) return "every input of the node must be an unconnected float";
                    auto& P = Node.m_Params[Pin.m_ParamIndex];
                    if (P.m_Type != xmaterial_graph::node_param::type::FLOAT || P.m_ParamID != 0) return "every input of the node must be an unconnected float that reads no parameter";
                    Comps.push_back(&P);
                }
                const int n = static_cast<int>(Comps.size());
                if (n < 2 || n > 4 || (Type == xmaterial::param_type::COLOR ? n < 3 : n != Size)) return std::format("a {} parameter needs a Vec node with {} inputs", It->m_pName, Type == xmaterial::param_type::COLOR ? "3 or 4" : std::to_string(Size));

                xmaterial::param_id ID = Node.m_InputPins[0].m_PinGUID.m_Value;
                std::string Text, Err;
                const bool bAttach = GetArg(m_Parser, m_hId, Text);
                if (bAttach && !ResolveParamText(G, Text, ID, &Err)) return Err;
                auto* pDecl = FindParam(G, ID);
                if (pDecl && pDecl->m_Type != Type) return std::format("parameter '{}' has another type", pDecl->m_Name);
                if (pDecl && n > Size) return "the node has more inputs than the parameter has components";
                if (!pDecl)
                {
                    pDecl = &G.m_Params.emplace_back();
                    if (!GetArg(m_Parser, m_hName, Text)) Text = StepLabel(Node);
                    pDecl->m_ID = ID; pDecl->m_Type = Type; pDecl->m_Name = Text;
                    float V[4] = { 0, 0, 0, Type == xmaterial::param_type::COLOR ? 1.f : 0.f };
                    for (int i = 0; i < n; ++i) V[i] = Comps[i]->m_Value.get<float>();
                    pDecl->m_Default = { V[0], V[1], V[2], V[3] };
                    if (Type == xmaterial::param_type::COLOR && n == 3) pDecl->m_bAlpha = false;
                }
                for (auto* pP : Comps) pP->m_ParamID = ID;
                return {};
            }
            std::string Apply() noexcept override
            {
                xmaterial_graph::node* pNode = nullptr; xmaterial_graph::input_pin* pPin = nullptr;
                if (auto Err = ParamInput(m_Doc, m_Parser, m_hNode, m_hParam, pNode, pPin); !Err.empty()) return Err;
                if (std::string TypeText; GetArg(m_Parser, m_hType, TypeText)) return ExposeVector(*pNode, TypeText);
                auto& G = m_Doc.m_Graph;
                auto& Prop = pNode->m_Params[pPin->m_ParamIndex];
                if (pPin->m_ConnectionGUID.m_Value != 0) return "the input is connected: disconnect it first";
                if (Prop.m_ParamID != 0) return std::format("already reads parameter {:016X}", Prop.m_ParamID);
                const bool bTexture = Prop.m_Type == xmaterial_graph::node_param::type::TEXTURE_RESOURCE;
                if (Prop.m_Type != xmaterial_graph::node_param::type::FLOAT && !bTexture) return "only float and texture inputs can be parameters";
                const auto Type = bTexture ? xmaterial::param_type::TEXTURE : xmaterial::param_type::FLOAT;

                xmaterial::param_id ID = pPin->m_PinGUID.m_Value;
                std::string Text, Err;
                const bool bAttach = GetArg(m_Parser, m_hId, Text);
                if (bAttach && !ResolveParamText(G, Text, ID, &Err)) return Err;
                auto* pDecl = FindParam(G, ID);
                if (pDecl && pDecl->m_Type != Type) return std::format("parameter '{}' has another type", pDecl->m_Name);

                if (!pDecl || (!bAttach && ParamReaders(G, ID).empty()))        // new, or this input exposed again: its literal is the default
                {
                    if (!pDecl) pDecl = &G.m_Params.emplace_back();
                    std::string Name;
                    if (!GetArg(m_Parser, m_hName, Name)) Name = Prop.m_ExposeName.empty() ? Prop.m_Name : Prop.m_ExposeName;
                    pDecl->m_ID = ID; pDecl->m_Type = Type; pDecl->m_Name = Name;
                    if (bTexture) pDecl->m_DefaultTexture = Prop.m_Value.get<xresource::full_guid>();
                    else          pDecl->m_Default.m_X = Prop.m_Value.get<float>();
                }
                Prop.m_ParamID = ID;
                return {};
            }
            xcmdline::parser::handle m_hNode, m_hParam, m_hId, m_hName, m_hType;
        };

        //---- UnexposeParam: the input is a literal again (the parameter's default); the declaration stays
        struct unexpose_param_cmd : graph_cmd
        {
            unexpose_param_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "UnexposeParam") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Makes an input a literal again, with the parameter's default as its value (undoable); the declaration stays. Usage: UnexposeParam -Node hexguid|@StepName -Param input name|pin guid"; }
            void RegisterArguments() noexcept override
            {
                m_hNode  = m_Parser.addOption("Node",  "The node",                        true, 1);
                m_hParam = m_Parser.addOption("Param", "The input: its name or pin guid", true, 1);
            }
            std::string Apply() noexcept override
            {
                xmaterial_graph::node* pNode = nullptr; xmaterial_graph::input_pin* pPin = nullptr;
                if (auto Err = ParamInput(m_Doc, m_Parser, m_hNode, m_hParam, pNode, pPin); !Err.empty()) return Err;
                auto& Prop = pNode->m_Params[pPin->m_ParamIndex];
                if (Prop.m_ParamID == 0) return "the input does not read a parameter";
                if (auto* pDecl = FindParam(m_Doc.m_Graph, Prop.m_ParamID))
                {
                    if (Prop.m_Type == xmaterial_graph::node_param::type::TEXTURE_RESOURCE) Prop.m_Value.set<xresource::full_guid>(pDecl->m_DefaultTexture);
                    else if (pDecl->m_Type != xmaterial::param_type::FLOAT)                 // a vector parameter: every input of the node that reads it, each its component
                    {
                        const float V[4] = { pDecl->m_Default.m_X, pDecl->m_Default.m_Y, pDecl->m_Default.m_Z, pDecl->m_Default.m_W };
                        int i = 0;
                        for (auto& P : pNode->m_Params) if (P.m_ParamID == pDecl->m_ID && P.m_Type == xmaterial_graph::node_param::type::FLOAT) { P.m_Value.set<float>(V[i++ & 3]); P.m_ParamID = 0; }
                    }
                    else                                                                    Prop.m_Value.set<float>(pDecl->m_Default.m_X);
                }
                Prop.m_ParamID = 0;
                return {};
            }
            xcmdline::parser::handle m_hNode, m_hParam;
        };

        //---- SetParamInfo: the metadata of a parameter. A rename touches no instance (ids are the identity).
        struct set_param_info_cmd : graph_cmd
        {
            set_param_info_cmd(xundo::system& System, graph_document& Doc) noexcept : graph_cmd(System, Doc, "SetParamInfo") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Sets what a parameter says about itself (undoable). Usage: SetParamInfo -Id hexguid|@Name [-Name text] [-Group text] [-UiMin v] [-UiMax v] [-ValidMin v] [-ValidMax v] [-HDR 0|1] [-Alpha 0|1] [-EntityField NONE|TINT|PARAM0|PARAM1]. A range is 1 to 4 numbers separated by commas (one number sets all four)."; }
            void RegisterArguments() noexcept override
            {
                m_hId       = m_Parser.addOption("Id",          "The parameter: id hex or @Name",      true,  1);
                m_hName     = m_Parser.addOption("Name",        "New name (instances are untouched)",  false, 1);
                m_hGroup    = m_Parser.addOption("Group",       "Inspector group",                     false, 1);
                m_hUiMin    = m_Parser.addOption("UiMin",       "Slider minimum",                      false, 1);
                m_hUiMax    = m_Parser.addOption("UiMax",       "Slider maximum",                      false, 1);
                m_hValidMin = m_Parser.addOption("ValidMin",    "Smallest value an instance may have", false, 1);
                m_hValidMax = m_Parser.addOption("ValidMax",    "Largest value an instance may have",  false, 1);
                m_hHDR      = m_Parser.addOption("HDR",         "COLOR: 1 lets rgb exceed 1",          false, 1);
                m_hAlpha    = m_Parser.addOption("Alpha",       "COLOR: 0 forces w to 1",              false, 1);
                m_hField    = m_Parser.addOption("EntityField", "NONE, TINT, PARAM0 or PARAM1",        false, 1);
            }
            static bool Vec(const std::string& Text, xmaterial::float4v& Out) noexcept
            {
                float V[4] = {}; int n = 0;
                for (std::size_t Start = 0; Start <= Text.size() && n < 5;)
                {
                    auto End = Text.find(',', Start);
                    if (End == std::string::npos) End = Text.size();
                    float F = 0;
                    const auto R = std::from_chars(Text.data() + Start, Text.data() + End, F);
                    if (R.ec != std::errc() || R.ptr != Text.data() + End) return false;
                    if (n < 4) V[n] = F;
                    ++n; Start = End + 1;
                }
                if (n < 1 || n > 4) return false;
                if (n == 1) V[1] = V[2] = V[3] = V[0];
                Out = { V[0], V[1], V[2], V[3] };
                return true;
            }
            std::string Apply() noexcept override
            {
                auto& G = m_Doc.m_Graph;
                std::string Text, Err;
                xmaterial::param_id ID = 0;
                GetArg(m_Parser, m_hId, Text);
                if (!ResolveParamText(G, Text, ID, &Err)) return Err;
                auto D = *FindParam(G, ID);                                     // a copy: nothing changes unless every option is good
                bool bAny = false;

                if (GetArg(m_Parser, m_hName,  Text)) { if (Text.empty()) return "the name is empty"; D.m_Name = Text; bAny = true; }
                if (GetArg(m_Parser, m_hGroup, Text)) { D.m_Group = Text; bAny = true; }
                const std::pair<xcmdline::parser::handle, xmaterial::float4v*> Ranges[] = { { m_hUiMin, &D.m_UiMin }, { m_hUiMax, &D.m_UiMax }, { m_hValidMin, &D.m_ValidMin }, { m_hValidMax, &D.m_ValidMax } };
                for (auto& [H, pV] : Ranges) if (GetArg(m_Parser, H, Text)) { if (!Vec(Text, *pV)) return std::format("'{}' is not 1 to 4 numbers", Text); bAny = true; }
                for (auto [H, pB] : { std::pair{ m_hHDR, &D.m_bHDR }, std::pair{ m_hAlpha, &D.m_bAlpha } })
                    if (GetArg(m_Parser, H, Text)) { if (Text != "0" && Text != "1") return "-HDR and -Alpha take 0 or 1"; *pB = Text == "1"; bAny = true; }
                if (GetArg(m_Parser, m_hField, Text))
                {
                    const auto It = std::ranges::find_if(xmaterial::entity_field_enum_v, [&](auto& E) { return Lower(E.m_pName) == Lower(Text); });
                    if (It == xmaterial::entity_field_enum_v.end()) return "-EntityField takes NONE, TINT, PARAM0 or PARAM1";
                    D.m_EntityField = static_cast<xmaterial::entity_field>(It->m_Value); bAny = true;
                }
                if (!bAny) return "nothing to set";
                *FindParam(G, ID) = D;
                return {};
            }
            xcmdline::parser::handle m_hId, m_hName, m_hGroup, m_hUiMin, m_hUiMax, m_hValidMin, m_hValidMax, m_hHDR, m_hAlpha, m_hField;
        };

        //---- The migration ops (documentation/material_contract.md section 6): a graph edit that may also rewrite the instance files of the material. The undo puts both back.
        struct migration_cmd : graph_cmd
        {
            using graph_cmd::graph_cmd;
            std::wstring  Descriptors() const noexcept { return xmaterial_instance::DescriptorsOf(m_Doc.m_DescriptorPath); }
            std::uint64_t Material()    const noexcept { return m_Doc.m_Guid.m_Instance.m_Value; }
            void BackupCurrenState(xundo::undo_file& File) noexcept override { graph_cmd::BackupCurrenState(File); xeditor::WriteString(File, xmaterial_instance::Snapshot(Descriptors(), Material())); }
            void Undo(xundo::undo_file& File) noexcept override
            {
                graph_cmd::Undo(File);
                xmaterial_instance::Restore(Descriptors(), m_Doc.m_LibraryGuid, xeditor::ReadString(File));
            }
        };

        //---- SetParamDefault: the default of a parameter. Inherit: every instance that did not set it follows. PreserveLook: the instances made directly from the material that did not set it
        // get the old default written first, so they look as before (deeper ones inherit it through them).
        struct set_param_default_cmd : migration_cmd
        {
            set_param_default_cmd(xundo::system& System, graph_document& Doc) noexcept : migration_cmd(System, Doc, "SetParamDefault") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Sets the default of a parameter (undoable). Usage: SetParamDefault -Id hexguid|@Name -Value 1 to 4 comma separated numbers [-Policy Inherit|PreserveLook]. Inherit (default): instances that did not set it follow the new default. PreserveLook: instances made directly from the material that did not set it keep the old value (written as an override). Not for TEXTURE parameters."; }
            void RegisterArguments() noexcept override
            {
                m_hId     = m_Parser.addOption("Id",     "The parameter: id hex or @Name",       true,  1);
                m_hValue  = m_Parser.addOption("Value",  "The new default, 1 to 4 numbers",      true,  1);
                m_hPolicy = m_Parser.addOption("Policy", "Inherit (default) or PreserveLook",    false, 1);
            }
            std::string Apply() noexcept override
            {
                auto& G = m_Doc.m_Graph;
                std::string Text, Err;
                xmaterial::param_id ID = 0;
                GetArg(m_Parser, m_hId, Text);
                if (!ResolveParamText(G, Text, ID, &Err)) return Err;
                auto& D = *FindParam(G, ID);
                if (D.m_Type == xmaterial::param_type::TEXTURE) return "a texture has no value default";
                xmaterial::float4v New;
                GetArg(m_Parser, m_hValue, Text);
                if (!set_param_info_cmd::Vec(Text, New)) return std::format("'{}' is not 1 to 4 numbers", Text);
                bool bPreserve = false;
                if (GetArg(m_Parser, m_hPolicy, Text))
                {
                    const auto P = Lower(Text);
                    if (P == "preservelook" || P == "preserve") bPreserve = true;
                    else if (P != "inherit")                    return "-Policy takes Inherit or PreserveLook";
                }
                if (bPreserve)
                {
                    auto Files = xmaterial_instance::ReadInstances(Descriptors(), Material());
                    xmaterial_instance::PreserveLookFiles(Files, Material(), D, D.m_Default);
                    xmaterial_instance::WriteChanged(Descriptors(), m_Doc.m_LibraryGuid, Files);
                }
                D.m_Default = New;
                return {};
            }
            xcmdline::parser::handle m_hId, m_hValue, m_hPolicy;
        };

        //---- RetypeParam: the parameter keeps its id and gets another type. With -Convert the default and the overrides of the old type are converted (the instance files are rewritten);
        // without it the overrides become W_ORPHAN_TYPE (kept, not applied) until they are set again.
        struct retype_param_cmd : migration_cmd
        {
            retype_param_cmd(xundo::system& System, graph_document& Doc) noexcept : migration_cmd(System, Doc, "RetypeParam") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Changes the type of a parameter, keeping its id (undoable). Usage: RetypeParam -Id hexguid|@Name -Type FLOAT|FLOAT2|FLOAT3|FLOAT4|COLOR|TEXTURE [-Convert Splat|Truncate]. With -Convert the default and the instances' values of the old type are converted: the leading components stay, the rest is zero (Splat: filled with the first), and the instance files are rewritten; not between TEXTURE and the others. Without it the instances' values become W_ORPHAN_TYPE. Inputs that read the parameter must be changed to match (the compiler reports E_PARAM_TYPE)."; }
            void RegisterArguments() noexcept override
            {
                m_hId      = m_Parser.addOption("Id",      "The parameter: id hex or @Name",   true,  1);
                m_hType    = m_Parser.addOption("Type",    "The new type",                     true,  1);
                m_hConvert = m_Parser.addOption("Convert", "Splat or Truncate",                false, 1);
            }
            std::string Apply() noexcept override
            {
                auto& G = m_Doc.m_Graph;
                std::string Text, Err;
                xmaterial::param_id ID = 0;
                GetArg(m_Parser, m_hId, Text);
                if (!ResolveParamText(G, Text, ID, &Err)) return Err;
                auto& D = *FindParam(G, ID);
                GetArg(m_Parser, m_hType, Text);
                const auto It = std::ranges::find_if(xmaterial::param_type_enum_v, [&](auto& E) { return Lower(E.m_pName) == Lower(Text); });
                if (It == xmaterial::param_type_enum_v.end()) return "-Type takes FLOAT, FLOAT2, FLOAT3, FLOAT4, COLOR or TEXTURE";
                const auto To = static_cast<xmaterial::param_type>(It->m_Value), From = D.m_Type;
                if (To == From) return "the parameter already has that type";
                bool bConvert = false, bSplat = false;
                if (GetArg(m_Parser, m_hConvert, Text))
                {
                    const auto C = Lower(Text);
                    if (C != "splat" && C != "truncate")                                                return "-Convert takes Splat or Truncate";
                    if (To == xmaterial::param_type::TEXTURE || From == xmaterial::param_type::TEXTURE) return "a texture cannot be converted to or from a value";
                    bConvert = true; bSplat = C == "splat";
                }
                if (bConvert)
                {
                    auto Files = xmaterial_instance::ReadInstances(Descriptors(), Material());
                    xmaterial_instance::RetypeFiles(Files, ID, From, To, bSplat);
                    xmaterial_instance::WriteChanged(Descriptors(), m_Doc.m_LibraryGuid, Files);
                    D.m_Default = xmaterial_instance::Convert(D.m_Default, From, To, bSplat);
                }
                D.m_Type = To;
                return {};
            }
            xcmdline::parser::handle m_hId, m_hType, m_hConvert;
        };

        //---- PurgeUnusedParams: the declarations no input reads (the only way a declaration goes). Instance values for them stay: they are orphans (W_ORPHAN_ID), cleared with ClearOverride.
        struct purge_unused_params_cmd : migration_cmd
        {
            purge_unused_params_cmd(xundo::system& System, graph_document& Doc) noexcept : migration_cmd(System, Doc, "PurgeUnusedParams") { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "Deletes the parameter declarations that no input reads (undoable). Instances' values for them stay as orphans. Usage: PurgeUnusedParams"; }
            void RegisterArguments() noexcept override {}
            std::string Apply() noexcept override
            {
                auto& G = m_Doc.m_Graph;
                const auto n = std::erase_if(G.m_Params, [&](auto& D) { return ParamReaders(G, D.m_ID).empty(); });
                return n ? std::string{} : "every parameter is read";
            }
        };

        // "param link: Input -> parameter 'Name' (id)" for every input of the node that reads one
        inline std::string ParamLinks(const xmaterial_graph::graph& G, const xmaterial_graph::node& N) noexcept
        {
            std::string Out;
            for (auto& Pin : N.m_InputPins)
                if (Pin.m_ParamIndex >= 0 && Pin.m_ParamIndex < static_cast<int>(N.m_Params.size()) && N.m_Params[Pin.m_ParamIndex].m_ParamID != 0)
                {
                    const auto ID = N.m_Params[Pin.m_ParamIndex].m_ParamID;
                    auto It = std::ranges::find(G.m_Params, ID, &xmaterial::param_decl::m_ID);
                    Out += std::format("param link: {} -> parameter '{}' ({:016X})\n", Pin.m_Name, It == G.m_Params.end() ? "?" : It->m_Name, ID);
                }
            return Out;
        }

        //---- ListParams: the declarations, who reads them, where the last compile put them
        struct list_params_cmd : xundo::query_command_base
        {
            graph_document& m_Doc;
            list_params_cmd(xundo::system& System, graph_document& Doc) noexcept : query_command_base(System, "ListParams", nullptr), m_Doc(Doc) {}
            const char* getCommandHelp() const noexcept override { return "Lists the material parameters: id, name, type, entity field, the inputs that read each, and its place in the block of the last compile. Usage: ListParams"; }
            void RegisterArguments() noexcept override {}
            std::string Query() noexcept override
            {
                if (!m_Doc.isLoaded()) return "ListParams: nothing loaded";
                auto& G = m_Doc.m_Graph;

                xmaterial::manifest Last;                                       // MaterialLayout.txt, written by the compiler beside shader.txt
                bool bLast = false;
                if (const auto Path = m_Doc.m_LogPath + L"/MaterialLayout.txt"; std::filesystem::exists(Path))
                {
                    xtextfile::stream Stream; xproperty::settings::context C{};
                    bLast = !Stream.Open(true, Path, xtextfile::file_type::TEXT) && !xproperty::sprop::serializer::Stream(Stream, Last, C);
                }

                std::string Out;
                for (auto& D : G.m_Params)
                {
                    auto Readers = ParamReaders(G, D.m_ID);
                    std::string Where = "-";
                    if (bLast)
                        if (auto It = std::ranges::find(Last.m_Entries, D.m_ID, &xmaterial::manifest_entry::m_ID); It != Last.m_Entries.end())
                            Where = It->m_Type == xmaterial::param_type::TEXTURE ? std::format("sampler {}", It->m_Sampler) : std::format("byte {}", It->m_ByteOffset);
                    Out += std::format("{:016X}  {}  {}  entity={}  offset={}  readers={}:", D.m_ID, D.m_Name, xmaterial::param_type_enum_v[int(D.m_Type)].m_pName, xmaterial::entity_field_enum_v[int(D.m_EntityField)].m_pName, Where, Readers.size());
                    for (auto& R : Readers) Out += " " + R;
                    Out += '\n';
                }
                return Out.empty() ? "(no parameters)" : Out;
            }
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
                return ListProperties(set_node_property_cmd::TargetOf(*pNode), Filter) + ParamLinks(m_Doc.m_Graph, *pNode);
            }
            xcmdline::parser::handle m_hNode, m_hFilter;
        };
    }
}

#endif // XMATERIAL_GRAPH_EDITING_H
