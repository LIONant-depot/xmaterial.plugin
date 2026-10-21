#ifndef XMATERIAL_STACK_ROUTES_H
#define XMATERIAL_STACK_ROUTES_H
#pragma once

// Highway routing of the material stack (documentation/stack_canvas.md section 5). Every connection becomes an orthogonal route: a stub from
// the producer's right edge into a highway (vertical lanes in the gutter beside a column), channels (horizontal lanes in the gaps between steps)
// to cross the columns in between, a highway down or up to the consumer's row, a stub into its pin. Nothing ever crosses a step.
// One trunk per source output per lane container; short hops take the lanes nearest the column, long spans go outer; crossings are minimised,
// then drawn at right angles with a gap in the horizontal; junction dots only where one output splits.
// Lanes are derived from the order (a load is a deterministic full pack, nothing is saved) and kept stable while editing by lane memory.
//
// The core is pure: sections of step sizes and edges in, boxes and polylines out (no ImGui, no graph, no editor state). Define
// XMATERIAL_STACK_ROUTES_CORE_ONLY for the standalone self-check; otherwise the graph glue (graph -> input) follows, and the view commands
// (LayoutStats, RouteInfo, RepackRoutes, StackSelfTest) when xmaterial_stack_commands.h was included first.
//
// Deviation from 4.1: channels are chosen and packed on discrete keys before pass C (as specified), but highway lanes are packed on pass C's
// Y. A gutter holds runs between two columns whose keys are not comparable (channel growth shifts each column differently), so only the final
// Y tells whether two trunks overlap. Stability comes from the lane memory instead.

#include "xmaterial_stack_layout.h"
#include <map>
#include <string>
#include <format>
#include <random>
#include <numeric>
#include <chrono>
#include <cassert>

namespace xmaterial_editor::stack_routes
{
    namespace sl = stack_layout;

    namespace geo
    {
        constexpr float CLEARANCE         = 6.0f;   // two trunks share a lane only when their runs are at least this far apart
        constexpr float MIN_SEP           = 6.0f;   // two outputs' horizontals in one gutter at least this far apart (else the right column moves down)
        constexpr float OFFSET_STEP       = 2.0f;   // tried column offsets: 0, 2, 4 ... (OFFSET_TRIES of them)
        constexpr int   OFFSET_TRIES      = 100;
        constexpr float CROSS_GAP         = 4.0f;   // a crossing breaks the horizontal this much on each side
        constexpr int   MAX_LANES         = 6;      // a highway past this is fully repacked (5.5 step 4)
        constexpr int   MAX_CHANNEL_LANES = 4;      // a channel past this is fully repacked
    }

    // A connection: global step indices (order invariant: From before To), the rows of its two ports (sl::RowY), the source output it leaves
    // from (a pin id; a sub-element is its own output, so its own trunk). m_Id is the caller's (the connection guid).
    struct edge { int m_From = 0, m_FromRow = 0, m_To = 0, m_ToRow = 0; std::uint64_t m_Source = 0, m_Id = 0; };

    // A trunk: all branches of one source output in one lane container (5.1)
    struct trunk_key
    {
        std::uint8_t    m_Kind   = 0;       // 0 highway, 1 channel
        std::int32_t    m_Column = 0;       // highway: the gutter (right of this column); channel: the column it crosses
        std::uint64_t   m_Where  = 0;       // highway: 1 = H(gutter,'R'), 0 = H(gutter+1,'L'); channel: id of the step below the gap (~0 = below the last), so inserts above keep it
        std::uint64_t   m_Source = 0;
        auto operator<=>(const trunk_key&) const = default;
    };
    struct lane_memo { int m_Lane = 0; float m_Lo = 0, m_Hi = 0; bool operator==(const lane_memo&) const = default; };

    struct view_memory      // view state (5.5, 4.3, 4.4): never saved; a new one per load, so a load is a full pack
    {
        sl::memory                      m_Layout;
        std::map<trunk_key, lane_memo>  m_Lanes;        // never pruned within a session: an expanded section finds its trunks' lanes again
        std::vector<float>              m_Offsets;      // per column (see Route)
    };

    // A vertical run in one gutter, from Ya (a row or channel of the left column) to Yb (of the right column; same-column routes: back to the left)
    struct hop      { int m_Gutter = 0, m_Side = 1; float m_Ya = 0, m_Yb = 0; bool m_bSame = false; int m_Lane = 0, m_Trunk = -1; };
    struct point    { float m_X = 0, m_Y = 0; };
    struct mark     { float m_X = 0, m_Y = 0; int m_Source = 0; };
    struct segment  { float m_X0 = 0, m_Y0 = 0, m_X1 = 0, m_Y1 = 0; int m_Source = 0; };     // axis aligned, X0 <= X1, Y0 <= Y1; source indexes result::m_Sources

    struct route
    {
        std::vector<std::pair<int,int>> m_Channels;     // (column, gap) per column crossed
        std::vector<int>                m_ChannelLane;
        std::vector<hop>                m_Hops;
        std::vector<point>              m_Points;       // producer pin to consumer pin; empty = not drawn (the edge breaks the order)
    };

    struct result
    {
        sl::result                      m_Layout;
        std::vector<route>              m_Routes;       // parallel to the edges
        std::vector<std::uint64_t>      m_Sources;      // distinct source outputs, ascending
        std::vector<std::int64_t>       m_SourcePin;    // per source: the pin it leaves (step, row); an output and its sub-elements share it
        std::vector<int>                m_RouteSource;  // per edge: index into m_Sources
        std::vector<segment>            m_Wires;        // every route merged per source, horizontals broken at crossings: what to draw
        std::vector<mark>               m_Junctions;    // dots
        std::vector<mark>               m_Crossings;    // the source is the one whose horizontal is broken
        std::vector<int>                m_LanesL, m_LanesR;     // per column: lanes of H(c,'L'), H(c,'R')
        std::vector<std::vector<int>>   m_ChannelLanes;         // per column, per gap
        std::vector<float>              m_Offsets;              // per column
        int                             m_StepHits = 0, m_Touches = 0, m_Overlaps = 0, m_SelfCrossings = 0, m_Skipped = 0;   // must all be 0
        std::map<trunk_key, lane_memo>  m_Memo;                 // the lanes used; Build commits them to the view memory
    };

    //------------------------------------------------------------------------------------------------------------------------------------
    // Lane packing
    //------------------------------------------------------------------------------------------------------------------------------------
    struct trunk { trunk_key m_Key; float m_Lo = 0, m_Hi = 0; int m_Lane = -1; };

    inline int LaneCount(const std::vector<trunk>& T) noexcept { int n = 0; for (auto& t : T) n = std::max(n, t.m_Lane + 1); return n; }

    inline bool Fits(const std::vector<trunk>& T, int t, int Lane) noexcept
    {
        for (int u = 0; u < static_cast<int>(T.size()); ++u)
            if (u != t && T[u].m_Lane == Lane && T[t].m_Lo <= T[u].m_Hi + geo::CLEARANCE && T[u].m_Lo <= T[t].m_Hi + geo::CLEARANCE) return false;
        return true;
    }

    inline void FirstFit(std::vector<trunk>& T, const std::vector<int>& Order) noexcept
    {
        for (int t : Order) for (int L = 0; ; ++L) if (Fits(T, t, L)) { T[t].m_Lane = L; break; }
    }

    inline void Compact(std::vector<trunk>& T) noexcept       // drop empty lanes, keeping the order of the rest
    {
        std::vector<int> Map(LaneCount(T), -1);
        for (auto& t : T) Map[t.m_Lane] = 0;
        for (int L = 0, k = 0; L < static_cast<int>(Map.size()); ++L) if (Map[L] == 0) Map[L] = k++;
        for (auto& t : T) t.m_Lane = Map[t.m_Lane];
    }

