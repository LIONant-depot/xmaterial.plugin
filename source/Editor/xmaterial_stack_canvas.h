#ifndef XMATERIAL_STACK_CANVAS_H
#define XMATERIAL_STACK_CANVAS_H
#pragma once

// The Material Stack canvas (documentation/stack_canvas.md sections 4-6, 10): the graph in columns by dependency depth, flowing left to
// right, every step placed and every route drawn by the layout and router (xmaterial_stack_layout.h, xmaterial_stack_routes.h). Drawn with
// ImDrawList, no node editor, in the old canvas's flat look: inputs on a step's left, outputs on its right, the literal of an unconnected
// input outside the step on the left (gone once connected). Right drag pans, the wheel zooms (0.5-1.5) under the cursor, Home resets.
// Routes are dimmed in their type's colour; a selected step lights its inputs and consumers, a selected route its path from the source (the
// other branches of that output at 0.75), U / D trace the selected step upstream / downstream. Literals edit through SetNodeProperty, like
// everywhere. Sections are not drawn: they are reading order, and depth columns spread a section over several columns.
// Selection, trace, pan and zoom are view state: never saved, never a command.
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_graph_widgets.h"
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_stack_commands.h"
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_stack_routes.h"
#include "dependencies/xeditor/include/xeditor/hint.h"
#include "dependencies/imgui/imgui_internal.h"

namespace xmaterial_editor
{
    namespace stack_style       // the old canvas's flat theme (xmaterial_graph_canvas.h): grey inputs half, black outputs half, kind headers
    {
        constexpr ImU32 Canvas        = IM_COL32( 30,  31,  34, 255);
        constexpr ImU32 Grid          = IM_COL32( 36,  37,  41, 255);
        constexpr ImU32 GridMajor     = IM_COL32( 42,  43,  48, 255);
        constexpr ImU32 InputsHalf    = IM_COL32( 62,  62,  64, 255);     // the old (96,96,96,128) over its node background
        constexpr ImU32 OutputsHalf   = IM_COL32( 30,  30,  31, 255);     // the old (32,32,32,128)
        constexpr ImU32 HeaderStep    = IM_COL32( 64,  64,  66, 255);     // the old headers at alpha 128: function, exposed, input, output
        constexpr ImU32 HeaderExposed = IM_COL32(114, 114,  64, 255);
        constexpr ImU32 HeaderInput   = IM_COL32( 26,  80,  28, 255);
        constexpr ImU32 HeaderOutput  = IM_COL32( 88,  38,  90, 255);
        constexpr ImU32 StepBorder    = IM_COL32( 40,  40,  40, 255);
        constexpr ImU32 Text          = IM_COL32(220, 220, 220, 255);
        constexpr ImU32 TextDim       = IM_COL32(150, 150, 154, 255);
        constexpr ImU32 Chip          = IM_COL32( 40,  40,  42, 255);
        constexpr ImU32 Literal       = IM_COL32( 70,  70,  72, 255);
        constexpr ImU32 LiteralLink   = IM_COL32( 64, 200,  64, 255);     // the old canvas's literal dot and link
        constexpr ImU32 Selected      = IM_COL32( 74, 144, 226, 255);
        constexpr ImU32 Related       = IM_COL32(140, 170, 205, 255);
        constexpr ImU32 Error         = IM_COL32(230,  72,  72, 255);

        constexpr float ROUND      = 3.5f;
        constexpr float PIN_R      = 4.5f;
        constexpr float ROW_GAP    = 8.0f;      // between an input's name and its chip
        constexpr float CHIP_PAD   = 4.0f;
        constexpr float CHIP_H     = 15.0f;
        constexpr float NUM_W      = 48.0f;
        constexpr float BUTTON_MIN = 64.0f, BUTTON_MAX = 120.0f;
        constexpr float LIT_PAD    = 4.0f;      // inside a literal's box
        constexpr float LIT_LINK   = 16.0f;     // from a literal's box to its pin
        constexpr float DIM = 0.55f, DIM_TRACE = 0.15f, SIBLING = 0.75f, HOVER = 0.9f;     // route alphas (5.6)
        constexpr float WIRE_W = 1.5f, LIT_W = 2.5f;                                        // route widths, screen px
        constexpr float ZOOM_MIN = 0.5f, ZOOM_MAX = 1.5f;
        constexpr float PICK = 5.0f;                                                        // a click this close to a route picks it, screen px
    }

    class stack_canvas
    {
    public:
        enum class trace : std::uint8_t { NONE, UP, DOWN };

        stack_canvas(graph_document& Doc, xundo::system& Undo, cmds::stack_view& View) noexcept : m_Doc(Doc), m_Undo(Undo), m_View(View)
        {
            m_View.m_Measure = [this](const std::string& s) { return Measure(s); };
            m_View.m_InputW  = [this](const xmaterial_graph::node& N, const xmaterial_graph::input_pin& P) { return InputW(N, P); };
            m_View.m_LitW    = [this](const xmaterial_graph::node& N, const xmaterial_graph::input_pin& P) { return LitW(N, P); };
        }
        stack_canvas(const stack_canvas&) = delete;

        std::uint64_t SelectedNode() const noexcept { return m_SelStep; }

