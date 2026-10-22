#ifndef XMATERIAL_STACK_LAYOUT_H
#define XMATERIAL_STACK_LAYOUT_H
#pragma once

// Layout of the material stack (documentation/stack_canvas.md section 4): a layered layout by dependency depth. Column = longest path from
// the inputs (steps with no inputs in column 0, the output section in the last column), so every connection flows left to right. A wire
// that skips columns gets a slot (a "dummy") in every column it crosses, so it passes between steps, never behind one. The order in a column
// starts from the reading order (sections, then steps) and is refined by barycentre sweeps (fewest crossings wins); steps then slide up and
// down to the median of what they connect to (least wire, straight wires where possible) without overlapping.
// Pure: ordered sections of step sizes and the edges in, columns, items and y out. No ImGui, no graph, no editor state; the caller measures
// text. X needs the router's gutter widths (xmaterial_stack_routes.h calls PlaceX).
// Sections are reading order only here: they decide ties, never columns.
// A step the person placed (step_in::m_PinColumn) keeps its column (clamped into its free ones) and its top; the layout works around it.

#include <vector>
#include <map>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace xmaterial_editor::stack_layout
{
    namespace geo
    {
        constexpr float HEADER_H        = 26.0f;
        constexpr float ROW_H           = 20.0f;
        constexpr float NODE_PAD_BOTTOM = 8.0f;
        constexpr float NODE_GAP        = 24.0f;    // between two steps of a column
        constexpr float DUMMY_GAP       = 14.0f;    // between a wire passing a column and a step
        constexpr float DUMMY_SEP       = 12.0f;    // between two wires passing a column
        constexpr float TOP             = 10.0f;
        constexpr float PORT_PAD        = 12.0f;
        constexpr float MID_GAP         = 16.0f;    // between the input and the output half of a step
        constexpr float ICON_CLEARANCE  = 14.0f;    // a gutter's first lane from the column edge
        constexpr float LANE_GAP        = 10.0f;
        constexpr float COLUMN_MARGIN   = 56.0f;    // the narrowest gutter
        constexpr float MARGIN          = 20.0f;    // left and right of the whole layout
        constexpr float MIN_STEP_W      = 110.0f;
        constexpr int   ORDER_SWEEPS    = 8;
        constexpr int   PLACE_SWEEPS    = 12;
    }

    // A step box: inputs on the left half, outputs on the right half, row r of each side at RowY(r). TextW = header or the widest
    // input + output pair content (the caller adds PORT_PAD and MID_GAP through StepW).
    inline float StepW (float TextW) noexcept { return std::max(geo::MIN_STEP_W, TextW + 2 * geo::PORT_PAD); }
    inline float StepH (int nRows)   noexcept { return geo::HEADER_H + nRows * geo::ROW_H + geo::NODE_PAD_BOTTOM; }
    inline float RowY  (int Row)     noexcept { return geo::HEADER_H + (Row + 0.5f) * geo::ROW_H; }       // a row's ports, from the step's top

    // A connection: global step indices (order invariant: From before To), the rows of its two ports, the source output it leaves from (a
    // pin id; a sub-element is its own output, so its own trunk). m_Id is the caller's (the connection guid).
    struct edge         { int m_From = 0, m_FromRow = 0, m_To = 0, m_ToRow = 0; std::uint64_t m_Source = 0, m_Id = 0; };
    struct step_in      { std::uint64_t m_Id = 0; float m_W = geo::MIN_STEP_W; int m_nRows = 1; float m_LitW = 0;       // LitW: literal fields left of the step
                          int m_PinColumn = -1; float m_PinY = 0; };                                                    // placed by the person: column (>= 0) and top
    struct section_in   { std::uint64_t m_Id = 0; std::vector<step_in> m_Steps; bool m_bOutput = false; };
    using  input        = std::vector<section_in>;      // in order, OUTPUT last; a step's global index is its position in the concatenation

    struct box  { float m_X = 0, m_Y = 0, m_W = 0, m_H = 0; };
    struct item { int m_Step = -1, m_Source = -1, m_Column = 0; float m_Y = 0; };   // a step, or a dummy: where source m_Source crosses a column
    struct link { int m_L = 0, m_LRow = -1, m_R = 0, m_RRow = -1, m_Source = 0; };  // items of columns c and c+1; rows -1 at a dummy

    struct result
    {
        int                             m_K = 0;
        float                           m_Width = 0, m_Height = 0;
        std::vector<int>                m_Column;       // per global step
        std::vector<box>                m_Steps;        // per global step
        std::vector<float>              m_LitW;         // per global step
        std::vector<char>               m_Fed;          // per global step: some input connected
        std::vector<item>               m_Items;
        std::vector<std::vector<int>>   m_Order;        // per column: items, top to bottom
        std::vector<std::vector<link>>  m_Links;        // per gutter g (between columns g and g+1)
        std::vector<std::vector<int>>   m_Chain;        // per edge: its items column by column (from step, dummies, to step); empty = not drawn
        std::vector<std::uint64_t>      m_Sources;      // distinct source outputs, ascending
        std::vector<int>                m_EdgeSource;   // per edge: index into m_Sources
        std::vector<float>              m_X, m_Band, m_W;   // per column: left edge, literal band, widest step (steps start at m_X + m_Band)
    };

    inline float PortY(const result& R, int Item, int Row) noexcept { const auto& I = R.m_Items[Item]; return I.m_Y + (I.m_Step >= 0 ? RowY(Row) : 0.0f); }
    inline float ItemH(const result& R, int Item, const std::vector<int>& Rows) noexcept { const auto& I = R.m_Items[Item]; return I.m_Step >= 0 ? StepH(Rows[I.m_Step]) : 0.0f; }
    inline float Gap(const result& R, int a, int b) noexcept
    {
        const bool da = R.m_Items[a].m_Step < 0, db = R.m_Items[b].m_Step < 0;
        return da && db ? geo::DUMMY_SEP : da || db ? geo::DUMMY_GAP : geo::NODE_GAP;
    }

    // Crossings between two neighbouring columns: two links cross when their port order flips (links sharing a port never cross)
    inline int Crossings(const result& R, int g, const std::vector<int>& Pos) noexcept
    {
        const auto& L = R.m_Links[g];
        int n = 0;
        for (std::size_t i = 0; i < L.size(); ++i)
            for (std::size_t j = i + 1; j < L.size(); ++j)
            {
                const long a0 = Pos[L[i].m_L] * 256L + L[i].m_LRow, b0 = Pos[L[j].m_L] * 256L + L[j].m_LRow;
                const long a1 = Pos[L[i].m_R] * 256L + L[i].m_RRow, b1 = Pos[L[j].m_R] * 256L + L[j].m_RRow;
                n += (a0 < b0 && a1 > b1) || (a0 > b0 && a1 < b1);
            }
        return n;
    }

    // Columns, dummies, order, y. Deterministic: the same input always gives the same layout, and an unwired new step goes to the bottom
    // of column 0, so it moves nothing else.
    inline result Layer(const input& In, const std::vector<edge>& E) noexcept
    {
        result R;
        std::vector<int> Rows;
        std::vector<char> bOut;
        for (auto& S : In) for (auto& s : S.m_Steps) { Rows.push_back(std::max(1, s.m_nRows)); bOut.push_back(S.m_bOutput); R.m_LitW.push_back(s.m_LitW); }
        const int N = static_cast<int>(Rows.size()), nE = static_cast<int>(E.size());

        for (auto& e : E) R.m_Sources.push_back(e.m_Source);
        std::sort(R.m_Sources.begin(), R.m_Sources.end());
        R.m_Sources.erase(std::unique(R.m_Sources.begin(), R.m_Sources.end()), R.m_Sources.end());
        for (auto& e : E) R.m_EdgeSource.push_back(static_cast<int>(std::lower_bound(R.m_Sources.begin(), R.m_Sources.end(), e.m_Source) - R.m_Sources.begin()));

        // Columns: longest path from the inputs (the order invariant makes every valid edge point forward, so one pass in order); outputs last
        std::vector<char> Valid(nE);
        std::vector<std::vector<int>> Outs(N);
        for (int i = 0; i < nE; ++i)
            if ((Valid[i] = E[i].m_From >= 0 && E[i].m_To < N && E[i].m_From < E[i].m_To)) Outs[E[i].m_From].push_back(i);
        R.m_Column.assign(N, 0);
        R.m_Fed.assign(N, 0);
        for (int i = 0; i < nE; ++i) if (Valid[i]) R.m_Fed[E[i].m_To] = 1;
        int Last = 0;
        for (int v = 0; v < N; ++v)
        {
            if (!bOut[v]) Last = std::max(Last, R.m_Column[v] + 1);
            for (int i : Outs[v]) R.m_Column[E[i].m_To] = std::max(R.m_Column[E[i].m_To], R.m_Column[v] + 1);
        }
        for (int v = 0; v < N; ++v) if (bOut[v]) R.m_Column[v] = Last;

        // A step's free columns: after everything it reads, before everything reading it (the last before the output column when nothing does)
        std::vector<std::vector<int>> Ins(N);
        for (int i = 0; i < nE; ++i) if (Valid[i]) Ins[E[i].m_To].push_back(i);
        auto Lo = [&](int v) { int c = 0; for (int i : Ins[v]) c = std::max(c, R.m_Column[E[i].m_From] + 1); return c; };
        auto Hi = [&](int v) { int c = std::max(0, Last - 1); for (int i : Outs[v]) c = std::min(c, R.m_Column[E[i].m_To] - 1); return std::max(c, Lo(v)); };
        // The person's columns first (clamped into the free ones), then a step that reads nothing goes right before its first consumer: its
        // wires stay short instead of crossing columns (a Vec2 between the two Textures it feeds, not left of them)
        std::vector<float> PinY(N, 0);
        std::vector<char>  bPin(N, 0);
        {
            int v = 0;
            for (auto& S : In) for (auto& s : S.m_Steps) { if (s.m_PinColumn >= 0) { bPin[v] = 1; PinY[v] = s.m_PinY; } ++v; }
            v = 0;
            for (auto& S : In) for (auto& s : S.m_Steps) { if (bPin[v] && !bOut[v]) R.m_Column[v] = std::clamp(s.m_PinColumn, Lo(v), Hi(v)); ++v; }
        }
        for (int v = 0; v < N; ++v) if (!bPin[v] && !bOut[v] && Ins[v].empty() && !Outs[v].empty()) R.m_Column[v] = Hi(v);
        R.m_K =N == 0 ? 0 : *std::max_element(R.m_Column.begin(), R.m_Column.end()) + 1;

        // Items: every step, and one dummy per (source, column crossed): all wires of one output share it. Links between neighbours.
        for (int v = 0; v < N; ++v) R.m_Items.push_back({ v, -1, R.m_Column[v] });
        std::map<std::pair<int,int>, int> Dummy;                    // (source, column) -> item
        R.m_Links.assign(std::max(0, R.m_K - 1), {});
        R.m_Chain.assign(nE, {});
        for (int i = 0; i < nE; ++i)
        {
            if (!Valid[i]) continue;
            const auto& e = E[i];
            const int s = R.m_EdgeSource[i];
            auto& Ch = R.m_Chain[i];
            Ch.push_back(e.m_From);
            for (int c = R.m_Column[e.m_From] + 1; c < R.m_Column[e.m_To]; ++c)
            {
                auto [It, bNew] = Dummy.try_emplace({ s, c }, static_cast<int>(R.m_Items.size()));
                if (bNew) R.m_Items.push_back({ -1, s, c });
                Ch.push_back(It->second);
            }
            Ch.push_back(e.m_To);
            for (std::size_t k = 0; k + 1 < Ch.size(); ++k)
            {
                const link l{ Ch[k], k == 0 ? e.m_FromRow : -1, Ch[k + 1], k + 2 == Ch.size() ? e.m_ToRow : -1, s };
                auto& G = R.m_Links[R.m_Items[Ch[k]].m_Column];
                if (std::none_of(G.begin(), G.end(), [&](const link& o) { return o.m_L == l.m_L && o.m_LRow == l.m_LRow && o.m_R == l.m_R && o.m_RRow == l.m_RRow; })) G.push_back(l);
            }
        }
        const int nI = static_cast<int>(R.m_Items.size());

        // Order: reading order (a dummy right after its source step), unwired steps last; then barycentre sweeps, keeping the best order
        std::vector<char> Wired(nI, 0);
        for (auto& G : R.m_Links) for (auto& l : G) Wired[l.m_L] = Wired[l.m_R] = 1;
        std::vector<int> SrcStep(R.m_Sources.size(), 0);
        for (int i = 0; i < nE; ++i) SrcStep[R.m_EdgeSource[i]] = E[i].m_From;
        auto Reading = [&](int it) { const auto& I = R.m_Items[it]; return I.m_Step >= 0 ? (Wired[it] ? I.m_Step : N + I.m_Step) : SrcStep[I.m_Source] + 0.5; };
        R.m_Order.assign(R.m_K, {});
        for (int it = 0; it < nI; ++it) R.m_Order[R.m_Items[it].m_Column].push_back(it);
        for (auto& O : R.m_Order) std::stable_sort(O.begin(), O.end(), [&](int a, int b) { return Reading(a) < Reading(b); });

        std::vector<int> Pos(nI);
        auto Index = [&] { for (auto& O : R.m_Order) for (int k = 0; k < static_cast<int>(O.size()); ++k) Pos[O[k]] = k; };
        auto Total = [&] { int n = 0; for (int g = 0; g + 1 < R.m_K; ++g) n += Crossings(R, g, Pos); return n; };
        // Sort column c by the mean position of its neighbours in column c-1 (bLeft) or c+1; ports split a step's position by row. An item
        // with no neighbour there keeps its place; unwired steps stay last.
        auto Sweep = [&](int c, bool bLeft)
        {
            std::vector<double> Sum(nI, 0), Cnt(nI, 0);
            if (bLeft ? c > 0 : c + 1 < R.m_K)
                for (auto& l : R.m_Links[bLeft ? c - 1 : c])
                {
                    const int Me = bLeft ? l.m_R : l.m_L, Other = bLeft ? l.m_L : l.m_R, Row = bLeft ? l.m_LRow : l.m_RRow;
                    Sum[Me] += Pos[Other] + 0.02 * (Row + 1); Cnt[Me] += 1;
                }
            auto& O = R.m_Order[c];
            std::vector<double> Key(nI);
            for (int it : O) Key[it] = !Wired[it] ? 1e9 + Pos[it] : Cnt[it] ? Sum[it] / Cnt[it] : Pos[it];
            std::stable_sort(O.begin(), O.end(), [&](int a, int b) { return Key[a] < Key[b]; });
            for (int k = 0; k < static_cast<int>(O.size()); ++k) Pos[O[k]] = k;
        };
        Index();
        for (int c = 1; c < R.m_K; ++c) Sweep(c, true);                // dummies and steps find their first place from the left
        auto Best = R.m_Order;
        int BestN = Total();
        for (int p = 0; p < geo::ORDER_SWEEPS && BestN > 0; ++p)
        {
            if (p % 2 == 0) for (int c = R.m_K - 2; c >= 0; --c) Sweep(c, false);
            else            for (int c = 1; c < R.m_K; ++c)      Sweep(c, true);
            if (const int n = Total(); n < BestN) { BestN = n; Best = R.m_Order; }
        }
        R.m_Order = std::move(Best);
        Index();
        // Transpose: swap two neighbours of a column while that lowers the crossings of the column's two gutters (unwired steps stay last)
        // ponytail: recounts both gutters per try, O(items * links^2) a pass: ~8 ms at 200 steps (release); count only the pair's links if it shows
        auto Local = [&](int c) { return (c > 0 ? Crossings(R, c - 1, Pos) : 0) + (c + 1 < R.m_K ? Crossings(R, c, Pos) : 0); };
        for (bool bMore = BestN > 0; bMore; )
        {
            bMore = false;
            for (int c = 0; c < R.m_K; ++c)
                for (int k = 0; k + 1 < static_cast<int>(R.m_Order[c].size()); ++k)
                {
                    auto& O = R.m_Order[c];
                    if (!Wired[O[k]] || !Wired[O[k + 1]]) continue;
                    const int Before = Local(c);
                    auto Swap = [&] { std::swap(O[k], O[k + 1]); Pos[O[k]] = k; Pos[O[k + 1]] = k + 1; };
                    Swap();
                    if (Local(c) < Before) bMore = true; else Swap();
                }
        }

        // Y: per column, the least total |wire dy| (each item's wanted tops are its neighbours' ports minus its own) with items in order and
        // no overlap: isotonic L1 regression by pooling adjacent violators. Any value between a pool's lower and upper median is optimal; it
        // takes the one of the two nearer to where it is (a want met exactly is a straight wire; staying put keeps sweeps from drifting).
        // ponytail: Gauss-Seidel sweeps find a good local optimum, not the global one (Brandes-Koepf alignment if layouts look ragged)
        std::vector<std::vector<std::pair<int,int>>> Touch(nI);        // item -> (gutter, link index)
        for (int g = 0; g + 1 < R.m_K; ++g) for (int k = 0; k < static_cast<int>(R.m_Links[g].size()); ++k) { Touch[R.m_Links[g][k].m_L].push_back({ g, k }); Touch[R.m_Links[g][k].m_R].push_back({ g, k }); }
        auto Offsets = [&](int c)
        {
            const auto& O = R.m_Order[c];
            std::vector<float> Off(O.size(), 0);
            for (std::size_t k = 1; k < O.size(); ++k) Off[k] = Off[k - 1] + ItemH(R, O[k - 1], Rows) + Gap(R, O[k - 1], O[k]);
            return Off;
        };
        for (int c = 0; c < R.m_K; ++c) { const auto Off = Offsets(c); for (std::size_t k = 0; k < Off.size(); ++k) R.m_Items[R.m_Order[c][k]].m_Y = Off[k]; }
        auto PlaceColumn = [&](int c)
        {
            const auto& O = R.m_Order[c];
            const auto Off = Offsets(c);
            struct pool { std::vector<float> m_V; float m_M = 0, m_Cur = 0; int m_First = 0, m_N = 0; };
            std::vector<pool> P;
            for (int k = 0; k < static_cast<int>(O.size()); ++k)
            {
                pool p; p.m_First = k; p.m_N = 1; p.m_Cur = R.m_Items[O[k]].m_Y - Off[k];
                for (auto [g, li] : Touch[O[k]])
                {
                    const auto& l = R.m_Links[g][li];
                    const bool bL = l.m_L == O[k];
                    p.m_V.push_back(PortY(R, bL ? l.m_R : l.m_L, bL ? l.m_RRow : l.m_LRow) - (PortY(R, O[k], bL ? l.m_LRow : l.m_RRow) - R.m_Items[O[k]].m_Y) - Off[k]);
                }
                auto Median = [&](pool& q)
                {
                    if (q.m_V.empty()) return;
                    std::sort(q.m_V.begin(), q.m_V.end());
                    const float Lo = q.m_V[(q.m_V.size() - 1) / 2], Hi = q.m_V[q.m_V.size() / 2], Cur = q.m_Cur / q.m_N;
                    q.m_M = std::abs(Cur - Lo) <= std::abs(Cur - Hi) ? Lo : Hi;
                };
                Median(p);
                if (p.m_V.empty()) p.m_M = P.empty() ? R.m_Items[O[k]].m_Y - Off[k] : P.back().m_M;      // nothing wanted: follow the one above
                P.push_back(std::move(p));
                while (P.size() > 1 && P[P.size() - 2].m_M > P.back().m_M)
                {
                    auto q = std::move(P.back()); P.pop_back();
                    auto& t = P.back();
                    t.m_V.insert(t.m_V.end(), q.m_V.begin(), q.m_V.end());
                    t.m_Cur += q.m_Cur; t.m_N += q.m_N;
                    if (t.m_V.empty()) t.m_M = q.m_M; else Median(t);
                }
            }
            for (std::size_t p = 0; p < P.size(); ++p)
                for (int k = P[p].m_First; k < (p + 1 < P.size() ? P[p + 1].m_First : static_cast<int>(O.size())); ++k) R.m_Items[O[k]].m_Y = P[p].m_M + Off[k];
        };
        for (int p = 0; p < geo::PLACE_SWEEPS; ++p)
            for (int k = 0; k < R.m_K; ++k) PlaceColumn(p % 2 ? R.m_K - 1 - k : k);

        // Straighten: L1 has many equal optima, so an item moves to one of its wants when that fits between its neighbours and adds no wire
        // length but makes one more wire straight
        for (int p = 0; p < 3; ++p)
            for (int c = 0; c < R.m_K; ++c)
            {
                const auto& O = R.m_Order[c];
                for (int k = 0; k < static_cast<int>(O.size()); ++k)
                {
                    auto& I = R.m_Items[O[k]];
                    std::vector<float> W;
                    for (auto [g, li] : Touch[O[k]])
                    {
                        const auto& l = R.m_Links[g][li];
                        const bool bL = l.m_L == O[k];
                        W.push_back(PortY(R, bL ? l.m_R : l.m_L, bL ? l.m_RRow : l.m_LRow) - (PortY(R, O[k], bL ? l.m_LRow : l.m_RRow) - I.m_Y));
                    }
                    const float Lo = k > 0 ? R.m_Items[O[k - 1]].m_Y + ItemH(R, O[k - 1], Rows) + Gap(R, O[k - 1], O[k]) : -1e30f;
                    const float Hi = k + 1 < static_cast<int>(O.size()) ? R.m_Items[O[k + 1]].m_Y - ItemH(R, O[k], Rows) - Gap(R, O[k], O[k + 1]) : 1e30f;
                    auto Cost = [&](float y) { float n = 0; for (float w : W) n += std::abs(y - w); return n; };
                    auto Met  = [&](float y) { return std::count(W.begin(), W.end(), y); };
                    float Best = I.m_Y;
                    for (float w : W)
                        if (w >= Lo && w <= Hi && (Cost(w) < Cost(Best) - 0.01f || (Cost(w) <= Cost(Best) + 0.01f && Met(w) > Met(Best)))) Best = w;
                    I.m_Y = Best;
                }
            }

        // Shift everything so the top item sits at TOP (only wired items count: an unwired step at a column's bottom never moves the rest)
        float Min = 1e30f;
        for (int it = 0; it < nI; ++it) if (Wired[it]) Min = std::min(Min, R.m_Items[it].m_Y);
        if (Min == 1e30f) Min = 0;
        for (auto& I : R.m_Items) I.m_Y += geo::TOP - Min;

        // The person's places: a placed step sits exactly where it was put (overlapping, if put there); the rest of its column keeps its
        // order and moves down out of its way. The other columns do not follow it: moving one step moves nothing else that it does not hit.
        for (int c = 0; c < R.m_K; ++c)
        {
            auto& O = R.m_Order[c];
            std::vector<int> Pins;
            for (int it : O) if (const int s = R.m_Items[it].m_Step; s >= 0 && bPin[s]) { R.m_Items[it].m_Y = PinY[s]; Pins.push_back(it); }
            if (Pins.empty()) continue;
            int Prev = -1;
            for (int it : O)
            {
                if (std::find(Pins.begin(), Pins.end(), it) != Pins.end()) continue;
                float y = R.m_Items[it].m_Y;
                if (Prev >= 0) y = std::max(y, R.m_Items[Prev].m_Y + ItemH(R, Prev, Rows) + Gap(R, Prev, it));
                for (bool bHit = true; bHit; )
                {
                    bHit = false;
                    for (int p : Pins)
                        if (const float Top = R.m_Items[p].m_Y; y < Top + ItemH(R, p, Rows) + Gap(R, p, it) && y + ItemH(R, it, Rows) + Gap(R, it, p) > Top)
                            { y = Top + ItemH(R, p, Rows) + Gap(R, p, it); bHit = true; }
                }
                R.m_Items[it].m_Y = y;
                Prev = it;
            }
            std::stable_sort(O.begin(), O.end(), [&](int a, int b) { return R.m_Items[a].m_Y < R.m_Items[b].m_Y; });
        }
        R.m_Steps.resize(N);
        for (int v = 0; v < N; ++v) R.m_Steps[v] = { 0, R.m_Items[v].m_Y, 0, StepH(Rows[v]) };
        int v = 0;
        for (auto& S : In) for (auto& s : S.m_Steps) R.m_Steps[v++].m_W = s.m_W;
        for (auto& b : R.m_Steps) R.m_Height = std::max(R.m_Height, b.m_Y + b.m_H + geo::TOP);
        for (auto& I : R.m_Items) R.m_Height = std::max(R.m_Height, I.m_Y + geo::TOP);
        return R;
    }

    // X: columns left to right, each a literal band then its steps, then the gutter to the next (GutterW[g], from the router). A step with
    // a connected input starts at the band (inputs line up); one without (an input, a constant, a sampler) ends at the column's right edge,
    // so its wires leave from there
    inline void PlaceX(result& R, const std::vector<float>& GutterW) noexcept
    {
        R.m_X.assign(R.m_K, 0); R.m_Band.assign(R.m_K, 0); R.m_W.assign(R.m_K, 0);
        for (int v = 0; v < static_cast<int>(R.m_Steps.size()); ++v)
        {
            R.m_Band[R.m_Column[v]] = std::max(R.m_Band[R.m_Column[v]], R.m_LitW[v]);
            R.m_W[R.m_Column[v]]    = std::max(R.m_W[R.m_Column[v]], R.m_Steps[v].m_W);
        }
        float x = geo::MARGIN;
        for (int c = 0; c < R.m_K; ++c)
        {
            R.m_X[c] = x;
            x += R.m_Band[c] + R.m_W[c] + (c + 1 < R.m_K ? GutterW[c] : 0);
        }
        R.m_Width = x + geo::MARGIN;
        for (int v = 0; v < static_cast<int>(R.m_Steps.size()); ++v)
        {
            const int c = R.m_Column[v];
            R.m_Steps[v].m_X = R.m_X[c] + R.m_Band[c] + (R.m_Fed[v] ? 0.0f : R.m_W[c] - R.m_Steps[v].m_W);
        }
    }

    inline float ColumnRight(const result& R, int c) noexcept { return R.m_X[c] + R.m_Band[c] + R.m_W[c]; }

    // Moves an item Dy down, else Dy up, where that fits between its neighbours in its column; else Dy down, pushing the items below it
    inline void Nudge(result& R, int Item, float Dy) noexcept
    {
        auto H = [&](int it) { return R.m_Items[it].m_Step >= 0 ? R.m_Steps[R.m_Items[it].m_Step].m_H : 0.0f; };
        auto Set = [&](int it, float y)
        {
            auto& I = R.m_Items[it];
            I.m_Y = y;
            if (I.m_Step >= 0) R.m_Steps[I.m_Step].m_Y = y;
            R.m_Height = std::max(R.m_Height, y + H(it) + geo::TOP);
        };
        const auto& O = R.m_Order[R.m_Items[Item].m_Column];
        const int k = static_cast<int>(std::find(O.begin(), O.end(), Item) - O.begin());
        const float y = R.m_Items[Item].m_Y;
        const float Lo = k > 0 ? R.m_Items[O[k - 1]].m_Y + H(O[k - 1]) + Gap(R, O[k - 1], Item) : -1e30f;
        const float Hi = k + 1 < static_cast<int>(O.size()) ? R.m_Items[O[k + 1]].m_Y - H(Item) - Gap(R, Item, O[k + 1]) : 1e30f;
        if (y + Dy <= Hi)      { Set(Item, y + Dy); return; }
        if (y - Dy >= Lo)      { Set(Item, y - Dy); return; }
        Set(Item, y + Dy);
        for (int j = k + 1; j < static_cast<int>(O.size()); ++j)
            Set(O[j], std::max(R.m_Items[O[j]].m_Y, R.m_Items[O[j - 1]].m_Y + H(O[j - 1]) + Gap(R, O[j - 1], O[j])));
    }
}

#endif