    // The lanes of one container (5.3, 5.5). Full pack: short spans first, then start, then source (deterministic), greedy first fit (E27
    // NodeOS_UI_Canvas.h:451-470). With memory: trunks whose run did not change keep their lane first, moved ones keep it if it still fits,
    // the rest take the first free lane, short first; past Budget lanes it falls back to a full pack if that is narrower. Returns true when
    // it full-packed.
    inline bool Pack(std::vector<trunk>& T, const std::map<trunk_key, lane_memo>& Memo, int Budget) noexcept
    {
        auto Short = [&](int a, int b) { return std::tuple(T[a].m_Hi - T[a].m_Lo, T[a].m_Lo, T[a].m_Key.m_Source) < std::tuple(T[b].m_Hi - T[b].m_Lo, T[b].m_Lo, T[b].m_Key.m_Source); };
        std::vector<int> Same, Moved, New;
        for (int t = 0; t < static_cast<int>(T.size()); ++t)
        {
            T[t].m_Lane = -1;
            auto It = Memo.find(T[t].m_Key);
            if (It == Memo.end()) New.push_back(t);
            else (It->second.m_Lo == T[t].m_Lo && It->second.m_Hi == T[t].m_Hi ? Same : Moved).push_back(t);
        }
        if (!Same.empty() || !Moved.empty())
        {
            auto Prev = [&](int a, int b)
            {
                const auto& A = Memo.at(T[a].m_Key); const auto& B = Memo.at(T[b].m_Key);
                return std::tuple(A.m_Lane, A.m_Lo, T[a].m_Key.m_Source) < std::tuple(B.m_Lane, B.m_Lo, T[b].m_Key.m_Source);
            };
            std::sort(Same.begin(), Same.end(), Prev);
            std::sort(Moved.begin(), Moved.end(), Prev);
            for (auto* pList : { &Same, &Moved })
                for (int t : *pList)
                    if (const int L = Memo.at(T[t].m_Key).m_Lane; Fits(T, t, L)) T[t].m_Lane = L;
                    else New.push_back(t);
            std::sort(New.begin(), New.end(), Short);
            FirstFit(T, New);
            if (LaneCount(T) <= Budget) return false;
        }
        auto Kept = T;
        std::vector<int> All(T.size());
        std::iota(All.begin(), All.end(), 0);
        for (auto& t : T) t.m_Lane = -1;
        std::sort(All.begin(), All.end(), Short);
        FirstFit(T, All);
        if ((!Same.empty() || !Moved.empty()) && LaneCount(T) >= LaneCount(Kept)) { T = std::move(Kept); return false; }   // a full pack would not be narrower: keep the lanes
        return true;
    }

