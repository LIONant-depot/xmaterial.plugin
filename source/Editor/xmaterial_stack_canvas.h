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
// Source chips (section 6): hover lights the route and the source, click selects the connection, double click / Enter goes to the source
// (Alt+Left / Mouse4 back), right click opens Go to source / Find all uses / Replace source / Disconnect, Tab cycles the chips of a step.
// Inserting (section 7): a (+) on the selected route and the route menu insert on that connection; a drag from a pin onto a pin connects,
// into empty space opens a search of what fits (a ghost of the new step, a port chooser when several fit); the background menu adds a step.
// Every edit is a command (one undo step); nothing is wired without an explicit pick.
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_graph_widgets.h"
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_stack_commands.h"
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_stack_routes.h"
#include "dependencies/xeditor/include/xeditor/hint.h"
#include "dependencies/imgui/imgui_internal.h"

#include <limits>
#include <optional>
#include <unordered_set>

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
        constexpr ImU32 ChipHover     = IM_COL32( 58,  58,  62, 255);
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
        constexpr float PLUS_R = 8.0f;                                                      // the (+) on a selected route, screen px
        constexpr float GHOST = 0.6f;                                                       // a new step's preview alpha
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

        // Where a step (and its pins) or a route (its middle, where its (+) sits, and its chip) is on screen in the last frame drawn (what a script clicks)
        std::string Where(std::uint64_t Step, std::uint64_t Connection) const noexcept
        {
            if (auto It = m_StepIndex.find(Step); It != m_StepIndex.end())
            {
                const auto& b = m_R.m_Layout.m_Steps[It->second];
                const ImVec2 a = ToScreen(b.m_X, b.m_Y);
                std::string Out = std::format("screen {:.0f} {:.0f} {:.0f} {:.0f}", a.x, a.y, b.m_W * m_Zoom, b.m_H * m_Zoom);
                auto Pins = [&](const auto& List, const char* pSide)
                {
                    for (auto& P : List)
                        if (auto Pa = m_PinAt.find(P.m_PinGUID.m_Value); Pa != m_PinAt.end())
                        {
                            const ImVec2 s = ToScreen(Pa->second.m_W.x, Pa->second.m_W.y);
                            Out += std::format("  {}:{}={:.0f},{:.0f}", pSide, P.m_Name, s.x, s.y);
                        }
                };
                if (auto* pN = m_Doc.FindNode(Step)) { Pins(pN->m_InputPins, "in"); Pins(pN->m_OutputPins, "out"); }
                return Out;
            }
            if (const int e = EdgeOf(Connection); e >= 0 && m_R.m_Routes[e].m_Points.size() >= 2)
            {
                const ImVec2 m = RouteMid(e), a = ToScreen(m.x, m.y);
                std::string Out = std::format("screen {:.0f} {:.0f}", a.x, a.y);
                if (auto C = m_ChipAt.find(Connection); C != m_ChipAt.end()) Out += std::format("  chip {:.0f} {:.0f}", C->second.x, C->second.y);
                return Out;
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
            m_Avail = Avail;
            Relayout();
            if (m_SelStep && !m_StepIndex.contains(m_SelStep)) Select(0, 0);           // an undo or a delete took it
            if (m_SelConn && EdgeOf(m_SelConn) < 0) m_SelConn = 0;
            m_HoverChip = m_HoverChipNext; m_HoverChipNext = 0;                         // what the last frame's chips saw (lighting is drawn before them)
            const bool bPopup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

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
                    if (m_PanTravel < 3)                                                // a right click, not a drag: the menu of what is under it
                    {
                        if      (m_HoverChip)      { Select(0, m_HoverChip); m_Open = "##ChipMenu"; }
                        else if (m_HoverStep)      { Select(m_HoverStep, 0); m_Open = "##StepMenu"; }
                        else if (m_HoverEdge >= 0) { Select(0, m_In.m_Edges[m_HoverEdge].m_Id); m_Open = "##RouteMenu"; }
                        else                       { m_MenuAt = ToWorld(IO.MousePos); m_Open = "##CanvasMenu"; }
                    }
                }
            }
            if (bInside && ImGui::IsMouseClicked(3)) Back();                           // Mouse4
            if (ImGui::IsWindowFocused() && !IO.WantTextInput)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_U, false)) Trace(m_Trace == trace::UP   ? trace::NONE : trace::UP);
                if (ImGui::IsKeyPressed(ImGuiKey_D, false)) Trace(m_Trace == trace::DOWN ? trace::NONE : trace::DOWN);
                if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) { m_Zoom = 1; m_Pan = {}; }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { if (m_Trace != trace::NONE) m_Trace = trace::NONE; else Select(0, 0); }
                if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) FocusChip(IO.KeyShift ? -1 : 1);
                if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && m_SelConn) GoToSource(m_SelConn);
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && m_SelConn) Disconnect(m_SelConn);
                if (IO.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) Back();
            }

            // A drag from a pin: onto a pin of the other side connects (refused with the reason), into empty space opens the search of what fits
            if (bInside && !bPopup && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) if (auto* p = PinAt(IO.MousePos)) { m_DragPin = p->first; m_DragFrom = IO.MousePos; }
            if (m_DragPin && !m_PinAt.contains(m_DragPin)) m_DragPin = 0;               // an undo took it
            if (m_DragPin && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const auto  From = m_PinAt.at(m_DragPin);
                const auto* pTo  = PinAt(IO.MousePos);
                if (pTo && pTo->second.m_bOut != From.m_bOut)
                {
                    const std::uint64_t Out = From.m_bOut ? m_DragPin : pTo->first, In = From.m_bOut ? pTo->first : m_DragPin;
                    if (auto Why = WhyNot(Out, In); !Why.empty()) Note(Why, ToWorld(IO.MousePos));
                    else xeditor::Run(m_Undo, std::format("Connect -Output {:016X} -Input {:016X} -Connection {:016X}", Out, In, xresource::guid_generator::Type64()));
                }
                else if (!pTo && std::abs(IO.MousePos.x - m_DragFrom.x) + std::abs(IO.MousePos.y - m_DragFrom.y) > 8)
                    OpenSearch(From.m_bOut ? search::kind::FROM_OUTPUT : search::kind::FROM_INPUT, ToWorld(IO.MousePos), m_DragPin);
                m_DragPin = 0;
            }

            ImDrawList* D = ImGui::GetWindowDrawList();
            D->PushClipRect(m_Origin, Max, true);
            ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);                        // Tab cycles chips here, not the literals' widgets
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
            m_ChipAt.clear();
            for (int g = 0; g < static_cast<int>(m_In.m_Steps.size()); ++g) DrawStep(D, g, Max);
            DrawInsertButton(D);
            DrawDrag(D);
            DrawGhost(D);
            if (ImGui::GetTime() < m_NoteUntil) DrawNote(D, m_NoteAt, m_NoteText);

            D->PopClipRect();
            ImGui::PopItemFlag();
            DrawPopups();
            ImGui::SetCursorScreenPos(m_Origin);
            ImGui::Dummy(Avail);                                                        // the last item: the cursor moves never extend the window
        }

    private:
        using node = xmaterial_graph::node;
        struct pin_at  { ImVec2 m_W; std::uint64_t m_Node = 0; bool m_bOut = false, m_bSub = false; };     // a drawn pin, world units
        struct visit   { std::uint64_t m_Step = 0, m_Conn = 0; ImVec2 m_Pan; };                           // a navigation history entry

        //------------------------------------------------------------------------------------------
        // Measuring (layout units = screen pixels at zoom 1)
        //------------------------------------------------------------------------------------------
        float Measure(const std::string& Text) const noexcept
        {
            if (!m_pFont) return 7.0f * static_cast<float>(Text.size());
            return m_pFont->CalcTextSizeA(m_FontSize, FLT_MAX, 0.0f, Text.c_str()).x;
        }

        // The connection feeding an input: only one that ends at this input and leaves from a pin of a node. A stale m_ConnectionGUID (an
        // old file's, naming a connection between pins that no longer exist) is not one: the input shows its literal, as on the old canvas.
        const xmaterial_graph::connection* ConnectionOf(const xmaterial_graph::input_pin& P) const noexcept
        {
            auto& G = m_Doc.m_Graph;
            auto It = G.m_Connections.find(P.m_ConnectionGUID);
            if (!P.m_ConnectionGUID.m_Value || It == G.m_Connections.end()) return nullptr;
            const auto& C = *It->second;
            return C.m_InputPinGuid == P.m_PinGUID && G.findNodeByPin(C.m_OutputPinGuid) ? &C : nullptr;
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
            m_In = std::move(In); m_bLaidOut = true; m_HoverEdge = -1;                 // an edge index of the old layout
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

            // Every pin drawn, in world units (the rows DrawStep draws); a sub-element sits on its output's pin
            m_PinAt.clear();
            for (int g = 0; g < static_cast<int>(m_In.m_Steps.size()); ++g)
            {
                auto* pN = m_Doc.FindNode(m_In.m_Steps[g].m_Value);
                if (!pN) continue;
                const auto& b = m_R.m_Layout.m_Steps[g];
                int Row = 0;
                for (auto& P : pN->m_InputPins) m_PinAt[P.m_PinGUID.m_Value] = { { b.m_X, b.m_Y + stack_layout::RowY(Row++) }, pN->m_Guid.m_Value, false };
                Row = 0;
                for (std::size_t o = 0; o < pN->m_OutputPins.size(); ++o)
                {
                    const auto& Op = pN->m_OutputPins[o];
                    if (o > 0 && !IsUsed(Op)) continue;
                    const ImVec2 W{ b.m_X + b.m_W, b.m_Y + stack_layout::RowY(Row++) };
                    m_PinAt[Op.m_PinGUID.m_Value] = { W, pN->m_Guid.m_Value, true };
                    for (auto& Sub : Op.m_SubElements) m_PinAt[Sub.m_PinGUID.m_Value] = { W, pN->m_Guid.m_Value, true, true };
                }
            }
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
            if (const int e = EdgeOf(m_HoverChip); e >= 0) Lit[e] = 1.0f;              // a hovered chip reveals its route
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
                    // The chip of its source (6): an item over the step's button, so it takes the hover and the clicks
                    const std::uint64_t Id = pC->m_Guid.m_Value;
                    const std::string Chip = cmds::ChipText(m_Doc.m_Graph, pC->m_OutputPinGuid);
                    const float x = P0.x + Len(geo::PORT_PAD + Measure(P.m_Name) + st::ROW_GAP);
                    const ImVec2 C0{ x, cy - Len(st::CHIP_H) * 0.5f }, C1{ x + Len(ChipW(P)), cy + Len(st::CHIP_H) * 0.5f };
                    ImGui::SetCursorScreenPos(C0);
                    ImGui::PushID(Row);
                    ImGui::InvisibleButton("##Chip", { std::max(1.0f, C1.x - C0.x), std::max(1.0f, C1.y - C0.y) });
                    ImGui::PopID();
                    const bool bHover = ImGui::IsItemHovered();
                    if (bHover) m_HoverChipNext = Id;
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) Select(0, Id);
                    if (bHover && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) GoToSource(Id);
                    m_ChipAt[Id] = { (C0.x + C1.x) * 0.5f, cy };
                    D->AddRectFilled(C0, C1, bHover ? st::ChipHover : st::Chip, Len(2));
                    if (Id == m_SelConn) D->AddRect(C0, C1, st::Selected, Len(2), 0, 1.5f);         // the focused chip
                    Text(D, { C0.x + Len(st::CHIP_PAD), cy - FontPx() * 0.5f }, bHover || Id == m_SelConn ? st::Text : st::TextDim, Chip);
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
            const bool  bSource = m_HoverChip && SourceOf(m_HoverChip) == N.m_Guid.m_Value;         // a hovered chip outlines its source
            const ImU32 Border  = N.m_HasErrMsg ? st::Error : bSelected || bSource ? st::Selected : bRelated ? st::Related : st::StepBorder;
            if (bSelected || bSource) D->AddRect({ P0.x - 3, P0.y - 3 }, { P1.x + 3, P1.y + 3 }, Alpha(st::Selected, 0.25f), R + 3, 0, 3.0f);
            D->AddRect(P0, P1, Border, R, 0, bSelected || bSource || N.m_HasErrMsg ? 2.0f : bRelated ? 1.5f : 1.0f);
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

        //------------------------------------------------------------------------------------------
        // Connections, pins, navigation (6)
        //------------------------------------------------------------------------------------------
        const xmaterial_graph::connection* Conn(std::uint64_t Id) const noexcept
        {
            auto It = m_Doc.m_Graph.m_Connections.find(xmaterial_graph::connection_guid{ Id });
            return It == m_Doc.m_Graph.m_Connections.end() ? nullptr : It->second.get();
        }
        std::uint64_t SourceOf(std::uint64_t Id) const noexcept   { auto* c = Conn(Id); auto* n = c ? m_Doc.m_Graph.findNodeByPin(c->m_OutputPinGuid) : nullptr; return n ? n->m_Guid.m_Value : 0; }
        std::uint64_t ConsumerOf(std::uint64_t Id) const noexcept { auto* c = Conn(Id); auto* n = c ? m_Doc.m_Graph.findNodeByPin(c->m_InputPinGuid)  : nullptr; return n ? n->m_Guid.m_Value : 0; }

        const xmaterial_graph::pin* PinOf(std::uint64_t Pin, const node** ppNode = nullptr) const noexcept
        {
            auto& G = m_Doc.m_Graph;
            auto* pN = G.findNodeByPin(xmaterial_graph::pin_guid{ Pin });
            if (ppNode) *ppNode = pN;
            bool bIn; int i, Sub;
            return pN ? G.findPinConst(*pN, xmaterial_graph::pin_guid{ Pin }, bIn, i, Sub) : nullptr;
        }
        std::string PinLabel(std::uint64_t Pin) const noexcept { const node* pN; auto* p = PinOf(Pin, &pN); return p ? cmds::StepLabel(*pN) + "." + p->m_Name : "?"; }

        // The type rule of IsPinCompatible (xmaterial_graph_editing.h) for types alone: node types' pins are on no node of the graph.
        bool Fits(xmaterial_graph::type_guid Out, xmaterial_graph::type_guid In) const noexcept
        {
            auto* o = m_Doc.m_Graph.findType(Out);
            auto* i = m_Doc.m_Graph.findType(In);
            return o && i && (o->m_GUID == i->m_GUID || (o->m_CodeString == "float" && i->m_CodeString.starts_with("vec")));
        }

        // Why Connect would refuse Out -> In, checked before it runs so a refusal leaves no history (7.7); "" = it would connect
        std::string WhyNot(std::uint64_t Out, std::uint64_t In) const noexcept
        {
            auto& G = m_Doc.m_Graph;
            const node *pO = nullptr, *pI = nullptr;
            const auto *po = PinOf(Out, &pO), *pi = PinOf(In, &pI);
            if (!po || !pi) return "no such pin";
            if (pO == pI) return "the pins are on the same step";
            if (!Fits(po->m_TypeGUID, pi->m_TypeGUID)) return cmds::TypesDiffer(G, *po, *pi);
            std::vector<const node*> Todo{ pI };
            std::unordered_set<const node*> Seen{ pI };
            while (!Todo.empty())
            {
                auto* p = Todo.back(); Todo.pop_back();
                if (p == pO) return std::format("would make a cycle through {}", cmds::StepLabel(*pI));
                for (auto& [Id, C] : G.m_Connections)
                    if (G.findNodeByPin(C->m_OutputPinGuid) == p)
                        if (auto* q = G.findNodeByPin(C->m_InputPinGuid); q && Seen.insert(q).second) Todo.push_back(q);
            }
            return {};
        }

        // The pin under the mouse (a sub-element is never one: it sits on its output's pin)
        const std::pair<const std::uint64_t, pin_at>* PinAt(ImVec2 Mouse) const noexcept
        {
            const float R = std::max(4.0f, Len(stack_style::PIN_R)) + 3;
            for (auto& P : m_PinAt)
            {
                if (P.second.m_bSub) continue;
                const ImVec2 s = ToScreen(P.second.m_W.x, P.second.m_W.y);
                if ((s.x - Mouse.x) * (s.x - Mouse.x) + (s.y - Mouse.y) * (s.y - Mouse.y) <= R * R) return &P;
            }
            return nullptr;
        }

        ImVec2 RouteMid(int e) const noexcept                                           // the middle of a route's longest segment
        {
            const auto& P = m_R.m_Routes[e].m_Points;
            std::size_t k = 0;
            auto SegLen = [&](std::size_t i) { return std::abs(P[i + 1].m_X - P[i].m_X) + std::abs(P[i + 1].m_Y - P[i].m_Y); };
            for (std::size_t i = 1; i + 1 < P.size(); ++i) if (SegLen(i) > SegLen(k)) k = i;
            return { (P[k].m_X + P[k + 1].m_X) * 0.5f, (P[k].m_Y + P[k + 1].m_Y) * 0.5f };
        }

        // Scroll to a step and select it; where the view was goes on the history (Alt+Left / Mouse4 come back)
        void GoTo(std::uint64_t Step) noexcept
        {
            auto It = m_StepIndex.find(Step);
            if (It == m_StepIndex.end()) return;
            m_History.push_back({ m_SelStep, m_SelConn, m_Pan });
            Select(Step, 0);
            const auto& b = m_R.m_Layout.m_Steps[It->second];
            m_Pan = { m_Avail.x * 0.5f - (b.m_X + b.m_W * 0.5f) * m_Zoom, m_Avail.y * 0.5f - (b.m_Y + b.m_H * 0.5f) * m_Zoom };
        }
        void GoToSource(std::uint64_t Connection) noexcept { if (const auto s = SourceOf(Connection)) GoTo(s); }
        void Back() noexcept
        {
            if (m_History.empty()) return;
            const auto H = m_History.back();
            m_History.pop_back();
            Select(H.m_Step, H.m_Conn);
            m_Pan = H.m_Pan;
        }

        // Tab / Shift+Tab: the next / previous chip of the selected step (or of the focused chip's step)
        void FocusChip(int Dir) noexcept
        {
            auto* pN = m_Doc.FindNode(m_SelStep ? m_SelStep : ConsumerOf(m_SelConn));
            if (!pN) return;
            std::vector<std::uint64_t> Chips;
            for (auto& P : pN->m_InputPins) if (auto* c = ConnectionOf(P)) Chips.push_back(c->m_Guid.m_Value);
            if (Chips.empty()) return;
            const int n = static_cast<int>(Chips.size());
            const int i = static_cast<int>(std::find(Chips.begin(), Chips.end(), m_SelConn) - Chips.begin());
            Select(0, Chips[i == n ? (Dir > 0 ? 0 : n - 1) : (i + Dir + n) % n]);
        }

        void Disconnect(std::uint64_t Connection) noexcept { xeditor::Run(m_Undo, std::format("Disconnect -Connection {:016X}", Connection)); }

        void Note(std::string Text, ImVec2 AtWorld) noexcept { m_NoteText = std::move(Text); m_NoteAt = AtWorld; m_NoteUntil = ImGui::GetTime() + 3.0; }
        void DrawNote(ImDrawList* D, ImVec2 AtWorld, const std::string& S) const noexcept           // a refusal, flat, at the drop
        {
            const ImVec2 a = ToScreen(AtWorld.x, AtWorld.y), p{ a.x + 14, a.y + 10 };
            const ImVec2 sz = m_pFont->CalcTextSizeA(m_FontSize, FLT_MAX, 0.0f, S.c_str());
            D->AddRectFilled(p, { p.x + sz.x + 12, p.y + sz.y + 8 }, stack_style::OutputsHalf, 3);
            D->AddRect(p, { p.x + sz.x + 12, p.y + sz.y + 8 }, stack_style::Error, 3);
            D->AddText(m_pFont, m_FontSize, { p.x + 6, p.y + 4 }, stack_style::Error, S.c_str());
        }

        // The (+) on the selected route: insert a step on exactly that connection (7.1)
        void DrawInsertButton(ImDrawList* D) noexcept
        {
            namespace st = stack_style;
            const int e = EdgeOf(m_SelConn);
            if (e < 0 || m_R.m_Routes[e].m_Points.size() < 2) return;
            const ImVec2 m = RouteMid(e), c = ToScreen(m.x, m.y);
            ImGui::SetCursorScreenPos({ c.x - st::PLUS_R, c.y - st::PLUS_R });
            ImGui::InvisibleButton("##InsertOnRoute", { 2 * st::PLUS_R, 2 * st::PLUS_R });
            const bool bHover = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) OpenSearch(search::kind::INSERT, m, 0, m_SelConn);
            if (bHover) xeditor::hint::Text("Insert a step on this connection");
            const ImU32 C = Alpha(m_SourceColor[m_R.m_RouteSource[e]], 1.0f);
            D->AddCircleFilled(c, st::PLUS_R, bHover ? st::ChipHover : st::OutputsHalf);
            D->AddCircle(c, st::PLUS_R, C, 0, 1.5f);
            D->AddLine({ c.x - 4, c.y }, { c.x + 4.5f, c.y }, st::Text, 1.5f);
            D->AddLine({ c.x, c.y - 4 }, { c.x, c.y + 4.5f }, st::Text, 1.5f);
        }

        // A drag from a pin: a wire to the mouse; over a pin of the other side, why it would be refused
        void DrawDrag(ImDrawList* D) noexcept
        {
            if (!m_DragPin) return;
            const auto& From = m_PinAt.at(m_DragPin);
            const ImVec2 a = ToScreen(From.m_W.x, From.m_W.y), b = ImGui::GetIO().MousePos;
            const auto* p = PinOf(m_DragPin);
            const ImU32 C = p ? TypeColor(p->m_TypeGUID) : stack_style::Text;
            const float mx = (a.x + b.x) * 0.5f;
            const ImVec2 Path[] = { a, { mx, a.y }, { mx, b.y }, b };
            D->AddPolyline(Path, 4, C, 0, stack_style::LIT_W);
            if (const auto* pTo = PinAt(b); pTo && pTo->second.m_bOut != From.m_bOut)
            {
                const auto Why = WhyNot(From.m_bOut ? m_DragPin : pTo->first, From.m_bOut ? pTo->first : m_DragPin);
                if (!Why.empty()) DrawNote(D, ToWorld(b), Why);
                else D->AddCircle(b, Len(stack_style::PIN_R) + 3, stack_style::Selected, 0, 2.0f);
            }
        }

        //------------------------------------------------------------------------------------------
        // The search (7.4): what fits a pin, a connection or nothing; a pick may need a port chooser; then one undo step
        //------------------------------------------------------------------------------------------
        struct search
        {
            enum class kind : std::uint8_t { NONE, ADD, FROM_OUTPUT, FROM_INPUT, REPLACE_ALL, INSERT };
            kind                        m_Kind = kind::NONE;
            std::uint64_t               m_Pin  = 0;         // FROM_OUTPUT / FROM_INPUT: the pin; REPLACE_ALL: the source output being replaced
            std::uint64_t               m_Conn = 0;         // INSERT: the connection
            std::vector<std::uint64_t>  m_Uses;             // REPLACE_ALL: the inputs it feeds
            ImVec2                      m_At   = {};        // world: where the new step's ghost sits
            char                        m_Filter[64] = {};
        };
        struct entry
        {
            std::string                     m_Label;
            std::uint64_t                   m_Prefab = 0, m_Source = 0;     // a new step of a type, or an existing output (or sub-element)
            std::vector<std::pair<int,int>> m_Ports;                        // the new step's (input, output) that fit; -1 = none needed
        };

        void OpenSearch(search::kind K, ImVec2 AtWorld, std::uint64_t Pin, std::uint64_t Connection = 0) noexcept
        {
            m_Search = {};
            m_Search.m_Kind = K; m_Search.m_Pin = Pin; m_Search.m_Conn = Connection; m_Search.m_At = AtWorld;
            if (K == search::kind::REPLACE_ALL)
                for (auto& [Id, C] : m_Doc.m_Graph.m_Connections) if (C->m_OutputPinGuid.m_Value == Pin) m_Search.m_Uses.push_back(C->m_InputPinGuid.m_Value);
            m_Picked.reset();
            m_HoverEntry = m_HoverPort = -1;
            m_Open = "##Search";
        }

        bool FeedsAnInput() const noexcept { return m_Search.m_Kind == search::kind::FROM_INPUT || m_Search.m_Kind == search::kind::REPLACE_ALL; }

        // The inputs a new source would feed: the dragged input, the uses, the connection's consumer
        std::vector<std::uint64_t> Targets() const noexcept
        {
            using k = search::kind;
            if (m_Search.m_Kind == k::FROM_INPUT)  return { m_Search.m_Pin };
            if (m_Search.m_Kind == k::REPLACE_ALL) return m_Search.m_Uses;
            if (auto* c = m_Search.m_Kind == k::INSERT ? Conn(m_Search.m_Conn) : nullptr) return { c->m_InputPinGuid.m_Value };
            return {};
        }
        std::uint64_t SourcePin() const noexcept
        {
            if (m_Search.m_Kind == search::kind::FROM_OUTPUT) return m_Search.m_Pin;
            if (auto* c = m_Search.m_Kind == search::kind::INSERT ? Conn(m_Search.m_Conn) : nullptr) return c->m_OutputPinGuid.m_Value;
            return 0;
        }

        std::vector<entry> Entries() noexcept
        {
            using k = search::kind;
            auto& G = m_Doc.m_Graph;
            const auto Want = Targets();
            const auto* pIn  = Want.empty() ? nullptr : PinOf(Want[0]);
            const auto* pOut = SourcePin() ? PinOf(SourcePin()) : nullptr;
            if ((FeedsAnInput() || m_Search.m_Kind == k::INSERT) && !pIn) return {};
            if ((m_Search.m_Kind == k::FROM_OUTPUT || m_Search.m_Kind == k::INSERT) && !pOut) return {};
            const std::string Filter = cmds::Lower(m_Search.m_Filter);
            auto Keep = [&](const std::string& Label) { return Filter.empty() || cmds::Lower(Label).find(Filter) != std::string::npos; };
            std::vector<entry> R;

            // Existing outputs (sub-elements too) that sit before every input they would feed
            if (FeedsAnInput())
            {
                G.m_Stack.Reindex();
                int Before = std::numeric_limits<int>::max();
                for (auto In : Want) if (auto* n = G.findNodeByPin(xmaterial_graph::pin_guid{ In })) if (auto It = G.m_Stack.m_Global.find(n->m_Guid); It != G.m_Stack.m_Global.end()) Before = std::min(Before, It->second);
                for (auto& Sec : G.m_Stack.m_Sections)
                    for (auto Guid : Sec.m_Steps)
                    {
                        auto It = G.m_Stack.m_Global.find(Guid);
                        auto* pN = m_Doc.FindNode(Guid.m_Value);
                        if (!pN || It == G.m_Stack.m_Global.end() || It->second >= Before) continue;
                        for (auto& Op : pN->m_OutputPins)
                        {
                            auto Offer = [&](const xmaterial_graph::pin& P)
                            {
                                if (P.m_PinGUID.m_Value == m_Search.m_Pin || !Fits(P.m_TypeGUID, pIn->m_TypeGUID)) return;
                                if (std::string L = cmds::ChipText(G, P.m_PinGUID); Keep(L)) R.push_back({ std::move(L), 0, P.m_PinGUID.m_Value, {} });
                            };
                            Offer(Op);
                            for (auto& Sub : Op.m_SubElements) Offer(Sub);
                        }
                    }
            }

            // New steps of the types that fit, by name
            std::vector<const node*> Types;
            for (auto& [Guid, pN] : G.m_PrefabNodes) if (!pN->isCommentNode()) Types.push_back(pN.get());
            std::sort(Types.begin(), Types.end(), [](auto* a, auto* b) { return a->m_Name < b->m_Name; });
            for (auto* pT : Types)
            {
                if (!Keep(pT->m_Name)) continue;
                entry E{ pT->m_Name, pT->m_Guid.m_Value, 0, {} };
                const int nI = static_cast<int>(pT->m_InputPins.size()), nO = static_cast<int>(pT->m_OutputPins.size());
                switch (m_Search.m_Kind)
                {
                case k::ADD:         E.m_Ports.push_back({ -1, -1 }); break;
                case k::FROM_OUTPUT: for (int i = 0; i < nI; ++i) if (Fits(pOut->m_TypeGUID, pT->m_InputPins[i].m_TypeGUID)) E.m_Ports.push_back({ i, -1 }); break;
                case k::FROM_INPUT:
                case k::REPLACE_ALL: for (int o = 0; o < nO; ++o) if (Fits(pT->m_OutputPins[o].m_TypeGUID, pIn->m_TypeGUID)) E.m_Ports.push_back({ -1, o }); break;
                case k::INSERT:
                    if (pT->isInputNode() || pT->isOutputNode()) break;
                    for (int i = 0; i < nI; ++i) for (int o = 0; o < nO; ++o)
                        if (Fits(pOut->m_TypeGUID, pT->m_InputPins[i].m_TypeGUID) && Fits(pT->m_OutputPins[o].m_TypeGUID, pIn->m_TypeGUID)) E.m_Ports.push_back({ i, o });
                    break;
                default: break;
                }
                if (!E.m_Ports.empty()) R.push_back(std::move(E));
            }
            return R;
        }

        // The pick, as one undo step. Every check Connect makes is already true here (types fit, a new step makes no cycle, a source
        // offered sits before its uses), so a group never stops half way.
        void Commit(const entry& E, std::pair<int,int> Port) noexcept
        {
            using k = search::kind;
            auto& G = m_Doc.m_Graph;
            auto Id = [] { return xresource::guid_generator::Type64(); };
            const auto Want = Targets();
            if (E.m_Source)                                                             // an existing output feeds the inputs
            {
                std::vector<std::string> Cmds;
                for (auto In : Want) Cmds.push_back(std::format("Connect -Output {:016X} -Input {:016X} -Connection {:016X}", E.m_Source, In, Id()));
                if (!xeditor::RunGroup(m_Undo, "Replace source", Cmds)) Note("not connected", m_Search.m_At);
                return;
            }
            auto Type = G.m_PrefabNodes.find(xmaterial_graph::node_guid{ E.m_Prefab });
            if (Type == G.m_PrefabNodes.end()) return;
            const auto& T = *Type->second;
            const std::uint64_t Node = Id();
            auto PortName = [&](bool bOut, int i) { return xeditor::Quote(bOut ? T.m_OutputPins[i].m_Name : T.m_InputPins[i].m_Name); };
            if (m_Search.m_Kind == k::INSERT)
            {
                xeditor::Run(m_Undo, std::format("InsertOnConnection -Connection {:016X} -Prefab {:016X} -Node {:016X} -Input {} -Output {}", m_Search.m_Conn, E.m_Prefab, Node, PortName(false, Port.first), PortName(true, Port.second)));
                return;
            }

            // A new step: right after the step it reads, right before the first step it feeds (an output step: in the output stack)
            const std::string Name = cmds::UniqueStepName(G, T.m_Name);
            std::string Place;
            auto InUserSection = [&](std::uint64_t Step) { auto It = G.m_Stack.m_Where.find(xmaterial_graph::node_guid{ Step }); return It != G.m_Stack.m_Where.end() && G.m_Stack.m_Sections[It->second.first].m_Kind == xmaterial_graph::section::kind::USER; };
            G.m_Stack.Reindex();
            if (m_Search.m_Kind == k::FROM_OUTPUT && !T.isOutputNode())
            {
                if (auto* n = G.findNodeByPin(xmaterial_graph::pin_guid{ m_Search.m_Pin }); n && InUserSection(n->m_Guid.m_Value)) Place = std::format(" -After {:016X}", n->m_Guid.m_Value);
            }
            else if (FeedsAnInput())
            {
                const node* pFirst = nullptr;
                for (auto In : Want)
                    if (auto* n = G.findNodeByPin(xmaterial_graph::pin_guid{ In }); n && G.m_Stack.m_Global.contains(n->m_Guid))
                        if (!pFirst || G.m_Stack.m_Global[n->m_Guid] < G.m_Stack.m_Global[pFirst->m_Guid]) pFirst = n;
                if (pFirst && InUserSection(pFirst->m_Guid.m_Value)) Place = std::format(" -Before {:016X}", pFirst->m_Guid.m_Value);
            }
            std::vector<std::string> Cmds{ std::format("CreateNode -Prefab {:016X} -Node {:016X} -Name {}{}", E.m_Prefab, Node, Name, Place) };
            if (m_Search.m_Kind == k::FROM_OUTPUT)
                Cmds.push_back(std::format("Connect -Output {:016X} -Input {} -Connection {:016X}", m_Search.m_Pin, xeditor::Quote("@" + Name + "." + T.m_InputPins[Port.first].m_Name), Id()));
            else for (auto In : Want)
                Cmds.push_back(std::format("Connect -Output {} -Input {:016X} -Connection {:016X}", xeditor::Quote("@" + Name + "." + T.m_OutputPins[Port.second].m_Name), In, Id()));
            if (!xeditor::RunGroup(m_Undo, "Add step", Cmds)) Note("the step was not added", m_Search.m_At);
        }

        // The new step the search would make, where it would go, with the binding the pick (or the hovered port) makes
        void DrawGhost(ImDrawList* D) noexcept
        {
            namespace st = stack_style;
            namespace geo = stack_layout::geo;
            using k = search::kind;
            if (m_Search.m_Kind == k::NONE) return;
            const entry* pE = m_Picked ? &*m_Picked : m_HoverEntry >= 0 && m_HoverEntry < static_cast<int>(m_Entries.size()) ? &m_Entries[m_HoverEntry] : nullptr;
            if (!pE || !pE->m_Prefab) return;
            auto Type = m_Doc.m_Graph.m_PrefabNodes.find(xmaterial_graph::node_guid{ pE->m_Prefab });
            if (Type == m_Doc.m_Graph.m_PrefabNodes.end()) return;
            const auto& T = *Type->second;
            const auto Port = pE->m_Ports[m_Picked && m_HoverPort >= 0 && m_HoverPort < static_cast<int>(pE->m_Ports.size()) ? m_HoverPort : 0];

            float InW = 0, OutW = 0;
            for (auto& P : T.m_InputPins)  InW  = std::max(InW,  Measure(P.m_Name));
            for (auto& P : T.m_OutputPins) OutW = std::max(OutW, Measure(P.m_Name));
            const int   nRows = std::max({ 1, static_cast<int>(T.m_InputPins.size()), static_cast<int>(T.m_OutputPins.size()) });
            const float W = stack_layout::StepW(std::max(Measure(T.m_Name), InW + OutW + geo::MID_GAP)), H = stack_layout::StepH(nRows);
            float x = m_Search.m_At.x, y = m_Search.m_At.y - stack_layout::RowY(std::max(0, FeedsAnInput() ? Port.second : Port.first));
            if (FeedsAnInput())                  x -= W;
            else if (m_Search.m_Kind == k::INSERT) x -= W * 0.5f;
            else if (m_Search.m_Kind == k::ADD)    y = m_Search.m_At.y;

            const ImVec2 P0 = ToScreen(x, y), P1 = ToScreen(x + W, y + H);
            const float  R = Len(st::ROUND), HeaderB = P0.y + Len(geo::HEADER_H), Split = P0.x + Len(geo::PORT_PAD + InW + geo::MID_GAP * 0.5f);
            auto A = [](ImU32 C) { return Alpha(C, st::GHOST); };
            D->AddRectFilled(P0, { P1.x, HeaderB }, A(T.isInputNode() ? st::HeaderInput : T.isOutputNode() ? st::HeaderOutput : st::HeaderStep), R, ImDrawFlags_RoundCornersTop);
            D->AddRectFilled({ P0.x, HeaderB }, { T.m_OutputPins.empty() ? P1.x : Split, P1.y }, A(st::InputsHalf), R, ImDrawFlags_RoundCornersBottomLeft);
            if (!T.m_OutputPins.empty()) D->AddRectFilled({ T.m_InputPins.empty() ? P0.x : Split, HeaderB }, P1, A(st::OutputsHalf), R, ImDrawFlags_RoundCornersBottomRight);
            D->AddRect(P0, P1, A(st::Selected), R, 0, 1.5f);
            Text(D, { P0.x + Len(geo::PORT_PAD), P0.y + (Len(geo::HEADER_H) - FontPx()) * 0.5f }, A(st::Text), T.m_Name);

            auto Link = [&](ImVec2 a, ImVec2 b, ImU32 C)                                // world points: a wire the pick would make
            {
                const ImVec2 sa = ToScreen(a.x, a.y), sb = ToScreen(b.x, b.y);
                const float  mx = (sa.x + sb.x) * 0.5f;
                const ImVec2 Path[] = { sa, { mx, sa.y }, { mx, sb.y }, sb };
                D->AddPolyline(Path, 4, A(C), 0, st::WIRE_W + 0.5f);
            };
            for (int i = 0; i < static_cast<int>(T.m_InputPins.size()); ++i)
            {
                const float cy = y + stack_layout::RowY(i);
                const ImVec2 s = ToScreen(x, cy);
                Text(D, { s.x + Len(geo::PORT_PAD), s.y - FontPx() * 0.5f }, A(i == Port.first ? st::Text : st::TextDim), T.m_InputPins[i].m_Name);
                Pin(D, s, A(TypeColor(T.m_InputPins[i].m_TypeGUID)), i == Port.first);
                if (i == Port.first) if (auto Src = m_PinAt.find(SourcePin()); Src != m_PinAt.end()) Link(Src->second.m_W, { x, cy }, TypeColor(T.m_InputPins[i].m_TypeGUID));
            }
            for (int o = 0; o < static_cast<int>(T.m_OutputPins.size()); ++o)
            {
                const float cy = y + stack_layout::RowY(o);
                const ImVec2 s = ToScreen(x + W, cy);
                Text(D, { s.x - Len(geo::PORT_PAD) - TextW(T.m_OutputPins[o].m_Name), s.y - FontPx() * 0.5f }, A(o == Port.second ? st::Text : st::TextDim), T.m_OutputPins[o].m_Name);
                Pin(D, s, A(TypeColor(T.m_OutputPins[o].m_TypeGUID)), o == Port.second);
                if (o == Port.second) for (auto In : Targets()) if (auto Dst = m_PinAt.find(In); Dst != m_PinAt.end()) Link({ x + W, cy }, Dst->second.m_W, TypeColor(T.m_OutputPins[o].m_TypeGUID));
            }
        }

        // The chooser names only the side that varies among the ports that fit: 1 = input, 2 = output, 3 = both
        static int Varies(const std::vector<std::pair<int,int>>& Ports) noexcept
        {
            return (std::ranges::any_of(Ports, [&](auto& p) { return p.first  != Ports[0].first;  }) ? 1 : 0)
                 | (std::ranges::any_of(Ports, [&](auto& p) { return p.second != Ports[0].second; }) ? 2 : 0);
        }
        static std::string PortLabel(const node& T, int Side, std::pair<int,int> Port) noexcept
        {
            if (Side == 3) return T.m_InputPins[Port.first].m_Name + "  ->  " + T.m_OutputPins[Port.second].m_Name;
            return Side == 1 ? T.m_InputPins[Port.first].m_Name : T.m_OutputPins[Port.second].m_Name;
        }

        void DrawSearch() noexcept
        {
            using k = search::kind;
            if (m_Search.m_Kind == k::NONE) return;
            const ImVec2 A = ToScreen(m_Search.m_At.x, m_Search.m_At.y);
            const float Away = Len(m_Search.m_Kind == k::INSERT ? 110.0f : 220.0f);        // beside the ghost, never over it
            ImGui::SetNextWindowPos(FeedsAnInput() ? ImVec2{ A.x - Away - 16, A.y } : ImVec2{ A.x + Away + 16, A.y }, ImGuiCond_Appearing, { FeedsAnInput() ? 1.0f : 0.0f, 0.15f });
            if (!ImGui::BeginPopup("##Search"))
            {
                if (m_Open != "##Search") m_Search.m_Kind = k::NONE;                    // closed (Esc, a click outside, a pick)
                return;
            }
            const auto Want = Targets();
            switch (m_Search.m_Kind)
            {
            case k::ADD:         ImGui::TextDisabled("Add step"); break;
            case k::FROM_OUTPUT: ImGui::TextDisabled("Feed %s into", cmds::ChipText(m_Doc.m_Graph, xmaterial_graph::pin_guid{ m_Search.m_Pin }).c_str()); break;
            case k::FROM_INPUT:  ImGui::TextDisabled("Source for %s", PinLabel(m_Search.m_Pin).c_str()); break;
            case k::REPLACE_ALL: ImGui::TextDisabled("Replace %s in %d uses with", cmds::ChipText(m_Doc.m_Graph, xmaterial_graph::pin_guid{ m_Search.m_Pin }).c_str(), static_cast<int>(Want.size())); break;
            case k::INSERT:      ImGui::TextDisabled("Insert on %s -> %s", cmds::ChipText(m_Doc.m_Graph, xmaterial_graph::pin_guid{ SourcePin() }).c_str(), Want.empty() ? "?" : PinLabel(Want[0]).c_str()); break;
            default: break;
            }
            ImGui::Separator();

            if (m_Picked)                                                               // the port chooser: several ports fit, the person picks one
            {
                auto Type = m_Doc.m_Graph.m_PrefabNodes.find(xmaterial_graph::node_guid{ m_Picked->m_Prefab });
                if (Type == m_Doc.m_Graph.m_PrefabNodes.end()) { m_Picked.reset(); ImGui::EndPopup(); return; }
                const int Side = Varies(m_Picked->m_Ports);
                ImGui::TextUnformatted(std::format("Which {} of {}?", Side == 3 ? "input and output" : Side == 1 ? "input" : "output", m_Picked->m_Label).c_str());
                m_HoverPort = -1;
                for (int i = 0; i < static_cast<int>(m_Picked->m_Ports.size()); ++i)
                {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(PortLabel(*Type->second, Side, m_Picked->m_Ports[i]).c_str())) { Commit(*m_Picked, m_Picked->m_Ports[i]); ImGui::CloseCurrentPopup(); }
                    if (ImGui::IsItemHovered()) m_HoverPort = i;
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::Selectable("< Back", false, ImGuiSelectableFlags_NoAutoClosePopups)) m_Picked.reset();
                ImGui::EndPopup();
                return;
            }

            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(260);
            const bool bEnter = ImGui::InputTextWithHint("##Filter", "search", m_Search.m_Filter, sizeof(m_Search.m_Filter), ImGuiInputTextFlags_EnterReturnsTrue);
            m_Entries = Entries();
            int Pick = bEnter && !m_Entries.empty() ? 0 : -1;
            m_HoverEntry = -1;
            const float H = std::min(360.0f, ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(m_Entries.size()) + 2.5f));
            if (ImGui::BeginChild("##List", { 260, H }))
            {
                bool bExisting = false, bNew = false;
                for (int i = 0; i < static_cast<int>(m_Entries.size()); ++i)
                {
                    auto& E = m_Entries[i];
                    if (E.m_Source && !bExisting) { bExisting = true; ImGui::SeparatorText("Steps"); }
                    if (E.m_Prefab && !bNew)      { bNew = true;      ImGui::SeparatorText("New step"); }
                    ImGui::PushID(i);
                    if (ImGui::Selectable(E.m_Label.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups)) Pick = i;
                    if (ImGui::IsItemHovered()) m_HoverEntry = i;
                    ImGui::PopID();
                }
                if (m_Entries.empty()) ImGui::TextDisabled("nothing fits");
            }
            ImGui::EndChild();
            if (Pick >= 0)
            {
                const auto E = m_Entries[Pick];
                if (E.m_Ports.size() > 1) m_Picked = E;                                 // ask which port; nothing is wired yet
                else { Commit(E, E.m_Ports.empty() ? std::pair{ -1, -1 } : E.m_Ports[0]); ImGui::CloseCurrentPopup(); }
            }
            ImGui::EndPopup();
        }

        //------------------------------------------------------------------------------------------
        // Menus
        //------------------------------------------------------------------------------------------
        void DrawPopups() noexcept
        {
            auto& G = m_Doc.m_Graph;
            if (!m_Open.empty()) { ImGui::OpenPopup(m_Open.c_str()); m_Open.clear(); }

            if (ImGui::BeginPopup("##StepMenu"))
            {
                if (auto* pN = m_Doc.FindNode(m_SelStep))
                {
                    ImGui::TextDisabled("%s", cmds::StepLabel(*pN).c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Trace upstream",   "U", m_Trace == trace::UP))   Trace(m_Trace == trace::UP   ? trace::NONE : trace::UP);
                    if (ImGui::MenuItem("Trace downstream", "D", m_Trace == trace::DOWN)) Trace(m_Trace == trace::DOWN ? trace::NONE : trace::DOWN);
                }
                ImGui::EndPopup();
            }

            if (ImGui::BeginPopup("##ChipMenu"))
            {
                if (auto* c = Conn(m_SelConn))
                {
                    const auto Id = m_SelConn;
                    ImGui::TextDisabled("%s  <-  %s", PinLabel(c->m_InputPinGuid.m_Value).c_str(), cmds::ChipText(G, c->m_OutputPinGuid).c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Go to source", "Enter")) GoToSource(Id);
                    if (ImGui::MenuItem("Find all uses"))       { m_UsesOf = c->m_OutputPinGuid.m_Value; m_Open = "##Uses"; }
                    if (ImGui::MenuItem("Replace source..."))   if (auto P = m_PinAt.find(c->m_InputPinGuid.m_Value); P != m_PinAt.end()) OpenSearch(search::kind::FROM_INPUT, { P->second.m_W.x - 48, P->second.m_W.y }, P->first);
                    if (ImGui::MenuItem("Disconnect", "Del"))   Disconnect(Id);
                }
                ImGui::EndPopup();
            }

            if (ImGui::BeginPopup("##Uses"))                                           // every consumer of one source output; pick = go there
            {
                ImGui::TextDisabled("Uses of %s", cmds::ChipText(G, xmaterial_graph::pin_guid{ m_UsesOf }).c_str());
                ImGui::Separator();
                std::uint64_t First = 0;
                for (auto& [Id, C] : G.m_Connections)
                    if (C->m_OutputPinGuid.m_Value == m_UsesOf)
                    {
                        if (!First) First = C->m_InputPinGuid.m_Value;
                        ImGui::PushID(static_cast<int>(Id.m_Value));
                        if (ImGui::Selectable(PinLabel(C->m_InputPinGuid.m_Value).c_str())) GoTo(ConsumerOf(Id.m_Value));
                        ImGui::PopID();
                    }
                ImGui::Separator();
                if (ImGui::Selectable("Replace all...") && First)
                    if (auto P = m_PinAt.find(First); P != m_PinAt.end()) OpenSearch(search::kind::REPLACE_ALL, { P->second.m_W.x - 48, P->second.m_W.y }, m_UsesOf);
                ImGui::EndPopup();
            }

            if (ImGui::BeginPopup("##RouteMenu"))
            {
                if (auto* c = Conn(m_SelConn); c && EdgeOf(m_SelConn) >= 0)
                {
                    const auto Id = m_SelConn;
                    ImGui::TextDisabled("%s  ->  %s", cmds::ChipText(G, c->m_OutputPinGuid).c_str(), PinLabel(c->m_InputPinGuid.m_Value).c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Insert step on this connection...")) OpenSearch(search::kind::INSERT, RouteMid(EdgeOf(Id)), 0, Id);
                    if (ImGui::MenuItem("Go to source"))   GoToSource(Id);
                    if (ImGui::MenuItem("Go to consumer")) GoTo(ConsumerOf(Id));
                    if (ImGui::MenuItem("Disconnect", "Del")) Disconnect(Id);
                }
                ImGui::EndPopup();
            }

            if (ImGui::BeginPopup("##CanvasMenu"))
            {
                if (ImGui::MenuItem("Add step...")) OpenSearch(search::kind::ADD, m_MenuAt, 0);
                ImGui::EndPopup();
            }

            DrawSearch();
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
        ImVec2                                      m_Avail    = {};
        std::unordered_map<std::uint64_t, pin_at>   m_PinAt;            // pin guid -> where it is drawn (rebuilt with the layout)
        std::unordered_map<std::uint64_t, ImVec2>   m_ChipAt;           // connection guid -> its chip's centre on screen, last frame
        std::uint64_t                               m_HoverChip = 0, m_HoverChipNext = 0;     // the connection of the hovered chip
        std::vector<visit>                          m_History;          // Go to pushes, Back pops
        std::string                                 m_Open;             // a popup to open at window level next
        ImVec2                                      m_MenuAt   = {};    // world: where the background menu was opened
        std::uint64_t                               m_UsesOf   = 0;     // Find all uses: the source output
        std::uint64_t                               m_DragPin  = 0;     // a pin being dragged from
        ImVec2                                      m_DragFrom = {};
        std::string                                 m_NoteText;         // a refusal shown at the drop for a while
        ImVec2                                      m_NoteAt   = {};
        double                                      m_NoteUntil = 0;
        search                                      m_Search;
        std::vector<entry>                          m_Entries;          // what the search offers, last frame
        std::optional<entry>                        m_Picked;           // a pick waiting for its port
        int                                         m_HoverEntry = -1, m_HoverPort = -1;
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
