#ifndef XMATERIAL_SUBGRAPH_H
#define XMATERIAL_SUBGRAPH_H
#pragma once

// Sub-graphs (documentation/subgraphs_plan.md): what the compiler and the editor share about them. No ImGui, no editor.
// A group node (the "Group" prefab) names a MaterialSubgraph; its pins mirror that sub-graph's interface (CallPins). Before code is made, every
// group node is replaced by a copy of its sub-graph's steps (FlattenCalls), nested ones first, so the code generator never sees one.
#include "xmaterial_graph.h"
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <unordered_set>
#include <algorithm>
#include <string>

namespace xmaterial_graph
{
    inline std::uint64_t MixGuid(std::uint64_t A, std::uint64_t B) noexcept               // splitmix64 of a combination: derived ids, the same every time
    {
        std::uint64_t z = A + B * 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // An output pin's sub-elements follow its type (as CompletePrefab does for node types), their ids derived from the pin's
    inline void SetPinType(pin& P, bool bOutputPin, const sType& T) noexcept
    {
        P.m_TypeGUID = T.m_GUID;
        if (!bOutputPin) return;
        auto& O = static_cast<output_pin&>(P);
        O.m_SubElements.resize(T.m_Sub.size());
        for (std::size_t i = 0; i < T.m_Sub.size(); ++i)
        {
            O.m_SubElements[i].m_Name     = T.m_Sub[i].m_Name;
            O.m_SubElements[i].m_TypeGUID = T.m_Sub[i].m_TypeGUID;
            O.m_SubElements[i].m_PinGUID  = pin_guid{ MixGuid(P.m_PinGUID.m_Value, i + 1) };
        }
    }

    inline const node* FindGroupNode(const graph& G, bool bIn) noexcept
    {
        for (auto& [Id, p] : G.m_InstanceNodes) if (bIn ? p->isGroupInputNode() : p->isGroupOutputNode()) return p.get();
        return nullptr;
    }

    inline std::uint64_t CallSubgraph(const node& Call) noexcept
    {
        for (auto& P : Call.m_Params) if (P.m_Type == node_param::type::SUBGRAPH_RESOURCE) return P.m_Value.get<xresource::full_guid>().m_Instance.m_Value;
        return 0;
    }

    // "" when the graph is a sound sub-graph, else why not: one Group Output with at least one pin, every one of them fed, at most one Group
    // Input, no material output, every material parameter read declared (a sub-graph's parameters become the parameters of the materials using it)
    inline std::string ValidateSubgraph(const graph& G) noexcept
    {
        const node *pIn = nullptr, *pOut = nullptr;
        for (auto& [Id, p] : G.m_InstanceNodes)
        {
            if (p->isGroupInputNode())       { if (pIn)  return "a sub-graph has one Group Input";  pIn  = p.get(); }
            else if (p->isGroupOutputNode()) { if (pOut) return "a sub-graph has one Group Output"; pOut = p.get(); }
            else if (p->isOutputNode()) return std::format("'{}' is a material output: a sub-graph's outputs are the Group Output's pins", p->m_Name);
            for (auto& Prm : p->m_Params)
                if (Prm.m_ParamID && !std::ranges::any_of(G.m_Params, [&](auto& D) { return D.m_ID == Prm.m_ParamID; }))
                    return std::format("'{}' reads parameter {:016X}, which the sub-graph does not declare", p->m_StepName, Prm.m_ParamID);
        }
        if (!pOut || pOut->m_InputPins.empty()) return "the sub-graph has no output: add one with AddGroupPin -Side Out";
        for (auto& P : pOut->m_InputPins)
            if (P.m_ConnectionGUID.m_Value == 0 || !G.m_Connections.contains(P.m_ConnectionGUID)) return std::format("the output '{}' is fed by nothing", P.m_Name);
        return {};
    }

    // The interface as text, one pin per line: "in|out <type> <name>" (the sub-graph's compiled resource; group nodes read the source graph)
    inline std::string InterfaceText(const graph& G) noexcept
    {
        std::string Out;
        auto TypeName = [&](type_guid T) { auto* p = G.findType(T); return p ? p->m_Name : std::string("?"); };
        if (auto* p = FindGroupNode(G, true))  for (auto& P : p->m_OutputPins) Out += std::format("in {} {}\n",  TypeName(P.m_TypeGUID), P.m_Name);
        if (auto* p = FindGroupNode(G, false)) for (auto& P : p->m_InputPins)  Out += std::format("out {} {}\n", TypeName(P.m_TypeGUID), P.m_Name);
        for (auto& D : G.m_Params) Out += std::format("param {} {}\n", xmaterial::param_type_enum_v[int(D.m_Type)].m_pName, D.m_Name);
        return Out;
    }

    // Reads a sub-graph's graph file (its Descriptor.txt); false when it is not there
    inline bool LoadSubgraph(graph& Sub, const std::wstring& Path) noexcept
    {
        if (Path.empty() || !std::filesystem::exists(Path)) return false;
        Sub.CreateGraph(Sub);
        Sub.m_bSubgraph = true;
        Sub.serialize(Sub, true, Path);
        return true;
    }

    // A group node's pins from its sub-graph's interface: an input per Group Input pin, an output per Group Output pin, with ids derived from the
    // group node's and the interface pin's (so a renamed or moved pin keeps its connections). Connections to pins that went away are dropped.
    inline void CallPins(graph& G, node& Call, const graph& Sub) noexcept
    {
        std::vector<input_pin>  In;
        std::vector<output_pin> Out;
        if (auto* p = FindGroupNode(Sub, true))
            for (auto& P : p->m_OutputPins)
            {
                auto& I = In.emplace_back();
                I.m_Name = P.m_Name; I.m_TypeGUID = P.m_TypeGUID; I.m_PinGUID = pin_guid{ MixGuid(Call.m_Guid.m_Value, P.m_PinGUID.m_Value) };
                for (auto& Old : Call.m_InputPins) if (Old.m_PinGUID == I.m_PinGUID) I.m_ConnectionGUID = Old.m_ConnectionGUID;
            }
        if (auto* p = FindGroupNode(Sub, false))
            for (auto& P : p->m_InputPins)
            {
                auto& O = Out.emplace_back();
                O.m_Name = P.m_Name; O.m_PinGUID = pin_guid{ MixGuid(Call.m_Guid.m_Value, P.m_PinGUID.m_Value) };
                if (auto* T = G.findType(P.m_TypeGUID)) SetPinType(O, true, *T); else O.m_TypeGUID = P.m_TypeGUID;
            }

        // what still exists keeps its connections; the rest goes
        std::unordered_set<std::uint64_t> Keep;
        for (auto& P : In)  Keep.insert(P.m_PinGUID.m_Value);
        for (auto& P : Out) { Keep.insert(P.m_PinGUID.m_Value); for (auto& S : P.m_SubElements) Keep.insert(S.m_PinGUID.m_Value); }
        std::vector<connection_guid> Drop;
        for (auto& [Id, pC] : G.m_Connections)
            for (auto Pin : { pC->m_InputPinGuid, pC->m_OutputPinGuid })
                if (auto It = G.m_PinToNode.find(Pin); It != G.m_PinToNode.end() && It->second == Call.m_Guid && !Keep.contains(Pin.m_Value)) { Drop.push_back(Id); break; }
        for (auto Id : Drop) G.RemoveConnection(Id);
        for (auto& I : In) if (I.m_ConnectionGUID.m_Value && !G.m_Connections.contains(I.m_ConnectionGUID)) I.m_ConnectionGUID = {};

        Call.m_InputPins  = std::move(In);
        Call.m_OutputPins = std::move(Out);
        Call.ComputeMaxInputChars(); Call.ComputeMaxOutputChars();
        if (G.m_InstanceNodes.contains(Call.m_Guid)) G.RebuildPinGuidForNode(Call);       // a step (not a search's picture of one)
    }

    // The sub-graphs a graph uses, transitively: true when Target is among them (or a sub-graph cannot be read: then Why says which)
    inline bool UsesSubgraph(const graph& G, std::uint64_t Target, const std::function<std::wstring(std::uint64_t)>& PathOf, std::vector<std::uint64_t> Chain = {}) noexcept
    {
        for (auto& [Id, p] : G.m_InstanceNodes)
        {
            if (!p->isGroupCallNode()) continue;
            const auto S = CallSubgraph(*p);
            if (S == 0) continue;
            if (S == Target) return true;
            if (std::ranges::find(Chain, S) != Chain.end()) continue;           // a loop that does not reach Target: FlattenCalls reports it
            graph Sub;
            if (!LoadSubgraph(Sub, PathOf(S))) continue;
            Chain.push_back(S);
            if (UsesSubgraph(Sub, Target, PathOf, Chain)) return true;
            Chain.pop_back();
        }
        return false;
    }

    namespace details
    {
        inline connection& Link(graph& G, std::uint64_t Guid, pin_guid From, pin_guid To, std::string Swizzle) noexcept
        {
            auto& C = G.createConnection(connection_guid{ Guid });
            C.m_OutputPinGuid = From; C.m_InputPinGuid = To; C.m_Swizzle = std::move(Swizzle);
            if (auto* pN = G.findNodeByPin(To)) for (auto& P : pN->m_InputPins) if (P.m_PinGUID == To) P.m_ConnectionGUID = C.m_Guid;
            return C;
        }

        // The swizzle B of what A already takes: "zyx" of "wzyx" is "yzw"
        inline std::optional<std::string> Compose(const std::string& A, const std::string& B) noexcept
        {
            if (A.empty()) return B;
            if (B.empty()) return A;
            std::string R;
            for (char c : B) { const auto i = std::string_view("xyzw").find(c); if (i == std::string_view::npos || i >= A.size()) return std::nullopt; R += A[i]; }
            return R;
        }

        // Component Sub of a source taken with Swizzle: the sub-element pin of the whole output
        inline std::optional<pin_guid> Component(const graph& G, pin_guid Source, const std::string& Swizzle, int Sub) noexcept
        {
            int i = Sub;
            if (!Swizzle.empty()) { if (Sub >= (int)Swizzle.size()) return std::nullopt; i = (int)std::string_view("xyzw").find(Swizzle[Sub]); }
            auto* pN = G.findNodeByPin(Source);
            if (!pN) return std::nullopt;
            bool bIn; int Idx, S;
            if (!G.findPinConst(*pN, Source, bIn, Idx, S) || bIn || S >= 0) return std::nullopt;          // only a whole output has components to pick
            auto& Subs = pN->m_OutputPins[Idx].m_SubElements;
            if (i < 0 || i >= (int)Subs.size()) return std::nullopt;
            return Subs[i].m_PinGUID;
        }
    }

    // Replaces every group node of G by a copy of its sub-graph's steps (flattened first), wired as the group node was: what fed a group input
    // feeds the steps that read that Group Input pin, what read a group output reads what feeds that Group Output pin. Copies get ids derived from
    // the group node's and step names "<group>_<step>" (GLSL names stay readable and unique). An unconnected group input leaves the steps that read
    // it unconnected (their own literals). Only: that group node alone. PathOf gives a sub-graph's Descriptor.txt; every file read is added to *pUsed. "" or why not.
    inline std::string FlattenCalls(graph& G, const std::function<std::wstring(std::uint64_t)>& PathOf, std::vector<std::wstring>* pUsed = nullptr, node_guid Only = {}, std::vector<std::uint64_t> Chain = {}) noexcept
    {
        std::vector<node_guid> Calls;
        for (auto& [Id, p] : G.m_InstanceNodes) if (p->isGroupCallNode() && (Only.m_Value == 0 || Id == Only)) Calls.push_back(Id);     // Only: that group node alone (Ungroup)

        for (auto CallId : Calls)
        {
            node& Call = *G.m_InstanceNodes.at(CallId);
            const std::string Name = Call.m_StepName.empty() ? Call.m_Name : Call.m_StepName;
            const auto SubId = CallSubgraph(Call);
            if (SubId == 0) return std::format("the group '{}' names no sub-graph", Name);
            if (std::ranges::find(Chain, SubId) != Chain.end()) return std::format("the group '{}' uses sub-graph {:X}, which uses itself", Name, SubId);

            graph Sub;
            const auto Path = PathOf(SubId);
            if (!LoadSubgraph(Sub, Path)) return std::format("the group '{}' names sub-graph {:X}, which is not there", Name, SubId);
            if (pUsed && std::ranges::find(*pUsed, Path) == pUsed->end()) pUsed->push_back(Path);
            if (auto Why = ValidateSubgraph(Sub); !Why.empty()) return std::format("the group '{}': {}", Name, Why);
            Chain.push_back(SubId);
            if (auto Why = FlattenCalls(Sub, PathOf, pUsed, {}, Chain); !Why.empty()) return Why;
            Chain.pop_back();

            // Its parameters are G's (by id: every group node of that sub-graph reads the same one; one G declares already stays as G has it)
            for (auto& D : Sub.m_Params)
                if (!std::ranges::any_of(G.m_Params, [&](auto& E) { return E.m_ID == D.m_ID; })) G.m_Params.push_back(D);

            const std::uint64_t C = CallId.m_Value;
            auto New = [&](auto Guid) { return MixGuid(C, Guid.m_Value); };

            // What feeds each group input (by the group input pin's id)
            std::unordered_map<std::uint64_t, std::pair<pin_guid, std::string>> Feed;
            for (auto& ip : Call.m_InputPins)
                if (auto It = G.m_Connections.find(ip.m_ConnectionGUID); ip.m_ConnectionGUID.m_Value && It != G.m_Connections.end())
                    Feed[ip.m_PinGUID.m_Value] = { It->second->m_OutputPinGuid, It->second->m_Swizzle };

            // The copies of the steps (not the Group Input/Output)
            for (auto& [Id, p] : Sub.m_InstanceNodes)
            {
                if (p->isGroupInputNode() || p->isGroupOutputNode() || p->isCommentNode()) continue;
                auto Copy = std::make_unique<node>(*p);
                Copy->m_Guid     = node_guid{ New(Id) };
                Copy->m_StepName = Name + "_" + (p->m_StepName.empty() ? p->m_Name : p->m_StepName);
                for (auto& P : Copy->m_InputPins)  { P.m_PinGUID = pin_guid{ New(P.m_PinGUID) }; P.m_ConnectionGUID = {}; }
                for (auto& P : Copy->m_OutputPins) { P.m_PinGUID = pin_guid{ New(P.m_PinGUID) }; for (auto& S : P.m_SubElements) S.m_PinGUID = pin_guid{ New(S.m_PinGUID) }; }
                auto& Ref = *Copy;
                G.m_InstanceNodes[Copy->m_Guid] = std::move(Copy);
                G.RebuildPinGuidForNode(Ref);
            }

            // A source inside the sub-graph, as seen from G: a copied step's pin, or (a Group Input pin) what feeds the group's input; nullopt: nothing
            auto Source = [&](pin_guid From, const std::string& Swizzle) -> std::optional<std::pair<pin_guid, std::string>>
            {
                auto* pN = Sub.findNodeByPin(From);
                if (!pN) return std::nullopt;
                if (!pN->isGroupInputNode()) return std::pair{ pin_guid{ New(From) }, Swizzle };
                bool bIn; int Idx, S;
                Sub.findPinConst(*pN, From, bIn, Idx, S);
                auto It = Feed.find(MixGuid(C, pN->m_OutputPins[Idx].m_PinGUID.m_Value));
                if (It == Feed.end()) return std::nullopt;
                if (S < 0) { auto Sw = details::Compose(It->second.second, Swizzle); return Sw ? std::optional{ std::pair{ It->second.first, *Sw } } : std::nullopt; }
                auto Comp = details::Component(G, It->second.first, It->second.second, S);
                return Comp ? std::optional{ std::pair{ *Comp, Swizzle } } : std::nullopt;
            };

            // The connections inside, but those into the Group Output (they are the group's outputs, below)
            for (auto& [Id, pC] : Sub.m_Connections)
            {
                auto* pTo = Sub.findNodeByPin(pC->m_InputPinGuid);
                if (!pTo || pTo->isGroupOutputNode()) continue;
                if (auto From = Source(pC->m_OutputPinGuid, pC->m_Swizzle)) details::Link(G, New(Id), From->first, pin_guid{ New(pC->m_InputPinGuid) }, From->second);
            }

            // What read a group output now reads what feeds that Group Output pin
            const node* pOut = FindGroupNode(Sub, false);
            std::vector<connection_guid> Unfed;
            for (auto& [Id, pC] : G.m_Connections)
            {
                bool bIn; int Idx, S;
                if (G.findNodeByPin(pC->m_OutputPinGuid) != &Call || !G.findPinConst(Call, pC->m_OutputPinGuid, bIn, Idx, S)) continue;
                std::optional<std::pair<pin_guid, std::string>> From;
                for (auto& q : pOut->m_InputPins)
                    if (MixGuid(C, q.m_PinGUID.m_Value) == Call.m_OutputPins[Idx].m_PinGUID.m_Value)
                        if (auto It = Sub.m_Connections.find(q.m_ConnectionGUID); It != Sub.m_Connections.end()) From = Source(It->second->m_OutputPinGuid, It->second->m_Swizzle);
                if (From && S >= 0)
                {
                    auto Comp = details::Component(G, From->first, From->second, S);
                    From = Comp ? std::optional{ std::pair{ *Comp, std::string{} } } : std::nullopt;
                }
                std::optional<std::string> Sw = From ? details::Compose(From->second, pC->m_Swizzle) : std::nullopt;
                if (!From || !Sw) { Unfed.push_back(Id); continue; }
                pC->m_OutputPinGuid = From->first;
                pC->m_Swizzle       = *Sw;
            }
            for (auto Id : Unfed) G.RemoveConnection(Id);

            // The steps take the group node's place in its section, in the sub-graph's order
            std::vector<node_guid> Steps;
            for (auto& Sec : Sub.m_Stack.m_Sections)
                for (auto S : Sec.m_Steps)
                    if (auto It = Sub.m_InstanceNodes.find(S); It != Sub.m_InstanceNodes.end() && !It->second->isGroupInputNode() && !It->second->isGroupOutputNode() && !It->second->isCommentNode())
                        Steps.push_back(node_guid{ New(S) });
            for (auto& Sec : G.m_Stack.m_Sections)
                if (auto It = std::ranges::find(Sec.m_Steps, CallId); It != Sec.m_Steps.end()) { It = Sec.m_Steps.erase(It); Sec.m_Steps.insert(It, Steps.begin(), Steps.end()); break; }

            G.RemoveNode(CallId);                                                       // with its incoming connections
            G.m_Stack.Reindex();
        }
        return {};
    }

    // Make Group (subgraphs_plan.md 2.9): Steps (reading order; the caller closed them over every path between two of them) move into Sub, a
    // new sub-graph with a Group Input pin per distinct source outside feeding them and a Group Output pin per output of theirs used outside;
    // in G they become one group node, CallGuid, wired as they were. The steps keep their ids in Sub. "" or why not; a refusal can come after
    // G changed (no slot keeps the order): the caller puts G back (a snapshot).
    inline std::string ExtractSubgraph(graph& G, const std::vector<node_guid>& Steps, std::uint64_t SubGuid, node_guid CallGuid, graph& Sub) noexcept
    {
        std::unordered_set<std::uint64_t> In;
        for (auto S : Steps)
        {
            auto It = G.m_InstanceNodes.find(S);
            if (It == G.m_InstanceNodes.end()) return std::format("no step {:016X}", S.m_Value);
            const node& N = *It->second;
            if (N.isOutputNode() || N.isGroupInputNode() || N.isCommentNode()) return std::format("'{}' cannot go into a sub-graph", N.m_StepName);
            In.insert(S.m_Value);
        }
        if (In.empty()) return "no steps";
        auto Inside   = [&](const graph& Of, pin_guid P) { auto* n = Of.findNodeByPin(P); return n && In.contains(n->m_Guid.m_Value); };
        auto PinType  = [&](pin_guid P) { bool b; int i, s; auto* n = G.findNodeByPin(P); auto* p = n ? G.findPinConst(*n, P, b, i, s) : nullptr; return p ? p->m_TypeGUID : type_guid{}; };
        auto BareName = [](std::string N) { N = N.substr(0, N.find('(')); while (!N.empty() && N.back() == ' ') N.pop_back(); return N.empty() ? std::string("In") : N; };
        auto Unique   = [](auto& Pins, std::string N) { std::string R = N; for (int k = 2; std::ranges::any_of(Pins, [&](auto& P) { return P.m_Name == R; }); ++k) R = N + std::to_string(k); return R; };

        // Where the group node goes: where the last step was, else where the first was (the order must hold either way; checked at the end)
        G.m_Stack.Reindex();

        Sub.CreateGraph(Sub);
        Sub.m_bSubgraph = true;
        auto& GIn  = Sub.CreateNode(group_input_prefab_v,  node_guid{ MixGuid(SubGuid, 1) });
        auto& GOut = Sub.CreateNode(group_output_prefab_v, node_guid{ MixGuid(SubGuid, 2) });
        GIn.m_StepName = "Inputs"; GOut.m_StepName = "Outputs";
        for (auto S : Steps)
        {
            auto Copy = std::make_unique<node>(*G.m_InstanceNodes.at(S));
            for (auto& P : Copy->m_InputPins) P.m_ConnectionGUID = {};
            auto& Ref = *Copy;
            Sub.m_InstanceNodes[S] = std::move(Copy);
            Sub.RebuildPinGuidForNode(Ref);
        }

        // The connections: inside ones are copied; one crossing in per distinct source (a Group Input pin), one crossing out per distinct inner output
        struct outer { connection_guid m_Guid; pin_guid m_Pin; std::string m_Swizzle; pin_guid m_Iface; };
        std::vector<outer> Incoming, Outgoing;
        for (auto& [Id, pC] : G.m_Connections)
        {
            const bool bFrom = Inside(G, pC->m_OutputPinGuid), bTo = Inside(G, pC->m_InputPinGuid);
            if (bFrom && bTo) details::Link(Sub, Id.m_Value, pC->m_OutputPinGuid, pC->m_InputPinGuid, pC->m_Swizzle);
            else if (bTo)
            {
                const pin_guid Iface{ MixGuid(SubGuid, pC->m_OutputPinGuid.m_Value) };
                if (!std::ranges::any_of(GIn.m_OutputPins, [&](auto& P) { return P.m_PinGUID == Iface; }))
                {
                    std::string Name = "In";
                    if (auto* n = G.findNodeByPin(pC->m_InputPinGuid)) for (auto& P : n->m_InputPins) if (P.m_PinGUID == pC->m_InputPinGuid) Name = BareName(P.m_Name);
                    auto& P = GIn.m_OutputPins.emplace_back();
                    P.m_Name = Unique(GIn.m_OutputPins, Name); P.m_PinGUID = Iface;
                    if (auto* T = G.findType(PinType(pC->m_OutputPinGuid))) SetPinType(P, true, *T);
                    Incoming.push_back({ Id, pC->m_OutputPinGuid, {}, Iface });
                }
                Sub.RebuildPinGuidForNode(GIn);
                details::Link(Sub, Id.m_Value, Iface, pC->m_InputPinGuid, pC->m_Swizzle);
            }
            else if (bFrom)
            {
                const pin_guid Iface{ MixGuid(SubGuid, pC->m_OutputPinGuid.m_Value ^ 0x0F0F0F0F0F0F0F0Full) };
                if (!std::ranges::any_of(GOut.m_InputPins, [&](auto& P) { return P.m_PinGUID == Iface; }))
                {
                    auto* n = G.findNodeByPin(pC->m_OutputPinGuid);
                    bool b; int i = 0, s = -1;
                    auto* p = G.findPinConst(*n, pC->m_OutputPinGuid, b, i, s);
                    std::string Name = n->m_StepName.empty() ? n->m_Name : n->m_StepName;
                    if (n->m_OutputPins.size() > 1 || s >= 0) Name += "." + (s >= 0 ? p->m_Name : n->m_OutputPins[i].m_Name);
                    auto& P = GOut.m_InputPins.emplace_back();
                    P.m_Name = Unique(GOut.m_InputPins, Name); P.m_PinGUID = Iface; P.m_TypeGUID = p->m_TypeGUID;
                    Sub.RebuildPinGuidForNode(GOut);
                    details::Link(Sub, MixGuid(SubGuid, Iface.m_Value), pC->m_OutputPinGuid, Iface, {});
                }
                Outgoing.push_back({ Id, pC->m_InputPinGuid, pC->m_Swizzle, Iface });
            }
        }
        if (GOut.m_InputPins.empty()) return "nothing outside reads these steps: a sub-graph needs an output";
        GIn.ComputeMaxOutputChars(); GOut.ComputeMaxInputChars();

        Sub.m_Stack.m_Sections = { section{ .m_Guid = section_guid{ MixGuid(SubGuid, 10) }, .m_Name = "Inputs", .m_Steps = { GIn.m_Guid } }
                                 , section{ .m_Guid = section_guid{ MixGuid(SubGuid, 11) }, .m_Name = "Steps",  .m_Steps = Steps }
                                 , section{ .m_Guid = section_guid{ MixGuid(SubGuid, 12) }, .m_Name = "Output", .m_Kind = section::kind::OUTPUT, .m_Steps = { GOut.m_Guid } } };
        Sub.m_Stack.Reindex();

        // G: the steps go, one group node takes their place, wired as they were
        const auto Last = G.m_Stack.m_Where.find(Steps.back()), First = G.m_Stack.m_Where.find(Steps.front());
        if (Last == G.m_Stack.m_Where.end() || First == G.m_Stack.m_Where.end()) return "the steps are not in the stack";
        const section_guid LastSec = G.m_Stack.m_Sections[Last->second.first].m_Guid, FirstSec = G.m_Stack.m_Sections[First->second.first].m_Guid;
        const node_guid AfterLast  = Last->second.second + 1 < (int)G.m_Stack.m_Sections[Last->second.first].m_Steps.size() ? G.m_Stack.m_Sections[Last->second.first].m_Steps[Last->second.second + 1] : node_guid{};
        const node_guid AfterFirst = [&] { auto& V = G.m_Stack.m_Sections[First->second.first].m_Steps; for (int k = First->second.second + 1; k < (int)V.size(); ++k) if (!In.contains(V[k].m_Value)) return V[k]; return node_guid{}; }();

        for (auto S : Steps) G.RemoveNode(S);
        for (auto& Sec : G.m_Stack.m_Sections) std::erase_if(Sec.m_Steps, [&](node_guid S) { return In.contains(S.m_Value); });

        // The parameters the steps read go with them: the sub-graph declares them, G keeps those its other steps still read (flattening gives
        // G the rest back, by id, so a material instance's values stay)
        auto Reads = [](const graph& Of, xmaterial::param_id Id) { return std::ranges::any_of(Of.m_InstanceNodes, [&](auto& N) { return std::ranges::any_of(N.second->m_Params, [&](auto& P) { return P.m_ParamID == Id; }); }); };
        for (auto& D : G.m_Params) if (Reads(Sub, D.m_ID)) Sub.m_Params.push_back(D);
        std::erase_if(G.m_Params, [&](auto& D) { return Reads(Sub, D.m_ID) && !Reads(G, D.m_ID); });

        auto& Call = G.CreateNode(group_call_prefab_v, CallGuid);
        xresource::full_guid Ref; Ref.m_Instance.m_Value = SubGuid; Ref.m_Type = subgraph_type_guid_v;
        for (auto& P : Call.m_Params) if (P.m_Type == node_param::type::SUBGRAPH_RESOURCE) P.m_Value.set<xresource::full_guid>(Ref);
        CallPins(G, Call, Sub);
        for (auto& O : Incoming) details::Link(G, O.m_Guid.m_Value, O.m_Pin, pin_guid{ MixGuid(CallGuid.m_Value, O.m_Iface.m_Value) }, O.m_Swizzle);
        for (auto& O : Outgoing) details::Link(G, O.m_Guid.m_Value, pin_guid{ MixGuid(CallGuid.m_Value, O.m_Iface.m_Value) }, O.m_Pin, O.m_Swizzle);

        auto Place = [&](section_guid Sec, node_guid Before)
        {
            for (auto& S : G.m_Stack.m_Sections) std::erase(S.m_Steps, CallGuid);
            for (auto& S : G.m_Stack.m_Sections)
                if (S.m_Guid == Sec) { auto It = Before.m_Value ? std::ranges::find(S.m_Steps, Before) : S.m_Steps.end(); S.m_Steps.insert(It, CallGuid); }
            G.m_Stack.Reindex();
            return G.m_Stack.IsOrderValid(G);
        };
        if (!Place(LastSec, AfterLast) && !Place(FirstSec, AfterFirst))
            return "a step outside reads these steps and feeds them too: move it out of the way first";
        return {};
    }
}

#endif