    //------------------------------------------------------------------------------------------------------------------------------------
    // Segments: merging, crossings, junctions
    //------------------------------------------------------------------------------------------------------------------------------------
    inline segment Seg(float x0, float y0, float x1, float y1, int Source) noexcept { return { std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1), Source }; }
    inline bool    IsH(const segment& s) noexcept { return s.m_Y0 == s.m_Y1; }           // zero-length segments never survive Merge

    // A hop as three segments: in from the left at Ya, the vertical, out at Yb (to the right; a same-column hop goes back to the left)
    inline void HopSegments(const hop& H, int Source, float x, float xLeft, float xRight, std::vector<segment>& Out) noexcept
    {
        Out.push_back(Seg(xLeft, H.m_Ya, x, H.m_Ya, Source));
        Out.push_back(Seg(x, H.m_Ya, x, H.m_Yb, Source));
        Out.push_back(H.m_bSame ? Seg(xLeft, H.m_Yb, x, H.m_Yb, Source) : Seg(x, H.m_Yb, xRight, H.m_Yb, Source));
    }

    // Union of the collinear overlapping or touching runs of each source: one wire per output, so a shared run is drawn and counted once
    inline std::vector<segment> Merge(std::vector<segment> S) noexcept
    {
        std::erase_if(S, [](const segment& s) { return s.m_X0 == s.m_X1 && s.m_Y0 == s.m_Y1; });
        auto Key = [](const segment& s) { const bool h = IsH(s); return std::tuple(s.m_Source, h, h ? s.m_Y0 : s.m_X0, h ? s.m_X0 : s.m_Y0, h ? s.m_X1 : s.m_Y1); };
        std::sort(S.begin(), S.end(), [&](const segment& a, const segment& b) { return Key(a) < Key(b); });
        std::vector<segment> Out;
        for (auto& s : S)
        {
            if (!Out.empty())
            {
                auto& p = Out.back();
                if (p.m_Source == s.m_Source && IsH(p) == IsH(s))
                {
                    if ( IsH(s) && p.m_Y0 == s.m_Y0 && s.m_X0 <= p.m_X1) { p.m_X1 = std::max(p.m_X1, s.m_X1); continue; }
                    if (!IsH(s) && p.m_X0 == s.m_X0 && s.m_Y0 <= p.m_Y1) { p.m_Y1 = std::max(p.m_Y1, s.m_Y1); continue; }
                }
            }
            Out.push_back(s);
        }
        return Out;
    }

    struct diag { int m_Crossings = 0, m_Touches = 0, m_Overlaps = 0, m_Self = 0; };

    // Crossing: a horizontal and a vertical of two outputs cross strictly inside both (drawn with a gap). Touch: one ends on the other (it
    // would read as a junction). Overlap: two outputs run collinear, or horizontals closer than MIN_SEP. Self: an output crossing itself.
    // Outputs leaving the same pin (Pin[source]) may touch and overlap: a pin's sub-elements share its first run.
    inline diag Classify(const std::vector<segment>& S, const std::vector<std::int64_t>& Pin, std::vector<mark>* pCross = nullptr, std::vector<int>* pCrossSeg = nullptr) noexcept
    {
        diag D;
        for (std::size_t i = 0; i < S.size(); ++i)
            for (std::size_t j = 0; j < S.size(); ++j)
            {
                const auto& a = S[i];
                const auto& b = S[j];
                if (i == j) continue;
                const bool bSame = a.m_Source == b.m_Source, bPin = Pin[a.m_Source] == Pin[b.m_Source];
                if (IsH(a) && !IsH(b))
                {
                    if (b.m_X0 < a.m_X0 || b.m_X0 > a.m_X1 || a.m_Y0 < b.m_Y0 || a.m_Y0 > b.m_Y1) continue;
                    const bool bStrict = b.m_X0 > a.m_X0 && b.m_X0 < a.m_X1 && a.m_Y0 > b.m_Y0 && a.m_Y0 < b.m_Y1;
                    if (bSame)        D.m_Self += bStrict;
                    else if (bPin)    continue;                                         // sub-elements of one pin run together from it: no gap between them
                    else if (bStrict) { ++D.m_Crossings; if (pCross) { pCross->push_back({ b.m_X0, a.m_Y0, a.m_Source }); pCrossSeg->push_back(static_cast<int>(i)); } }
                    else              ++D.m_Touches;
                }
                else if (j > i && IsH(a) == IsH(b) && !bPin)
                {
                    if (IsH(a)) D.m_Overlaps += std::abs(a.m_Y0 - b.m_Y0) < geo::MIN_SEP - 0.01f && a.m_X0 < b.m_X1 && b.m_X0 < a.m_X1;
                    else        D.m_Overlaps += a.m_X0 == b.m_X0 && a.m_Y0 <= b.m_Y1 && b.m_Y0 <= a.m_Y1;
                }
            }
        return D;
    }

    // A dot wherever a run of an output ends inside another run of the same output: the branch leaves a trunk that continues (5.4). Corners
    // (a trunk's ends) get none, and different outputs never share a dot.
    inline std::vector<mark> Junctions(const std::vector<segment>& S) noexcept
    {
        auto Inside = [](const segment& s, float x, float y) { return IsH(s) ? y == s.m_Y0 && x > s.m_X0 && x < s.m_X1 : x == s.m_X0 && y > s.m_Y0 && y < s.m_Y1; };
        std::vector<mark> Out;
        for (auto& a : S)
            for (auto& b : S)
                if (&a != &b && a.m_Source == b.m_Source)
                    for (auto [x, y] : { std::pair{ a.m_X0, a.m_Y0 }, std::pair{ a.m_X1, a.m_Y1 } })
                        if (Inside(b, x, y) && std::none_of(Out.begin(), Out.end(), [&](const mark& m) { return m.m_X == x && m.m_Y == y && m.m_Source == a.m_Source; }))
                            Out.push_back({ x, y, a.m_Source });
        return Out;
    }

    inline bool OnPolyline(const std::vector<point>& P, float x, float y) noexcept
    {
        for (std::size_t i = 0; i + 1 < P.size(); ++i)
            if (const auto s = Seg(P[i].m_X, P[i].m_Y, P[i + 1].m_X, P[i + 1].m_Y, 0); x >= s.m_X0 && x <= s.m_X1 && y >= s.m_Y0 && y <= s.m_Y1) return true;
        return false;
    }

    //------------------------------------------------------------------------------------------------------------------------------------
    // Pass D (routes) inside B..F for one column count
    //------------------------------------------------------------------------------------------------------------------------------------
    inline result Route(const sl::input& In, const std::vector<edge>& E, int K, const std::vector<int>& Col, const view_memory& VM) noexcept
    {
        result R;
        const int nE = static_cast<int>(E.size());
        R.m_Routes.resize(nE);
        R.m_RouteSource.resize(nE);

        std::vector<std::uint64_t> StepId;
        for (auto& S : In) for (auto& s : S.m_Steps) StepId.push_back(s.m_Id);

        for (auto& e : E) R.m_Sources.push_back(e.m_Source);
        std::sort(R.m_Sources.begin(), R.m_Sources.end());
        R.m_Sources.erase(std::unique(R.m_Sources.begin(), R.m_Sources.end()), R.m_Sources.end());
        R.m_SourcePin.resize(R.m_Sources.size());
        for (int i = 0; i < nE; ++i)
        {
            R.m_RouteSource[i] = static_cast<int>(std::lower_bound(R.m_Sources.begin(), R.m_Sources.end(), E[i].m_Source) - R.m_Sources.begin());
            R.m_SourcePin[R.m_RouteSource[i]] = static_cast<std::int64_t>(E[i].m_From) << 20 | E[i].m_FromRow;
        }

        const auto Base = sl::PassC(In, Col, K);
        auto RowAt = [](const sl::result& L, int Step, int Row) { return L.m_Steps[Step].m_Y + sl::RowY(Row); };

        // Order invariant: the consumer is lower in the producer's column, or in a later column (5.2). Anything else is not drawn.
        std::vector<char> Valid(nE, 0);
        for (int i = 0; i < nE; ++i)
        {
            const auto& e = E[i];
            const bool bIn = e.m_From >= 0 && e.m_To >= 0 && e.m_From < static_cast<int>(StepId.size()) && e.m_To < static_cast<int>(StepId.size());
            Valid[i] = bIn && (Base.m_Column[e.m_From] < Base.m_Column[e.m_To] || (Base.m_Column[e.m_From] == Base.m_Column[e.m_To] && Base.m_Pos[e.m_From] < Base.m_Pos[e.m_To]));
            R.m_Skipped += !Valid[i];
        }

        auto AddTrunk = [](std::vector<trunk>& T, const trunk_key& Key, float y0, float y1) -> int
        {
            for (int t = 0; t < static_cast<int>(T.size()); ++t)
                if (T[t].m_Key == Key) { T[t].m_Lo = std::min({ T[t].m_Lo, y0, y1 }); T[t].m_Hi = std::max({ T[t].m_Hi, y0, y1 }); return t; }
            T.push_back({ Key, std::min(y0, y1), std::max(y0, y1) });
            return static_cast<int>(T.size()) - 1;
        };

        // D1, channels (5.2): a route crosses each column between its ends in the gap whose base Y is closest to the straight line from
        // producer to consumer (columns as equal steps in x). Every haul spans the whole column, so all trunks of a channel overlap.
        std::map<std::pair<int,int>, std::vector<trunk>> Channels;
        for (int i = 0; i < nE; ++i)
        {
            if (!Valid[i]) continue;
            const auto& e = E[i];
            auto& Rt = R.m_Routes[i];
            const int cp = Base.m_Column[e.m_From], cq = Base.m_Column[e.m_To];
            const float yp = RowAt(Base, e.m_From, e.m_FromRow), yq = RowAt(Base, e.m_To, e.m_ToRow);
            for (int m = cp + 1; m < cq; ++m)
            {
                const float Target = yp + (yq - yp) * static_cast<float>(m - cp) / static_cast<float>(cq - cp);
                const auto& Gaps = Base.m_GapTop[m];
                int g = 0;
                for (int k = 1; k < static_cast<int>(Gaps.size()); ++k) if (std::abs(Gaps[k] - Target) < std::abs(Gaps[g] - Target)) g = k;
                const auto& Steps = Base.m_ColumnSteps[m];
                Rt.m_Channels.push_back({ m, g });
                Rt.m_ChannelLane.push_back(AddTrunk(Channels[{ m, g }], { 1, m, g < static_cast<int>(Steps.size()) ? StepId[Steps[g]] : ~std::uint64_t(0), e.m_Source }, 0, 0));
            }
        }
        R.m_ChannelLanes.resize(K);
        for (int c = 0; c < K; ++c) R.m_ChannelLanes[c].assign(Base.m_GapTop[c].size(), 0);
        // ponytail: channel lanes are ordered by source id only (no crossing minimisation); order by where each trunk goes next if it shows
        for (auto& [mg, T] : Channels) { Pack(T, VM.m_Lanes, geo::MAX_CHANNEL_LANES); R.m_ChannelLanes[mg.first][mg.second] = LaneCount(T); }
        for (int i = 0; i < nE; ++i)
            for (std::size_t k = 0; k < R.m_Routes[i].m_Channels.size(); ++k)
                R.m_Routes[i].m_ChannelLane[k] = Channels[R.m_Routes[i].m_Channels[k]][R.m_Routes[i].m_ChannelLane[k]].m_Lane;

        // The Y of every turn of every route, as (column, y): producer row, channel lanes, consumer row
        auto Turns = [&](const sl::result& L)
        {
            std::vector<std::vector<std::pair<int, float>>> Y(nE);
            for (int i = 0; i < nE; ++i)
            {
                if (!Valid[i]) continue;
                const auto& e = E[i];
                Y[i].push_back({ L.m_Column[e.m_From], RowAt(L, e.m_From, e.m_FromRow) });
                for (std::size_t k = 0; k < R.m_Routes[i].m_Channels.size(); ++k)
                {
                    const auto [m, g] = R.m_Routes[i].m_Channels[k];
                    Y[i].push_back({ m, L.m_GapTop[m][g] + sl::geo::NODE_GAP * 0.5f + R.m_Routes[i].m_ChannelLane[k] * sl::geo::LANE_GAP });
                }
                Y[i].push_back({ L.m_Column[e.m_To], RowAt(L, e.m_To, e.m_ToRow) });
            }
            return Y;
        };

        // Pass C with channel growth, then column offsets: rows of neighbouring columns often line up exactly, and a horizontal leaving the
        // left column would merge with one arriving at the right column. Column c moves down by the first offset that keeps every such pair
        // in gutter c-1 MIN_SEP apart (the offset it had last time first, so nothing jumps while editing; else the one with fewest conflicts).
        // A pair cannot meet when the left one turns on H(c-1,'R') and the right one comes from H(c,'L'): those x ranges never overlap.
        auto Lay = sl::PassC(In, Col, K, R.m_ChannelLanes);
        R.m_Offsets.assign(K, 0);
        {
            const auto Y = Turns(Lay);
            for (int c = 1; c < K; ++c)
            {
                struct run { float m_Ya, m_Yb; std::int64_t m_Pin; bool m_bSame; };    // a hop of gutter c-1; Yb unshifted
                std::vector<run> Runs;
                for (int i = 0; i < nE; ++i)
                {
                    const auto& y = Y[i];
                    if (!Valid[i]) continue;
                    const auto Pin = R.m_SourcePin[R.m_RouteSource[i]];
                    if (y.size() == 2 && y[0].first == y[1].first) { if (y[0].first == c - 1) Runs.push_back({ y[0].second + R.m_Offsets[c - 1], y[1].second + R.m_Offsets[c - 1], Pin, true }); continue; }
                    for (std::size_t k = 0; k + 1 < y.size(); ++k)
                        if (y[k].first == c - 1) Runs.push_back({ y[k].second + R.m_Offsets[c - 1], y[k + 1].second, Pin, false });
                }
                auto Conflicts = [&](float o)
                {
                    struct end { float m_Y; std::int64_t m_Pin; bool m_bOnR; };            // left: turns on H(c-1,'R'); right: comes from it
                    std::vector<end> Left, Right;
                    for (auto& r : Runs)
                    {
                        if (r.m_bSame) { Left.push_back({ r.m_Ya, r.m_Pin, true }); Left.push_back({ r.m_Yb, r.m_Pin, true }); continue; }
                        const bool bDown = r.m_Yb + o >= r.m_Ya;
                        Left.push_back({ r.m_Ya, r.m_Pin, bDown });
                        Right.push_back({ r.m_Yb + o, r.m_Pin, bDown });
                    }
                    int n = 0;
                    for (auto& l : Left) for (auto& r : Right) n += l.m_Pin != r.m_Pin && std::abs(l.m_Y - r.m_Y) < geo::MIN_SEP && !(l.m_bOnR && !r.m_bOnR);
                    return n;
                };
                std::vector<float> Try;
                if (c < static_cast<int>(VM.m_Offsets.size())) Try.push_back(VM.m_Offsets[c]);
                for (int k = 0; k < geo::OFFSET_TRIES; ++k) Try.push_back(k * geo::OFFSET_STEP);
                int Best = 1 << 30;
                for (float o : Try) if (const int n = Conflicts(o); n < Best) { Best = n; R.m_Offsets[c] = o; if (n == 0) break; }
            }
        }
        Lay = sl::PassC(In, Col, K, R.m_ChannelLanes, R.m_Offsets);
        const auto Y = Turns(Lay);

        // D2, highways: one hop per gutter crossed; the vertical lives in the left column's H(g,'R') when it goes down and in the right
        // column's H(g+1,'L') when it goes up (E27 OwnerColumnOf: right and down agree => the source side). Same column: H(c,'R').
        std::vector<std::vector<trunk>> HW(2 * K);                      // [gutter * 2 + side]
        std::vector<std::vector<std::pair<int,int>>> GutterHops(K);     // (route, hop)
        for (int i = 0; i < nE; ++i)
        {
            if (!Valid[i]) continue;
            auto& Rt = R.m_Routes[i];
            const auto& y = Y[i];
            if (y.size() == 2 && y[0].first == y[1].first) Rt.m_Hops.push_back({ y[0].first, 1, y[0].second, y[1].second, true });
            else for (std::size_t k = 0; k + 1 < y.size(); ++k) Rt.m_Hops.push_back({ y[k].first, y[k + 1].second >= y[k].second ? 1 : 0, y[k].second, y[k + 1].second, false });
            for (int k = 0; k < static_cast<int>(Rt.m_Hops.size()); ++k)
            {
                auto& h = Rt.m_Hops[k];
                h.m_Trunk = AddTrunk(HW[h.m_Gutter * 2 + h.m_Side], { 0, h.m_Gutter, static_cast<std::uint64_t>(h.m_Side), E[i].m_Source }, h.m_Ya, h.m_Yb);
                GutterHops[h.m_Gutter].push_back({ i, k });
            }
        }
        std::vector<char> Full(2 * K);
        for (int c = 0; c < 2 * K; ++c) Full[c] = Pack(HW[c], VM.m_Lanes, geo::MAX_LANES);

        // Crossing minimisation (5.3 step 4) on the containers just full-packed. Crossings are counted with a virtual x per gutter (0 = left
        // column edge, 1+lane on H(g,'R'), 1000-lane on H(g+1,'L'), 1001 = right column edge): only the order of x decides crossings, so the
        // count equals the real one. Swap two neighbouring lanes, or move one trunk to another lane where it fits, whenever that lowers the
        // crossings of the outputs it touches (the rest of the gutter cannot change); until nothing helps or 4*lanes passes. Never adds a lane.
        const int nS = static_cast<int>(R.m_Sources.size());
        std::vector<std::vector<segment>> Segs(nS);                     // per source: its merged segments in the gutter being minimised
        std::vector<std::vector<std::pair<int,int>>> SrcHops(nS);       // per source: its (route, hop) in that gutter
        std::vector<char> InF(nS, 0);
        auto Crosses = [](const segment& h, const segment& v) { return v.m_X0 > h.m_X0 && v.m_X0 < h.m_X1 && h.m_Y0 > v.m_Y0 && h.m_Y0 < v.m_Y1; };
        for (int g = 0; g < K; ++g)
        {
            if (!Full[g * 2] && !Full[g * 2 + 1]) continue;
            std::vector<int> Here;                                      // sources with hops in this gutter
            for (auto [i, k] : GutterHops[g]) { auto& H = SrcHops[R.m_RouteSource[i]]; if (H.empty()) Here.push_back(R.m_RouteSource[i]); H.push_back({ i, k }); }
            auto Rebuild = [&](int s)
            {
                std::vector<segment> S;
                for (auto [i, k] : SrcHops[s])
                {
                    const auto& h = R.m_Routes[i].m_Hops[k];
                    const int Lane = HW[g * 2 + h.m_Side][h.m_Trunk].m_Lane;
                    HopSegments(h, s, h.m_Side ? 1.0f + Lane : 1000.0f - Lane, 0, 1001, S);
                }
                Segs[s] = Merge(std::move(S));
            };
            auto Count = [&](const std::vector<int>& F)                 // crossings with at least one side in F, each once
            {
                int n = 0;
                for (int s : F) InF[s] = 1;
                for (int s : F)
                    for (auto& a : Segs[s])
                        for (int s2 : Here)
                            if (s2 != s)
                                for (auto& b : Segs[s2]) n += IsH(a) ? !IsH(b) && Crosses(a, b) : IsH(b) && !InF[s2] && Crosses(b, a);
                for (int s : F) InF[s] = 0;
                return n;
            };
            for (int s : Here) Rebuild(s);
            [[maybe_unused]] const int C0 = Count(Here);
            const int Passes = 4 * std::max(1, LaneCount(HW[g * 2]) + LaneCount(HW[g * 2 + 1]));
            for (int p = 0; p < Passes; ++p)
            {
                bool bBetter = false;
                auto Try = [&](std::vector<int> F, auto&& Do, auto&& Undo)
                {
                    std::sort(F.begin(), F.end()); F.erase(std::unique(F.begin(), F.end()), F.end());
                    const int Before = Count(F);
                    Do();     for (int s : F) Rebuild(s);
                    if (Count(F) < Before) { bBetter = true; return; }
                    Undo();   for (int s : F) Rebuild(s);
                };
                for (int Side = 0; Side < 2; ++Side)
                {
                    if (!Full[g * 2 + Side]) continue;
                    auto& T = HW[g * 2 + Side];
                    auto SrcOf = [&](int t) { return static_cast<int>(std::lower_bound(R.m_Sources.begin(), R.m_Sources.end(), T[t].m_Key.m_Source) - R.m_Sources.begin()); };
                    const int nL = LaneCount(T);
                    for (int L = 0; L + 1 < nL; ++L)
                    {
                        std::vector<int> F;
                        for (int t = 0; t < static_cast<int>(T.size()); ++t) if (T[t].m_Lane == L || T[t].m_Lane == L + 1) F.push_back(SrcOf(t));
                        auto Swap = [&] { for (auto& t : T) t.m_Lane = t.m_Lane == L ? L + 1 : t.m_Lane == L + 1 ? L : t.m_Lane; };
                        Try(F, Swap, Swap);
                    }
                    for (int t = 0; t < static_cast<int>(T.size()); ++t)
                        for (int L = 0; L < nL; ++L)
                        {
                            const int Old = T[t].m_Lane;
                            if (L != Old && Fits(T, t, L)) Try({ SrcOf(t) }, [&] { T[t].m_Lane = L; }, [&] { T[t].m_Lane = Old; });
                        }
                    Compact(T);
                }
                if (!bBetter) break;
                for (int s : Here) Rebuild(s);                          // Compact renumbered lanes (same order: crossings unchanged)
            }
            assert(Count(Here) <= C0);
            for (int s : Here) SrcHops[s].clear();
        }

        // Pass E with the highway lanes
        R.m_LanesL.assign(K, 0);
        R.m_LanesR.assign(K, 0);
        for (int c = 0; c < K; ++c) { R.m_LanesR[c] = LaneCount(HW[c * 2 + 1]); if (c > 0) R.m_LanesL[c] = LaneCount(HW[(c - 1) * 2]); }
        sl::PassE(Lay, R.m_LanesL, R.m_LanesR);

        // F, geometry: the polyline of every route, then the wires (merged per source), crossings, junctions, gaps
        std::vector<segment> All;
        for (int i = 0; i < nE; ++i)
        {
            if (!Valid[i]) continue;
            auto& Rt = R.m_Routes[i];
            const auto& P = Lay.m_Steps[E[i].m_From];
            const auto& Q = Lay.m_Steps[E[i].m_To];
            std::vector<point> Pts{ { P.m_X + P.m_W, Y[i].front().second } };
            for (auto& h : Rt.m_Hops)
            {
                h.m_Lane = HW[h.m_Gutter * 2 + h.m_Side][h.m_Trunk].m_Lane;
                const float x = sl::HighwayX(Lay, h.m_Side ? h.m_Gutter : h.m_Gutter + 1, h.m_Side, h.m_Lane);
                Pts.push_back({ x, h.m_Ya });
                Pts.push_back({ x, h.m_Yb });
            }
            Pts.push_back({ Rt.m_Hops.front().m_bSame ? Q.m_X + Q.m_W : Q.m_X, Y[i].back().second });
            for (auto& p : Pts)                     // drop repeats and the middle of straight runs
            {
                auto& O = Rt.m_Points;
                if (!O.empty() && O.back().m_X == p.m_X && O.back().m_Y == p.m_Y) continue;
                if (O.size() >= 2 && ((O[O.size() - 2].m_X == O.back().m_X && O.back().m_X == p.m_X) || (O[O.size() - 2].m_Y == O.back().m_Y && O.back().m_Y == p.m_Y))) O.back() = p;
                else O.push_back(p);
            }
            for (std::size_t k = 0; k + 1 < Rt.m_Points.size(); ++k)
                All.push_back(Seg(Rt.m_Points[k].m_X, Rt.m_Points[k].m_Y, Rt.m_Points[k + 1].m_X, Rt.m_Points[k + 1].m_Y, R.m_RouteSource[i]));
        }
        const auto Wires = Merge(std::move(All));
        std::vector<int> CrossSeg;
        const auto D = Classify(Wires, R.m_SourcePin, &R.m_Crossings, &CrossSeg);
        R.m_Touches = D.m_Touches; R.m_Overlaps = D.m_Overlaps; R.m_SelfCrossings = D.m_Self;
        R.m_Junctions = Junctions(Wires);

        std::vector<std::vector<float>> Cuts(Wires.size());
        for (std::size_t k = 0; k < CrossSeg.size(); ++k) Cuts[CrossSeg[k]].push_back(R.m_Crossings[k].m_X);
        for (std::size_t s = 0; s < Wires.size(); ++s)
        {
            const auto& w = Wires[s];
            for (auto& b : Lay.m_Steps) R.m_StepHits += w.m_X1 > b.m_X && w.m_X0 < b.m_X + b.m_W && w.m_Y1 > b.m_Y && w.m_Y0 < b.m_Y + b.m_H;
            std::sort(Cuts[s].begin(), Cuts[s].end());
            float x = w.m_X0;
            for (float c : Cuts[s]) { if (c - geo::CROSS_GAP > x) R.m_Wires.push_back({ x, w.m_Y0, c - geo::CROSS_GAP, w.m_Y1, w.m_Source }); x = c + geo::CROSS_GAP; }
            if (Cuts[s].empty() || x < w.m_X1) R.m_Wires.push_back({ x, w.m_Y0, w.m_X1, w.m_Y1, w.m_Source });
        }

        for (auto& [mg, T] : Channels) for (auto& t : T) R.m_Memo[t.m_Key] = { t.m_Lane, t.m_Lo, t.m_Hi };
        for (auto& T : HW)              for (auto& t : T) R.m_Memo[t.m_Key] = { t.m_Lane, t.m_Lo, t.m_Hi };
        R.m_Layout = std::move(Lay);
        return R;
    }

    // Passes B..F: pick K (hysteresis), partition (memory), route; while the real width (with lanes) is wider than the view and K > 1, one
    // column less (4.3; with HYSTERESIS of slack, or narrowing would undo ChooseK's hysteresis). Only the final layout updates the view
    // memory. bRepack = RepackRoutes (forget every lane), bRepartition = -Columns.
    inline result Build(const sl::input& In, const std::vector<edge>& E, float Width, view_memory& VM, bool bRepack = false, bool bRepartition = false) noexcept
    {
        if (bRepack) { VM.m_Lanes.clear(); VM.m_Offsets.clear(); }
        for (int K = sl::ChooseK(Width, sl::ColumnWidth(In), static_cast<int>(In.size()), VM.m_Layout); ; --K)
        {
            const auto Col = sl::Columns(In, K, VM.m_Layout, bRepartition);
            auto R = Route(In, E, K, Col, VM);
            if (R.m_Layout.m_Width <= Width + sl::geo::HYSTERESIS || K == 1)
            {
                sl::Remember(In, Col, K, VM.m_Layout);
                for (auto& [k, v] : R.m_Memo) VM.m_Lanes[k] = v;
                VM.m_Offsets = R.m_Offsets;
                return R;
            }
        }
    }

    //------------------------------------------------------------------------------------------------------------------------------------
    // Queries (LayoutStats, RouteInfo)
    //------------------------------------------------------------------------------------------------------------------------------------
    inline std::string Stats(const result& R, const std::vector<std::string>& SectionNames = {})
    {
        const auto& L = R.m_Layout;
        std::string Out = std::format("columns {}  width {:.0f}  height {:.0f}  routes {}  outputs {}\n", L.m_K, L.m_Width, L.m_Height, R.m_Routes.size(), R.m_Sources.size());
        float Widest = 0;
        for (int c = 0; c < L.m_K; ++c)
        {
            Out += std::format("column {}: highway lanes L {} R {}  sections:", c, R.m_LanesL[c], R.m_LanesR[c]);
            for (std::size_t s = 0; s < L.m_SectionColumn.size(); ++s)
                if (L.m_SectionColumn[s] == c) Out += " " + (s < SectionNames.size() ? SectionNames[s] : std::format("#{}", s));
            std::string Ch;
            for (std::size_t g = 0; g < R.m_ChannelLanes[c].size(); ++g) if (R.m_ChannelLanes[c][g]) Ch += std::format(" gap{}:{}", g, R.m_ChannelLanes[c][g]);
            Out += (Ch.empty() ? "" : "  channel lanes" + Ch) + "\n";
            if (c + 1 < L.m_K) Widest = std::max(Widest, L.m_X[c + 1] - L.m_X[c] - L.m_CW);
        }
        Out += std::format("crossings {}  junctions {}  segments through steps {}  false junctions {}  merged runs {}  self crossings {}  not drawn {}  widest gutter {:.0f}\n",
            R.m_Crossings.size(), R.m_Junctions.size(), R.m_StepHits, R.m_Touches, R.m_Overlaps, R.m_SelfCrossings, R.m_Skipped, Widest);
        return Out;
    }

    inline std::string Info(const result& R, int Edge)
    {
        const auto& Rt = R.m_Routes[Edge];
        if (Rt.m_Points.empty()) return "not drawn: the connection breaks the order";
        std::string Out = "route:";
        for (std::size_t k = 0; k < Rt.m_Hops.size(); ++k)
        {
            const auto& h = Rt.m_Hops[k];
            Out += std::format(" H({},{}) lane {}", h.m_Side ? h.m_Gutter : h.m_Gutter + 1, h.m_Side ? 'R' : 'L', h.m_Lane);
            if (k < Rt.m_Channels.size()) Out += std::format(" -> CH({},gap{}) lane {} ->", Rt.m_Channels[k].first, Rt.m_Channels[k].second, Rt.m_ChannelLane[k]);
        }
        const int Src = R.m_RouteSource[Edge];
        int nJ = 0, nX = 0;
        for (auto& j : R.m_Junctions) nJ += j.m_Source == Src && OnPolyline(Rt.m_Points, j.m_X, j.m_Y);
        for (auto& x : R.m_Crossings) nX += OnPolyline(Rt.m_Points, x.m_X, x.m_Y);
        Out += std::format("\njunctions {}  crossings {}  points", nJ, nX);
        for (auto& p : Rt.m_Points) Out += std::format(" ({:.0f},{:.0f})", p.m_X, p.m_Y);
        return Out + "\n";
    }

    //------------------------------------------------------------------------------------------------------------------------------------
    // Self-check (stack_canvas.md section 11): column thresholds, tests 1, 3, 7, 10 from code, junction vs crossing, determinism, stability,
    // a fuzz over random valid stacks. assert-based: run it in a build with asserts (debug, or the standalone test).
    //------------------------------------------------------------------------------------------------------------------------------------
    struct test_case
    {
        sl::input           m_In;
        std::vector<edge>   m_E;
        std::vector<int>    m_nIn, m_nOut;      // per global step: rows 0..nIn-1 are inputs, the outputs follow

        int Section() { m_In.push_back({ m_In.size() + 1, {} }); return static_cast<int>(m_In.size()) - 1; }
        int Step(int nIn, int nOut, float W = 180)
        {
            m_In.back().m_Steps.push_back({ 1000 + m_nIn.size(), W, std::max(1, nIn + nOut) });
            m_nIn.push_back(nIn); m_nOut.push_back(nOut);
            return static_cast<int>(m_nIn.size()) - 1;
        }
        void Link(int From, int Out, int To, int In, int Sub = 0)
        {
            m_E.push_back({ From, m_nIn[From] + Out, To, In, static_cast<std::uint64_t>(From) << 16 | static_cast<std::uint64_t>(Out) << 4 | static_cast<std::uint64_t>(Sub), m_E.size() + 1 });
        }
    };

    // Everything the canvas relies on: nothing through a step, no false junction, no merged runs, each route from its pins, axis aligned
    inline void CheckRoutes(const result& R, const std::vector<edge>& E)
    {
        assert(R.m_StepHits == 0 && R.m_Touches == 0 && R.m_Overlaps == 0 && R.m_Skipped == 0);
        for (std::size_t i = 0; i < E.size(); ++i)
        {
            const auto& P = R.m_Routes[i].m_Points;
            [[maybe_unused]] const auto& A = R.m_Layout.m_Steps[E[i].m_From];
            [[maybe_unused]] const auto& B = R.m_Layout.m_Steps[E[i].m_To];
            assert(P.size() >= 2);
            assert(P.front().m_X == A.m_X + A.m_W && P.front().m_Y == A.m_Y + sl::RowY(E[i].m_FromRow));
            assert((P.back().m_X == B.m_X || P.back().m_X == B.m_X + B.m_W) && P.back().m_Y == B.m_Y + sl::RowY(E[i].m_ToRow));
            for (std::size_t k = 0; k + 1 < P.size(); ++k) assert(P[k].m_X == P[k + 1].m_X || P[k].m_Y == P[k + 1].m_Y);
        }
        for ([[maybe_unused]] auto& j : R.m_Junctions) for ([[maybe_unused]] auto& x : R.m_Crossings) assert(j.m_X != x.m_X || j.m_Y != x.m_Y);
    }

    inline test_case RandomCase(std::mt19937& Rng, int MaxSections, int MaxSteps)
    {
        test_case C;
        const int nSec = 1 + static_cast<int>(Rng() % MaxSections);
        for (int s = 0; s < nSec; ++s)
        {
            C.Section();
            for (int n = static_cast<int>(Rng() % (MaxSteps + 1)); n > 0; --n) C.Step(static_cast<int>(Rng() % 4), 1 + static_cast<int>(Rng() % 2), 160.0f + static_cast<float>(Rng() % 100));
        }
        C.Section();                                                                    // the output section
        C.Step(3, 0);
        for (int b = 0; b < static_cast<int>(C.m_nIn.size()); ++b)
            for (int i = 0; i < C.m_nIn[b]; ++i)
            {
                if (b == 0 || Rng() % 10 >= 6) continue;
                const int a = static_cast<int>(Rng() % b);
                if (C.m_nOut[a] == 0) continue;
                C.Link(a, static_cast<int>(Rng() % C.m_nOut[a]), b, i, Rng() % 4 == 0 ? 1 + static_cast<int>(Rng() % 3) : 0);
            }
        return C;
    }

    // The lanes of the trunks in A whose source is not Except that changed in B
    inline int ChangedLanes(const result& A, const result& B, std::uint64_t Except = ~std::uint64_t(0))
    {
        int n = 0;
        for (auto& [k, v] : A.m_Memo)
            if (k.m_Source != Except) if (auto It = B.m_Memo.find(k); It != B.m_Memo.end() && It->second.m_Lane != v.m_Lane) ++n;
        return n;
    }

    inline std::string SelfTest(int FuzzCases = 300)
    {
        std::string Out;

        // Column thresholds: 6 equal sections, CW 200 => K columns first fit at 292K - 20 px. Sweeping 500 -> 1600 -> 500, K steps up exactly
        // HYSTERESIS past a threshold and down HYSTERESIS (+1) below it, one column at a time, nowhere else.
        {
            test_case C;
            for (int s = 0; s < 6; ++s) { C.Section(); C.Step(1, 1, 200); }
            sl::memory M;
            int Prev = 0, Changes = 0;
            for (int w = 500; w <= 1600; ++w)
            {
                const int K = sl::Build(C.m_In, static_cast<float>(w), M).m_K;
                if (Prev && K != Prev) { assert(K == Prev + 1 && w == 292 * K - 20 + 24); ++Changes; }
                Prev = K;
            }
            for (int w = 1600; w >= 500; --w)
            {
                const int K = sl::Build(C.m_In, static_cast<float>(w), M).m_K;
                if (K != Prev) { assert(K == Prev - 1 && w == 292 * Prev - 20 - 25); ++Changes; }
                Prev = K;
            }
            assert(Changes == 8);
            // partition memory: one more step keeps every section in its column; -Columns re-partitions
            sl::memory M2;
            const auto A = sl::Build(C.m_In, 900, M2);
            C.m_In[1].m_Steps.push_back({ 77, 200, 3 });
            const auto B = sl::Build(C.m_In, 900, M2);
            assert(A.m_K == 3 && A.m_SectionColumn == B.m_SectionColumn);
            Out += std::format("thresholds ok ({} K changes 500..1600..500)\n", Changes);
        }

        // Test 1: UV feeds 8 inputs over 3 sections: one trunk per container, junction dots, nothing parallel
        {
            test_case C;
            C.Section(); const int UV = C.Step(0, 1); const int a = C.Step(2, 1);
            C.Section(); const int n1 = C.Step(2, 1), n2 = C.Step(2, 1), n3 = C.Step(2, 1);
            C.Section(); const int c1 = C.Step(2, 1), c2 = C.Step(2, 1), c3 = C.Step(2, 1);
            C.Section(); const int o = C.Step(3, 0);
            for (int t : { a, n1, n2, n3, c1, c2, c3 }) C.Link(UV, 0, t, 0);
            C.Link(UV, 0, o, 2);
            C.Link(a, 0, n1, 1); C.Link(n1, 0, n2, 1); C.Link(n2, 0, n3, 1); C.Link(n3, 0, o, 0);
            C.Link(c1, 0, c2, 1); C.Link(c2, 0, c3, 1); C.Link(c3, 0, o, 1);
            for (float W : { 1600.0f, 1000.0f, 500.0f })
            {
                view_memory VM;
                const auto R = Build(C.m_In, C.m_E, W, VM);
                CheckRoutes(R, C.m_E);
                const int Src = static_cast<int>(std::lower_bound(R.m_Sources.begin(), R.m_Sources.end(), C.m_E[0].m_Source) - R.m_Sources.begin());
                assert(std::count_if(R.m_Junctions.begin(), R.m_Junctions.end(), [&](const mark& m) { return m.m_Source == Src; }) >= 1);
                if (W == 1600.0f) Out += std::format("test 1 (UV x8) at {:.0f}px: columns {}, crossings {}, UV junctions {}\n", W, R.m_Layout.m_K, R.m_Crossings.size(),
                    std::count_if(R.m_Junctions.begin(), R.m_Junctions.end(), [&](const mark& m) { return m.m_Source == Src; }));
            }
        }

        // Test 3: Wetness outputs Roughness and Darken, each used twice: two trunks, never merged
        {
            test_case C;
            C.Section(); const int w = C.Step(1, 2);
            C.Section(); const int a = C.Step(2, 1), b = C.Step(2, 1);
            C.Section(); const int c = C.Step(2, 1), d = C.Step(2, 1);
            C.Section(); const int o = C.Step(4, 0);
            C.Link(w, 0, a, 0); C.Link(w, 1, a, 1); C.Link(w, 0, c, 0); C.Link(w, 1, d, 1);
            C.Link(a, 0, b, 0); C.Link(b, 0, o, 0); C.Link(c, 0, d, 0); C.Link(d, 0, o, 1);
            view_memory VM;
            const auto R = Build(C.m_In, C.m_E, 1600, VM);
            CheckRoutes(R, C.m_E);
            assert(R.m_Sources.size() == 6);
            Out += std::format("test 3 (two outputs) ok: crossings {}\n", R.m_Crossings.size());
        }

        // Junction vs crossing: P1 fans out to Q2 and Q3 (a dot on its trunk); P3 goes up-right to Q1 across P1's trunk (a crossing, no dot)
        {
            test_case C;
            C.Section(); const int p1 = C.Step(0, 1), p2 = C.Step(0, 1), p3 = C.Step(0, 1);
            C.Section(); const int q1 = C.Step(5, 1), q2 = C.Step(5, 1), q3 = C.Step(5, 1);
            C.Link(p1, 0, q2, 0); C.Link(p1, 0, q3, 0); C.Link(p3, 0, q1, 0); C.Link(p2, 0, q3, 1);
            view_memory VM;
            const auto R = Build(C.m_In, C.m_E, 600, VM);
            CheckRoutes(R, C.m_E);
            assert(R.m_Layout.m_K == 2);
            assert(R.m_Junctions.size() == 1 && R.m_Junctions[0].m_Source == R.m_RouteSource[0]);
            assert(!R.m_Crossings.empty());
            Out += std::format("junction vs crossing ok: junctions {}, crossings {}\n", R.m_Junctions.size(), R.m_Crossings.size());
        }

        // Test 7: collapse a 30-step section (4 external inputs, 2 outputs): no section changes column; expanding restores the lanes
        {
            auto Make = [](bool bCollapsed)
            {
                test_case C;
                C.Section(); int in[4]; for (auto& x : in) x = C.Step(0, 1);
                C.Section(); const int a0 = C.Step(2, 1), a1 = C.Step(2, 1);
                C.Link(in[0], 0, a0, 0); C.Link(a0, 0, a1, 0);
                C.Section();
                int Out0, Out1;
                if (bCollapsed)
                {
                    const int Box = C.Step(4, 2);                                       // summary row folded into the box rows: 4 boundary inputs, 2 outputs
                    for (int k = 0; k < 4; ++k) C.Link(in[k], 0, Box, k);
                    Out0 = Out1 = Box;
                }
                else
                {
                    int Prev = -1, First = -1;
                    for (int k = 0; k < 30; ++k) { const int s = C.Step(2, 1); if (Prev >= 0) C.Link(Prev, 0, s, 0); else First = s; Prev = s; }
                    for (int k = 0; k < 4; ++k) C.Link(in[k], 0, First + k * 7, 1);
                    Out0 = Prev - 3; Out1 = Prev;
                }
                C.Section(); const int b0 = C.Step(2, 1), b1 = C.Step(2, 1);
                C.Section(); const int o = C.Step(3, 0);
                auto Src = [&](int s, int k) { return bCollapsed ? std::pair{ s, k } : std::pair{ s, 0 }; };
                // boundary outputs keep their source key whether collapsed or not (7.6): link by key
                auto LinkKey = [&](std::pair<int,int> From, std::uint64_t Key, int To, int In) { C.m_E.push_back({ From.first, C.m_nIn[From.first] + From.second, To, In, Key, C.m_E.size() + 1 }); };
                LinkKey(Src(Out0, 0), 0xA0, b0, 0); LinkKey(Src(Out1, 1), 0xA1, b1, 0);
                LinkKey(Src(Out1, 1), 0xA1, o, 2);
                C.Link(a1, 0, b0, 1); C.Link(b0, 0, o, 0); C.Link(b1, 0, o, 1);
                return C;
            };
            const auto Ex = Make(false), Co = Make(true);
            view_memory VM;
            const auto R1 = Build(Ex.m_In, Ex.m_E, 1000, VM);
            const auto R2 = Build(Co.m_In, Co.m_E, 1000, VM);
            const auto R3 = Build(Ex.m_In, Ex.m_E, 1000, VM);
            CheckRoutes(R1, Ex.m_E); CheckRoutes(R2, Co.m_E); CheckRoutes(R3, Ex.m_E);
            assert(R1.m_Layout.m_SectionColumn == R2.m_Layout.m_SectionColumn && R1.m_Layout.m_SectionColumn == R3.m_Layout.m_SectionColumn);
            const int Changed = ChangedLanes(R1, R3);
            assert(Changed == 0);
            Out += std::format("test 7 (collapse 30) ok: columns {}, column height {:.0f} -> {:.0f}, lanes changed after expand {}\n",
                R1.m_Layout.m_K, R1.m_Layout.m_Height, R2.m_Layout.m_Height, Changed);
        }

        // Test 10: 200 steps in 12 sections, add one unwired step in a middle section: no section changes column, only the steps below it in
        // its column move, unrelated lanes stay
        {
            std::mt19937 Rng(10);
            test_case C;
            for (int s = 0; s < 12; ++s) { C.Section(); for (int k = 0; k < (s < 8 ? 17 : 16); ++k) C.Step(2, 1); }
            C.Section(); C.Step(3, 0);
            const int N = static_cast<int>(C.m_nIn.size());
            for (int b = 1; b < N; ++b)
                for (int i = 0; i < C.m_nIn[b]; ++i)
                {
                    if (Rng() % 10 >= 7) continue;
                    const int a = Rng() % 10 == 0 ? static_cast<int>(Rng() % b) : std::max(0, b - 1 - static_cast<int>(Rng() % 6));
                    if (C.m_nOut[a]) C.Link(a, 0, b, i);
                }
            view_memory VM;
            const auto T0 = std::chrono::steady_clock::now();
            const auto R1 = Build(C.m_In, C.m_E, 1600, VM);
            const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
            CheckRoutes(R1, C.m_E);

            const int Sec = 5, At = 8;                                          // insert in the middle of section 5
            int Global = 0; for (int s = 0; s < Sec; ++s) Global += static_cast<int>(C.m_In[s].m_Steps.size());
            Global += At;
            auto D = C;
            D.m_In[Sec].m_Steps.insert(D.m_In[Sec].m_Steps.begin() + At, { 999999, 180, 3 });
            for (auto& e : D.m_E) { e.m_From += e.m_From >= Global; e.m_To += e.m_To >= Global; }
            const auto T1 = std::chrono::steady_clock::now();
            const auto R2 = Build(D.m_In, D.m_E, 1600, VM);
            const double Ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T1).count();
            CheckRoutes(R2, D.m_E);
            assert(R1.m_Layout.m_SectionColumn == R2.m_Layout.m_SectionColumn);
            const int Col = R1.m_Layout.m_SectionColumn[Sec];
            int Moved = 0;
            for (int g = 0; g < N; ++g)
            {
                const int g2 = g + (g >= Global);
                const bool bBelow = R1.m_Layout.m_Column[g] == Col && g >= Global;
                if (!bBelow) Moved += R1.m_Layout.m_Steps[g].m_Y != R2.m_Layout.m_Steps[g2].m_Y;
            }
            const int Changed = ChangedLanes(R1, R2);
            assert(Moved == 0);
            Out += std::format("test 10 (200 steps, insert) : columns {}, routes {}, crossings {}, other steps moved {}, lanes changed {}, layout {:.1f} ms (full pack), {:.1f} ms (after the insert)\n",
                R1.m_Layout.m_K, C.m_E.size(), R1.m_Crossings.size(), Moved, Changed, Ms, Ms2);
        }

        // Fuzz: random valid stacks at random widths. Invariants, determinism (two fresh builds agree), and stability: adding one route that
        // leaves every step where it was moves no trunk of another source.
        {
            std::mt19937 Rng(1234);
            int nStable = 0, nRoutes = 0, nCross = 0;
            for (int it = 0; it < FuzzCases; ++it)
            {
                auto C = RandomCase(Rng, 6, 6);
                const float W = 400.0f + static_cast<float>(Rng() % 1400);
                view_memory A, B;
                const auto R = Build(C.m_In, C.m_E, W, A);
                const auto R2 = Build(C.m_In, C.m_E, W, B);
                CheckRoutes(R, C.m_E);
                assert(R.m_Memo == R2.m_Memo);
                for (std::size_t i = 0; i < C.m_E.size(); ++i) assert(R.m_Routes[i].m_Points.size() == R2.m_Routes[i].m_Points.size());
                const auto R3 = Build(C.m_In, C.m_E, W, A);                     // same input with memory: nothing moves
                assert(ChangedLanes(R, R3) == 0);
                nRoutes += static_cast<int>(C.m_E.size()); nCross += static_cast<int>(R.m_Crossings.size());

                // one more route: a free input of a later step fed by an earlier output
                for (int Try = 0; Try < 20; ++Try)
                {
                    const int b = static_cast<int>(Rng() % C.m_nIn.size());
                    if (b == 0 || C.m_nIn[b] == 0) continue;
                    const int i = static_cast<int>(Rng() % C.m_nIn[b]);
                    if (std::any_of(C.m_E.begin(), C.m_E.end(), [&](const edge& e) { return e.m_To == b && e.m_ToRow == i; })) continue;
                    const int a = static_cast<int>(Rng() % b);
                    if (C.m_nOut[a] == 0) continue;
                    auto D = C;
                    D.Link(a, 0, b, i);
                    const auto R4 = Build(D.m_In, D.m_E, W, A);
                    CheckRoutes(R4, D.m_E);
                    bool bSameBoxes = R4.m_Layout.m_K == R3.m_Layout.m_K;
                    for (std::size_t g = 0; bSameBoxes && g < C.m_nIn.size(); ++g) bSameBoxes = R4.m_Layout.m_Steps[g].m_Y == R3.m_Layout.m_Steps[g].m_Y;
                    for (int c = 0; c < R3.m_Layout.m_K; ++c) bSameBoxes = bSameBoxes && std::max(R3.m_LanesL[c], R3.m_LanesR[c]) < geo::MAX_LANES;     // a full container repacks on any new trunk (5.5 step 4)
                    if (bSameBoxes) { assert(ChangedLanes(R3, R4, D.m_E.back().m_Source) == 0); ++nStable; }
                    break;
                }
            }
            Out += std::format("fuzz ok: {} stacks, {} routes, {} crossings, {} stability checks\n", FuzzCases, nRoutes, nCross, nStable);
        }
        return Out;
    }
}

