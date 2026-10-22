#ifndef XMATERIAL_STACK_ROUTES_H
#define XMATERIAL_STACK_ROUTES_H
#pragma once

// Routing of the material stack (documentation/stack_canvas.md section 5). The layered layout (xmaterial_stack_layout.h) makes every
// connection flow left to right, and gives a wire that skips columns a slot in each column it crosses. So a route is a horizontal from the
// producer's output, then per gutter it crosses one vertical (a lane) to the y of the next slot or of the consumer's input, then a horizontal
// on. Where the y already matches, the route is straight: lanes appear only where something is in the way. Nothing ever crosses a step.
// One trunk per source output per gutter; lanes are ordered to minimise crossings (nested routes nest, they never cross when they need not),
// crossings are drawn at right angles with a gap in the horizontal; junction dots only where one output splits.
// Deterministic: the same graph always gives the same routes; nothing is remembered between layouts.
//
// The core is pure: sections of step sizes and edges in, boxes and polylines out (no ImGui, no graph, no editor state). Define
// XMATERIAL_STACK_ROUTES_CORE_ONLY for the standalone self-check; otherwise the graph glue (graph -> input) follows, and the view commands
// (LayoutStats, RouteInfo, RepackRoutes, StackSelfTest) when xmaterial_stack_commands.h was included first.

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
    using edge = sl::edge;

    namespace geo
    {
        constexpr float CLEARANCE = 6.0f;   // two trunks share a lane only when their runs are at least this far apart
        constexpr float MIN_SEP   = 6.0f;   // two outputs' horizontals in one gutter at least this far apart
        constexpr float CROSS_GAP = 4.0f;   // a crossing breaks the horizontal this much on each side
    }

    struct point    { float m_X = 0, m_Y = 0; };
    struct mark     { float m_X = 0, m_Y = 0; int m_Source = 0; };
    struct segment  { float m_X0 = 0, m_Y0 = 0, m_X1 = 0, m_Y1 = 0; int m_Source = 0; };     // axis aligned, X0 <= X1, Y0 <= Y1; source indexes result::m_Sources

    struct route
    {
        std::vector<int>    m_Lanes;        // per gutter crossed: its lane (-1 = straight across)
        std::vector<point>  m_Points;       // producer pin to consumer pin; empty = not drawn (the edge breaks the order)
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
        std::vector<int>                m_Lanes;        // per gutter: its lane count
        float                           m_Length = 0, m_Ink = 0;    // sum of the route polylines; length of the wires drawn (shared runs once)
        int                             m_StepHits = 0, m_Touches = 0, m_Overlaps = 0, m_Skipped = 0;   // must all be 0
    };

    //------------------------------------------------------------------------------------------------------------------------------------
    // Segments: merging, crossings, junctions
    //------------------------------------------------------------------------------------------------------------------------------------
    inline segment Seg(float x0, float y0, float x1, float y1, int Source) noexcept { return { std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1), Source }; }
    inline bool    IsH(const segment& s) noexcept { return s.m_Y0 == s.m_Y1; }           // zero-length segments never survive Merge

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

    struct diag { int m_Crossings = 0, m_Touches = 0, m_Overlaps = 0; };

    // Crossing: a horizontal and a vertical of two outputs cross strictly inside both (drawn with a gap). Touch: one ends on the other (it
    // would read as a junction). Overlap: two outputs run collinear, or horizontals closer than MIN_SEP. An output meeting itself is a junction.
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
                    if (bSame || bPin) continue;                                         // sub-elements of one pin run together from it: no gap between them
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

    // A dot wherever a run of an output ends inside another run of the same output (the branch leaves a trunk that continues, 5.4), or crosses
    // it (a branch goes on straight through its trunk). Corners (a trunk's ends) get none, and different outputs never share a dot.
    inline std::vector<mark> Junctions(const std::vector<segment>& S) noexcept
    {
        auto Inside = [](const segment& s, float x, float y) { return IsH(s) ? y == s.m_Y0 && x > s.m_X0 && x < s.m_X1 : x == s.m_X0 && y > s.m_Y0 && y < s.m_Y1; };
        std::vector<mark> Out;
        auto Add = [&](float x, float y, int Src) { if (std::none_of(Out.begin(), Out.end(), [&](const mark& m) { return m.m_X == x && m.m_Y == y && m.m_Source == Src; })) Out.push_back({ x, y, Src }); };
        for (auto& a : S)
            for (auto& b : S)
                if (&a != &b && a.m_Source == b.m_Source)
                {
                    for (auto [x, y] : { std::pair{ a.m_X0, a.m_Y0 }, std::pair{ a.m_X1, a.m_Y1 } }) if (Inside(b, x, y)) Add(x, y, a.m_Source);
                    if (IsH(a) && !IsH(b) && Inside(a, b.m_X0, a.m_Y0) && Inside(b, b.m_X0, a.m_Y0)) Add(b.m_X0, a.m_Y0, a.m_Source);
                }
        return Out;
    }

    inline bool OnPolyline(const std::vector<point>& P, float x, float y) noexcept
    {
        for (std::size_t i = 0; i + 1 < P.size(); ++i)
            if (const auto s = Seg(P[i].m_X, P[i].m_Y, P[i + 1].m_X, P[i + 1].m_Y, 0); x >= s.m_X0 && x <= s.m_X1 && y >= s.m_Y0 && y <= s.m_Y1) return true;
        return false;
    }

    //------------------------------------------------------------------------------------------------------------------------------------
    // Gutter lanes (5.3)
    //------------------------------------------------------------------------------------------------------------------------------------
    // A trunk: one source in one gutter. Left: the y of its runs coming in from the left column; right: of its runs leaving to the right one.
    struct trunk { int m_Source = 0; std::vector<float> m_Left, m_Right; float m_Lo = 0, m_Hi = 0; int m_Lane = -1; };

    inline bool Overlap(const trunk& A, const trunk& B) noexcept { return A.m_Lo <= B.m_Hi + geo::CLEARANCE && B.m_Lo <= A.m_Hi + geo::CLEARANCE; }

    // Crossings when A's vertical is left of B's: A's runs to the right pass B's vertical, B's runs from the left pass A's. Only those can
    // cross, so the order of two trunks decides their crossings alone. Runs of two outputs on one y would merge: a heavy cost.
    inline int Cost(const trunk& A, const trunk& B, const std::vector<std::int64_t>& Pin) noexcept
    {
        int n = 0;
        for (float y : A.m_Right) n += y > B.m_Lo && y < B.m_Hi;
        for (float y : B.m_Left)  n += y > A.m_Lo && y < A.m_Hi;
        if (Pin[A.m_Source] != Pin[B.m_Source]) for (float a : A.m_Right) for (float b : B.m_Left) n += 1000 * (std::abs(a - b) < geo::MIN_SEP);
        return n;
    }

    // Left to right order: each trunk inserted where it adds the fewest crossings (top first), then neighbours swapped while that helps; then
    // lanes, each trunk one lane right of the overlapping trunks before it (the order of non-overlapping trunks changes no crossing).
    // Straight trunks (one y) need no lane. Returns the lane count.
    inline int Lanes(std::vector<trunk>& T, const std::vector<std::int64_t>& Pin) noexcept
    {
        std::vector<int> In, O;
        for (int t = 0; t < static_cast<int>(T.size()); ++t) if (T[t].m_Hi > T[t].m_Lo) In.push_back(t);
        std::sort(In.begin(), In.end(), [&](int a, int b) { return std::tuple(T[a].m_Lo, T[a].m_Hi, T[a].m_Source) < std::tuple(T[b].m_Lo, T[b].m_Hi, T[b].m_Source); });
        for (int t : In)
        {
            int BestP = 0, BestC = 1 << 30;
            for (int p = 0; p <= static_cast<int>(O.size()); ++p)
            {
                int C = 0;
                for (int k = 0; k < static_cast<int>(O.size()); ++k) C += k < p ? Cost(T[O[k]], T[t], Pin) : Cost(T[t], T[O[k]], Pin);
                if (C < BestC) { BestC = C; BestP = p; }
            }
            O.insert(O.begin() + BestP, t);
        }
        for (bool bMore = true; bMore; )
        {
            bMore = false;
            for (std::size_t k = 0; k + 1 < O.size(); ++k)
                if (Cost(T[O[k + 1]], T[O[k]], Pin) < Cost(T[O[k]], T[O[k + 1]], Pin)) { std::swap(O[k], O[k + 1]); bMore = true; }
        }
        int n = 0;
        for (std::size_t k = 0; k < O.size(); ++k)
        {
            auto& t = T[O[k]];
            t.m_Lane = 0;
            for (std::size_t j = 0; j < k; ++j) if (Overlap(T[O[j]], t)) t.m_Lane = std::max(t.m_Lane, T[O[j]].m_Lane + 1);
            n = std::max(n, t.m_Lane + 1);
        }
        return n;
    }

    inline float GutterW(int Lanes) noexcept { return std::max(sl::geo::COLUMN_MARGIN, 2 * sl::geo::ICON_CLEARANCE + std::max(0, Lanes - 1) * sl::geo::LANE_GAP); }

    //------------------------------------------------------------------------------------------------------------------------------------
    // Build: layout, lanes, X, geometry
    //------------------------------------------------------------------------------------------------------------------------------------
    inline result Build(const sl::input& In, const std::vector<edge>& E) noexcept
    {
        result R;
        R.m_Layout = sl::Layer(In, E);
        auto& L = R.m_Layout;
        const int nE = static_cast<int>(E.size());
        R.m_Sources = L.m_Sources;
        R.m_RouteSource = L.m_EdgeSource;
        R.m_Routes.resize(nE);
        R.m_SourcePin.resize(R.m_Sources.size());
        for (int i = 0; i < nE; ++i) R.m_SourcePin[R.m_RouteSource[i]] = static_cast<std::int64_t>(E[i].m_From) << 20 | E[i].m_FromRow;

        // Trunks per gutter, their lanes, the gutter widths
        const int nG = std::max(0, L.m_K - 1);
        std::vector<std::vector<trunk>> T(nG);
        std::vector<std::map<int,int>> TrunkOf(nG);                 // source -> trunk
        auto Trunks = [&](int g)
        {
            T[g].clear(); TrunkOf[g].clear();
            for (auto& l : L.m_Links[g])
            {
                auto [It, bNew] = TrunkOf[g].try_emplace(l.m_Source, static_cast<int>(T[g].size()));
                if (bNew) T[g].push_back({ l.m_Source });
                auto& t = T[g][It->second];
                const float yl = sl::PortY(L, l.m_L, l.m_LRow), yr = sl::PortY(L, l.m_R, l.m_RRow);
                if (std::find(t.m_Left.begin(),  t.m_Left.end(),  yl) == t.m_Left.end())  t.m_Left.push_back(yl);
                if (std::find(t.m_Right.begin(), t.m_Right.end(), yr) == t.m_Right.end()) t.m_Right.push_back(yr);
            }
            for (auto& t : T[g])
            {
                t.m_Lo = std::min(*std::min_element(t.m_Left.begin(), t.m_Left.end()), *std::min_element(t.m_Right.begin(), t.m_Right.end()));
                t.m_Hi = std::max(*std::max_element(t.m_Left.begin(), t.m_Left.end()), *std::max_element(t.m_Right.begin(), t.m_Right.end()));
            }
        };
        // Untangle: two outputs whose runs to the right and from the left meet on one y merge whatever their lanes (they swap rows between two
        // aligned steps, or one runs straight), or in the lane order the crossings asked for. Move the right end of one a little (MIN_SEP + 2,
        // down, else up), a few times at most.
        // ponytail: a local fix, bounded; whatever it cannot fix LayoutStats reports as merged runs
        auto Untangle = [&](int g)
        {
            Trunks(g);
            Lanes(T[g], R.m_SourcePin);
            for (auto& A : T[g])
                for (auto& B : T[g])
                    if (&A != &B && Cost(A, B, R.m_SourcePin) >= 1000 && (Cost(B, A, R.m_SourcePin) >= 1000 || (A.m_Lane >= 0 && A.m_Lane < B.m_Lane)))
                        for (auto& l : L.m_Links[g])                     // A's run to the right, B's run from the left on (about) one y
                            for (auto& m : L.m_Links[g])
                                if (l.m_Source == A.m_Source && m.m_Source == B.m_Source && std::abs(sl::PortY(L, l.m_R, l.m_RRow) - sl::PortY(L, m.m_L, m.m_LRow)) < geo::MIN_SEP)
                                { sl::Nudge(L, l.m_R, geo::MIN_SEP + 2); return true; }
            return false;
        };
        for (int Try = 0, bAny = 1; bAny && Try < 4 * nG; ) { bAny = 0; for (int g = 0; g < nG; ++g) if (Untangle(g)) { bAny = 1; ++Try; } }
        R.m_Lanes.assign(nG, 0);
        std::vector<float> W(nG);
        for (int g = 0; g < nG; ++g)
        {
            Trunks(g);
            R.m_Lanes[g] = Lanes(T[g], R.m_SourcePin);
            W[g] = GutterW(R.m_Lanes[g]);
        }
        sl::PlaceX(L, W);
        auto LaneX = [&](int g, int Lane)
        {
            const float x0 = sl::ColumnRight(L, g) + (W[g] - std::max(0, R.m_Lanes[g] - 1) * sl::geo::LANE_GAP) * 0.5f;
            return Lane < 0 ? sl::ColumnRight(L, g) + W[g] * 0.5f : x0 + Lane * sl::geo::LANE_GAP;
        };

        // Geometry: the polyline of every route, then the wires (merged per source), crossings, junctions, gaps
        std::vector<segment> All;
        for (int i = 0; i < nE; ++i)
        {
            const auto& Ch = L.m_Chain[i];
            if (Ch.empty()) { ++R.m_Skipped; continue; }
            auto& Rt = R.m_Routes[i];
            const int s = R.m_RouteSource[i], c0 = L.m_Column[E[i].m_From];
            auto Y = [&](std::size_t k) { return sl::PortY(L, Ch[k], k == 0 ? E[i].m_FromRow : k + 1 == Ch.size() ? E[i].m_ToRow : -1); };
            const auto& P = L.m_Steps[E[i].m_From];
            const auto& Q = L.m_Steps[E[i].m_To];
            std::vector<point> Pts{ { P.m_X + P.m_W, Y(0) } };
            for (std::size_t k = 0; k + 1 < Ch.size(); ++k)
            {
                const int g = c0 + static_cast<int>(k);
                const int Lane = T[g][TrunkOf[g].at(s)].m_Lane;
                Rt.m_Lanes.push_back(Lane);
                Pts.push_back({ LaneX(g, Lane), Y(k) });
                Pts.push_back({ LaneX(g, Lane), Y(k + 1) });
            }
            Pts.push_back({ Q.m_X, Y(Ch.size() - 1) });
            for (auto& p : Pts)                     // drop repeats and the middle of straight runs
            {
                auto& O = Rt.m_Points;
                if (!O.empty() && O.back().m_X == p.m_X && O.back().m_Y == p.m_Y) continue;
                if (O.size() >= 2 && ((O[O.size() - 2].m_X == O.back().m_X && O.back().m_X == p.m_X) || (O[O.size() - 2].m_Y == O.back().m_Y && O.back().m_Y == p.m_Y))) O.back() = p;
                else O.push_back(p);
            }
            for (std::size_t k = 0; k + 1 < Rt.m_Points.size(); ++k)
            {
                All.push_back(Seg(Rt.m_Points[k].m_X, Rt.m_Points[k].m_Y, Rt.m_Points[k + 1].m_X, Rt.m_Points[k + 1].m_Y, s));
                R.m_Length += std::abs(Rt.m_Points[k + 1].m_X - Rt.m_Points[k].m_X) + std::abs(Rt.m_Points[k + 1].m_Y - Rt.m_Points[k].m_Y);
            }
        }
        const auto Wires = Merge(std::move(All));
        for (auto& w : Wires) R.m_Ink += (w.m_X1 - w.m_X0) + (w.m_Y1 - w.m_Y0);
        std::vector<int> CrossSeg;
        const auto D = Classify(Wires, R.m_SourcePin, &R.m_Crossings, &CrossSeg);
        R.m_Touches = D.m_Touches; R.m_Overlaps = D.m_Overlaps;
        R.m_Junctions = Junctions(Wires);

        std::vector<std::vector<float>> Cuts(Wires.size());
        for (std::size_t k = 0; k < CrossSeg.size(); ++k) Cuts[CrossSeg[k]].push_back(R.m_Crossings[k].m_X);
        for (std::size_t s = 0; s < Wires.size(); ++s)
        {
            const auto& w = Wires[s];
            for (std::size_t v = 0; v < L.m_Steps.size(); ++v)
            {
                const auto& b = L.m_Steps[v];
                R.m_StepHits += w.m_X1 > b.m_X && w.m_X0 < b.m_X + b.m_W && w.m_Y1 > b.m_Y && w.m_Y0 < b.m_Y + b.m_H;
                const bool bOwnInput = IsH(w) && w.m_X1 == b.m_X;                                   // the literal band of a step: only its own input runs reach it
                R.m_StepHits += !bOwnInput && L.m_LitW[v] > 0 && w.m_X1 > b.m_X - L.m_LitW[v] && w.m_X0 < b.m_X && w.m_Y1 > b.m_Y && w.m_Y0 < b.m_Y + b.m_H;
            }
            std::sort(Cuts[s].begin(), Cuts[s].end());
            float x = w.m_X0;
            for (float c : Cuts[s]) { if (c - geo::CROSS_GAP > x) R.m_Wires.push_back({ x, w.m_Y0, c - geo::CROSS_GAP, w.m_Y1, w.m_Source }); x = c + geo::CROSS_GAP; }
            if (Cuts[s].empty() || x < w.m_X1) R.m_Wires.push_back({ x, w.m_Y0, w.m_X1, w.m_Y1, w.m_Source });
        }
        return R;
    }

    //------------------------------------------------------------------------------------------------------------------------------------
    // Queries (LayoutStats, RouteInfo)
    //------------------------------------------------------------------------------------------------------------------------------------
    inline std::string Stats(const result& R)
    {
        const auto& L = R.m_Layout;
        std::string Out = std::format("columns {}  width {:.0f}  height {:.0f}  routes {}  outputs {}\n", L.m_K, L.m_Width, L.m_Height, R.m_Routes.size(), R.m_Sources.size());
        for (int c = 0; c < L.m_K; ++c)
        {
            int nSteps = 0;
            for (int it : L.m_Order[c]) nSteps += L.m_Items[it].m_Step >= 0;
            Out += std::format("column {}: steps {}  passing wires {}", c, nSteps, static_cast<int>(L.m_Order[c].size()) - nSteps);
            Out += c + 1 < L.m_K ? std::format("  gutter lanes {}\n", R.m_Lanes[c]) : "\n";
        }
        Out += std::format("crossings {}  junctions {}  route length {:.0f}  ink {:.0f}  segments through steps {}  false junctions {}  merged runs {}  not drawn {}\n",
            R.m_Crossings.size(), R.m_Junctions.size(), R.m_Length, R.m_Ink, R.m_StepHits, R.m_Touches, R.m_Overlaps, R.m_Skipped);
        return Out;
    }

    inline std::string Info(const result& R, const std::vector<edge>& E, int Edge)
    {
        const auto& Rt = R.m_Routes[Edge];
        if (Rt.m_Points.empty()) return "not drawn: the connection breaks the order";
        const int c0 = R.m_Layout.m_Column[E[Edge].m_From];
        std::string Out = std::format("route: column {} -> {}:", c0, R.m_Layout.m_Column[E[Edge].m_To]);
        for (std::size_t k = 0; k < Rt.m_Lanes.size(); ++k)
            Out += Rt.m_Lanes[k] < 0 ? std::format(" gutter {} straight", c0 + k) : std::format(" gutter {} lane {}", c0 + k, Rt.m_Lanes[k]);
        const int Src = R.m_RouteSource[Edge];
        int nJ = 0, nX = 0;
        for (auto& j : R.m_Junctions) nJ += j.m_Source == Src && OnPolyline(Rt.m_Points, j.m_X, j.m_Y);
        for (auto& x : R.m_Crossings) nX += OnPolyline(Rt.m_Points, x.m_X, x.m_Y);
        Out += std::format("\njunctions {}  crossings {}  points", nJ, nX);
        for (auto& p : Rt.m_Points) Out += std::format(" ({:.0f},{:.0f})", p.m_X, p.m_Y);
        return Out + "\n";
    }

    //------------------------------------------------------------------------------------------------------------------------------------
    // Self-check (stack_canvas.md section 11): depth columns, tests 1, 3, 10 from code, the One Texture material, nesting, direct routes,
    // junction vs crossing, determinism, stability, a fuzz over random valid stacks. assert-based: run it in a build with asserts (debug, or
    // the standalone test).
    //------------------------------------------------------------------------------------------------------------------------------------
    struct test_case
    {
        sl::input           m_In;
        std::vector<edge>   m_E;
        std::vector<int>    m_nIn, m_nOut;      // per global step: input rows 0..nIn-1 on the left, output rows 0..nOut-1 on the right

        int Section(bool bOutput = false) { m_In.push_back({ m_In.size() + 1, {}, bOutput }); return static_cast<int>(m_In.size()) - 1; }
        int Step(int nIn, int nOut, float W = 180, float LitW = 0)
        {
            m_In.back().m_Steps.push_back({ 1000 + m_nIn.size(), W, std::max({ 1, nIn, nOut }), LitW });
            m_nIn.push_back(nIn); m_nOut.push_back(nOut);
            return static_cast<int>(m_nIn.size()) - 1;
        }
        void Link(int From, int Out, int To, int In, int Sub = 0)
        {
            m_E.push_back({ From, Out, To, In, static_cast<std::uint64_t>(From) << 16 | static_cast<std::uint64_t>(Out) << 4 | static_cast<std::uint64_t>(Sub), m_E.size() + 1 });
        }
    };

    // Everything the canvas relies on: nothing through a step, no false junction, no merged runs; each route from its pins, axis aligned,
    // left to right, at most two bends per gutter; consumers in later columns, inputs in column 0, outputs in the last column
    inline void CheckRoutes(const result& R, const std::vector<edge>& E, const sl::input& In)
    {
        assert(R.m_StepHits == 0 && R.m_Touches == 0 && R.m_Overlaps == 0 && R.m_Skipped == 0);
        const auto& L = R.m_Layout;
        std::vector<char> Fed(L.m_Steps.size(), 0);
        for (std::size_t i = 0; i < E.size(); ++i)
        {
            Fed[E[i].m_To] = 1;
            const auto& P = R.m_Routes[i].m_Points;
            [[maybe_unused]] const auto& A = L.m_Steps[E[i].m_From];
            [[maybe_unused]] const auto& B = L.m_Steps[E[i].m_To];
            [[maybe_unused]] const int nG = L.m_Column[E[i].m_To] - L.m_Column[E[i].m_From];
            assert(nG > 0 && P.size() >= 2 && P.size() <= 2 + 2 * static_cast<std::size_t>(nG));
            assert(P.front().m_X == A.m_X + A.m_W && P.front().m_Y == A.m_Y + sl::RowY(E[i].m_FromRow));
            assert(P.back().m_X == B.m_X && P.back().m_Y == B.m_Y + sl::RowY(E[i].m_ToRow));
            for (std::size_t k = 0; k + 1 < P.size(); ++k) assert((P[k].m_X == P[k + 1].m_X || P[k].m_Y == P[k + 1].m_Y) && P[k].m_X <= P[k + 1].m_X);
        }
        std::vector<int> First(L.m_Steps.size(), 1);                                         // a step reading nothing: right before its first consumer (else column 0)
        std::vector<char> Feeds(L.m_Steps.size(), 0);
        for (auto& e : E) { First[e.m_From] = Feeds[e.m_From] ? std::min(First[e.m_From], L.m_Column[e.m_To]) : L.m_Column[e.m_To]; Feeds[e.m_From] = 1; }
        int g = 0;
        for (auto& S : In)
            for ([[maybe_unused]] auto& s : S.m_Steps) { assert(Fed[g] || S.m_bOutput || s.m_PinColumn >= 0 || L.m_Column[g] == First[g] - 1); assert(!S.m_bOutput || L.m_Column[g] == L.m_K - 1); ++g; }
        for (std::size_t a = 0; a < L.m_Steps.size(); ++a)                                    // steps of a column never overlap
            for (std::size_t b = a + 1; b < L.m_Steps.size(); ++b)
                if (L.m_Column[a] == L.m_Column[b]) assert(L.m_Steps[a].m_Y + L.m_Steps[a].m_H <= L.m_Steps[b].m_Y || L.m_Steps[b].m_Y + L.m_Steps[b].m_H <= L.m_Steps[a].m_Y);
        for ([[maybe_unused]] auto& j : R.m_Junctions) for ([[maybe_unused]] auto& x : R.m_Crossings) assert(j.m_X != x.m_X || j.m_Y != x.m_Y);
    }

    inline bool SameRoutes(const result& A, const result& B)
    {
        if (A.m_Routes.size() != B.m_Routes.size()) return false;
        for (std::size_t i = 0; i < A.m_Routes.size(); ++i)
        {
            const auto& P = A.m_Routes[i].m_Points; const auto& Q = B.m_Routes[i].m_Points;
            if (P.size() != Q.size()) return false;
            for (std::size_t k = 0; k < P.size(); ++k) if (P[k].m_X != Q[k].m_X || P[k].m_Y != Q[k].m_Y) return false;
        }
        return true;
    }

    inline test_case RandomCase(std::mt19937& Rng, int MaxSections, int MaxSteps)
    {
        test_case C;
        const int nSec = 1 + static_cast<int>(Rng() % MaxSections);
        for (int s = 0; s < nSec; ++s)
        {
            C.Section();
            for (int n = static_cast<int>(Rng() % (MaxSteps + 1)); n > 0; --n)
                C.Step(static_cast<int>(Rng() % 4), 1 + static_cast<int>(Rng() % 2), 120.0f + static_cast<float>(Rng() % 100), Rng() % 3 ? 0.0f : 60.0f + static_cast<float>(Rng() % 60));
        }
        C.Section(true);                                                                // the output section
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

    // The One Texture example material (39C2185971104003) as the stack holds it: sections Steps, Inputs, Steps (2), Output
    inline test_case OneTexture()
    {
        test_case C;
        C.Section(); const int s1 = C.Step(0, 1, 150, 150), s2 = C.Step(0, 1, 150, 150), v2 = C.Step(2, 1, 110, 70), v4 = C.Step(4, 1, 130), v42 = C.Step(4, 1, 130);
        C.Section(); const int in = C.Step(0, 2, 170);
        C.Section(); const int t = C.Step(2, 1, 230), t2 = C.Step(2, 1, 230), m = C.Step(2, 1, 170), m2 = C.Step(2, 1, 170), a = C.Step(2, 1, 210), m3 = C.Step(2, 1, 220), v3 = C.Step(1, 1, 170);
        C.Section(true); const int o = C.Step(2, 0, 180, 70);
        C.Link(s2, 0, t, 0); C.Link(in, 1, t, 1); C.Link(s1, 0, t2, 0); C.Link(in, 1, t2, 1);
        for (int k = 0; k < 4; ++k) { C.Link(v2, 0, v4, k, 1); C.Link(v2, 0, v42, k, 2); }
        C.Link(t, 0, m, 0); C.Link(v4, 0, m, 1); C.Link(t2, 0, m2, 0); C.Link(v42, 0, m2, 1);
        C.Link(m, 0, a, 0); C.Link(m2, 0, a, 1); C.Link(in, 0, m3, 0); C.Link(a, 0, m3, 1); C.Link(m3, 0, v3, 0); C.Link(v3, 0, o, 0);
        return C;
    }

    inline std::string SelfTest(int FuzzCases = 300)
    {
        std::string Out;

        // Test 1: UV feeds 8 inputs over 3 sections: one trunk per gutter, junction dots, nothing parallel
        {
            test_case C;
            C.Section(); const int UV = C.Step(0, 1); const int a = C.Step(2, 1);
            C.Section(); const int n1 = C.Step(2, 1), n2 = C.Step(2, 1), n3 = C.Step(2, 1);
            C.Section(); const int c1 = C.Step(2, 1), c2 = C.Step(2, 1), c3 = C.Step(2, 1);
            C.Section(true); const int o = C.Step(3, 0);
            for (int t : { a, n1, n2, n3, c1, c2, c3 }) C.Link(UV, 0, t, 0);
            C.Link(UV, 0, o, 2);
            C.Link(a, 0, n1, 1); C.Link(n1, 0, n2, 1); C.Link(n2, 0, n3, 1); C.Link(n3, 0, o, 0);
            C.Link(c1, 0, c2, 1); C.Link(c2, 0, c3, 1); C.Link(c3, 0, o, 1);
            const auto R = Build(C.m_In, C.m_E);
            CheckRoutes(R, C.m_E, C.m_In);
            const int Src = static_cast<int>(std::lower_bound(R.m_Sources.begin(), R.m_Sources.end(), C.m_E[0].m_Source) - R.m_Sources.begin());
            const auto nJ = std::count_if(R.m_Junctions.begin(), R.m_Junctions.end(), [&](const mark& m) { return m.m_Source == Src; });
            assert(nJ >= 1 && R.m_Layout.m_K == 6);
            Out += std::format("test 1 (UV x8): columns {}, crossings {}, UV junctions {}, route length {:.0f}\n", R.m_Layout.m_K, R.m_Crossings.size(), nJ, R.m_Length);
        }

        // Test 3: Wetness outputs Roughness and Darken, each used twice: two trunks, never merged
        {
            test_case C;
            C.Section(); const int w = C.Step(1, 2);
            C.Section(); const int a = C.Step(2, 1), b = C.Step(2, 1);
            C.Section(); const int c = C.Step(2, 1), d = C.Step(2, 1);
            C.Section(true); const int o = C.Step(4, 0);
            C.Link(w, 0, a, 0); C.Link(w, 1, a, 1); C.Link(w, 0, c, 0); C.Link(w, 1, d, 1);
            C.Link(a, 0, b, 0); C.Link(b, 0, o, 0); C.Link(c, 0, d, 0); C.Link(d, 0, o, 1);
            const auto R = Build(C.m_In, C.m_E);
            CheckRoutes(R, C.m_E, C.m_In);
            assert(R.m_Sources.size() == 6);
            Out += std::format("test 3 (two outputs) ok: crossings {}\n", R.m_Crossings.size());
        }

        // Junction vs crossing: P1 and P2 both feed Q1 and Q2 on different inputs (K2,2: one crossing whatever the order), dots on both trunks
        {
            test_case C;
            C.Section(); const int p1 = C.Step(0, 1), p2 = C.Step(0, 1);
            C.Section(); const int q1 = C.Step(2, 1), q2 = C.Step(2, 1);
            C.Link(p1, 0, q1, 0); C.Link(p1, 0, q2, 0); C.Link(p2, 0, q1, 1); C.Link(p2, 0, q2, 1);
            const auto R = Build(C.m_In, C.m_E);
            CheckRoutes(R, C.m_E, C.m_In);
            assert(R.m_Layout.m_K == 2 && R.m_Crossings.size() == 1 && R.m_Junctions.size() >= 1);
            Out += std::format("junction vs crossing ok: junctions {}, crossings {}\n", R.m_Junctions.size(), R.m_Crossings.size());
        }

        // Nesting (owner 2026-10-21): four stacked sources feed rows of one tall step lower down, in order: the routes nest, none crosses.
        // Reversed, column 0 reorders itself. A long hop over a free column goes straight (two bends at most).
        {
            for (bool bReversed : { false, true })
            {
                test_case C;
                C.Section(); int s[4]; for (auto& x : s) x = C.Step(0, 1);
                C.Section(); const int t = C.Step(12, 1);
                for (int k = 0; k < 4; ++k) C.Link(s[k], 0, t, bReversed ? 11 - 3 * k : 3 * k);
                const auto R = Build(C.m_In, C.m_E);
                CheckRoutes(R, C.m_E, C.m_In);
                assert(R.m_Crossings.empty());
            }
            test_case C;
            C.Section(); const int x = C.Step(0, 1), a = C.Step(0, 1); const int y = C.Step(1, 1); const int w = C.Step(2, 1);
            C.Link(x, 0, y, 0); C.Link(y, 0, w, 0); C.Link(a, 0, w, 1);
            const auto R = Build(C.m_In, C.m_E);
            CheckRoutes(R, C.m_E, C.m_In);
            assert(R.m_Routes[0].m_Points.size() == 2 && R.m_Routes[1].m_Points.size() == 2 && R.m_Routes[2].m_Points.size() <= 4);
            Out += "nesting ok: no crossings in order or reversed; straight hops 2 points, a hop over a free column " + std::to_string(R.m_Routes[2].m_Points.size()) + "\n";
        }

        // The One Texture material: 7 columns (inputs, constants and samplers first, the output last), crossings at most 3 (the old canvas has
        // 4 there and one wire behind a step; two of the 3 are forced by which rows feed which)
        {
            const auto C = OneTexture();
            const auto R = Build(C.m_In, C.m_E);
            CheckRoutes(R, C.m_E, C.m_In);
            assert(R.m_Layout.m_K == 7 && R.m_Crossings.size() <= 3);
            Out += std::format("one texture ok: columns {}, crossings {}, route length {:.0f}, ink {:.0f}\n", R.m_Layout.m_K, R.m_Crossings.size(), R.m_Length, R.m_Ink);
        }

        // Owner 2026-10-22: a Vec2 feeding two multiplies sits in the Textures' column, between them (not in column 0, its wires through
        // that column). The person's place wins: a column it may take and a top, kept exactly; a column it may not take is clamped.
        {
            test_case C;
            C.Section(); const int sa = C.Step(1, 1), sb = C.Step(1, 1), in = C.Step(0, 2), v = C.Step(2, 1, 110);
            C.Section(); const int t1 = C.Step(2, 1, 230), t2 = C.Step(2, 1, 230), m1 = C.Step(2, 1), m2 = C.Step(2, 1);
            C.Section(true); const int o = C.Step(2, 0);
            C.Link(sa, 0, t1, 0); C.Link(in, 1, t1, 1); C.Link(sb, 0, t2, 0); C.Link(in, 1, t2, 1);
            C.Link(t1, 0, m1, 0); C.Link(v, 0, m1, 1, 1); C.Link(t2, 0, m2, 0); C.Link(v, 0, m2, 1, 2); C.Link(m1, 0, o, 0); C.Link(m2, 0, o, 1);
            const auto R = Build(C.m_In, C.m_E);
            CheckRoutes(R, C.m_E, C.m_In);
            const auto& L = R.m_Layout;
            [[maybe_unused]] const auto Mid = [&](int s) { return L.m_Steps[s].m_Y + L.m_Steps[s].m_H * 0.5f; };
            assert(L.m_Column[v] == L.m_Column[t1] && L.m_Column[t1] == L.m_Column[t2]);
            assert(std::min(Mid(t1), Mid(t2)) < Mid(v) && Mid(v) < std::max(Mid(t1), Mid(t2)));

            auto D = C;
            D.m_In[0].m_Steps[v].m_PinColumn = 0; D.m_In[0].m_Steps[v].m_PinY = 600;
            const auto P = Build(D.m_In, D.m_E);
            CheckRoutes(P, D.m_E, D.m_In);
            assert(P.m_Layout.m_Column[v] == 0 && P.m_Layout.m_Steps[v].m_Y == 600);
            D.m_In[0].m_Steps[v].m_PinColumn = 9;
            assert(Build(D.m_In, D.m_E).m_Layout.m_Column[v] == L.m_Column[m1] - 1);
            Out += std::format("vec2 between the textures ok: column {}; placed by hand: column 0 at 600 kept, column 9 clamped to {}\n", L.m_Column[v], L.m_Column[m1] - 1);
        }

        // Test 10: 200 steps in 12 sections; adding one unwired step moves no wired step (it waits at the bottom of column 0)
        {
            std::mt19937 Rng(10);
            test_case C;
            for (int s = 0; s < 12; ++s) { C.Section(); for (int k = 0; k < (s < 8 ? 17 : 16); ++k) C.Step(2, 1); }
            C.Section(true); C.Step(3, 0);
            const int N = static_cast<int>(C.m_nIn.size());
            for (int b = 1; b < N; ++b)
                for (int i = 0; i < C.m_nIn[b]; ++i)
                {
                    if (Rng() % 10 >= 7) continue;
                    const int a = Rng() % 10 == 0 ? static_cast<int>(Rng() % b) : std::max(0, b - 1 - static_cast<int>(Rng() % 6));
                    if (C.m_nOut[a]) C.Link(a, 0, b, i);
                }
            const auto T0 = std::chrono::steady_clock::now();
            const auto R1 = Build(C.m_In, C.m_E);
            const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
            CheckRoutes(R1, C.m_E, C.m_In);

            const int Sec = 5, At = 8;
            int Global = 0; for (int s = 0; s < Sec; ++s) Global += static_cast<int>(C.m_In[s].m_Steps.size());
            Global += At;
            auto D = C;
            D.m_In[Sec].m_Steps.insert(D.m_In[Sec].m_Steps.begin() + At, { 999999, 180, 3 });
            for (auto& e : D.m_E) { e.m_From += e.m_From >= Global; e.m_To += e.m_To >= Global; }
            const auto R2 = Build(D.m_In, D.m_E);
            CheckRoutes(R2, D.m_E, D.m_In);
            std::vector<char> Wired(N, 0);
            for (auto& e : C.m_E) Wired[e.m_From] = Wired[e.m_To] = 1;
            int Moved = 0;
            for (int g = 0; g < N; ++g)
            {
                if (!Wired[g]) continue;
                const auto& a = R1.m_Layout.m_Steps[g]; const auto& b = R2.m_Layout.m_Steps[g + (g >= Global)];
                Moved += a.m_X != b.m_X || a.m_Y != b.m_Y;
            }
            assert(Moved == 0);
            Out += std::format("test 10 (200 steps, insert): columns {}, routes {}, crossings {}, other steps moved {}, layout {:.1f} ms\n", R1.m_Layout.m_K, C.m_E.size(), R1.m_Crossings.size(), Moved, Ms);
        }

        // Fuzz: random valid stacks. Invariants and determinism (two builds agree)
        {
            std::mt19937 Rng(1234);
            int nRoutes = 0, nCross = 0;
            for (int it = 0; it < FuzzCases; ++it)
            {
                const auto C = RandomCase(Rng, 6, 6);
                const auto R = Build(C.m_In, C.m_E);
                CheckRoutes(R, C.m_E, C.m_In);
                assert(SameRoutes(R, Build(C.m_In, C.m_E)));
                nRoutes += static_cast<int>(C.m_E.size()); nCross += static_cast<int>(R.m_Crossings.size());
            }
            Out += std::format("fuzz ok: {} stacks, {} routes, {} crossings\n", FuzzCases, nRoutes, nCross);
        }
        return Out;
    }
}

