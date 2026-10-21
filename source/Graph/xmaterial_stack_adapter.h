#ifndef XMATERIAL_STACK_ADAPTER_H
#define XMATERIAL_STACK_ADAPTER_H
#pragma once

// Order functions (stack_canvas.md section 3) and the v1 -> stack migration adapter (section 8).
// The order functions are pure: nodes are dense ints 0..N-1, an edge is (producer, consumer), the order is sections of ints.
// Define XMATERIAL_STACK_ADAPTER_CORE_ONLY to get only those (standalone self-check, no graph headers).

#include <vector>
#include <queue>
#include <algorithm>
#include <cassert>

namespace xmaterial_graph::stack_order
{
    using edges    = std::vector<std::pair<int,int>>;           // (producer, consumer)
    using sections = std::vector<std::vector<int>>;             // ordered sections of ordered steps; the global order is their concatenation

    inline std::vector<int> Flatten(const sections& S) noexcept
    {
        std::vector<int> F;
        for (auto& s : S) F.insert(F.end(), s.begin(), s.end());
        return F;
    }

    // Kahn with a min-heap keyed by the current index: producers first, everything already valid keeps its relative order.
    // Returns false on a cycle (the nodes inside or behind it are appended in their current order, nothing is lost).
    inline bool StableTopoSort(std::vector<int>& Order, const edges& E) noexcept
    {
        const int N = static_cast<int>(Order.size());
        std::vector<int> Key(N), In(N, 0);
        std::vector<std::vector<int>> Out(N);
        for (int i = 0; i < N; ++i) Key[Order[i]] = i;
        for (auto [p, c] : E) { Out[p].push_back(c); ++In[c]; }

        std::priority_queue<int, std::vector<int>, std::greater<int>> Ready;
        for (int i = 0; i < N; ++i) if (In[Order[i]] == 0) Ready.push(i);

        std::vector<int> R;
        while (!Ready.empty())
        {
            const int n = Order[Ready.top()]; Ready.pop();
            R.push_back(n);
            for (int c : Out[n]) if (--In[c] == 0) Ready.push(Key[c]);
        }

        const bool bOk = static_cast<int>(R.size()) == N;
        if (!bOk) for (int n : Order) if (In[n] > 0) R.push_back(n);
        Order = std::move(R);
        return bOk;
    }

    // Global index of every node of S (-1 for Skip and for ids not in S), and the (section, index) of each.
    struct where { std::vector<int> m_Global, m_Sec, m_Idx; };
    inline where Locate(const sections& S, int N, int Skip = -1) noexcept
    {
        where W{ std::vector<int>(N, -1), std::vector<int>(N, -1), std::vector<int>(N, -1) };
        for (int s = 0, g = 0; s < static_cast<int>(S.size()); ++s)
            for (int i = 0, k = 0; i < static_cast<int>(S[s].size()); ++i)
            {
                if (S[s][i] == Skip) continue;
                W.m_Global[S[s][i]] = g++; W.m_Sec[S[s][i]] = s; W.m_Idx[S[s][i]] = k++;
            }
        return W;
    }

    inline int SizeWithout(const sections& S, int Sec, int Skip) noexcept
    {
        return static_cast<int>(S[Sec].size()) - static_cast<int>(std::count(S[Sec].begin(), S[Sec].end(), Skip));
    }

    // Slots of Sec (0..size, counted with Node removed) where Node may sit: after its latest producer, before its earliest consumer.
    // Empty (first > second) when the producer is in a later section or the consumer in an earlier one.
    inline std::pair<int,int> ValidRange(const sections& S, const edges& E, int Node, int Sec) noexcept
    {
        const int N = static_cast<int>(Flatten(S).size());
        const auto W = Locate(S, N, Node);
        int P = -1, C = static_cast<int>(std::count_if(W.m_Global.begin(), W.m_Global.end(), [](int g){ return g >= 0; }));    // nodes without Node
        for (auto [p, c] : E)
        {
            if (c == Node && W.m_Global[p] >= 0) P = std::max(P, W.m_Global[p]);
            if (p == Node && W.m_Global[c] >= 0) C = std::min(C, W.m_Global[c]);
        }
        int Base = 0; for (int s = 0; s < Sec; ++s) Base += SizeWithout(S, s, Node);
        return { std::max(0, P + 1 - Base), std::min(SizeWithout(S, Sec, Node), C - Base) };
    }