#ifndef XMATERIAL_STACK_ROUTES_CORE_ONLY

#include "plugins/xmaterial.plugin/source/Graph/xmaterial_graph.h"
#include <unordered_set>

namespace xmaterial_editor::stack_routes
{
    constexpr float CHIP_W = 80.0f;     // ponytail: room for a chip or a param widget beside an input's name; measure the real chip text in task 9

    // The stack of a graph as router input (4.2): per step one row per input, then one per used output (the first always). A sub-element is
    // no row: its routes leave from its output's row, as their own trunk. Measure(std::string) -> width in pixels (the canvas passes ImGui's).
    // ponytail: a collapsed section is laid out expanded until the collapsed box exists (task 11)
    struct graph_input
    {
        sl::input                               m_In;
        std::vector<edge>                       m_Edges;
        std::vector<xmaterial_graph::node_guid> m_Steps;            // per global step
        std::vector<std::string>                m_SectionNames;
    };

    template<typename T_MEASURE>
    graph_input FromGraph(const xmaterial_graph::graph& G, T_MEASURE&& Measure) noexcept
    {
        graph_input R;
        std::unordered_set<std::uint64_t> Used;                                     // output pins feeding something (directly or by a sub-element)
        for (auto& [Id, C] : G.m_Connections)
            if (auto* pN = G.findNodeByPin(C->m_OutputPinGuid))
                for (auto& O : pN->m_OutputPins)
                    if (O.m_PinGUID.m_Value == C->m_OutputPinGuid.m_Value || std::any_of(O.m_SubElements.begin(), O.m_SubElements.end(), [&](const xmaterial_graph::pin& s) { return s.m_PinGUID.m_Value == C->m_OutputPinGuid.m_Value; }))
                        Used.insert(O.m_PinGUID.m_Value);

        std::unordered_map<std::uint64_t, std::pair<int,int>> OutRow, InRow;        // pin -> (global step, row)
        for (auto& S : G.m_Stack.m_Sections)
        {
            auto& Sec = R.m_In.emplace_back();
            Sec.m_Id = S.m_Guid.m_Value;
            R.m_SectionNames.push_back(S.m_Name);
            for (auto Guid : S.m_Steps)
            {
                auto It = G.m_InstanceNodes.find(Guid);
                if (It == G.m_InstanceNodes.end()) continue;
                const auto& N = *It->second;
                const int g = static_cast<int>(R.m_Steps.size());
                R.m_Steps.push_back(Guid);
                float TextW = Measure(N.m_StepName.empty() ? N.m_Name : N.m_StepName + "  " + N.m_Name);
                int Row = 0;
                for (auto& P : N.m_InputPins) { InRow[P.m_PinGUID.m_Value] = { g, Row++ }; TextW = std::max(TextW, Measure(P.m_Name) + CHIP_W); }
                for (std::size_t o = 0; o < N.m_OutputPins.size(); ++o)
                {
                    const auto& P = N.m_OutputPins[o];
                    if (o > 0 && !Used.contains(P.m_PinGUID.m_Value)) continue;
                    OutRow[P.m_PinGUID.m_Value] = { g, Row };
                    for (auto& Sub : P.m_SubElements) OutRow[Sub.m_PinGUID.m_Value] = { g, Row };
                    TextW = std::max(TextW, Measure(P.m_Name));
                    ++Row;
                }
                Sec.m_Steps.push_back({ Guid.m_Value, sl::StepW(TextW), std::max(1, Row) });
            }
        }
        for (auto& [Id, C] : G.m_Connections)
        {
            auto O = OutRow.find(C->m_OutputPinGuid.m_Value);
            auto I = InRow.find(C->m_InputPinGuid.m_Value);
            if (O != OutRow.end() && I != InRow.end())
                R.m_Edges.push_back({ O->second.first, O->second.second, I->second.first, I->second.second, C->m_OutputPinGuid.m_Value, Id.m_Value });
        }
        return R;
    }
}