#ifndef XMATERIAL_STACK_ROUTES_CORE_ONLY

#include "plugins/xmaterial.plugin/source/Graph/xmaterial_graph.h"
#include <unordered_set>

namespace xmaterial_editor::stack_routes
{
    constexpr float CHIP_W = 80.0f;     // without a canvas: room for a chip beside a connected input's name
    constexpr float LIT_W  = 90.0f;     // without a canvas: a literal field left of an unconnected input

    // The stack of a graph as router input (4.2): inputs on the left rows, outputs (the first always, the others when used) on the right rows.
    // A sub-element is no row: its routes leave from its output's row, as their own trunk. Measure(std::string) -> width in pixels;
    // InputW(node, input_pin) -> an input row's content inside the step (its name, its chip); LitW(node, input_pin) -> the literal field left
    // of the step for an unconnected input (0 for none). The canvas measures them with ImGui.
    // A collapsed USER section (7.6) is one box: an input row per distinct source outside it feeding a step inside, an output row per inside
    // output used outside; routes attach to those rows, routes inside it are not drawn. pSections replaces the stack's sections (the canvas
    // lays out the order a drag would make, to preview it).
    struct graph_input
    {
        struct box { int m_Section = 0, m_nSteps = 0; std::string m_Name; std::vector<std::uint64_t> m_In, m_Out; std::vector<std::string> m_InText, m_OutText; };
        sl::input                               m_In;
        std::vector<edge>                       m_Edges;
        std::vector<xmaterial_graph::node_guid> m_Steps;            // per global step (a box: its section's guid)
        std::vector<int>                        m_Box;              // per global step: index into m_Boxes, -1 = a step
        std::vector<box>                        m_Boxes;
        std::vector<std::string>                m_SectionNames;
    };