    // Where a new/orphan step goes: right after its latest producer, else right before its earliest consumer, else the end of the
    // last USER section. Result is (section, slot) with Node removed from S; section -1 = no USER section, create "Steps" before the output.
    inline std::pair<int,int> DefaultSlot(const sections& S, const std::vector<char>& IsOutput, const edges& E, int Node) noexcept
    {
        const int N  = static_cast<int>(Flatten(S).size()) + 1;          // Node may be an orphan (ids are dense, Node included)
        const auto W = Locate(S, N, Node);
        int P = -1, C = -1;
        for (auto [p, c] : E)
        {
            if (c == Node && W.m_Global[p] >= 0 && (P < 0 || W.m_Global[p] > W.m_Global[P])) P = p;
            if (p == Node && W.m_Global[c] >= 0 && (C < 0 || W.m_Global[c] < W.m_Global[C])) C = c;
        }
        if (P >= 0) return { W.m_Sec[P], W.m_Idx[P] + 1 };
        if (C >= 0) return { W.m_Sec[C], W.m_Idx[C] };
        for (int s = static_cast<int>(S.size()) - 1; s >= 0; --s)
            if (!IsOutput[s]) return { s, SizeWithout(S, s, Node) };
        return { -1, 0 };
    }

    struct move_result { sections m_Dependents, m_Nearest; };

    // Move the block Nodes to (Sec, Slot), Slot counted in Sec with the block removed. Two valid orders (previewed, one undo step either way):
    //  m_Dependents: consumers left before the block (transitively) are dragged to right after it, producers left after it (transitively) to right before it.
    //  m_Nearest:    the valid slot closest to the drop (clamped by the block's external producers/consumers; across sections the nearest boundary);
    //                equals m_Dependents when no single slot fits (a producer of one member lies after a consumer of another).
    // Precondition: the block is convex (no path between two members), and its members are in a valid relative order.
    inline move_result ResolveMove(const sections& S, const edges& E, const std::vector<int>& Nodes, int Sec, int Slot) noexcept
    {
        const int N = static_cast<int>(Flatten(S).size());
        std::vector<char> InBlock(N, 0);
        for (int n : Nodes) InBlock[n] = 1;

        sections R = S;                                                     // S without the block
        for (auto& s : R) s.erase(std::remove_if(s.begin(), s.end(), [&](int n){ return InBlock[n] != 0; }), s.end());
        auto BaseOf = [&](int s){ int B = 0; for (int i = 0; i < s; ++i) B += static_cast<int>(R[i].size()); return B; };
        const int T = BaseOf(Sec) + std::clamp(Slot, 0, static_cast<int>(R[Sec].size()));


        move_result Res;

        // dependents
        {
            std::vector<int> M = Flatten(R);
            M.insert(M.begin() + T, Nodes.begin(), Nodes.end());
            const int K = static_cast<int>(Nodes.size());
            std::vector<std::vector<int>> Pred(N), Succ(N);
            for (auto [p, c] : E) { Pred[c].push_back(p); Succ[p].push_back(c); }

            std::vector<char> Drag(N, 0);                                   // in D or U (joins Sec)
            std::vector<char> Mark = InBlock;
            for (int i = 0; i < T; ++i)                                     // consumers before the block, ascending (R is topological)
                for (int p : Pred[M[i]]) if (Mark[p]) { Drag[M[i]] = Mark[M[i]] = 1; break; }
            std::vector<char> Mark2 = InBlock;
            for (int i = static_cast<int>(M.size()) - 1; i >= T + K; --i)   // producers after the block, descending
                for (int c : Succ[M[i]]) if (Mark2[c]) { Drag[M[i]] = Mark2[M[i]] = 1; break; }

            std::vector<int> F, U, D, Lab(N);
            for (int s = 0; s < static_cast<int>(R.size()); ++s) for (int n : R[s]) Lab[n] = s;
            for (int n : Nodes) Lab[n] = Sec;
            for (int i = 0; i < T; ++i)                   (Drag[M[i]] ? D : F).push_back(M[i]);
            for (int i = T + K; i < (int)M.size(); ++i)   (Drag[M[i]] ? U : F).push_back(M[i]);
            // F holds the stay-put nodes in order (first the ones before T, then after); split at the first element that was at/after T
            const int nBefore = T - static_cast<int>(D.size());
            std::vector<int> Flat(F.begin(), F.begin() + nBefore);
            Flat.insert(Flat.end(), U.begin(), U.end());                    // U: they were after the block and must now be before it
            Flat.insert(Flat.end(), Nodes.begin(), Nodes.end());
            Flat.insert(Flat.end(), D.begin(), D.end());
            Flat.insert(Flat.end(), F.begin() + nBefore, F.end());
            for (int n : U) Lab[n] = Sec;
            for (int n : D) Lab[n] = Sec;
            Res.m_Dependents.assign(S.size(), {});
            for (int n : Flat) Res.m_Dependents[Lab[n]].push_back(n);
        }

        // nearest
        {
            const auto W = Locate(R, N);
            int P = -1, C = static_cast<int>(Flatten(R).size());
            for (auto [p, c] : E)
            {
                if (InBlock[c] && !InBlock[p]) P = std::max(P, W.m_Global[p]);
                if (InBlock[p] && !InBlock[c]) C = std::min(C, W.m_Global[c]);
            }
            if (P + 1 > C) { Res.m_Nearest = Res.m_Dependents; return Res; }    // no slot fits the block as one piece: only dragging works
            const int Pos = std::clamp(T, P + 1, C);
            int S2 = Sec, Slot2 = T - BaseOf(Sec);
            if (Pos != T)
            {
                int Best = -1;
                for (int s = 0; s < static_cast<int>(R.size()) && Best < 0; ++s)    // nearest section to the drop whose closed range holds Pos
                    for (int d : { Sec - s, Sec + s })
                        if (d >= 0 && d < static_cast<int>(R.size()) && BaseOf(d) <= Pos && Pos <= BaseOf(d) + static_cast<int>(R[d].size())) { Best = d; break; }
                S2 = Best; Slot2 = Pos - BaseOf(Best);
            }
            Res.m_Nearest = R;
            Res.m_Nearest[S2].insert(Res.m_Nearest[S2].begin() + Slot2, Nodes.begin(), Nodes.end());
        }
        return Res;
    }
}