#endif // XMATERIAL_STACK_ROUTES_CORE_ONLY

//----------------------------------------------------------------------------------------------------------------------------------------
// View commands: only when the editor's stack commands are in this translation unit (include this header after xmaterial_stack_commands.h)
//----------------------------------------------------------------------------------------------------------------------------------------
#if defined(XMATERIAL_STACK_COMMANDS_H) && !defined(XMATERIAL_STACK_ROUTES_CORE_ONLY)

#include <functional>

namespace xmaterial_editor::cmds
{
    // View state of the stack canvas, shared by the canvas and the view commands (never saved, never a history entry)
    struct stack_view
    {
        stack_routes::view_memory                   m_Memory;
        float                                       m_Width   = 1600.0f;
        std::function<float(const std::string&)>    m_Measure = [](const std::string& s) { return 7.0f * static_cast<float>(s.size()); };     // ponytail: 7 px per char until the canvas (task 8) passes ImGui::CalcTextSize
    };

    // The layout the canvas would show at Width, on a copy of the view memory: a query never changes the view
    inline stack_routes::result StackLayoutOf(const graph_document& Doc, const stack_view& View, float Width, stack_routes::graph_input& In, double& Ms) noexcept
    {
        In = stack_routes::FromGraph(Doc.m_Graph, View.m_Measure);
        auto Memory = View.m_Memory;
        const auto T0 = std::chrono::steady_clock::now();
        auto R = stack_routes::Build(In.m_In, In.m_Edges, Width, Memory);
        Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
        return R;
    }