    constexpr float ARROW_W = 14.0f;    // the open / close arrow left of a vector output's name, and the indent of its component rows

    // A pin of O, or one of its components, feeds something
    inline bool OutputUsed(const xmaterial_graph::graph& G, const xmaterial_graph::output_pin& O) noexcept
    {
        for (auto& [Id, C] : G.m_Connections)
        {
            if (C->m_OutputPinGuid == O.m_PinGUID) return true;
            for (auto& S : O.m_SubElements) if (C->m_OutputPinGuid == S.m_PinGUID) return true;
        }
        return false;
    }

    // An output shows its components (.x .y .z .w; .r .g .b .a for a colour) as pins of their own on rows under it when one of them feeds
    // something (so a component's route always leaves from its own pin) or when the person opened it (pOpen: output pins, view state).
    inline bool ShowsComponents(const xmaterial_graph::graph& G, const xmaterial_graph::output_pin& O, const std::unordered_set<std::uint64_t>* pOpen) noexcept
    {
        if (O.m_SubElements.empty()) return false;
        if (pOpen && pOpen->contains(O.m_PinGUID.m_Value)) return true;
        for (auto& [Id, C] : G.m_Connections) for (auto& S : O.m_SubElements) if (C->m_OutputPinGuid == S.m_PinGUID) return true;
        return false;
    }

