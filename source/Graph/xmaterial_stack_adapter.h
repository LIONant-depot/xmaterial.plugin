#ifndef XMATERIAL_STACK_ADAPTER_H
#define XMATERIAL_STACK_ADAPTER_H
#pragma once

// The order functions are xgraph's (dependencies/xgraph/source/xgraph_order.h; render graph stage F1); here the v1 -> stack migration
// adapter (stack_canvas.md section 8). Define XMATERIAL_STACK_ADAPTER_CORE_ONLY to get only the order functions (standalone self-check).
#include "dependencies/xgraph/source/xgraph_order.h"

namespace xmaterial_graph { namespace stack_order = xgraph::order; }

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