    inline bool WidthArg(const xcmdline::parser& P, xcmdline::parser::handle H, float& Width) noexcept
    {
        std::string Text;
        if (!GetArg(P, H, Text)) return true;
        const auto Res = std::from_chars(Text.data(), Text.data() + Text.size(), Width);
        return Res.ec == std::errc() && Width > 0;
    }

    //---- LayoutStats: columns, sections per column, lanes, crossings, segments through steps
    struct layout_stats_cmd : xundo::query_command_base
    {
        graph_document& m_Doc;
        stack_view&     m_View;
        layout_stats_cmd(xundo::system& System, graph_document& Doc, stack_view& View) noexcept : query_command_base(System, "LayoutStats", nullptr), m_Doc(Doc), m_View(View) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Lays out the stack: columns, sections per column, lanes per highway and channel, crossings, junctions, segments through steps (must be 0), widest gutter, layout ms. Usage: LayoutStats [-Width px]"; }
        void RegisterArguments() noexcept override { m_hWidth = m_Parser.addOption("Width", "View width in pixels (default: the canvas's)", false, 1); }
        std::string Query() noexcept override
        {
            if (!m_Doc.isLoaded()) return "LayoutStats: nothing loaded";
            float Width = m_View.m_Width;
            if (!WidthArg(m_Parser, m_hWidth, Width)) return "LayoutStats: bad -Width";
            stack_routes::graph_input In; double Ms = 0;
            const auto R = StackLayoutOf(m_Doc, m_View, Width, In, Ms);
            return stack_routes::Stats(R, In.m_SectionNames) + std::format("layout {:.2f} ms\n", Ms);
        }
        xcmdline::parser::handle m_hWidth;
    };

