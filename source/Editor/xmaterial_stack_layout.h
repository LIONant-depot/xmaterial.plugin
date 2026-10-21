#ifndef XMATERIAL_STACK_LAYOUT_H
#define XMATERIAL_STACK_LAYOUT_H
#pragma once

// Layout of the material stack (documentation/stack_canvas.md section 4): pass A (step boxes), B (sections into columns), C (Y), E (X).
// Pure: the input is ordered sections of step sizes, the output is boxes. No ImGui, no graph, no editor state; the caller measures text.
// The router (xmaterial_stack_routes.h) runs pass D between B and C/E and feeds its lane counts back into PassC and PassE; Build here is
// the layout with base gaps (no routes).

#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace xmaterial_editor::stack_layout
{
    namespace geo       // port geometry from E27 NodeOS_CanvasSupport.h:230-257, plus the stack's section frames
    {
        constexpr float HEADER_H         = 26.0f;
        constexpr float ROW_H            = 20.0f;
        constexpr float NODE_PAD_BOTTOM  = 12.0f;
        constexpr float NODE_GAP         = 28.0f;
        constexpr float TOP              = 10.0f;
        constexpr float PORT_PAD         = 14.0f;
        constexpr float ICON_CLEARANCE   = 16.0f;
        constexpr float LANE_GAP         = 14.0f;
        constexpr float COLUMN_MARGIN    = 60.0f;
        constexpr float MARGIN           = 20.0f;   // left and right of the whole stack
        constexpr float MIN_STEP_W       = 160.0f;
        constexpr float SECTION_HEADER_H = 28.0f;   // the section's name; holds its top slot
        constexpr float SECTION_FOOTER   = 6.0f;
        constexpr float SECTION_GAP      = 24.0f;
        constexpr float FRAME_PAD        = 8.0f;    // a section frame reaches this far past the column width
        constexpr float HYSTERESIS       = 24.0f;   // the column count changes only once the width is this far past a threshold
        constexpr float REPACK_BUDGET    = 1.25f;   // a kept partition may make a column this much taller than the optimum
    }

    // Pass A: a step box. Rows: one per input, one per used output (the first output always). TextW = its widest header or row content.
    inline float StepW (float TextW) noexcept { return std::max(geo::MIN_STEP_W, TextW + 2 * geo::PORT_PAD); }
    inline float StepH (int nRows)   noexcept { return geo::HEADER_H + nRows * geo::ROW_H + geo::NODE_PAD_BOTTOM; }
    inline float RowY  (int Row)     noexcept { return geo::HEADER_H + (Row + 0.5f) * geo::ROW_H; }       // a row's ports, from the step's top

    struct step_in      { std::uint64_t m_Id = 0; float m_W = geo::MIN_STEP_W; int m_nRows = 1; };
    struct section_in   { std::uint64_t m_Id = 0; std::vector<step_in> m_Steps; };
    using  input        = std::vector<section_in>;      // in order, OUTPUT last; a step's global index is its position in the concatenation

    struct memory       // view state (4.3, 4.4): never saved
    {
        int                         m_K     = 0;        // the column count the width rule picked last time (hysteresis)
        int                         m_PartK = 0;        // m_Breaks was made for this many columns
        std::vector<std::uint64_t>  m_Breaks;           // id of the first section of every column after the first
        float                       m_Tallest = 0;      // its tallest column (base heights) when it was last used
    };

    struct box { float m_X = 0, m_Y = 0, m_W = 0, m_H = 0; };

    struct result
    {
        int                             m_K      = 1;
        float                           m_CW     = 0, m_Width = 0, m_Height = 0;
        std::vector<int>                m_SectionColumn;    // per section
        std::vector<std::vector<int>>   m_ColumnSteps;      // per column: global step indices, top to bottom
        std::vector<int>                m_Column, m_Pos;    // per global step: its column, its position in that column
        std::vector<box>                m_Steps;            // per global step
        std::vector<box>                m_Sections;         // per section: its frame
        std::vector<std::vector<float>> m_GapTop;           // per column, per gap (0 = above the first step, g = below step g-1): top of its channel band
        std::vector<float>              m_X;                // per column: centre
    };

    inline float ColumnWidth(const input& In) noexcept
    {
        float CW = geo::MIN_STEP_W;
        for (auto& S : In) for (auto& s : S.m_Steps) CW = std::max(CW, s.m_W);
        return CW;
    }

    // Columns that fit Width with no lanes: 2*MARGIN + K*(CW + 2*ICON_CLEARANCE) + (K-1)*COLUMN_MARGIN <= Width, clamped to 1..sections
    inline int Fit(float Width, float CW, int nSections) noexcept
    {
        const int K = static_cast<int>(std::floor((Width - 2 * geo::MARGIN + geo::COLUMN_MARGIN) / (CW + 2 * geo::ICON_CLEARANCE + geo::COLUMN_MARGIN)));
        return std::clamp(K, 1, std::max(1, nSections));
    }

    // Pass B's column count with hysteresis: it moves only when Width is HYSTERESIS past a threshold, so resize jitter never flips columns
    inline int ChooseK(float Width, float CW, int nSections, memory& M) noexcept
    {
        int K = Fit(Width, CW, nSections);
        if (M.m_K > 0)
        {
            const int Prev = std::clamp(M.m_K, 1, std::max(1, nSections));
            const int Up   = Fit(Width - geo::HYSTERESIS, CW, nSections);
            const int Down = Fit(Width + geo::HYSTERESIS, CW, nSections);
            K = Up > Prev ? Up : Down < Prev ? Down : Prev;
        }
        return M.m_K = K;
    }

    // A section's height with base gaps: header, steps each with the gap below it, footer, the gap to the next section
    inline float SectionH(const section_in& S) noexcept
    {
        float H = geo::SECTION_HEADER_H + geo::SECTION_FOOTER + geo::SECTION_GAP + (S.m_Steps.empty() ? geo::NODE_GAP : 0);
        for (auto& s : S.m_Steps) H += StepH(s.m_nRows) + geo::NODE_GAP;
        return H;
    }

    // Linear partition (DP, O(K n^2)): K contiguous runs, none empty, minimising the tallest. Returns the first section of each run.
    inline std::vector<int> Partition(const std::vector<float>& H, int K) noexcept
    {
        const int n = static_cast<int>(H.size());
        std::vector<float> Sum(n + 1, 0);
        for (int i = 0; i < n; ++i) Sum[i + 1] = Sum[i] + H[i];
        std::vector<std::vector<float>> Best(K + 1, std::vector<float>(n + 1, 1e30f));     // Best[k][i]: tallest run when the first i sections make k runs
        std::vector<std::vector<int>>   From(K + 1, std::vector<int>(n + 1, 0));            // where that k-th run starts
        Best[0][0] = 0;
        for (int k = 1; k <= K; ++k)
            for (int i = k; i <= n; ++i)
                for (int j = k - 1; j < i; ++j)
                    if (const float v = std::max(Best[k - 1][j], Sum[i] - Sum[j]); v < Best[k][i]) { Best[k][i] = v; From[k][i] = j; }
        std::vector<int> Starts(K, 0);
        for (int k = K, i = n; k > 0; --k) { Starts[k - 1] = From[k][i]; i = From[k][i]; }
        return Starts;
    }

    // Pass B: the column of every section. The remembered partition is kept while K is the same, its break sections still exist in order,
    // and its tallest column is no taller than REPACK_BUDGET times the optimum or than it was last time (shrinking never re-partitions):
    // adding a step or collapsing a section never moves other sections.
    inline std::vector<int> Columns(const input& In, int K, const memory& M, bool bRepartition = false) noexcept
    {
        const int n = static_cast<int>(In.size());
        if (n == 0) return {};
        std::vector<float> H(n);
        for (int s = 0; s < n; ++s) H[s] = SectionH(In[s]);
        auto Tallest = [&](const std::vector<int>& Starts)
        {
            float T = 0;
            for (int k = 0; k < static_cast<int>(Starts.size()); ++k)
            {
                float C = 0;
                for (int s = Starts[k]; s < (k + 1 < static_cast<int>(Starts.size()) ? Starts[k + 1] : n); ++s) C += H[s];
                T = std::max(T, C);
            }
            return T;
        };

        std::vector<int> Starts = Partition(H, K);
        if (!bRepartition && M.m_PartK == K && static_cast<int>(M.m_Breaks.size()) == K - 1)
        {
            std::vector<int> Kept{ 0 };
            for (auto Id : M.m_Breaks)
            {
                auto It = std::find_if(In.begin(), In.end(), [&](const section_in& S) { return S.m_Id == Id; });
                const int s = static_cast<int>(It - In.begin());
                if (It == In.end() || s <= Kept.back()) { Kept.clear(); break; }
                Kept.push_back(s);
            }
            if (!Kept.empty() && Tallest(Kept) <= std::max(geo::REPACK_BUDGET * Tallest(Starts), M.m_Tallest)) Starts = std::move(Kept);
        }

        std::vector<int> Col(n, 0);
        for (int k = 0; k < K; ++k)
            for (int s = Starts[k]; s < (k + 1 < K ? Starts[k + 1] : n); ++s) Col[s] = k;
        return Col;
    }

    inline void Remember(const input& In, const std::vector<int>& Col, int K, memory& M) noexcept
    {
        M.m_PartK = K;
        M.m_Breaks.clear();
        for (int s = 1; s < static_cast<int>(In.size()); ++s) if (Col[s] != Col[s - 1]) M.m_Breaks.push_back(In[s].m_Id);
        std::vector<float> H(K, 0);
        for (int s = 0; s < static_cast<int>(In.size()); ++s) H[Col[s]] += SectionH(In[s]);
        M.m_Tallest = In.empty() ? 0 : *std::max_element(H.begin(), H.end());
    }

    // Pass C (port of E27 spine_layout + GapRelY, NodeOS_UI_Canvas.h:98-141, with the channel term): per column, the band of gap 0, then per
    // section its header, each step followed by the band of the gap below it, the footer, SECTION_GAP. A band is NODE_GAP + (lanes-1)*LANE_GAP
    // tall (ChannelLanes[c][g], missing = 0), so channel lanes never reach a step. Offset[c] (missing = 0) shifts a whole column down.
    inline result PassC(const input& In, const std::vector<int>& Col, int K, const std::vector<std::vector<int>>& ChannelLanes = {}, const std::vector<float>& Offset = {}) noexcept
    {
        result R;
        R.m_K = K;
        R.m_CW = ColumnWidth(In);
        R.m_SectionColumn = Col;
        R.m_ColumnSteps.resize(K);
        R.m_GapTop.resize(K);
        R.m_Sections.resize(In.size());
        int nSteps = 0;
        for (auto& S : In) nSteps += static_cast<int>(S.m_Steps.size());
        R.m_Column.resize(nSteps); R.m_Pos.resize(nSteps); R.m_Steps.resize(nSteps);

        std::vector<int> First(In.size() + 1, 0);
        for (std::size_t s = 0; s < In.size(); ++s) First[s + 1] = First[s] + static_cast<int>(In[s].m_Steps.size());

        for (int c = 0; c < K; ++c)
        {
            auto Band = [&](int g)
            {
                const int L = c < static_cast<int>(ChannelLanes.size()) && g < static_cast<int>(ChannelLanes[c].size()) ? ChannelLanes[c][g] : 0;
                return geo::NODE_GAP + std::max(0, L - 1) * geo::LANE_GAP;
            };
            float y = geo::TOP + (c < static_cast<int>(Offset.size()) ? Offset[c] : 0);
            auto& Gaps = R.m_GapTop[c];
            auto& Steps = R.m_ColumnSteps[c];
            Gaps.push_back(y);
            y += Band(0);
            for (std::size_t s = 0; s < In.size(); ++s)
            {
                if (Col[s] != c) continue;
                const float Top = y;
                y += geo::SECTION_HEADER_H;
                for (std::size_t i = 0; i < In[s].m_Steps.size(); ++i)
                {
                    const int g = First[s] + static_cast<int>(i);
                    const auto& St = In[s].m_Steps[i];
                    R.m_Column[g] = c;
                    R.m_Pos[g]    = static_cast<int>(Steps.size());
                    R.m_Steps[g]  = { 0, y, St.m_W, StepH(St.m_nRows) };
                    Steps.push_back(g);
                    y += R.m_Steps[g].m_H;
                    Gaps.push_back(y);
                    y += Band(static_cast<int>(Steps.size()));
                }
                if (In[s].m_Steps.empty()) y += geo::NODE_GAP;                 // the empty section's slot
                y += geo::SECTION_FOOTER;
                R.m_Sections[s] = { 0, Top, 0, y - Top };
                y += geo::SECTION_GAP;
            }
            R.m_Height = std::max(R.m_Height, y);
        }
        return R;
    }

    // How far a column side reaches from its centre: half the column, the icon clearance, then one LANE_GAP per highway lane past the first
    inline float Extent(const result& R, int Lanes) noexcept { return R.m_CW * 0.5f + geo::ICON_CLEARANCE + std::max(0, Lanes - 1) * geo::LANE_GAP; }

    // Pass E (port of E27 Pass C, NodeOS_UI_Canvas.h:479-518, left to right instead of root-outward): column centres from the lane extents
    // of the gutter between them (LanesL/LanesR[c], missing = 0); steps centred on their column.
    inline void PassE(result& R, const std::vector<int>& LanesL = {}, const std::vector<int>& LanesR = {}) noexcept
    {
        auto L  = [&](int c) { return c < static_cast<int>(LanesL.size()) ? LanesL[c] : 0; };
        auto Rr = [&](int c) { return c < static_cast<int>(LanesR.size()) ? LanesR[c] : 0; };
        R.m_X.assign(R.m_K, 0);
        R.m_X[0] = geo::MARGIN + Extent(R, L(0));
        for (int c = 0; c + 1 < R.m_K; ++c) R.m_X[c + 1] = R.m_X[c] + Extent(R, Rr(c)) + geo::COLUMN_MARGIN + Extent(R, L(c + 1));
        R.m_Width = R.m_X[R.m_K - 1] + Extent(R, Rr(R.m_K - 1)) + geo::MARGIN;
        for (std::size_t g = 0; g < R.m_Steps.size(); ++g) R.m_Steps[g].m_X = R.m_X[R.m_Column[g]] - R.m_Steps[g].m_W * 0.5f;
        for (std::size_t s = 0; s < R.m_Sections.size(); ++s)
        {
            R.m_Sections[s].m_X = R.m_X[R.m_SectionColumn[s]] - R.m_CW * 0.5f - geo::FRAME_PAD;
            R.m_Sections[s].m_W = R.m_CW + 2 * geo::FRAME_PAD;
        }
    }

    // X of a highway lane (E27 HighwayX, line 521): lane 0 nearest the column
    inline float HighwayX(const result& R, int Column, int Side, int Lane) noexcept      // Side: 0 = left of the column, 1 = right
    {
        const float D = R.m_CW * 0.5f + geo::ICON_CLEARANCE + Lane * geo::LANE_GAP;
        return Side ? R.m_X[Column] + D : R.m_X[Column] - D;
    }

    // Passes A..E with base gaps (no routes): columns, partition memory, hysteresis. The router's Build adds the lanes.
    inline result Build(const input& In, float Width, memory& M, bool bRepartition = false) noexcept
    {
        const float CW = ColumnWidth(In);
        for (int K = ChooseK(Width, CW, static_cast<int>(In.size()), M); ; --K)
        {
            auto Col = Columns(In, K, M, bRepartition);
            auto R   = PassC(In, Col, K);
            PassE(R);
            if (R.m_Width <= Width + geo::HYSTERESIS || K == 1) { Remember(In, Col, K, M); return R; }      // the slack keeps ChooseK's hysteresis
        }
    }
}

#endif