#ifndef XMATERIAL_STACK_ADAPTER_CORE_ONLY

#include "xmaterial_graph.h"
#include <cctype>
#include <set>

namespace xmaterial_graph::stack_adapter
{
    // Builds graph.m_Stack from a v1 graph (no StackLayout): sections from comment rectangles, order from a stable topological sort seeded
    // by the old left-to-right positions, step names from prefab names. Only organisation changes (sections, order, m_StepName).
    inline void Migrate(graph& G)
    {
        // steps in seed order (m_Pos.x, then m_Pos.y; the map is guid ordered so ties are deterministic)
        std::vector<node*> Steps, Comments;
        for (auto& [g, n] : G.m_InstanceNodes) (n->isCommentNode() ? Comments : Steps).push_back(n.get());
        std::stable_sort(Steps.begin(), Steps.end(), [](const node* a, const node* b){ return a->m_Pos.m_X != b->m_Pos.m_X ? a->m_Pos.m_X < b->m_Pos.m_X : a->m_Pos.m_Y < b->m_Pos.m_Y; });

        std::unordered_map<node_guid, int> Id;
        for (int i = 0; i < (int)Steps.size(); ++i) Id[Steps[i]->m_Guid] = i;

        stack_order::edges E;
        for (auto& [cid, c] : G.m_Connections)
        {
            auto* p = G.findNodeByPin(c->m_OutputPinGuid);
            auto* q = G.findNodeByPin(c->m_InputPinGuid);
            if (p && q && p != q && Id.count(p->m_Guid) && Id.count(q->m_Guid)) E.push_back({ Id[p->m_Guid], Id[q->m_Guid] });
        }

        std::vector<int> Order(Steps.size());
        for (int i = 0; i < (int)Order.size(); ++i) Order[i] = i;
        stack_order::StableTopoSort(Order, E);

        // section hints: a comment's rectangle (name = first line, 40 chars) owns the nodes whose m_Pos is in it; smallest rectangle wins
        struct rect { std::string m_Name; float x, y, w, h; };
        std::vector<rect> Rects;
        for (auto* c : Comments)
        {
            rect R{ {}, c->m_Pos.m_X, c->m_Pos.m_Y, 0, 0 };
            for (auto& P : c->m_Params)
            {
                if      (P.m_Name == "Comment"     && P.m_Type == node_param::type::STRING) R.m_Name = P.m_Value.get<std::string>();
                else if (P.m_Name == "Size width"  && P.m_Type == node_param::type::FLOAT)  R.w      = P.m_Value.get<float>();
                else if (P.m_Name == "Size Height" && P.m_Type == node_param::type::FLOAT)  R.h      = P.m_Value.get<float>();
            }
            R.m_Name = R.m_Name.substr(0, R.m_Name.find_first_of("\r\n")).substr(0, 40);
            if (R.m_Name.empty()) R.m_Name = "Group " + std::to_string(Rects.size() + 1);
            Rects.push_back(std::move(R));
        }

        // group per step: 0..Rects-1 rectangle, then Inputs, Steps, Output. Output nodes always go to Output (the output stack holds them)
        const int nR = (int)Rects.size(), gInputs = nR, gSteps = nR + 1, gOutput = nR + 2;
        std::vector<int> Group(Steps.size());
        for (int i = 0; i < (int)Steps.size(); ++i)
        {
            node& n = *Steps[i];
            int Best = -1; float BestArea = 0;
            for (int r = 0; r < nR; ++r)
            {
                auto& R = Rects[r];
                if (n.m_Pos.m_X < R.x || n.m_Pos.m_X > R.x + R.w || n.m_Pos.m_Y < R.y || n.m_Pos.m_Y > R.y + R.h) continue;
                if (Best < 0 || R.w * R.h < BestArea) { Best = r; BestArea = R.w * R.h; }
            }
            Group[i] = n.isOutputNode() ? gOutput : Best >= 0 ? Best : n.isInputNode() ? gInputs : gSteps;
        }

        // sections ordered by the topological index of their first member, OUTPUT last; concatenate and re-sort
        std::vector<int> First(nR + 3, -1);
        for (int k = 0; k < (int)Order.size(); ++k) if (First[Group[Order[k]]] < 0) First[Group[Order[k]]] = k;
        std::vector<int> GOrder;
        for (int g = 0; g < nR + 2; ++g) if (First[g] >= 0) GOrder.push_back(g);
        std::stable_sort(GOrder.begin(), GOrder.end(), [&](int a, int b){ return First[a] < First[b]; });
        GOrder.push_back(gOutput);

        std::vector<int> Cat;
        for (int g : GOrder) for (int n : Order) if (Group[n] == g) Cat.push_back(n);
        stack_order::StableTopoSort(Cat, E);

        // step names: prefab name as an identifier, unique ignoring case (GLSL names are lowercased later), in step order
        std::set<std::string> Used;
        auto Lower = [](std::string s){ for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
        for (auto* n : Steps) if (!n->m_StepName.empty()) Used.insert(Lower(n->m_StepName));
        for (int i : Cat)
        {
            node& n = *Steps[i];
            if (!n.m_StepName.empty()) continue;
            std::string Base;
            for (char c : n.m_Name) Base += std::isalnum((unsigned char)c) ? c : '_';
            if (Base.empty()) Base = "Step";
            if (std::isdigit((unsigned char)Base[0])) Base.insert(Base.begin(), '_');
            std::string Name = Base;
            for (int k = 2; Used.count(Lower(Name)); ++k) Name = Base + std::to_string(k);
            Used.insert(Lower(Name));
            n.m_StepName = std::move(Name);
        }

        // sections: a group that is no longer contiguous splits into Name, Name (2)...; names stay unique
        G.m_Stack = {};
        std::set<std::string> SecNames;
        for (size_t k = 0; k < Cat.size(); ++k)
        {
            const int g = Group[Cat[k]];
            if (k == 0 || g != Group[Cat[k - 1]])
            {
                const bool bOut = g == gOutput;
                std::string Base = bOut ? "Output" : g == gInputs ? "Inputs" : g == gSteps ? "Steps" : Rects[g].m_Name;
                std::string Name = Base;
                for (int p = 2; SecNames.count(Name); ++p) Name = Base + " (" + std::to_string(p) + ")";
                SecNames.insert(Name);
                section S;
                S.m_Guid = section_guid{ xresource::guid_generator::Type64() };
                S.m_Name = Name;
                S.m_Kind = bOut ? section::kind::OUTPUT : section::kind::USER;
                G.m_Stack.m_Sections.push_back(std::move(S));
            }
            G.m_Stack.m_Sections.back().m_Steps.push_back(Steps[Cat[k]]->m_Guid);
        }

        if (G.m_Stack.m_Sections.empty() || G.m_Stack.m_Sections.back().m_Kind != section::kind::OUTPUT)    // exactly one OUTPUT, always last
        {
            section S;
            S.m_Guid = section_guid{ xresource::guid_generator::Type64() };
            S.m_Name = "Output";
            S.m_Kind = section::kind::OUTPUT;
            G.m_Stack.m_Sections.push_back(std::move(S));
        }
        G.m_Stack.Reindex();
        assert(G.m_Stack.IsOrderValid(G));
    }
}

#endif
#endif