    //---- RouteInfo: the containers, lanes, junctions and crossings of one connection's route
    struct route_info_cmd : xundo::query_command_base
    {
        graph_document& m_Doc;
        stack_view&     m_View;
        route_info_cmd(xundo::system& System, graph_document& Doc, stack_view& View) noexcept : query_command_base(System, "RouteInfo", nullptr), m_Doc(Doc), m_View(View) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Shows how a connection is routed: highways and channels with their lanes, junctions and crossings on it, its points. Usage: RouteInfo -Connection hexguid [-Width px]"; }
        void RegisterArguments() noexcept override
        {
            m_hConnection = m_Parser.addOption("Connection", "The connection", true,  1);
            m_hWidth      = m_Parser.addOption("Width",      "View width in pixels (default: the canvas's)", false, 1);
        }
        std::string Query() noexcept override
        {
            if (!m_Doc.isLoaded()) return "RouteInfo: nothing loaded";
            std::uint64_t Id = 0;
            float Width = m_View.m_Width;
            if (!HexArg(m_Parser, m_hConnection, Id) || !WidthArg(m_Parser, m_hWidth, Width)) return "RouteInfo: bad arguments";
            stack_routes::graph_input In; double Ms = 0;
            const auto R = StackLayoutOf(m_Doc, m_View, Width, In, Ms);
            for (std::size_t i = 0; i < In.m_Edges.size(); ++i) if (In.m_Edges[i].m_Id == Id) return stack_routes::Info(R, static_cast<int>(i));
            return "RouteInfo: no such connection between two steps";
        }
        xcmdline::parser::handle m_hConnection, m_hWidth;
    };