    // A component row's label: .x / .r
    inline std::string ComponentLabel(const xmaterial_graph::output_pin& O, int Sub) noexcept { return std::string(".") + xmaterial_graph::ComponentLetters(O.m_TypeGUID)[Sub & 3]; }

    // The output rows of a step, top to bottom: every output shown (the first always, the others when used), each followed by its component
    // rows when it shows them. Fn(Row, Output, Sub): Sub -1 for the output itself, else the component. Returns the number of rows.
    template<typename T_FN>
    int ForOutputRows(const xmaterial_graph::graph& G, const xmaterial_graph::node& N, const std::unordered_set<std::uint64_t>* pOpen, T_FN&& Fn) noexcept
    {
        int Row = 0;
        for (std::size_t o = 0; o < N.m_OutputPins.size(); ++o)
        {
            const auto& O = N.m_OutputPins[o];
            if (o > 0 && !OutputUsed(G, O)) continue;
            Fn(Row++, O, -1);
            if (ShowsComponents(G, O, pOpen)) for (int k = 0; k < static_cast<int>(O.m_SubElements.size()); ++k) Fn(Row++, O, k);
        }
        return Row;
    }

    inline std::string BoxTitle(const std::string& Name, int nSteps) { return std::format("{}  {} step{}", Name, nSteps, nSteps == 1 ? "" : "s"); }