        // View state, also driven by the view commands (SelectStep, TraceRoute)
        void Select(std::uint64_t Step, std::uint64_t Connection) noexcept { m_SelStep = Step; m_SelConn = Step ? 0 : Connection; if (!Step) m_Trace = trace::NONE; }
        void Trace(trace T) noexcept { m_Trace = m_SelStep ? T : trace::NONE; }
        bool HasStep(std::uint64_t Id) const noexcept { return m_StepIndex.contains(Id); }
        bool HasRoute(std::uint64_t Id) const noexcept { const int e = EdgeOf(Id); return e >= 0 && !m_R.m_Routes[e].m_Points.empty(); }

        // Where a step or a route is on screen in the last frame drawn (what a script clicks)
        std::string Where(std::uint64_t Step, std::uint64_t Connection) const noexcept
        {
            if (auto It = m_StepIndex.find(Step); It != m_StepIndex.end())
            {
                const auto& b = m_R.m_Layout.m_Steps[It->second];
                const ImVec2 a = ToScreen(b.m_X, b.m_Y);
                return std::format("screen {:.0f} {:.0f} {:.0f} {:.0f}", a.x, a.y, b.m_W * m_Zoom, b.m_H * m_Zoom);
            }
            if (const int e = EdgeOf(Connection); e >= 0 && m_R.m_Routes[e].m_Points.size() >= 2)
            {
                const auto& P = m_R.m_Routes[e].m_Points;                       // the middle of its longest segment
                std::size_t k = 0;
                auto SegLen = [&](std::size_t i) { return std::abs(P[i + 1].m_X - P[i].m_X) + std::abs(P[i + 1].m_Y - P[i].m_Y); };
                for (std::size_t i = 1; i + 1 < P.size(); ++i) if (SegLen(i) > SegLen(k)) k = i;
                const ImVec2 a = ToScreen((P[k].m_X + P[k + 1].m_X) * 0.5f, (P[k].m_Y + P[k + 1].m_Y) * 0.5f);
                return std::format("screen {:.0f} {:.0f}", a.x, a.y);
            }
            return "not on the canvas";
        }