    //---- RepackRoutes: forget the lane memory (the next layout is a full pack); -Columns 1 also re-partitions the columns
    struct repack_routes_cmd : xundo::query_command_base
    {
        stack_view& m_View;
        repack_routes_cmd(xundo::system& System, stack_view& View) noexcept : query_command_base(System, "RepackRoutes", nullptr), m_View(View) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "View only: packs every route afresh (shortest hops nearest the columns, fewest crossings); -Columns 1 also re-splits the sections into columns. Usage: RepackRoutes [-Columns 1]"; }
        void RegisterArguments() noexcept override { m_hColumns = m_Parser.addOption("Columns", "Also re-partition the columns", false, 1); }
        std::string Query() noexcept override
        {
            std::string Text;
            m_View.m_Memory.m_Lanes.clear();
            m_View.m_Memory.m_Offsets.clear();
            if (GetArg(m_Parser, m_hColumns, Text) && Text != "0") { m_View.m_Memory.m_Layout.m_PartK = 0; return "routes and columns repacked"; }
            return "routes repacked";
        }
        xcmdline::parser::handle m_hColumns;
    };

    //---- StackSelfTest: the layout and router self-check (assert-based: meaningful in a build with asserts)
    struct stack_self_test_cmd : xundo::query_command_base
    {
        stack_self_test_cmd(xundo::system& System) noexcept : query_command_base(System, "StackSelfTest", nullptr) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Runs the stack layout and router self-check (column thresholds, routing tests, a fuzz); asserts on failure. Usage: StackSelfTest"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override { return stack_routes::SelfTest(); }
    };
}

#endif
#endif // XMATERIAL_STACK_ROUTES_H