    // A chip's text (cmds::ChipText): Step, Step.Output (several outputs), Step.x (a sub-element)
    inline std::string SourceText(const xmaterial_graph::graph& G, std::uint64_t Out) noexcept
    {
        auto* pN = G.findNodeByPin(xmaterial_graph::pin_guid{ Out });
        if (!pN) return "?";
        const std::string Step = pN->m_StepName.empty() ? pN->m_Name : pN->m_StepName;
        for (auto& O : pN->m_OutputPins)
        {
            const std::string Base = Step + (pN->m_OutputPins.size() > 1 ? "." + O.m_Name : "");
            if (O.m_PinGUID.m_Value == Out) return Base;
            for (auto& Sub : O.m_SubElements) if (Sub.m_PinGUID.m_Value == Out) return Base + "." + Sub.m_Name;
        }
        return "?";
    }

    // PreviewRows: rows under a step's ports its in-node preview takes (stack_canvas.md 13); no port sits there, so routes never aim at them.
    // pOpen: the outputs the person opened (ShowsComponents).
    template<typename T_MEASURE, typename T_INPUT_W, typename T_LIT_W, typename T_PREVIEW_ROWS>
    graph_input FromGraph(const xmaterial_graph::graph& G, T_MEASURE&& Measure, T_INPUT_W&& InputW, T_LIT_W&& LitW, T_PREVIEW_ROWS&& PreviewRows, const std::vector<xmaterial_graph::section>* pSections = nullptr
                         , const std::unordered_set<std::uint64_t>* pOpen = nullptr) noexcept
    {
        graph_input R;
        const auto& Sections = pSections ? *pSections : G.m_Stack.m_Sections;
        std::unordered_map<std::uint64_t, std::uint64_t> MainOf;                    // output or sub-element pin -> its output pin
        for (auto& [Id, pN] : G.m_InstanceNodes) for (auto& O : pN->m_OutputPins) { MainOf[O.m_PinGUID.m_Value] = O.m_PinGUID.m_Value; for (auto& s : O.m_SubElements) MainOf[s.m_PinGUID.m_Value] = O.m_PinGUID.m_Value; }
        std::unordered_map<std::uint64_t, std::pair<int,int>> OutRow, InRow;        // pin -> (global step, row)
        std::unordered_map<std::uint64_t, int> BoxOf;                               // a node inside a collapsed section -> its box's global step
        for (int s = 0; s < static_cast<int>(Sections.size()); ++s)
        {
            const auto& S = Sections[s];
            auto& Sec = R.m_In.emplace_back();
            Sec.m_Id = S.m_Guid.m_Value;
            Sec.m_bOutput = S.m_Kind == xmaterial_graph::section::kind::OUTPUT;
            R.m_SectionNames.push_back(S.m_Name);
            if (S.m_bCollapsed && !Sec.m_bOutput && !S.m_Steps.empty())
            {
                const int g = static_cast<int>(R.m_Steps.size());
                R.m_Steps.push_back(xmaterial_graph::node_guid{ S.m_Guid.m_Value });
                R.m_Box.push_back(static_cast<int>(R.m_Boxes.size()));
                auto& B = R.m_Boxes.emplace_back();
                B.m_Section = s; B.m_Name = S.m_Name;
                for (auto Guid : S.m_Steps) if (G.m_InstanceNodes.contains(Guid)) { BoxOf[Guid.m_Value] = g; ++B.m_nSteps; }
                Sec.m_Steps.push_back({ S.m_Guid.m_Value });                        // rows and width once its boundary is known (below)
                continue;
            }
            for (auto Guid : S.m_Steps)
            {
                auto It = G.m_InstanceNodes.find(Guid);
                if (It == G.m_InstanceNodes.end()) continue;
                const auto& N = *It->second;
                const int g = static_cast<int>(R.m_Steps.size());
                R.m_Steps.push_back(Guid);
                R.m_Box.push_back(-1);
                float InW = 0, OutW = 0, Lit = 0;
                int nIn = 0, nOut = 0;
                for (auto& P : N.m_InputPins) { InRow[P.m_PinGUID.m_Value] = { g, nIn++ }; InW = std::max(InW, InputW(N, P)); Lit = std::max(Lit, LitW(N, P)); }
                nOut = ForOutputRows(G, N, pOpen, [&](int Row, const xmaterial_graph::output_pin& P, int Sub)
                {
                    if (Sub >= 0) { OutRow[P.m_SubElements[Sub].m_PinGUID.m_Value] = { g, Row }; OutW = std::max(OutW, ARROW_W + Measure(ComponentLabel(P, Sub))); return; }
                    OutRow[P.m_PinGUID.m_Value] = { g, Row };
                    for (auto& S : P.m_SubElements) OutRow[S.m_PinGUID.m_Value] = { g, Row };       // a component's route leaves from its output's row unless it has its own (below)
                    OutW = std::max(OutW, (P.m_SubElements.empty() ? 0.0f : ARROW_W) + Measure(P.m_Name));
                });
                const float TextW = std::max(Measure(N.m_StepName.empty() ? N.m_Name : N.m_StepName + "  " + N.m_Name), InW + OutW + (nIn && nOut ? sl::geo::MID_GAP : 0));
                Sec.m_Steps.push_back({ Guid.m_Value, sl::StepW(TextW), std::max({ 1, nIn, nOut }) + PreviewRows(N), Lit });
            }
        }

        // The boundary of every box, in its steps' order: a row per source outside feeding it, a row per output of it used outside
        for (int g = 0; g < static_cast<int>(R.m_Steps.size()); ++g)
        {
            if (R.m_Box[g] < 0) continue;
            auto& B = R.m_Boxes[R.m_Box[g]];
            auto Inside = [&](const xmaterial_graph::node* p) { auto It = p ? BoxOf.find(p->m_Guid.m_Value) : BoxOf.end(); return It != BoxOf.end() && It->second == g; };
            for (auto Guid : Sections[B.m_Section].m_Steps)
            {
                auto It = G.m_InstanceNodes.find(Guid);
                if (It == G.m_InstanceNodes.end()) continue;
                for (auto& P : It->second->m_InputPins)
                {
                    auto C = G.m_Connections.find(P.m_ConnectionGUID);
                    if (!P.m_ConnectionGUID.m_Value || C == G.m_Connections.end() || C->second->m_InputPinGuid != P.m_PinGUID) continue;
                    const auto* pSrc = G.findNodeByPin(C->second->m_OutputPinGuid);
                    if (!pSrc || Inside(pSrc)) continue;
                    const std::uint64_t Src = C->second->m_OutputPinGuid.m_Value;
                    const auto k = std::find(B.m_In.begin(), B.m_In.end(), Src) - B.m_In.begin();
                    if (k == static_cast<std::ptrdiff_t>(B.m_In.size())) { B.m_In.push_back(Src); B.m_InText.push_back(SourceText(G, Src)); }
                    InRow[P.m_PinGUID.m_Value] = { g, static_cast<int>(k) };
                }
                for (auto& O : It->second->m_OutputPins)
                {
                    const bool bOut = std::any_of(G.m_Connections.begin(), G.m_Connections.end(), [&](auto& C)
                        { auto M = MainOf.find(C.second->m_OutputPinGuid.m_Value); return M != MainOf.end() && M->second == O.m_PinGUID.m_Value && !Inside(G.findNodeByPin(C.second->m_InputPinGuid)); });
                    if (!bOut) continue;
                    const int k = static_cast<int>(B.m_Out.size());
                    B.m_Out.push_back(O.m_PinGUID.m_Value); B.m_OutText.push_back(SourceText(G, O.m_PinGUID.m_Value));
                    OutRow[O.m_PinGUID.m_Value] = { g, k };
                    for (auto& Sub : O.m_SubElements) OutRow[Sub.m_PinGUID.m_Value] = { g, k };
                }
            }
            float InW = 0, OutW = 0;
            for (auto& t : B.m_InText)  InW  = std::max(InW,  Measure(t));
            for (auto& t : B.m_OutText) OutW = std::max(OutW, Measure(t));
            auto& Step = R.m_In[B.m_Section].m_Steps[0];
            Step.m_nRows = std::max({ 1, static_cast<int>(B.m_In.size()), static_cast<int>(B.m_Out.size()) });
            Step.m_W     = sl::StepW(std::max(Measure(BoxTitle(B.m_Name, B.m_nSteps)), InW + OutW + sl::geo::MID_GAP));
        }

        for (auto& [Id, C] : G.m_Connections)
        {
            auto O = OutRow.find(C->m_OutputPinGuid.m_Value);
            auto I = InRow.find(C->m_InputPinGuid.m_Value);
            if (O != OutRow.end() && I != InRow.end() && O->second.first != I->second.first)
                R.m_Edges.push_back({ O->second.first, O->second.second, I->second.first, I->second.second, C->m_OutputPinGuid.m_Value, Id.m_Value });
        }

        // Where the person put steps (the layout keeps them there)
        if (!G.m_Stack.m_Places.empty())
            for (auto& Sec : R.m_In) for (auto& s : Sec.m_Steps)
                for (auto& P : G.m_Stack.m_Places) if (P.m_Step.m_Value == s.m_Id) { s.m_PinColumn = std::max(0, P.m_Column); s.m_PinY = P.m_Y; }
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
    // View state of the stack canvas, shared by the canvas and the view commands (never saved, never a history entry): how it measures
    struct stack_view
    {
        // Text, input row and literal widths: the stack canvas sets ImGui's; until it has drawn once (or with no canvas), 7 px per character
        std::function<float(const std::string&)>    m_Measure = [](const std::string& s) { return 7.0f * static_cast<float>(s.size()); };
        std::function<float(const xmaterial_graph::node&, const xmaterial_graph::input_pin&)> m_InputW = [](const xmaterial_graph::node&, const xmaterial_graph::input_pin& P)
            { return 7.0f * static_cast<float>(P.m_Name.size()) + (P.m_ConnectionGUID.m_Value ? stack_routes::CHIP_W : 0.0f); };
        std::function<float(const xmaterial_graph::node&, const xmaterial_graph::input_pin&)> m_LitW = [](const xmaterial_graph::node& N, const xmaterial_graph::input_pin& P)
            { return !P.m_ConnectionGUID.m_Value && P.m_ParamIndex >= 0 && P.m_ParamIndex < static_cast<int>(N.m_Params.size()) ? stack_routes::LIT_W : 0.0f; };
        std::function<int(const xmaterial_graph::node&)> m_PreviewRows = [](const xmaterial_graph::node&) { return 0; };     // the canvas's in-node previews (none headless)
        std::unordered_set<std::uint64_t>                m_Open;            // the vector outputs the person opened on the canvas: their components show (ShowsComponents)
    };

    // The layout the canvas shows
    inline stack_routes::result StackLayoutOf(const graph_document& Doc, const stack_view& View, stack_routes::graph_input& In, double& Ms) noexcept
    {
        In = stack_routes::FromGraph(Doc.m_Graph, View.m_Measure, View.m_InputW, View.m_LitW, View.m_PreviewRows, nullptr, &View.m_Open);
        const auto T0 = std::chrono::steady_clock::now();
        auto R = stack_routes::Build(In.m_In, In.m_Edges);
        Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
        return R;
    }

    //---- LayoutStats: columns, lanes, crossings, route length, segments through steps
    struct layout_stats_cmd : xundo::query_command_base
    {
        graph_document& m_Doc;
        stack_view&     m_View;
        layout_stats_cmd(xundo::system& System, graph_document& Doc, stack_view& View) noexcept : query_command_base(System, "LayoutStats", nullptr), m_Doc(Doc), m_View(View) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Lays out the stack: columns (by dependency depth), steps and passing wires per column, lanes per gutter, crossings, junctions, route length, segments through steps (must be 0), layout ms. Usage: LayoutStats"; }
        // ponytail: -Width is accepted and ignored (the depth layout does not depend on the view width) so older scripts keep running
        void RegisterArguments() noexcept override { m_Parser.addOption("Width", "Ignored: the layout no longer depends on the view width", false, 1); }
        std::string Query() noexcept override
        {
            if (!m_Doc.isLoaded()) return "LayoutStats: nothing loaded";
            stack_routes::graph_input In; double Ms = 0;
            const auto R = StackLayoutOf(m_Doc, m_View, In, Ms);
            return stack_routes::Stats(R) + std::format("layout {:.2f} ms\n", Ms);
        }
    };

    //---- RouteInfo: the gutters, lanes, junctions and crossings of one connection's route
    struct route_info_cmd : xundo::query_command_base
    {
        graph_document& m_Doc;
        stack_view&     m_View;
        route_info_cmd(xundo::system& System, graph_document& Doc, stack_view& View) noexcept : query_command_base(System, "RouteInfo", nullptr), m_Doc(Doc), m_View(View) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Shows how a connection is routed: its columns, the lane it takes in every gutter (or straight), junctions and crossings on it, its points. Usage: RouteInfo -Connection hexguid"; }
        void RegisterArguments() noexcept override
        {
            m_hConnection = m_Parser.addOption("Connection", "The connection", true,  1);
            m_Parser.addOption("Width", "Ignored", false, 1);
        }
        std::string Query() noexcept override
        {
            if (!m_Doc.isLoaded()) return "RouteInfo: nothing loaded";
            std::uint64_t Id = 0;
            if (!HexArg(m_Parser, m_hConnection, Id)) return "RouteInfo: bad arguments";
            stack_routes::graph_input In; double Ms = 0;
            const auto R = StackLayoutOf(m_Doc, m_View, In, Ms);
            for (std::size_t i = 0; i < In.m_Edges.size(); ++i) if (In.m_Edges[i].m_Id == Id) return stack_routes::Info(R, In.m_Edges, static_cast<int>(i));
            return "RouteInfo: no such connection between two steps";
        }
        xcmdline::parser::handle m_hConnection;
    };

    //---- RepackRoutes: kept for scripts; the depth layout remembers no lanes, so every layout is already a full pack
    struct repack_routes_cmd : xundo::query_command_base
    {
        repack_routes_cmd(xundo::system& System, stack_view&) noexcept : query_command_base(System, "RepackRoutes", nullptr) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "View only, no effect: the stack layout is derived from the graph alone (no lane or column memory), so it is always fully packed. Usage: RepackRoutes"; }
        void RegisterArguments() noexcept override { m_Parser.addOption("Columns", "Ignored", false, 1); }
        std::string Query() noexcept override { return "nothing to repack: the layout keeps no memory"; }
    };

    //---- StackSelfTest: the layout and router self-check (assert-based: meaningful in a build with asserts)
    struct stack_self_test_cmd : xundo::query_command_base
    {
        stack_self_test_cmd(xundo::system& System) noexcept : query_command_base(System, "StackSelfTest", nullptr) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Runs the stack layout and router self-check (depth columns, routing tests, nesting, stability, a fuzz); asserts on failure. Usage: StackSelfTest"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override { return stack_routes::SelfTest(); }
    };
}

#endif
#endif // XMATERIAL_STACK_ROUTES_H