        void Render() noexcept
        {
            namespace st = stack_style;
            m_pFont    = ImGui::GetFont();
            m_FontSize = ImGui::GetFontSize();
            m_Origin   = ImGui::GetCursorScreenPos();
            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            if (Avail.x < 16 || Avail.y < 16) return;
            const ImVec2 Max{ m_Origin.x + Avail.x, m_Origin.y + Avail.y };
            Relayout();
            if (m_SelStep && !m_StepIndex.contains(m_SelStep)) Select(0, 0);           // an undo or a delete took it
            if (m_SelConn && EdgeOf(m_SelConn) < 0) m_SelConn = 0;

            // View: wheel zoom anchored under the cursor (E27 NodeOS_UI_Canvas.h:537-550), right drag pans (615-636), Home resets
            auto& IO = ImGui::GetIO();
            const bool bInside = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(m_Origin, Max);
            if (bInside && IO.MouseWheel != 0)
            {
                const ImVec2 W = ToWorld(IO.MousePos);
                m_Zoom = std::clamp(std::round((m_Zoom + (IO.MouseWheel > 0 ? 0.1f : -0.1f)) * 10.0f) / 10.0f, st::ZOOM_MIN, st::ZOOM_MAX);
                m_Pan  = { IO.MousePos.x - m_Origin.x - W.x * m_Zoom, IO.MousePos.y - m_Origin.y - W.y * m_Zoom };
            }
            if (bInside && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { m_bPanning = true; m_PanTravel = 0; }
            if (m_bPanning)
            {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) { m_Pan.x += IO.MouseDelta.x; m_Pan.y += IO.MouseDelta.y; m_PanTravel += std::abs(IO.MouseDelta.x) + std::abs(IO.MouseDelta.y); }
                else
                {
                    m_bPanning = false;
                    if (m_PanTravel < 3 && m_HoverStep) { Select(m_HoverStep, 0); ImGui::OpenPopup("##StepMenu"); }      // a right click, not a drag
                }
            }
            if (ImGui::IsWindowFocused() && !IO.WantTextInput)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_U, false)) Trace(m_Trace == trace::UP   ? trace::NONE : trace::UP);
                if (ImGui::IsKeyPressed(ImGuiKey_D, false)) Trace(m_Trace == trace::DOWN ? trace::NONE : trace::DOWN);
                if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) { m_Zoom = 1; m_Pan = {}; }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { if (m_Trace != trace::NONE) m_Trace = trace::NONE; else Select(0, 0); }
            }

            ImDrawList* D = ImGui::GetWindowDrawList();
            D->PushClipRect(m_Origin, Max, true);
            DrawBackground(D, Max);

            // The background catches clicks on empty space and on routes; steps and widgets submitted later win the overlap
            ImGui::SetCursorScreenPos(m_Origin);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##StackBackground", Avail);
            m_HoverEdge = ImGui::IsItemHovered() ? RouteAt(IO.MousePos) : -1;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) Select(0, m_HoverEdge >= 0 ? m_In.m_Edges[m_HoverEdge].m_Id : 0);

            const auto Lit = Lighting();
            DrawRoutes(D, Lit);
            m_HoverStep = 0;
            for (int g = 0; g < static_cast<int>(m_In.m_Steps.size()); ++g) DrawStep(D, g, Max);

            D->PopClipRect();
            DrawStepMenu();
            ImGui::SetCursorScreenPos(m_Origin);
            ImGui::Dummy(Avail);                                                        // the last item: the cursor moves never extend the window
        }

    private:
        using node = xmaterial_graph::node;

        //------------------------------------------------------------------------------------------
        // Measuring (layout units = screen pixels at zoom 1)
        //------------------------------------------------------------------------------------------
        float Measure(const std::string& Text) const noexcept
        {
            if (!m_pFont) return 7.0f * static_cast<float>(Text.size());
            return m_pFont->CalcTextSizeA(m_FontSize, FLT_MAX, 0.0f, Text.c_str()).x;
        }

        const xmaterial_graph::connection* ConnectionOf(const xmaterial_graph::input_pin& P) const noexcept
        {
            auto It = m_Doc.m_Graph.m_Connections.find(P.m_ConnectionGUID);
            return P.m_ConnectionGUID.m_Value && It != m_Doc.m_Graph.m_Connections.end() ? It->second.get() : nullptr;
        }

        static bool HasParam(const node& N, const xmaterial_graph::input_pin& P) noexcept { return P.m_ParamIndex >= 0 && P.m_ParamIndex < static_cast<int>(N.m_Params.size()); }

        // An input row inside the step: its name, then the chip of its source when connected
        float ChipW(const xmaterial_graph::input_pin& P) const noexcept
        {
            auto* pC = ConnectionOf(P);
            return pC ? Measure(cmds::ChipText(m_Doc.m_Graph, pC->m_OutputPinGuid)) + 2 * stack_style::CHIP_PAD : 0.0f;
        }
        float InputW(const node&, const xmaterial_graph::input_pin& P) const noexcept { const float C = ChipW(P); return Measure(P.m_Name) + (C > 0 ? stack_style::ROW_GAP + C : 0.0f); }

        // The literal left of an unconnected input (the old canvas's): a box with a label and the widget, then a link to the pin. 0 = none.
        static std::string LitLabel(const node& N, const xmaterial_graph::input_pin& P) noexcept
        {
            using type = xmaterial_graph::node_param::type;
            const auto T = N.m_Params[P.m_ParamIndex].m_Type;
            return T == type::TEXTURE_RESOURCE ? "Rsc" : T == type::FILE ? "File" : P.m_Name.substr(0, 1);
        }
        float WidgetW(const node& N, const xmaterial_graph::input_pin& P) const noexcept
        {
            namespace st = stack_style;
            const auto& Param = N.m_Params[P.m_ParamIndex];
            using type = xmaterial_graph::node_param::type;
            switch (Param.m_Type)
            {
            case type::FLOAT: case type::INT: return st::NUM_W;
            case type::TEXTURE_RESOURCE:      return std::clamp(Measure(graph_widgets::TextureLabel(Param)) + 16, st::BUTTON_MIN, st::BUTTON_MAX);
            case type::FILE:                  return std::clamp(Measure(graph_widgets::FileLabel(Param)) + 16, st::BUTTON_MIN, st::BUTTON_MAX);
            default:                          return 0;
            }
        }
        float LitW(const node& N, const xmaterial_graph::input_pin& P) const noexcept
        {
            namespace st = stack_style;
            if (ConnectionOf(P) || !HasParam(N, P) || WidgetW(N, P) == 0) return 0;
            return st::LIT_PAD + Measure(LitLabel(N, P)) + 4 + WidgetW(N, P) + st::LIT_PAD + st::LIT_LINK;
        }

        //------------------------------------------------------------------------------------------
        // Layout: rebuilt only when what it reads changes (the stack, the rows, the widths, the edges); the view width plays no part (4)
        //------------------------------------------------------------------------------------------
        static bool Same(const stack_routes::graph_input& A, const stack_routes::graph_input& B) noexcept
        {
            if (A.m_In.size() != B.m_In.size() || A.m_Edges.size() != B.m_Edges.size() || A.m_SectionNames != B.m_SectionNames) return false;
            for (std::size_t s = 0; s < A.m_In.size(); ++s)
            {
                const auto& a = A.m_In[s].m_Steps; const auto& b = B.m_In[s].m_Steps;
                if (A.m_In[s].m_Id != B.m_In[s].m_Id || A.m_In[s].m_bOutput != B.m_In[s].m_bOutput || a.size() != b.size()) return false;
                for (std::size_t i = 0; i < a.size(); ++i) if (a[i].m_Id != b[i].m_Id || a[i].m_W != b[i].m_W || a[i].m_nRows != b[i].m_nRows || a[i].m_LitW != b[i].m_LitW) return false;
            }
            for (std::size_t i = 0; i < A.m_Edges.size(); ++i)
            {
                const auto& a = A.m_Edges[i]; const auto& b = B.m_Edges[i];
                if (a.m_From != b.m_From || a.m_FromRow != b.m_FromRow || a.m_To != b.m_To || a.m_ToRow != b.m_ToRow || a.m_Source != b.m_Source || a.m_Id != b.m_Id) return false;
            }
            return true;
        }

        void Relayout() noexcept
        {
            auto In = stack_routes::FromGraph(m_Doc.m_Graph, m_View.m_Measure, m_View.m_InputW, m_View.m_LitW);
            if (m_bLaidOut && Same(In, m_In)) return;
            m_In = std::move(In); m_bLaidOut = true;
            m_R  = stack_routes::Build(m_In.m_In, m_In.m_Edges);

            auto& G = m_Doc.m_Graph;
            m_StepIndex.clear();
            for (int g = 0; g < static_cast<int>(m_In.m_Steps.size()); ++g) m_StepIndex[m_In.m_Steps[g].m_Value] = g;
            m_SourceColor.assign(m_R.m_Sources.size(), IM_COL32(200, 200, 200, 255));
            for (std::size_t s = 0; s < m_R.m_Sources.size(); ++s)
                if (auto* pN = G.findNodeByPin(xmaterial_graph::pin_guid{ m_R.m_Sources[s] }))
                {
                    bool bIn; int i, Sub;
                    if (auto* pPin = G.findPinConst(*pN, xmaterial_graph::pin_guid{ m_R.m_Sources[s] }, bIn, i, Sub)) m_SourceColor[s] = TypeColor(pPin->m_TypeGUID);
                }
            m_SourceWires.assign(m_R.m_Sources.size(), {});
            for (int w = 0; w < static_cast<int>(m_R.m_Wires.size()); ++w) m_SourceWires[m_R.m_Wires[w].m_Source].push_back(w);
        }

        int EdgeOf(std::uint64_t Connection) const noexcept
        {
            if (!Connection) return -1;
            for (int e = 0; e < static_cast<int>(m_In.m_Edges.size()); ++e) if (m_In.m_Edges[e].m_Id == Connection) return e;
            return -1;
        }

        ImU32 TypeColor(const xmaterial_graph::type_guid& Type) const noexcept
        {
            const auto* p = m_Doc.m_Graph.findType(Type);
            return p ? ImGui::ColorConvertFloat4ToU32({ p->m_Color.r, p->m_Color.g, p->m_Color.b, 1.0f }) : IM_COL32(200, 200, 200, 255);
        }

        static ImU32 Alpha(ImU32 C, float A) noexcept { return (C & ~IM_COL32_A_MASK) | (static_cast<ImU32>(((C >> IM_COL32_A_SHIFT) & 0xFF) * A) << IM_COL32_A_SHIFT); }

        //------------------------------------------------------------------------------------------
        // View transform (E27 ToScreen/ToScreenLen, NodeOS_UI_Canvas.h:552-553, anchored at the left of column 0)
        //------------------------------------------------------------------------------------------
        ImVec2 ToScreen(float x, float y) const noexcept { return { m_Origin.x + m_Pan.x + x * m_Zoom, m_Origin.y + m_Pan.y + y * m_Zoom }; }
        ImVec2 ToWorld(ImVec2 p) const noexcept { return { (p.x - m_Origin.x - m_Pan.x) / m_Zoom, (p.y - m_Origin.y - m_Pan.y) / m_Zoom }; }
        float  Len(float l) const noexcept { return l * m_Zoom; }
        float  FontPx() const noexcept { return m_FontSize * m_Zoom; }
        static float Snap(float v) noexcept { return std::floor(v) + 0.5f; }       // a thin line on a pixel centre reads crisp

        void Text(ImDrawList* D, ImVec2 At, ImU32 C, const std::string& S) const noexcept { D->AddText(m_pFont, FontPx(), At, C, S.c_str()); }
        float TextW(const std::string& S) const noexcept { return Measure(S) * m_Zoom; }

        //------------------------------------------------------------------------------------------
        // Drawing
        //------------------------------------------------------------------------------------------
        void DrawBackground(ImDrawList* D, ImVec2 Max) const noexcept
        {
            namespace st = stack_style;
            D->AddRectFilled(m_Origin, Max, st::Canvas);
            constexpr float Step = 32.0f;                                               // world units; every 4th line is a major one
            const ImVec2 W0 = ToWorld(m_Origin), W1 = ToWorld(Max);
            for (int i = static_cast<int>(std::floor(W0.x / Step)); i <= static_cast<int>(std::ceil(W1.x / Step)); ++i)
            {
                const float x = Snap(ToScreen(i * Step, 0).x);
                D->AddLine({ x, m_Origin.y }, { x, Max.y }, i % 4 ? st::Grid : st::GridMajor);
            }
            for (int i = static_cast<int>(std::floor(W0.y / Step)); i <= static_cast<int>(std::ceil(W1.y / Step)); ++i)
            {
                const float y = Snap(ToScreen(0, i * Step).y);
                D->AddLine({ m_Origin.x, y }, { Max.x, y }, i % 4 ? st::Grid : st::GridMajor);
            }
        }

        // Per edge: 0 = dimmed, else the alpha it is lit with (5.6)
        std::vector<float> Lighting() noexcept
        {
            namespace st = stack_style;
            const int nE = static_cast<int>(m_In.m_Edges.size());
            std::vector<float> Lit(nE, 0.0f);
            m_InTrace.clear();
            const auto Sel = m_StepIndex.find(m_SelStep);
            if (Sel != m_StepIndex.end() && m_Trace != trace::NONE)
            {
                // the transitive closure: what feeds the step (up) or what it feeds (down); every edge inside it lies on such a path
                m_InTrace.assign(m_In.m_Steps.size(), 0);
                m_InTrace[Sel->second] = 1;
                const bool bUp = m_Trace == trace::UP;
                for (bool bMore = true; bMore; )
                {
                    bMore = false;
                    for (auto& e : m_In.m_Edges)
                    {
                        const int Near = bUp ? e.m_To : e.m_From, Far = bUp ? e.m_From : e.m_To;
                        if (m_InTrace[Near] && !m_InTrace[Far]) { m_InTrace[Far] = 1; bMore = true; }
                    }
                }
                for (int e = 0; e < nE; ++e) if (m_InTrace[m_In.m_Edges[e].m_From] && m_InTrace[m_In.m_Edges[e].m_To]) Lit[e] = 1;
            }
            else if (Sel != m_StepIndex.end())
            {
                for (int e = 0; e < nE; ++e) if (m_In.m_Edges[e].m_From == Sel->second || m_In.m_Edges[e].m_To == Sel->second) Lit[e] = 1;
            }
            else if (const int Edge = EdgeOf(m_SelConn); Edge >= 0)
            {
                for (int e = 0; e < nE; ++e) if (m_In.m_Edges[e].m_Source == m_In.m_Edges[Edge].m_Source) Lit[e] = st::SIBLING;
                Lit[Edge] = 1;
            }
            if (m_HoverEdge >= 0 && Lit[m_HoverEdge] < st::HOVER) Lit[m_HoverEdge] = st::HOVER;
            return Lit;
        }

        // An axis aligned world segment in screen space: on a pixel centre, the ends pushed out half the width so corners close
        void Line(ImDrawList* D, float x0, float y0, float x1, float y1, ImU32 C, float Th) const noexcept
        {
            ImVec2 a = ToScreen(std::min(x0, x1), std::min(y0, y1)), b = ToScreen(std::max(x0, x1), std::max(y0, y1));
            const float e = Th * 0.5f;
            if (y0 == y1) { a.x -= e; b.x += e; a.y = b.y = Snap(a.y); }
            else          { a.y -= e; b.y += e; a.x = b.x = Snap(a.x); }
            D->AddLine(a, b, C, Th);
        }

        // One route's path, drawn from its output's wires so the crossing gaps stay
        void DrawPath(ImDrawList* D, int e, ImU32 C, float Th) const noexcept
        {
            const auto& P = m_R.m_Routes[e].m_Points;
            for (std::size_t k = 0; k + 1 < P.size(); ++k)
            {
                const auto s = stack_routes::Seg(P[k].m_X, P[k].m_Y, P[k + 1].m_X, P[k + 1].m_Y, 0);
                const bool bH = s.m_Y0 == s.m_Y1;
                for (int w : m_SourceWires[m_R.m_RouteSource[e]])
                {
                    const auto& W = m_R.m_Wires[w];
                    if (bH != (W.m_Y0 == W.m_Y1)) continue;
                    if (bH  && W.m_Y0 == s.m_Y0) if (const float a = std::max(W.m_X0, s.m_X0), b = std::min(W.m_X1, s.m_X1); a < b) Line(D, a, s.m_Y0, b, s.m_Y0, C, Th);
                    if (!bH && W.m_X0 == s.m_X0) if (const float a = std::max(W.m_Y0, s.m_Y0), b = std::min(W.m_Y1, s.m_Y1); a < b) Line(D, s.m_X0, a, s.m_X0, b, C, Th);
                }
            }
        }

        void DrawRoutes(ImDrawList* D, const std::vector<float>& Lit) const noexcept
        {
            namespace st = stack_style;
            const float Dim = m_InTrace.empty() ? st::DIM : st::DIM_TRACE;
            for (auto& W : m_R.m_Wires) Line(D, W.m_X0, W.m_Y0, W.m_X1, W.m_Y1, Alpha(m_SourceColor[W.m_Source], Dim), st::WIRE_W);
            for (auto& J : m_R.m_Junctions) D->AddCircleFilled(ToScreen(J.m_X, J.m_Y), 2.5f, Alpha(m_SourceColor[J.m_Source], Dim));

            for (float Level : { st::SIBLING, st::HOVER, 1.0f })                       // brightest on top
                for (int e = 0; e < static_cast<int>(Lit.size()); ++e)
                {
                    if (Lit[e] != Level || m_R.m_Routes[e].m_Points.empty()) continue;
                    const ImU32 C = Alpha(m_SourceColor[m_R.m_RouteSource[e]], Level);
                    DrawPath(D, e, C, Level == 1.0f ? st::LIT_W : st::WIRE_W + 0.5f);
                    for (auto& J : m_R.m_Junctions)
                        if (J.m_Source == m_R.m_RouteSource[e] && stack_routes::OnPolyline(m_R.m_Routes[e].m_Points, J.m_X, J.m_Y)) D->AddCircleFilled(ToScreen(J.m_X, J.m_Y), 3.5f, C);
                }
        }

        // The route under the mouse: the nearest drawn segment within PICK screen pixels
        int RouteAt(ImVec2 Mouse) const noexcept
        {
            int Best = -1;
            float BestD = stack_style::PICK;
            for (int e = 0; e < static_cast<int>(m_R.m_Routes.size()); ++e)
            {
                const auto& P = m_R.m_Routes[e].m_Points;
                for (std::size_t k = 0; k + 1 < P.size(); ++k)
                {
                    const ImVec2 a = ToScreen(P[k].m_X, P[k].m_Y), b = ToScreen(P[k + 1].m_X, P[k + 1].m_Y);
                    const float dx = std::max({ std::min(a.x, b.x) - Mouse.x, 0.0f, Mouse.x - std::max(a.x, b.x) });
                    const float dy = std::max({ std::min(a.y, b.y) - Mouse.y, 0.0f, Mouse.y - std::max(a.y, b.y) });
                    if (const float d = std::max(dx, dy); d < BestD) { BestD = d; Best = e; }
                }
            }
            return Best;
        }

        void Pin(ImDrawList* D, ImVec2 C, ImU32 Color, bool bConnected) const noexcept      // the old canvas's: filled when connected
        {
            const float r = std::max(2.5f, Len(stack_style::PIN_R));
            if (bConnected) D->AddCircleFilled(C, r, Color);
            else            { D->AddCircleFilled(C, r, stack_style::OutputsHalf); D->AddCircle(C, r, Color, 0, 1.5f); }
        }

        void DrawStep(ImDrawList* D, int g, ImVec2 ViewMax) noexcept
        {
            namespace st = stack_style;
            namespace geo = stack_layout::geo;
            auto* pN = m_Doc.FindNode(m_In.m_Steps[g].m_Value);
            if (!pN) return;
            auto& N = *pN;
            const auto& b = m_R.m_Layout.m_Steps[g];
            const ImVec2 P0 = ToScreen(b.m_X, b.m_Y), P1 = ToScreen(b.m_X + b.m_W, b.m_Y + b.m_H);
            if (P1.x < m_Origin.x || P1.y < m_Origin.y || P0.x - Len(m_R.m_Layout.m_LitW[g]) > ViewMax.x || P0.y > ViewMax.y) return;       // culled

            const bool bSelected = N.m_Guid.m_Value == m_SelStep;
            const int  SelEdge   = EdgeOf(m_SelConn);
            const bool bRelated  = SelEdge >= 0 && (m_In.m_Edges[SelEdge].m_From == g || m_In.m_Edges[SelEdge].m_To == g);
            const float R = Len(st::ROUND);

            // Box, flat: the header in the kind's colour, the inputs half grey, the outputs half black (the old canvas)
            float InW = 0;
            for (auto& P : N.m_InputPins) InW = std::max(InW, InputW(N, P));
            const float HeaderB = P0.y + Len(geo::HEADER_H);
            const float Split   = N.m_InputPins.empty() ? P0.x : N.m_OutputPins.empty() ? P1.x : std::min(P1.x, P0.x + Len(geo::PORT_PAD + InW + geo::MID_GAP * 0.5f));
            const bool bExposed = std::ranges::any_of(N.m_Params, [](auto& P) { return P.m_bExpose || P.m_ParamID != 0; });
            D->AddRectFilled(P0, { P1.x, HeaderB }, N.isInputNode() ? st::HeaderInput : N.isOutputNode() ? st::HeaderOutput : bExposed ? st::HeaderExposed : st::HeaderStep, R, ImDrawFlags_RoundCornersTop);
            if (Split > P0.x) D->AddRectFilled({ P0.x, HeaderB }, { Split, P1.y }, st::InputsHalf, R, ImDrawFlags_RoundCornersBottomLeft);
            if (Split < P1.x) D->AddRectFilled({ Split, HeaderB }, P1, st::OutputsHalf, R, ImDrawFlags_RoundCornersBottomRight);

            // Header: the step's name, its type dimmed when it says something more
            const float ty = P0.y + (Len(geo::HEADER_H) - FontPx()) * 0.5f;
            const std::string Name = cmds::StepLabel(N);
            Text(D, { P0.x + Len(geo::PORT_PAD), ty }, st::Text, Name);
            if (cmds::NoSpace(Name) != cmds::NoSpace(N.m_Name)) Text(D, { P1.x - Len(geo::PORT_PAD) - TextW(N.m_Name), ty }, st::TextDim, N.m_Name);

            // Selection: the whole box is the step's button (widgets drawn later win the overlap)
            ImGui::PushID(static_cast<int>(N.m_Guid.m_Value ^ (N.m_Guid.m_Value >> 32)));
            ImGui::SetCursorScreenPos(P0);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##Step", { std::max(1.0f, P1.x - P0.x), std::max(1.0f, P1.y - P0.y) });
            if (ImGui::IsItemHovered()) m_HoverStep = N.m_Guid.m_Value;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) Select(N.m_Guid.m_Value, 0);
            if (N.m_HasErrMsg && !N.m_ErrMsg.empty() && ImGui::IsItemHovered()) xeditor::hint::Text("%s", N.m_ErrMsg.c_str());

            // Input rows (left): pin on the left edge, name, the chip of its source; an unconnected input's literal outside, left of its pin
            const float FrameH = std::min(Len(geo::ROW_H - 3), FontPx() + Len(3));
            int Row = 0;
            ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * m_Zoom);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { Len(4), std::max(0.0f, (FrameH - FontPx()) * 0.5f) });
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Len(2));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, st::OutputsHalf);
            for (auto& P : N.m_InputPins)
            {
                const float cy = P0.y + Len(stack_layout::RowY(Row++));
                const auto* pC = ConnectionOf(P);
                Text(D, { P0.x + Len(geo::PORT_PAD), cy - FontPx() * 0.5f }, pC ? st::Text : st::TextDim, P.m_Name);
                if (pC)
                {
                    const std::string Chip = cmds::ChipText(m_Doc.m_Graph, pC->m_OutputPinGuid);
                    const float x = P0.x + Len(geo::PORT_PAD + Measure(P.m_Name) + st::ROW_GAP);
                    const ImVec2 C0{ x, cy - Len(st::CHIP_H) * 0.5f }, C1{ x + Len(ChipW(P)), cy + Len(st::CHIP_H) * 0.5f };
                    D->AddRectFilled(C0, C1, st::Chip, Len(2));
                    Text(D, { C0.x + Len(st::CHIP_PAD), cy - FontPx() * 0.5f }, st::TextDim, Chip);
                }
                else if (const float LW = LitW(N, P); LW > 0)
                {
                    using type = xmaterial_graph::node_param::type;
                    const int iParam = P.m_ParamIndex;
                    const ImVec2 B0{ P0.x - Len(LW), cy - FrameH * 0.5f - Len(1) }, B1{ P0.x - Len(st::LIT_LINK), cy + FrameH * 0.5f + Len(1) };
                    D->AddRectFilled(B0, B1, st::Literal, Len(2));
                    D->AddRect(B0, B1, st::StepBorder, Len(2));
                    Text(D, { B0.x + Len(st::LIT_PAD), cy - FontPx() * 0.5f }, st::TextDim, LitLabel(N, P));
                    const float W = Len(WidgetW(N, P));
                    ImGui::SetCursorScreenPos({ B1.x - Len(st::LIT_PAD) - W, cy - FrameH * 0.5f });
                    switch (N.m_Params[iParam].m_Type)
                    {
                    case type::FLOAT: case type::INT:
                        ImGui::SetNextItemWidth(W);
                        graph_widgets::NumberDrag(m_Undo, N, iParam, "##" + P.m_Name, m_Before);
                        break;
                    case type::TEXTURE_RESOURCE: if (auto H = graph_widgets::TextureButton(m_Undo, N, iParam, { W, FrameH }); !H.empty()) xeditor::hint::Text("%s", H.c_str()); break;
                    case type::FILE:             if (auto H = graph_widgets::FileButton(m_Undo, N, iParam, { W, FrameH }); !H.empty()) xeditor::hint::Text("%s", H.c_str()); break;
                    default: break;
                    }
                    D->AddLine({ B1.x, cy }, { P0.x, cy }, st::LiteralLink, std::max(1.0f, Len(1.5f)));
                    D->AddCircleFilled({ B1.x, cy }, std::max(2.0f, Len(3.0f)), st::LiteralLink);
                }
                Pin(D, { P0.x, cy }, TypeColor(P.m_TypeGUID), pC != nullptr);
            }
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            ImGui::PopFont();
            ImGui::PopID();

            // Output rows (right; the first always, the others when used): name right aligned, pin on the right edge
            Row = 0;
            for (std::size_t o = 0; o < N.m_OutputPins.size(); ++o)
            {
                const auto& Op = N.m_OutputPins[o];
                const bool bUsed = IsUsed(Op);
                if (o > 0 && !bUsed) continue;
                const float cy = P0.y + Len(stack_layout::RowY(Row++));
                Text(D, { P1.x - Len(geo::PORT_PAD) - TextW(Op.m_Name), cy - FontPx() * 0.5f }, bUsed ? st::Text : st::TextDim, Op.m_Name);
                Pin(D, { P1.x, cy }, TypeColor(Op.m_TypeGUID), bUsed);
            }

            // Border last: crisp over everything inside
            const ImU32 Border = N.m_HasErrMsg ? st::Error : bSelected ? st::Selected : bRelated ? st::Related : st::StepBorder;
            if (bSelected) D->AddRect({ P0.x - 3, P0.y - 3 }, { P1.x + 3, P1.y + 3 }, Alpha(st::Selected, 0.25f), R + 3, 0, 3.0f);
            D->AddRect(P0, P1, Border, R, 0, bSelected || N.m_HasErrMsg ? 2.0f : bRelated ? 1.5f : 1.0f);
            if (!m_InTrace.empty() && !m_InTrace[g]) D->AddRectFilled({ P0.x - Len(m_R.m_Layout.m_LitW[g]) - 1, P0.y - 1 }, { P1.x + 3, P1.y + 4 }, Alpha(st::Canvas, 0.6f), R);       // outside the trace
        }

        bool IsUsed(const xmaterial_graph::output_pin& Op) const noexcept
        {
            for (auto& [Id, C] : m_Doc.m_Graph.m_Connections)
            {
                if (C->m_OutputPinGuid == Op.m_PinGUID) return true;
                for (auto& Sub : Op.m_SubElements) if (C->m_OutputPinGuid == Sub.m_PinGUID) return true;
            }
            return false;
        }

        void DrawStepMenu() noexcept
        {
            if (!ImGui::BeginPopup("##StepMenu")) return;
            if (auto* pN = m_Doc.FindNode(m_SelStep))
            {
                ImGui::TextDisabled("%s", cmds::StepLabel(*pN).c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Trace upstream",   "U", m_Trace == trace::UP))   Trace(m_Trace == trace::UP   ? trace::NONE : trace::UP);
                if (ImGui::MenuItem("Trace downstream", "D", m_Trace == trace::DOWN)) Trace(m_Trace == trace::DOWN ? trace::NONE : trace::DOWN);
            }
            ImGui::EndPopup();
        }

        graph_document&                             m_Doc;
        xundo::system&                              m_Undo;
        cmds::stack_view&                           m_View;

        // the layout of the last change
        stack_routes::graph_input                   m_In;
        stack_routes::result                        m_R;
        bool                                        m_bLaidOut = false;
        std::unordered_map<std::uint64_t, int>      m_StepIndex;        // node guid -> global step
        std::vector<ImU32>                          m_SourceColor;      // per source output: its type's colour
        std::vector<std::vector<int>>               m_SourceWires;      // per source output: its wires

        // view state
        ImFont*                                     m_pFont    = nullptr;
        float                                       m_FontSize = 13.0f;
        ImVec2                                      m_Origin   = {};
        ImVec2                                      m_Pan      = {};
        float                                       m_Zoom     = 1.0f;
        bool                                        m_bPanning = false;
        float                                       m_PanTravel = 0;
        std::uint64_t                               m_SelStep  = 0, m_SelConn = 0, m_HoverStep = 0;
        int                                         m_HoverEdge = -1;
        trace                                       m_Trace    = trace::NONE;
        std::vector<char>                           m_InTrace;          // per global step, while tracing: inside the closure
        std::string                                 m_Before;           // the value of the widget being dragged, when the drag began
    };

    namespace cmds
    {
        //---- SelectStep: view only, a step or a route of the Material Stack selected (its routes lit), as a click does
        struct select_step_cmd : xundo::query_command_base
        {
            graph_document& m_Doc;
            stack_canvas&   m_Canvas;
            select_step_cmd(xundo::system& System, graph_document& Doc, stack_canvas& Canvas) noexcept : query_command_base(System, "SelectStep", nullptr), m_Doc(Doc), m_Canvas(Canvas) { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "View only: selects a step (its input and consumer routes lit) or a connection (its route lit) on the Material Stack, or nothing (no argument); replies where it is on screen. Usage: SelectStep [-Node hexguid|@StepName | -Connection hexguid]"; }
            void RegisterArguments() noexcept override
            {
                m_hNode       = m_Parser.addOption("Node",       "The step", false, 1);
                m_hConnection = m_Parser.addOption("Connection", "The connection", false, 1);
            }
            std::string Query() noexcept override
            {
                std::string Text, Err;
                std::uint64_t Id = 0;
                if (GetArg(m_Parser, m_hNode, Text))
                {
                    if (!ResolveNodeText(m_Doc, Text, Id, &Err)) return "SelectStep: " + Err;
                    if (!m_Canvas.HasStep(Id)) return "SelectStep: not a step on the canvas";
                    m_Canvas.Select(Id, 0);
                    return "selected " + StepLabel(*m_Doc.FindNode(Id)) + "  " + m_Canvas.Where(Id, 0);
                }
                if (!GetArg(m_Parser, m_hConnection, Text)) { m_Canvas.Select(0, 0); return "nothing selected"; }
                if (!HexArg(m_Parser, m_hConnection, Id)) return "SelectStep: bad -Connection";
                if (!m_Canvas.HasRoute(Id)) return "SelectStep: no route drawn for that connection";
                m_Canvas.Select(0, Id);
                return "selected the route  " + m_Canvas.Where(0, Id);
            }
            xcmdline::parser::handle m_hNode, m_hConnection;
        };

        //---- TraceRoute: view only, what feeds a step (-Up) or what it feeds (-Down) lit, the rest dimmed
        struct trace_route_cmd : xundo::query_command_base
        {
            graph_document& m_Doc;
            stack_canvas&   m_Canvas;
            trace_route_cmd(xundo::system& System, graph_document& Doc, stack_canvas& Canvas) noexcept : query_command_base(System, "TraceRoute", nullptr), m_Doc(Doc), m_Canvas(Canvas) { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "View only: selects a step and lights everything that feeds it (-Up) or that it feeds (-Down) on the Material Stack, dimming the rest. Usage: TraceRoute -Node hexguid|@StepName -Direction up|down, or TraceRoute -Direction off"; }
            void RegisterArguments() noexcept override
            {
                m_hNode      = m_Parser.addOption("Node",      "The step", false, 1);
                m_hDirection = m_Parser.addOption("Direction", "up: what feeds it, down: what it feeds, off: stop tracing", true, 1);
            }
            std::string Query() noexcept override
            {
                std::string Dir, Text, Err;
                std::uint64_t Id = 0;
                GetArg(m_Parser, m_hDirection, Dir);
                Dir = Lower(Dir);
                if (Dir == "off") { m_Canvas.Trace(stack_canvas::trace::NONE); return "trace off"; }
                if (Dir != "up" && Dir != "down") return "TraceRoute: -Direction is up, down or off";
                if (!GetArg(m_Parser, m_hNode, Text) || !ResolveNodeText(m_Doc, Text, Id, &Err)) return "TraceRoute: " + (Err.empty() ? std::string("-Node is needed") : Err);
                if (!m_Canvas.HasStep(Id)) return "TraceRoute: not a step on the canvas";
                m_Canvas.Select(Id, 0);
                m_Canvas.Trace(Dir == "up" ? stack_canvas::trace::UP : stack_canvas::trace::DOWN);
                return "tracing " + StepLabel(*m_Doc.FindNode(Id)) + (Dir == "up" ? " upstream" : " downstream");
            }
            xcmdline::parser::handle m_hNode, m_hDirection;
        };
    }
}

#endif // XMATERIAL_STACK_CANVAS_H
