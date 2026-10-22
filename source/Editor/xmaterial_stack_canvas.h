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
// Moves and collapse (section 7.3, 7.6): drag a step's header to a column (any it may sit in) and a height: the person decides, the place is
// saved (PlaceStep) and the layout keeps it, previewed while dragging; Clean graph (top right, background menu) forgets every place
// (CleanGraph). Ctrl+click adds steps to the selection; the step
// menu's Collapse puts them (and every step on a path between them) in a new collapsed section: one box with a row per source crossing in
// and per output crossing out. Double click a box (or its menu) to expand it. Bindings never change.
// Preview (section 10.4): P shows the selected step's output on the Mesh Preview, Esc returns to the material (the session compiles it).
// Components (section 6.5): a vector output's arrow shows its components as pins (always shown while one is connected); dragging from an
// input over a vector output fans its components out beside the pin; a drop the types refuse but components can fix opens the component
// picker; a chip's component suffix (.xyz, .a) opens it too ('.' on a focused chip). One command each: Connect [-Swizzle] / SetInputSwizzle.
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_graph_widgets.h"
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_stack_commands.h"
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_stack_routes.h"
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_subgraph_commands.h"
#include "dependencies/xeditor/include/xeditor/hint.h"
#include "dependencies/imgui/imgui_internal.h"

#include <limits>
#include <optional>
#include <unordered_set>

namespace xmaterial_editor
{
    namespace stack_style       // the old canvas's flat theme (xmaterial_graph_canvas.h): grey inputs half, black outputs half, kind headers
    {
        constexpr ImU32 Canvas        = IM_COL32( 59,  59,  67, 255);     // the old canvas's: the node editor's Bg (60,60,70,200) over the window
        constexpr ImU32 Grid          = IM_COL32(120, 120, 120,  40);     // its Grid, faded by zoom squared as the node editor does
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
        constexpr ImU32 Component     = IM_COL32(110, 205, 235, 255);     // the components an input takes (.xyz), a component pin's label, the open arrow

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
        constexpr int   THUMB_ROWS = 4, SWATCH_ROWS = 1;                                    // the rows an in-node preview takes under the ports (13)
        constexpr float THUMB_PAD = 4.0f;

        // A flat label centred under TopCentre: the preview's banner (also over the Mesh Preview), what a drag would do
        inline void Banner(ImDrawList* D, ImFont* pFont, float Size, ImVec2 TopCentre, const std::string& S) noexcept
        {
            const ImVec2 sz = pFont->CalcTextSizeA(Size, FLT_MAX, 0.0f, S.c_str());
            const ImVec2 a{ TopCentre.x - sz.x * 0.5f - 10, TopCentre.y }, b{ TopCentre.x + sz.x * 0.5f + 10, TopCentre.y + sz.y + 10 };
            D->AddRectFilled(a, b, OutputsHalf, 3);
            D->AddRect(a, b, Selected, 3, 0, 1.5f);
            D->AddText(pFont, Size, { a.x + 10, a.y + 5 }, Text, S.c_str());
        }
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
            m_View.m_PreviewRows = [this](const xmaterial_graph::node& N) { return PreviewRows(N); };
        }
        stack_canvas(const stack_canvas&) = delete;

        std::uint64_t SelectedNode() const noexcept { return m_SelStep; }

        // The previewed output pin (0 = the material) and what the session says about it (compiling, why it failed); PreviewStep drives it too
        std::uint64_t PreviewPin() const noexcept { return m_PreviewPin; }
        void Preview(std::uint64_t Pin) noexcept { m_PreviewPin = Pin; m_PreviewStatus.clear(); }
        void PreviewStatus(std::string S) noexcept { m_PreviewStatus = std::move(S); }
        std::string PreviewBanner() const noexcept
        {
            if (!m_PreviewPin) return {};
            return std::format("Previewing: {}  -  Esc to return{}{}", cmds::ChipText(m_Doc.m_Graph, xmaterial_graph::pin_guid{ m_PreviewPin }), m_PreviewStatus.empty() ? "" : "  -  ", m_PreviewStatus);
        }

        // View state, also driven by the view commands (SelectStep, TraceRoute)
        void Select(std::uint64_t Step, std::uint64_t Connection) noexcept { m_SelStep = Step; m_SelConn = Step ? 0 : Connection; if (!Step) { m_Trace = trace::NONE; m_Group.clear(); } }
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
                if (auto* pN = m_Doc.FindNode(Step))
                {
                    Pins(pN->m_InputPins, "in"); Pins(pN->m_OutputPins, "out");
                    for (auto& Op : pN->m_OutputPins)
                    {
                        for (std::size_t k = 0; k < Op.m_SubElements.size(); ++k)
                            if (auto Pa = m_PinAt.find(Op.m_SubElements[k].m_PinGUID.m_Value); Pa != m_PinAt.end() && !Pa->second.m_bSub)
                                { const ImVec2 s = ToScreen(Pa->second.m_W.x, Pa->second.m_W.y); Out += std::format("  out:{}{}={:.0f},{:.0f}", Op.m_Name, stack_routes::ComponentLabel(Op, static_cast<int>(k)), s.x, s.y); }
                        if (auto A = m_ArrowAt.find(Op.m_PinGUID.m_Value); A != m_ArrowAt.end()) Out += std::format("  arrow:{}={:.0f},{:.0f}", Op.m_Name, A->second.x, A->second.y);
                    }
                }
                return Out;
            }
            if (const int e = EdgeOf(Connection); e >= 0 && m_R.m_Routes[e].m_Points.size() >= 2)
            {
                const ImVec2 m = RouteMid(e), a = ToScreen(m.x, m.y);
                std::string Out = std::format("screen {:.0f} {:.0f}", a.x, a.y);
                if (auto C = m_ChipAt.find(Connection); C != m_ChipAt.end()) Out += std::format("  chip {:.0f} {:.0f}", C->second.x, C->second.y);
                if (auto C = m_SuffixAt.find(Connection); C != m_SuffixAt.end()) Out += std::format("  suffix {:.0f} {:.0f}", C->second.x, C->second.y);
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
                        else if (m_HoverStep)      { if (m_HoverStep != m_SelStep && !Has(m_Group, m_HoverStep)) { m_Group.clear(); Select(m_HoverStep, 0); } m_Open = "##StepMenu"; }     // a step of the selection keeps it
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
                if (ImGui::IsKeyPressed(ImGuiKey_P, false)) PreviewSelected();
                if (ImGui::IsKeyPressed(ImGuiKey_T, false)) m_bPreviews = !m_bPreviews;   // in-node previews: the layout follows (rows change)
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))                        // the first of: stop a drag, the preview, the trace, the selection
                {
                    if (m_DragStep) { m_DragStep = 0; m_bDragCancelled = true; }
                    else if (m_PreviewPin) Preview(0);
                    else if (m_Trace != trace::NONE) m_Trace = trace::NONE;
                    else Select(0, 0);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Tab, false))                           // Tab: into the selected group, back out of a sub-graph, else the next chip
                {
                    auto* pSel = m_SelStep ? m_Doc.FindNode(m_SelStep) : nullptr;
                    if (pSel && pSel->isGroupCallNode() && !m_SelConn) NoteIf(cmds::EnterGroup(m_Doc, m_SelStep), ToWorld(IO.MousePos));
                    else if (!m_SelStep && !m_SelConn && cmds::GroupTrails().contains(m_Doc.m_Guid.m_Instance.m_Value)) NoteIf(cmds::ExitGroup(m_Doc), ToWorld(IO.MousePos));
                    else FocusChip(IO.KeyShift ? -1 : 1);
                }
                if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false) && m_SelStep && m_Doc.FindNode(m_SelStep)) AskSubgraphName();
                if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && m_SelConn) GoToSource(m_SelConn);
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))                        // a selected connection (or focused chip) disconnects; selected steps go
                {
                    if (m_SelConn) Disconnect(m_SelConn);
                    else if (m_SelStep) DeleteSelected(ToWorld(IO.MousePos));
                }
                if (IO.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) Back();
                if (ImGui::IsKeyPressed(ImGuiKey_Period, false) && m_SelConn)          // the focused chip's components
                    if (auto* c = Conn(m_SelConn); c && HasComponents(c->m_OutputPinGuid.m_Value))
                    {
                        auto At = m_ChipAt.find(m_SelConn);
                        OpenComponents(c->m_OutputPinGuid.m_Value, c->m_InputPinGuid.m_Value, m_SelConn, At != m_ChipAt.end() ? ImVec2{ At->second.x, At->second.y + 10 } : IO.MousePos);
                    }
            }

            // A drag from a pin: onto a pin of the other side connects (refused with the reason), into empty space opens the search of what fits
            if (bInside && !bPopup && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) if (auto* p = PinAt(IO.MousePos)) { m_DragPin = p->first; m_DragFrom = IO.MousePos; }
            if (m_DragPin && !m_PinAt.contains(m_DragPin)) m_DragPin = 0;               // an undo took it
            UpdateFan(IO.MousePos);
            if (m_DragPin && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const auto  From = m_PinAt.at(m_DragPin);
                const auto* pTo  = PinAt(IO.MousePos);
                if (const int k = FanAt(IO.MousePos); k >= 0)                           // a component of the fanned output
                {
                    const std::uint64_t Out = MainOutput(m_Doc.m_Graph, xmaterial_graph::pin_guid{ m_Fan })->m_SubElements[k].m_PinGUID.m_Value;
                    if (auto Why = WhyNot(Out, m_DragPin); !Why.empty()) Note(Why, ToWorld(IO.MousePos));
                    else xeditor::Run(m_Undo, std::format("Connect -Output {:016X} -Input {:016X} -Connection {:016X}", Out, m_DragPin, xresource::guid_generator::Type64()));
                }
                else if (pTo && pTo->second.m_bOut != From.m_bOut)
                {
                    const std::uint64_t Out = From.m_bOut ? m_DragPin : pTo->first, In = From.m_bOut ? pTo->first : m_DragPin;
                    if (auto Why = WhyNot(Out, In); Why.empty()) xeditor::Run(m_Undo, std::format("Connect -Output {:016X} -Input {:016X} -Connection {:016X}", Out, In, xresource::guid_generator::Type64()));
                    else if (OffersComponents(Out, In)) OpenComponents(Out, In, 0, IO.MousePos);          // the types refuse, components fit: which ones
                    else Note(Why, ToWorld(IO.MousePos));
                }
                else if (!pTo && std::abs(IO.MousePos.x - m_DragFrom.x) + std::abs(IO.MousePos.y - m_DragFrom.y) > 8)
                    OpenSearch(From.m_bOut ? search::kind::FROM_OUTPUT : search::kind::FROM_INPUT, ToWorld(IO.MousePos), m_DragPin);
                m_DragPin = 0; m_Fan = 0;
            }

            // A step dragged: to the column under the mouse (clamped into its free ones) and that height; the layout it would give; a drop
            // that moves it is one PlaceStep
            if (m_DragStep && !m_StepIndex.contains(m_DragStep)) m_DragStep = 0;
            if (m_DragStep) PlanDrag(ToWorld(IO.MousePos));
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                if (m_DragStep && m_Drag.m_Column >= 0)
                {
                    const int g = m_StepIndex.at(m_DragStep);
                    const auto& L = m_R.m_Layout;
                    if (m_Drag.m_Column != L.m_Column[g] || std::abs(m_Drag.m_Y - L.m_Steps[g].m_Y) > 0.5f)
                        xeditor::Run(m_Undo, std::format("PlaceStep -Node {:016X} -Column {} -Y {:.0f}", m_DragStep, m_Drag.m_Column, m_Drag.m_Y));
                }
                m_DragStep = 0; m_bDragCancelled = false; m_Drag = {};
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
            m_ChipAt.clear(); m_SuffixAt.clear(); m_ArrowAt.clear();
            m_GroupHint = std::move(m_GroupHintNext); m_GroupHintNext.clear();         // what the menu's Collapse would take, last frame
            std::erase_if(m_Group, [&](std::uint64_t s) { return !m_StepIndex.contains(s); });
            for (int g = 0; g < static_cast<int>(m_In.m_Steps.size()); ++g) DrawStep(D, g, Max);
            DrawInsertButton(D);
            DrawDrag(D);
            DrawMove(D);
            DrawGhost(D);
            if (ImGui::GetTime() < m_NoteUntil) DrawNote(D, m_NoteAt, m_NoteText);
            if (const auto B = PreviewBanner(); !B.empty()) DrawBanner(D, { (m_Origin.x + Max.x) * 0.5f, m_Origin.y + 8 }, B);

            D->PopClipRect();
            ImGui::PopItemFlag();
            DrawBreadcrumb();
            DrawCleanButton();
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
            return pC ? Measure(cmds::InputChipText(m_Doc.m_Graph, *pC, P)) + 2 * stack_style::CHIP_PAD : 0.0f;
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
                for (std::size_t i = 0; i < a.size(); ++i) if (a[i].m_Id != b[i].m_Id || a[i].m_W != b[i].m_W || a[i].m_nRows != b[i].m_nRows || a[i].m_LitW != b[i].m_LitW
                                                              || a[i].m_PinColumn != b[i].m_PinColumn || a[i].m_PinY != b[i].m_PinY) return false;
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
            auto In = stack_routes::FromGraph(m_Doc.m_Graph, m_View.m_Measure, m_View.m_InputW, m_View.m_LitW, m_View.m_PreviewRows, nullptr, &m_View.m_Open);
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
                stack_routes::ForOutputRows(m_Doc.m_Graph, *pN, &m_View.m_Open, [&](int Row, const xmaterial_graph::output_pin& Op, int Sub)
                {
                    const ImVec2 W{ b.m_X + b.m_W, b.m_Y + stack_layout::RowY(Row) };
                    if (Sub >= 0) { m_PinAt[Op.m_SubElements[Sub].m_PinGUID.m_Value] = { W, pN->m_Guid.m_Value, true }; return; }      // a shown component: a pin of its own
                    m_PinAt[Op.m_PinGUID.m_Value] = { W, pN->m_Guid.m_Value, true };
                    for (auto& S : Op.m_SubElements) m_PinAt[S.m_PinGUID.m_Value] = { W, pN->m_Guid.m_Value, true, true };
                });
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
            constexpr float Step = 32.0f;                                               // world units, the node editor's grid
            const ImU32 Grid = Alpha(st::Grid, std::min(1.0f, m_Zoom * m_Zoom));
            const ImVec2 W0 = ToWorld(m_Origin), W1 = ToWorld(Max);
            for (int i = static_cast<int>(std::floor(W0.x / Step)); i <= static_cast<int>(std::ceil(W1.x / Step)); ++i)
            {
                const float x = Snap(ToScreen(i * Step, 0).x);
                D->AddLine({ x, m_Origin.y }, { x, Max.y }, Grid);
            }
            for (int i = static_cast<int>(std::floor(W0.y / Step)); i <= static_cast<int>(std::ceil(W1.y / Step)); ++i)
            {
                const float y = Snap(ToScreen(0, i * Step).y);
                D->AddLine({ m_Origin.x, y }, { Max.x, y }, Grid);
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
            const auto& b = m_R.m_Layout.m_Steps[g];
            const ImVec2 P0 = ToScreen(b.m_X, b.m_Y), P1 = ToScreen(b.m_X + b.m_W, b.m_Y + b.m_H);
            if (P1.x < m_Origin.x || P1.y < m_Origin.y || P0.x - Len(m_R.m_Layout.m_LitW[g]) > ViewMax.x || P0.y > ViewMax.y) return;       // culled
            if (m_In.m_Box[g] >= 0) { DrawBox(D, g, P0, P1); return; }
            auto* pN = m_Doc.FindNode(m_In.m_Steps[g].m_Value);
            if (!pN) return;
            auto& N = *pN;

            const bool bSelected = N.m_Guid.m_Value == m_SelStep || Has(m_Group, N.m_Guid.m_Value) || Has(m_GroupHint, N.m_Guid.m_Value);
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
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) ClickStep(N.m_Guid.m_Value);
            if (N.isGroupCallNode() && ImGui::IsItemHovered())
            {
                xeditor::hint::Text("Group: double click or Tab to go into its sub-graph");
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) NoteIf(cmds::EnterGroup(m_Doc, N.m_Guid.m_Value), ToWorld(ImGui::GetIO().MousePos));
            }
            if (ImGui::IsItemActive() && !m_DragStep && !m_DragPin && !m_bDragCancelled && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6)
                && ImGui::GetIO().MouseClickedPos[0].y < HeaderB)                                       // a header dragged: a move (7.3)
                { m_DragStep = N.m_Guid.m_Value; m_DragDy = ToWorld(ImGui::GetIO().MouseClickedPos[0]).y - m_R.m_Layout.m_Steps[g].m_Y; }
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
                    const std::string Chip   = cmds::InputChipText(m_Doc.m_Graph, *pC, P);
                    const std::string Taken  = TakenComponents(m_Doc.m_Graph, *pC, P);
                    const std::string Base   = Chip.substr(0, Chip.size() - (Taken.empty() ? 0 : Taken.size() + 1));
                    const float x = P0.x + Len(geo::PORT_PAD + Measure(P.m_Name) + st::ROW_GAP);
                    const ImVec2 C0{ x, cy - Len(st::CHIP_H) * 0.5f }, C1{ x + Len(ChipW(P)), cy + Len(st::CHIP_H) * 0.5f };
                    ImGui::SetCursorScreenPos(C0);
                    ImGui::PushID(Row);
                    const float ChipEnd = Taken.empty() ? C1.x : C0.x + Len(st::CHIP_PAD) + TextW(Base);      // the suffix is a button of its own
                    ImGui::InvisibleButton("##Chip", { std::max(1.0f, ChipEnd - C0.x), std::max(1.0f, C1.y - C0.y) });
                    ImGui::PopID();
                    const bool bHover = ImGui::IsItemHovered();
                    if (bHover) m_HoverChipNext = Id;
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) Select(0, Id);
                    if (bHover && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) GoToSource(Id);
                    m_ChipAt[Id] = { (C0.x + C1.x) * 0.5f, cy };
                    D->AddRectFilled(C0, C1, bHover ? st::ChipHover : st::Chip, Len(2));
                    if (Id == m_SelConn) D->AddRect(C0, C1, st::Selected, Len(2), 0, 1.5f);         // the focused chip
                    Text(D, { C0.x + Len(st::CHIP_PAD), cy - FontPx() * 0.5f }, bHover || Id == m_SelConn ? st::Text : st::TextDim, Base);
                    if (!Taken.empty())                                                     // the components it takes: a button of their own, the picker
                    {
                        const float sx = C0.x + Len(st::CHIP_PAD) + TextW(Base);
                        const ImVec2 S0{ sx, C0.y + Len(1) }, S1{ C1.x - Len(1), C1.y - Len(1) };
                        ImGui::SetCursorScreenPos(S0);
                        ImGui::PushID(Row);
                        const bool bPicked = ImGui::InvisibleButton("##Suffix", { std::max(1.0f, S1.x - S0.x), std::max(1.0f, S1.y - S0.y) });     // on release: a popup opened on the press closes at once
                        ImGui::PopID();
                        const bool bSufHover = ImGui::IsItemHovered();
                        if (bSufHover) { m_HoverChipNext = Id; xeditor::hint::Text("The components it takes: click to pick others (or select the chip and press .)"); }
                        if (bPicked) Select(0, Id);
                        if (bPicked) OpenComponents(pC->m_OutputPinGuid.m_Value, P.m_PinGUID.m_Value, Id, { S0.x, C1.y + 2 });
                        m_SuffixAt[Id] = { (S0.x + S1.x) * 0.5f, cy };
                        if (bSufHover) D->AddRectFilled(S0, S1, st::ChipHover, Len(2));
                        Text(D, { sx, cy - FontPx() * 0.5f }, st::Component, "." + Taken);
                        D->AddLine({ sx + Len(1), C1.y - Len(2.5f) }, { S1.x - Len(st::CHIP_PAD) + Len(2), C1.y - Len(2.5f) }, Alpha(st::Component, bSufHover ? 1.0f : 0.45f), 1.0f);     // underlined: it reads as a control
                    }
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

            // Output rows (right; the first always, the others when used): name right aligned, pin on the right edge. A vector output has an
            // arrow left of its name: open, its components show as pins of their own under it (.x .y .z .w, .r .g .b .a); a connected
            // component keeps them shown (stack_canvas.md 6.5)
            ImGui::PushID(static_cast<int>(N.m_Guid.m_Value ^ (N.m_Guid.m_Value >> 32)));
            stack_routes::ForOutputRows(m_Doc.m_Graph, N, &m_View.m_Open, [&](int r, const xmaterial_graph::output_pin& Op, int Sub)
            {
                const float cy = P0.y + Len(stack_layout::RowY(r));
                if (Sub >= 0)
                {
                    const auto& S = Op.m_SubElements[Sub];
                    const bool bUsed = Feeds(S.m_PinGUID.m_Value);
                    const std::string L = stack_routes::ComponentLabel(Op, Sub);
                    const float lx = P1.x - Len(geo::PORT_PAD + stack_routes::ARROW_W * 0.5f);
                    Text(D, { P1.x - Len(geo::PORT_PAD) - TextW(L), cy - FontPx() * 0.5f }, bUsed ? st::Component : st::TextDim, L);
                    D->AddLine({ lx - TextW(L), cy - Len(geo::ROW_H * 0.5f) }, { lx - TextW(L), cy }, Alpha(st::TextDim, 0.6f), 1.0f);            // a tree line from its output
                    D->AddLine({ lx - TextW(L), cy }, { lx - TextW(L) + Len(4), cy }, Alpha(st::TextDim, 0.6f), 1.0f);
                    Pin(D, { P1.x, cy }, TypeColor(S.m_TypeGUID), bUsed);
                    return;
                }
                const bool bOpen = stack_routes::ShowsComponents(m_Doc.m_Graph, Op, &m_View.m_Open);
                const bool bUsed = bOpen ? Feeds(Op.m_PinGUID.m_Value) : IsUsed(Op);
                const float tx = P1.x - Len(geo::PORT_PAD) - TextW(Op.m_Name);
                Text(D, { tx, cy - FontPx() * 0.5f }, bUsed || bOpen ? st::Text : st::TextDim, Op.m_Name);
                Pin(D, { P1.x, cy }, TypeColor(Op.m_TypeGUID), bUsed);
                if (Op.m_SubElements.empty()) return;
                const ImVec2 A0{ tx - Len(stack_routes::ARROW_W), cy - Len(geo::ROW_H * 0.5f) }, A1{ tx - Len(1), cy + Len(geo::ROW_H * 0.5f) };
                ImGui::SetCursorScreenPos(A0);
                ImGui::PushID(static_cast<int>(Op.m_PinGUID.m_Value ^ (Op.m_PinGUID.m_Value >> 32)));
                ImGui::InvisibleButton("##Open", { std::max(1.0f, A1.x - A0.x), std::max(1.0f, A1.y - A0.y) });
                ImGui::PopID();
                const bool bAuto  = bOpen && !m_View.m_Open.contains(Op.m_PinGUID.m_Value);    // a connected component holds it open
                const bool bHover = ImGui::IsItemHovered();
                if (bHover) xeditor::hint::Text("%s", bAuto ? "A component is connected: the components stay shown" : bOpen ? "Hide the components" : "Show the components as pins");
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !bAuto) { if (bOpen) m_View.m_Open.erase(Op.m_PinGUID.m_Value); else m_View.m_Open.insert(Op.m_PinGUID.m_Value); }
                m_ArrowAt[Op.m_PinGUID.m_Value] = { (A0.x + A1.x) * 0.5f, cy };
                const float h = std::max(2.5f, Len(3.5f)), ax = (A0.x + A1.x) * 0.5f;
                if (bHover) D->AddRectFilled(A0, A1, st::ChipHover, Len(2));
                const ImU32 AC = bHover ? st::Text : bAuto ? st::TextDim : st::Component;
                if (bOpen) D->AddTriangleFilled({ ax - h, cy - h * 0.6f }, { ax + h, cy - h * 0.6f }, { ax, cy + h * 0.8f }, AC);     // pointing down: shown
                else       D->AddTriangleFilled({ ax - h * 0.6f, cy - h }, { ax - h * 0.6f, cy + h }, { ax + h * 0.8f, cy }, AC);     // pointing right: hidden
            });
            ImGui::PopID();

            DrawPreview(D, N, P0, P1);

            // Border last: crisp over everything inside
            const bool  bSource = m_HoverChip && SourceOf(m_HoverChip) == N.m_Guid.m_Value;         // a hovered chip outlines its source
            const ImU32 Border  = N.m_HasErrMsg ? st::Error : bSelected || bSource ? st::Selected : bRelated ? st::Related : st::StepBorder;
            if (bSelected || bSource) D->AddRect({ P0.x - 3, P0.y - 3 }, { P1.x + 3, P1.y + 3 }, Alpha(st::Selected, 0.25f), R + 3, 0, 3.0f);
            D->AddRect(P0, P1, Border, R, 0, bSelected || bSource || N.m_HasErrMsg ? 2.0f : bRelated ? 1.5f : 1.0f);
            if (!m_InTrace.empty() && !m_InTrace[g]) D->AddRectFilled({ P0.x - Len(m_R.m_Layout.m_LitW[g]) - 1, P0.y - 1 }, { P1.x + 3, P1.y + 4 }, Alpha(st::Canvas, 0.6f), R);       // outside the trace
        }

        //------------------------------------------------------------------------------------------
        // In-node previews (section 13): what a step makes, at a glance. One hook: PreviewOf says the kind, PreviewRows its height (the layout
        // reads it, so the step grows and routes go around it), DrawPreview draws it under the ports.
        //------------------------------------------------------------------------------------------
        struct preview
        {
            enum class kind : std::uint8_t { NONE, TEXTURE, COLOR, RENDER } m_Kind = kind::NONE;
            xresource::full_guid m_Texture = {};
            ImVec4               m_Color   = {};
        };

        static constexpr std::uint64_t SAMPLER = xresource::type_guid{ "samplerPrefabs" }.m_Value, TEXTURE = xresource::type_guid{ "texturePrefabs" }.m_Value;     // the prefabs' guids (xmaterial_graph.cpp)
        static constexpr std::uint64_t VEC3 = xresource::type_guid{ "Vec3 Const Node" }.m_Value, VEC4 = xresource::type_guid{ "Vec4 Const Node" }.m_Value;
        static bool IsPrefab(const node& N, std::uint64_t Prefab) noexcept { return N.m_PrefabGuid.m_Value == Prefab; }

        preview PreviewOf(const node& N) const noexcept
        {
            using type = xmaterial_graph::node_param::type;
            preview P;
            if (!m_bPreviews) return P;
            if (IsPrefab(N, SAMPLER))                                                       // the texture it samples
            {
                for (auto& Param : N.m_Params) if (Param.m_Type == type::TEXTURE_RESOURCE) { P.m_Kind = preview::kind::TEXTURE; P.m_Texture = Param.m_Value.get<xresource::full_guid>(); break; }
            }
            else if (IsPrefab(N, TEXTURE) && !N.m_InputPins.empty())                        // fed by a sampler: what it samples (not where: the UV is not shown)
            {
                if (auto* pC = ConnectionOf(N.m_InputPins[0])) if (auto* pS = m_Doc.m_Graph.findNodeByPin(pC->m_OutputPinGuid); pS && IsPrefab(*pS, SAMPLER)) P = PreviewOf(*pS);
            }
            else if (IsPrefab(N, VEC3) || IsPrefab(N, VEC4))                                // built from literals only: their colour
            {
                float v[4] = { 0, 0, 0, 1 };
                for (std::size_t i = 0; i < N.m_InputPins.size() && i < 4; ++i)
                {
                    const auto& In = N.m_InputPins[i];
                    if (ConnectionOf(In) || !HasParam(N, In) || N.m_Params[In.m_ParamIndex].m_Type != xmaterial_graph::node_param::type::FLOAT) return Rendered(N);
                    v[i] = N.m_Params[In.m_ParamIndex].m_Value.get<float>();
                }
                // Literal colours are linear: encoded to sRGB (the exact piecewise curve) to be seen as they will look.
                // ponytail: alpha ignored; a checker under it when Vec4 alpha matters
                auto Srgb = [](float c) { c = std::clamp(c, 0.0f, 1.0f); return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; };
                P.m_Kind  = preview::kind::COLOR;
                P.m_Color = { Srgb(v[0]), Srgb(v[1]), Srgb(v[2]), 1.0f };
            }
            return P.m_Kind == preview::kind::NONE ? Rendered(N) : P;
        }

        // Any other step with an output the material's colour can take: its output rendered (m_Renders, the session's; none headless)
        preview Rendered(const node& N) const noexcept
        {
            preview P;
            if (m_Renders && m_Renders(N)) P.m_Kind = preview::kind::RENDER;
            return P;
        }

        int PreviewRows(const node& N) const noexcept
        {
            switch (PreviewOf(N).m_Kind)
            {
            case preview::kind::TEXTURE: case preview::kind::RENDER: return stack_style::THUMB_ROWS;
            case preview::kind::COLOR:   return stack_style::SWATCH_ROWS;
            default:                     return 0;
            }
        }

        // In the rows PreviewRows added at the bottom of the box. A texture is its thumbnail from the editor's thumbnail cache (what every
        // resource reference shows), its type's picture until that is ready; none assigned says so.
        void DrawPreview(ImDrawList* D, const node& N, ImVec2 P0, ImVec2 P1) const noexcept
        {
            namespace st = stack_style;
            namespace geo = stack_layout::geo;
            const auto P = PreviewOf(N);
            if (P.m_Kind == preview::kind::NONE) return;
            const float Bottom = P1.y - Len(geo::NODE_PAD_BOTTOM), Top = Bottom - Len(PreviewRows(N) * geo::ROW_H - st::THUMB_PAD);
            if (P.m_Kind == preview::kind::COLOR)
            {
                const ImVec2 a{ P0.x + Len(geo::PORT_PAD), Top }, b{ P1.x - Len(geo::PORT_PAD), Bottom };
                D->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(P.m_Color), Len(2));
                D->AddRect(a, b, st::StepBorder, Len(2));
                return;
            }
            const float S = Bottom - Top, x = std::floor((P0.x + P1.x - S) * 0.5f);
            const ImVec2 a{ x, Top }, b{ x + S, Bottom };
            if (P.m_Kind == preview::kind::RENDER)                                         // its picture; the older one while a newer is made
            {
                const auto R = m_Rendered(N);
                D->AddRectFilled(a, b, st::OutputsHalf, Len(2));
                if (R.m_pTexture) D->AddImage((ImTextureRef)(void*)R.m_pTexture, a, b, R.m_UV0, R.m_UV1);
                else { const char* t = R.m_Why.empty() ? "..." : "no preview"; Text(D, { (P0.x + P1.x - TextW(t)) * 0.5f, (Top + Bottom - FontPx()) * 0.5f }, st::TextDim, t); }
                if (!R.m_Why.empty() && ImGui::IsMouseHoveringRect(a, b)) xeditor::hint::Text("%s", R.m_Why.c_str());
                D->AddRect(a, b, st::StepBorder, Len(2));
                return;
            }
            xresource_editor::plugin_icon_ref Pic;
            auto& Host = xresource_editor::g_ReferenceHost;
            if (!P.m_Texture.empty() && Host.m_Thumbnail) Pic = Host.m_Thumbnail(P.m_Texture);
            if (!Pic.isValid() && !P.m_Texture.empty()) Pic = xresource_editor::g_LibMgr.m_AssetPluginsDB.getIconRef(P.m_Texture.m_Type, 0);
            D->AddRectFilled(a, b, st::OutputsHalf, Len(2));
            if (Pic.isValid()) D->AddImage((ImTextureRef)(void*)Pic.m_pTexture, a, b, { Pic.m_U0, Pic.m_V0 }, { Pic.m_U1, Pic.m_V1 });
            else               Text(D, { (P0.x + P1.x - TextW("no texture")) * 0.5f, (Top + Bottom - FontPx()) * 0.5f }, st::TextDim, "no texture");
            D->AddRect(a, b, st::StepBorder, Len(2));
        }

        bool Feeds(std::uint64_t Pin) const noexcept
        {
            for (auto& [Id, C] : m_Doc.m_Graph.m_Connections) if (C->m_OutputPinGuid.m_Value == Pin) return true;
            return false;
        }

        bool IsUsed(const xmaterial_graph::output_pin& Op) const noexcept { return stack_routes::OutputUsed(m_Doc.m_Graph, Op); }

        // A collapsed section (7.6): a box with a row per source feeding it from outside (left) and per output of it used outside (right).
        // A second card behind it says it holds several steps.
        void DrawBox(ImDrawList* D, int g, ImVec2 P0, ImVec2 P1) noexcept
        {
            namespace st = stack_style;
            namespace geo = stack_layout::geo;
            const auto& B  = m_In.m_Boxes[m_In.m_Box[g]];
            const auto  Id = m_In.m_Steps[g].m_Value;
            const float R  = Len(st::ROUND), HeaderB = P0.y + Len(geo::HEADER_H), Back = Len(4);
            float InW = 0;
            for (auto& t : B.m_InText) InW = std::max(InW, Measure(t) + 2 * st::CHIP_PAD);
            const float Split = B.m_In.empty() ? P0.x : B.m_Out.empty() ? P1.x : std::min(P1.x, P0.x + Len(geo::PORT_PAD + InW + geo::MID_GAP * 0.5f));
            D->AddRectFilled({ P0.x + Back, P0.y - Back }, { P1.x + Back, P1.y - Back }, st::HeaderStep, R);
            D->AddRect({ P0.x + Back, P0.y - Back }, { P1.x + Back, P1.y - Back }, st::StepBorder, R);
            D->AddRectFilled(P0, { P1.x, HeaderB }, st::HeaderStep, R, ImDrawFlags_RoundCornersTop);
            if (Split > P0.x) D->AddRectFilled({ P0.x, HeaderB }, { Split, P1.y }, st::InputsHalf, R, ImDrawFlags_RoundCornersBottomLeft);
            if (Split < P1.x) D->AddRectFilled({ Split, HeaderB }, P1, st::OutputsHalf, R, ImDrawFlags_RoundCornersBottomRight);
            const float ty = P0.y + (Len(geo::HEADER_H) - FontPx()) * 0.5f;
            const std::string Count = std::format("{} step{}", B.m_nSteps, B.m_nSteps == 1 ? "" : "s");
            Text(D, { P0.x + Len(geo::PORT_PAD), ty }, st::Text, B.m_Name);
            Text(D, { P1.x - Len(geo::PORT_PAD) - TextW(Count), ty }, st::TextDim, Count);

            ImGui::PushID(static_cast<int>(Id ^ (Id >> 32)));
            ImGui::SetCursorScreenPos(P0);
            ImGui::InvisibleButton("##Box", { std::max(1.0f, P1.x - P0.x), std::max(1.0f, P1.y - P0.y) });
            ImGui::PopID();
            if (ImGui::IsItemHovered()) { m_HoverStep = Id; xeditor::hint::Text("Collapsed section %s: double click to expand", B.m_Name.c_str()); }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) Select(Id, 0);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Expand(Id);

            auto Color = [&](std::uint64_t Pin) { auto* p = PinOf(Pin); return p ? TypeColor(p->m_TypeGUID) : st::Text; };
            for (int r = 0; r < static_cast<int>(B.m_In.size()); ++r)                  // what crosses in: the source, as a chip
            {
                const float cy = P0.y + Len(stack_layout::RowY(r));
                const ImVec2 C0{ P0.x + Len(geo::PORT_PAD), cy - Len(st::CHIP_H) * 0.5f }, C1{ C0.x + Len(Measure(B.m_InText[r]) + 2 * st::CHIP_PAD), cy + Len(st::CHIP_H) * 0.5f };
                D->AddRectFilled(C0, C1, st::Chip, Len(2));
                Text(D, { C0.x + Len(st::CHIP_PAD), cy - FontPx() * 0.5f }, st::Text, B.m_InText[r]);
                Pin(D, { P0.x, cy }, Color(B.m_In[r]), true);
            }
            for (int r = 0; r < static_cast<int>(B.m_Out.size()); ++r)                 // what crosses out: the output inside
            {
                const float cy = P0.y + Len(stack_layout::RowY(r));
                Text(D, { P1.x - Len(geo::PORT_PAD) - TextW(B.m_OutText[r]), cy - FontPx() * 0.5f }, st::Text, B.m_OutText[r]);
                Pin(D, { P1.x, cy }, Color(B.m_Out[r]), true);
            }
            const bool bSelected = Id == m_SelStep;
            if (bSelected) D->AddRect({ P0.x - 3, P0.y - 3 }, { P1.x + 3, P1.y + 3 }, Alpha(st::Selected, 0.25f), R + 3, 0, 3.0f);
            D->AddRect(P0, P1, bSelected ? st::Selected : st::StepBorder, R, 0, bSelected ? 2.0f : 1.0f);
            if (!m_InTrace.empty() && !m_InTrace[g]) D->AddRectFilled({ P0.x - 1, P0.y - Back - 1 }, { P1.x + Back + 3, P1.y + 4 }, Alpha(st::Canvas, 0.6f), R);
        }

        void DrawBanner(ImDrawList* D, ImVec2 TopCentre, const std::string& S) const noexcept { stack_style::Banner(D, m_pFont, m_FontSize, TopCentre, S); }

        static bool Has(const std::vector<std::uint64_t>& V, std::uint64_t x) noexcept { return std::find(V.begin(), V.end(), x) != V.end(); }

        // A click selects one step; Ctrl+click adds it to the selection (or takes it out), for Collapse
        void ClickStep(std::uint64_t Id) noexcept
        {
            if (!ImGui::GetIO().KeyCtrl || !m_SelStep) { m_Group.clear(); Select(Id, 0); return; }
            if (auto It = std::find(m_Group.begin(), m_Group.end(), Id); It != m_Group.end()) m_Group.erase(It);
            else if (Id != m_SelStep) m_Group.push_back(Id);
        }

        void PreviewSelected() noexcept                                                 // P: the selected step's first output, or the selected route's source
        {
            std::uint64_t Pin = 0;
            if (auto* pN = m_Doc.FindNode(m_SelStep); pN && !pN->m_OutputPins.empty()) Pin = pN->m_OutputPins[0].m_PinGUID.m_Value;
            else if (auto* c = Conn(m_SelConn)) Pin = c->m_OutputPinGuid.m_Value;
            if (Pin) Preview(Pin == m_PreviewPin ? 0 : Pin);
        }

        //------------------------------------------------------------------------------------------
        // Moves (7.3): the person is the authority on where a step sits: a header dragged to a column (any it may sit in: after what it
        // reads, before what reads it) and a height. The place is saved (PlaceStep); the layout keeps it and moves the rest out of its way.
        //------------------------------------------------------------------------------------------
        struct move_plan
        {
            int                         m_Column = -1;          // where the drop puts the step: the column (clamped), its top
            float                       m_Y      = 0;
            int                         m_Under  = -1;          // the column under the mouse (planned already)
            stack_routes::graph_input   m_In;                   // the layout the drop would give
            stack_routes::result        m_R;
        };

        void PlanDrag(ImVec2 W) noexcept
        {
            const auto& L = m_R.m_Layout;
            const int g = m_StepIndex.at(m_DragStep);
            int c = 0;
            while (c + 1 < L.m_K && W.x >= L.m_X[c + 1]) ++c;                          // the column under the mouse
            const float y = std::round(W.y - m_DragDy);
            if (m_Drag.m_Under == c && m_Drag.m_Y == y) return;                         // planned already
            m_Drag = {};
            m_Drag.m_Under = c; m_Drag.m_Y = y;
            m_Drag.m_In = m_In;
            int h = 0;
            for (auto& S : m_Drag.m_In.m_In) for (auto& s : S.m_Steps) { if (h++ == g) { s.m_PinColumn = c; s.m_PinY = y; } }
            m_Drag.m_R = stack_routes::Build(m_Drag.m_In.m_In, m_Drag.m_In.m_Edges);
            m_Drag.m_Column = m_Drag.m_R.m_Layout.m_Column[g];
        }

        bool Moved(int h) const noexcept                                                // a step of the drag's layout sits elsewhere than now
        {
            auto It = m_StepIndex.find(m_Drag.m_In.m_Steps[h].m_Value);
            if (It == m_StepIndex.end()) return true;
            const auto &a = m_R.m_Layout.m_Steps[It->second], &b = m_Drag.m_R.m_Layout.m_Steps[h];
            return std::abs(a.m_X - b.m_X) > 0.5f || std::abs(a.m_Y - b.m_Y) > 0.5f;
        }

        // Every step that would move outlined where it would go (the dragged one too), the routes it would draw
        void DrawMove(ImDrawList* D) const noexcept
        {
            namespace st = stack_style;
            if (!m_DragStep || m_Drag.m_Column < 0) return;
            for (auto& W : m_Drag.m_R.m_Wires) Line(D, W.m_X0, W.m_Y0, W.m_X1, W.m_Y1, Alpha(st::Selected, 0.35f), 1.0f);
            for (int h = 0; h < static_cast<int>(m_Drag.m_In.m_Steps.size()); ++h)
            {
                if (!Moved(h)) continue;
                const auto& b = m_Drag.m_R.m_Layout.m_Steps[h];
                const ImVec2 a0 = ToScreen(b.m_X, b.m_Y), a1 = ToScreen(b.m_X + b.m_W, b.m_Y + b.m_H);
                auto* pN = m_Doc.FindNode(m_Drag.m_In.m_Steps[h].m_Value);                          // a flat card with its name: "this one goes here"
                D->AddRectFilled(a0, a1, Alpha(st::OutputsHalf, 0.92f), Len(st::ROUND));
                D->AddRect(a0, a1, st::Selected, Len(st::ROUND), 0, 1.5f);
                if (pN) Text(D, { a0.x + Len(stack_layout::geo::PORT_PAD), a0.y + (Len(stack_layout::geo::HEADER_H) - FontPx()) * 0.5f }, st::Text, cmds::StepLabel(*pN));
            }
        }

        //------------------------------------------------------------------------------------------
        // Collapse (7.6): steps into a new collapsed section; expand clears the flag (the section stays: it is organisation)
        //------------------------------------------------------------------------------------------
        std::vector<std::uint64_t> Selection() const noexcept
        {
            std::vector<std::uint64_t> S;
            if (m_Doc.FindNode(m_SelStep)) S.push_back(m_SelStep);
            for (auto s : m_Group) if (m_Doc.FindNode(s) && !Has(S, s)) S.push_back(s);
            return S;
        }

        // The steps a collapse takes: the selection and every step on a path between two of them (else the box would feed itself), in reading order
        std::vector<std::uint64_t> Closure(const std::vector<std::uint64_t>& Sel) const noexcept
        {
            const cmds::order_model M(m_Doc.m_Graph);
            const int N = static_cast<int>(M.m_Ids.size());
            std::vector<char> In(N, 0), Up(N, 0), Down(N, 0);
            for (auto s : Sel) if (auto It = M.m_Id.find(xmaterial_graph::node_guid{ s }); It != M.m_Id.end()) In[It->second] = Up[It->second] = Down[It->second] = 1;
            for (bool bMore = true; bMore; )
            {
                bMore = false;
                for (auto [p, c] : M.m_E)
                {
                    if (Down[p] && !Down[c]) { Down[c] = 1; bMore = true; }
                    if (Up[c] && !Up[p])     { Up[p] = 1;   bMore = true; }
                }
            }
            std::vector<std::uint64_t> R;
            for (auto& S : M.m_S) for (int n : S) if (In[n] || (Up[n] && Down[n])) R.push_back(M.m_Ids[n].m_Value);
            return R;
        }

        // One undo step: the steps together at a slot after every producer outside them and before every consumer outside them (nothing else
        // moves), then split off as a new section, collapsed. Refused when no such slot exists without moving other steps.
        void Collapse(const std::vector<std::uint64_t>& Sel) noexcept
        {
            using kind = xmaterial_graph::section::kind;
            auto& G = m_Doc.m_Graph;
            G.m_Stack.Reindex();
            const auto Block = Closure(Sel);
            const ImVec2 At = ToWorld(ImGui::GetIO().MousePos);
            if (Block.empty()) return;
            const cmds::order_model M(G);
            auto SecOf = [&](int n) { return G.m_Stack.m_Where.at(M.m_Ids[n]).first; };
            auto Name  = [&](int n) { return cmds::StepLabel(*m_Doc.FindNode(M.m_Ids[n].m_Value)); };
            std::vector<char> InB(M.m_Ids.size(), 0);
            for (auto s : Block) InB[M.m_Id.at(xmaterial_graph::node_guid{ s })] = 1;
            for (int n = 0; n < static_cast<int>(InB.size()); ++n) if (InB[n] && G.m_Stack.m_Sections[SecOf(n)].m_Kind == kind::OUTPUT) { Note("the output stack does not collapse", At); return; }
            for (auto& S : G.m_Stack.m_Sections)                                       // exactly a section: collapse it
                if (S.m_Steps.size() == Block.size() && std::ranges::all_of(S.m_Steps, [&](auto g) { return Has(Block, g.m_Value); }))
                {
                    xeditor::Run(m_Undo, std::format("SetSectionCollapsed -Section {:016X} -Collapsed 1", S.m_Guid.m_Value));
                    m_Group.clear(); Select(S.m_Guid.m_Value, 0);
                    return;
                }

            std::vector<int> Rest, Pos(M.m_Ids.size(), -1);                            // the reading order without the block
            int T = -1;
            for (auto& S : M.m_S) for (int n : S) { if (InB[n]) { if (T < 0) T = static_cast<int>(Rest.size()); } else { Pos[n] = static_cast<int>(Rest.size()); Rest.push_back(n); } }
            int Lo = 0, Hi = static_cast<int>(Rest.size()), Prod = -1, Cons = -1;
            for (auto [p, c] : M.m_E)
            {
                if (InB[c] && !InB[p] && Pos[p] + 1 > Lo) { Lo = Pos[p] + 1; Prod = p; }
                if (InB[p] && !InB[c] && Pos[c] < Hi)     { Hi = Pos[c];     Cons = c; }
            }
            if (Lo > Hi) { Note(std::format("these steps cannot sit together: {} reads them before {} feeds them", Name(Cons), Name(Prod)), At); return; }
            const int  Slot    = std::clamp(T, Lo, Hi);
            const bool bBefore = Slot < static_cast<int>(Rest.size()) && G.m_Stack.m_Sections[SecOf(Rest[Slot])].m_Kind == kind::USER;
            if (!bBefore && Slot == 0) return;
            const int  Anchor  = bBefore ? Rest[Slot] : Rest[Slot - 1];
            const int  Sec     = SecOf(Anchor);
            const auto& S      = G.m_Stack.m_Sections[Sec];

            std::vector<std::uint64_t> Head, Tail;                                      // the anchor's section without the block, split at the slot
            bool bTail = false;
            for (auto Guid : S.m_Steps)
            {
                if (Has(Block, Guid.m_Value)) continue;
                if (bBefore && Guid == M.m_Ids[Anchor]) bTail = true;
                (bTail ? Tail : Head).push_back(Guid.m_Value);
                if (!bBefore && Guid == M.m_Ids[Anchor]) bTail = true;
            }
            auto Unique = [&](const std::string& Base) { std::string N = Base; for (int k = 2; cmds::SectionNameUsed(G.m_Stack, N); ++k) N = std::format("{} ({})", Base, k); return N; };
            auto List   = [](const std::vector<std::uint64_t>& V) { std::string L; for (auto v : V) L += std::format("{}{:016X}", L.empty() ? "" : ",", v); return L; };
            auto Id     = [] { return xresource::guid_generator::Type64(); };
            const std::uint64_t Group = Id(), SecId = S.m_Guid.m_Value;
            std::vector<std::string> Cmds
            { std::format("PlaceSteps -Nodes {} -{} {:016X}", List(Block), bBefore ? "Before" : "After", M.m_Ids[Anchor].m_Value)       // together at the slot
            , std::format("CreateSection -Section {:016X} -Name {} -{} {:016X}", Group, xeditor::Quote(Unique("Group")), Head.empty() ? "Before" : "After", SecId)
            };
            if (!Head.empty() && !Tail.empty())                                         // the rest of the section after the group: a section of its own
            {
                const std::uint64_t After = Id();
                Cmds.push_back(std::format("CreateSection -Section {:016X} -Name {} -After {:016X}", After, xeditor::Quote(Unique(S.m_Name)), Group));
                Cmds.push_back(std::format("PlaceSteps -Nodes {} -Section {:016X} -First 1", List(Tail), After));
            }
            Cmds.push_back(std::format("PlaceSteps -Nodes {} -Section {:016X} -First 1", List(Block), Group));
            Cmds.push_back(std::format("SetSectionCollapsed -Section {:016X} -Collapsed 1", Group));
            if (!xeditor::RunGroup(m_Undo, "Collapse steps", Cmds)) Note("not collapsed", At);
            else { m_Group.clear(); Select(Group, 0); }
        }

        const xmaterial_graph::section* SectionOf(std::uint64_t Id) const noexcept
        {
            for (auto& S : m_Doc.m_Graph.m_Stack.m_Sections) if (S.m_Guid.m_Value == Id) return &S;
            return nullptr;
        }
        void Expand(std::uint64_t Section) noexcept { xeditor::Run(m_Undo, std::format("SetSectionCollapsed -Section {:016X} -Collapsed 0", Section)); Select(0, 0); }

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
        bool Fits(xmaterial_graph::type_guid Out, xmaterial_graph::type_guid In) const noexcept { return AreTypesCompatible(m_Doc.m_Graph, Out, In); }

        // Why Connect would refuse Out -> In, checked before it runs so a refusal leaves no history (7.7); "" = it would connect
        std::string WhyNot(std::uint64_t Out, std::uint64_t In, bool bTypes = true) const noexcept
        {
            auto& G = m_Doc.m_Graph;
            const node *pO = nullptr, *pI = nullptr;
            const auto *po = PinOf(Out, &pO), *pi = PinOf(In, &pI);
            if (!po || !pi) return "no such pin";
            if (pO == pI) return "the pins are on the same step";
            if (bTypes && !Fits(po->m_TypeGUID, pi->m_TypeGUID)) return cmds::TypesDiffer(G, *po, *pi);
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

        //------------------------------------------------------------------------------------------
        // Components (6.5): the fan beside a vector output while an input is dragged, the picker
        //------------------------------------------------------------------------------------------
        // A whole output (or a component of one) of a vector with components to pick
        bool HasComponents(std::uint64_t Pin) const noexcept { auto* p = MainOutput(m_Doc.m_Graph, xmaterial_graph::pin_guid{ Pin }); return p && !p->m_SubElements.empty(); }

        // Out -> In refused by the types, but some components of Out's output would fit In (the picker can make it)
        bool OffersComponents(std::uint64_t Out, std::uint64_t In) const noexcept
        {
            auto& G = m_Doc.m_Graph;
            auto* pMain = MainOutput(G, xmaterial_graph::pin_guid{ Out });
            auto* pIn   = PinOf(In);
            return pMain && pIn && pMain->m_PinGUID.m_Value == Out && !pMain->m_SubElements.empty() && Components(G, pIn->m_TypeGUID) > 0
                && !*ColorSpaceClash(pMain->m_TypeGUID, pIn->m_TypeGUID) && WhyNot(Out, In, false).empty();
        }

        // The fan: while an input is dragged over a vector output whose components are not shown, a slot per component beside its pin (screen px)
        ImVec2 FanSlot(int k) const noexcept
        {
            const auto& P = m_PinAt.at(m_Fan);
            const ImVec2 s = ToScreen(P.m_W.x, P.m_W.y);
            return { s.x + 24 + 24 * static_cast<float>(k), s.y };
        }
        int FanSize() const noexcept { auto* p = m_Fan ? MainOutput(m_Doc.m_Graph, xmaterial_graph::pin_guid{ m_Fan }) : nullptr; return p ? static_cast<int>(p->m_SubElements.size()) : 0; }
        int FanAt(ImVec2 M) const noexcept
        {
            for (int k = 0; k < FanSize(); ++k) { const ImVec2 c = FanSlot(k); if (std::abs(M.x - c.x) <= 11 && std::abs(M.y - c.y) <= 11) return k; }
            return -1;
        }
        void UpdateFan(ImVec2 M) noexcept
        {
            if (!m_DragPin || m_PinAt.at(m_DragPin).m_bOut) { m_Fan = 0; return; }
            if (m_Fan && !m_PinAt.contains(m_Fan)) m_Fan = 0;
            if (const auto* p = PinAt(M); p && p->second.m_bOut)
            {
                auto* pMain = MainOutput(m_Doc.m_Graph, xmaterial_graph::pin_guid{ p->first });
                const bool bFan = pMain && pMain->m_PinGUID.m_Value == p->first && !pMain->m_SubElements.empty() && !stack_routes::ShowsComponents(m_Doc.m_Graph, *pMain, &m_View.m_Open);
                m_Fan = bFan ? p->first : 0;
                return;
            }
            if (!m_Fan) return;
            const ImVec2 a = FanSlot(0), b = FanSlot(FanSize() - 1);
            if (M.x < a.x - 36 || M.x > b.x + 14 || std::abs(M.y - a.y) > 14) m_Fan = 0;     // left the pin and its slots
        }

        void DrawFan(ImDrawList* D) const noexcept
        {
            namespace st = stack_style;
            const int n = FanSize();
            if (!n) return;
            auto* pMain = MainOutput(m_Doc.m_Graph, xmaterial_graph::pin_guid{ m_Fan });
            const char* L = ComponentLetters(pMain->m_TypeGUID);
            const ImVec2 a = FanSlot(0), b = FanSlot(n - 1);
            D->AddRectFilled({ a.x - 14, a.y - 13 }, { b.x + 13, b.y + 13 }, st::OutputsHalf, 4);
            D->AddRect({ a.x - 14, a.y - 13 }, { b.x + 13, b.y + 13 }, st::Component, 4, 0, 1.0f);
            const int Hover = FanAt(ImGui::GetIO().MousePos);
            for (int k = 0; k < n; ++k)
            {
                const ImVec2 c = FanSlot(k);
                const bool bHover = k == Hover;
                D->AddRectFilled({ c.x - 10, c.y - 10 }, { c.x + 10, c.y + 10 }, bHover ? st::Selected : st::Chip, 3);
                const char Txt[3] = { '.', L[k], 0 };
                const ImVec2 sz = m_pFont->CalcTextSizeA(m_FontSize, FLT_MAX, 0.0f, Txt);
                D->AddText(m_pFont, m_FontSize, { c.x - sz.x * 0.5f, c.y - sz.y * 0.5f }, bHover ? st::Text : st::Component, Txt);
            }
        }

        // The picker for In's components of Out's output: Conn 0 = a connection to make, else the one to change. At: screen.
        void OpenComponents(std::uint64_t Out, std::uint64_t In, std::uint64_t Conn, ImVec2 At) noexcept
        {
            auto* pMain = MainOutput(m_Doc.m_Graph, xmaterial_graph::pin_guid{ Out });
            if (!pMain) return;
            m_Comp = {};
            m_Comp.m_Out = pMain->m_PinGUID.m_Value; m_Comp.m_In = In; m_Comp.m_Conn = Conn; m_Comp.m_At = At;
            m_Open = "##Components";
        }

        void DrawComponents() noexcept
        {
            namespace st = stack_style;
            auto& G = m_Doc.m_Graph;
            ImGui::SetNextWindowPos(m_Comp.m_At, ImGuiCond_Appearing);
            if (!ImGui::BeginPopup("##Components")) return;
            auto* pMain = MainOutput(G, xmaterial_graph::pin_guid{ m_Comp.m_Out });
            auto* pIn   = PinOf(m_Comp.m_In);
            auto* pConn = m_Comp.m_Conn ? Conn(m_Comp.m_Conn) : nullptr;
            if (!pMain || !pIn || (m_Comp.m_Conn && !pConn)) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
            const int nOut = Components(G, pMain->m_TypeGUID), nIn = Components(G, pIn->m_TypeGUID);
            const char* L = ComponentLetters(pMain->m_TypeGUID);
            auto Show = [&](const std::string& Ask) { std::string S = Ask.size() == 1 && nIn >= 2 ? std::string(nIn, Ask[0]) : Ask; for (auto& c : S) if (auto k = std::string_view("xyzw").find(c); k != std::string_view::npos) c = L[k]; return "." + S; };
            auto Apply = [&](const std::string& Ask)
            {
                const auto P = PickComponents(G, *pMain, Ask, *pIn);
                if (!P.m_Error.empty()) return;
                if (pConn) { if (P.m_Out != pConn->m_OutputPinGuid || P.m_Swizzle != pConn->m_Swizzle) xeditor::Run(m_Undo, std::format("SetInputSwizzle -Input {:016X} -Swizzle {}", m_Comp.m_In, Ask.empty() ? "all" : Ask)); }
                else xeditor::Run(m_Undo, std::format("Connect -Output {:016X} -Input {:016X} -Connection {:016X}{}", m_Comp.m_Out, m_Comp.m_In, xresource::guid_generator::Type64(), Ask.empty() ? "" : " -Swizzle " + Ask));
                ImGui::CloseCurrentPopup();
            };

            ImGui::TextDisabled("%s  ->  %s", cmds::ChipText(G, pMain->m_PinGUID).c_str(), PinLabel(m_Comp.m_In).c_str());
            ImGui::Separator();
            // the common picks: all of it (or its first ones), the other runs as long as the input, each component (a vector input splats it)
            std::vector<std::pair<std::string, std::string>> Opt;                       // label, what to ask
            if (PickComponents(G, *pMain, "", *pIn).m_Error.empty()) Opt.push_back({ Show(std::string("xyzw", nOut > nIn && nIn >= 2 ? nIn : nOut)) + (nOut > nIn && nIn >= 2 ? "   first " + std::to_string(nIn) : "   all"), "" });
            for (int o = 1; nIn >= 2 && o + nIn <= nOut; ++o) Opt.push_back({ Show(std::string("xyzw" + o, nIn)), std::string("xyzw" + o, nIn) });
            for (int k = 0; k < nOut && k < 4; ++k) Opt.push_back({ Show(std::string(1, "xyzw"[k])) + (nIn > 1 ? "   splat" : ""), std::string(1, "xyzw"[k]) });
            for (int i = 0; i < static_cast<int>(Opt.size()); ++i)
            {
                const auto P = PickComponents(G, *pMain, Opt[i].second, *pIn);
                if (!P.m_Error.empty()) continue;
                const bool bNow = pConn && P.m_Out == pConn->m_OutputPinGuid && P.m_Swizzle == pConn->m_Swizzle;
                ImGui::PushID(i);
                if (ImGui::Selectable((" " + Opt[i].first).c_str(), bNow, ImGuiSelectableFlags_NoAutoClosePopups)) Apply(Opt[i].second);     // a space: the leading dot clears the edge
                ImGui::PopID();
            }
            ImGui::Separator();
            if (m_Comp.m_bFocus) ImGui::SetKeyboardFocusHere();                       // typing goes here from the start (asked until it holds: the click that opened it may still own the frame)
            ImGui::SetNextItemWidth(150);
            const bool bEnter = ImGui::InputTextWithHint("##Custom", nIn >= 2 ? "type: bgr, xxyy..." : "type: w, a...", m_Comp.m_Text, sizeof(m_Comp.m_Text), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank);
            if (ImGui::IsItemActive()) m_Comp.m_bFocus = false;
            if (m_Comp.m_Text[0])
            {
                const auto P = PickComponents(G, *pMain, m_Comp.m_Text, *pIn);
                if (!P.m_Error.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(st::Error), "%s", P.m_Error.c_str());
                else
                {
                    std::string Ask = m_Comp.m_Text;
                    for (auto& c : Ask) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); if (auto k = std::string_view("rgba").find(c); k != std::string_view::npos) c = "xyzw"[k]; }
                    ImGui::TextDisabled("Enter: takes %s", Show(Ask).c_str());
                    if (bEnter) Apply(Ask);
                }
            }
            ImGui::EndPopup();
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

        // The selected step and the Ctrl+click group, in one undo step (a collapsed box is not a node: it stays; DeleteNode refuses the last output)
        void DeleteSelected(ImVec2 At) noexcept
        {
            std::vector<std::string> Cmds;
            if (m_Doc.FindNode(m_SelStep)) Cmds.push_back(std::format("DeleteNode -Node {:016X}", m_SelStep));
            for (auto Id : m_Group) if (Id != m_SelStep && m_Doc.FindNode(Id)) Cmds.push_back(std::format("DeleteNode -Node {:016X}", Id));
            if (Cmds.empty()) return;
            if (!xeditor::RunGroup(m_Undo, Cmds.size() == 1 ? "Delete step" : "Delete steps", Cmds)) Note("not deleted (the last output node cannot go)", At);
            else { m_Group.clear(); Select(0, 0); }
        }

        void NoteIf(std::string Text, ImVec2 AtWorld) noexcept { if (!Text.empty()) Note(std::move(Text), AtWorld); }     // a refusal, when there is one
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
            DrawFan(D);
            if (const auto* pTo = PinAt(b); pTo && pTo->second.m_bOut != From.m_bOut)
            {
                const std::uint64_t Out = From.m_bOut ? m_DragPin : pTo->first, In = From.m_bOut ? pTo->first : m_DragPin;
                const auto Why = WhyNot(Out, In);
                if (Why.empty()) D->AddCircle(b, Len(stack_style::PIN_R) + 3, stack_style::Selected, 0, 2.0f);
                else if (OffersComponents(Out, In))
                {
                    D->AddCircle(b, Len(stack_style::PIN_R) + 3, stack_style::Component, 0, 2.0f);
                    if (!m_Fan) stack_style::Banner(D, m_pFont, m_FontSize, { b.x, b.y + 14 }, "release: pick components");
                }
                else DrawNote(D, ToWorld(b), Why);
            }
            else if (const int k = FanAt(b); k >= 0) D->AddCircle(FanSlot(k), 12, stack_style::Selected, 0, 2.0f);
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
            std::uint64_t                   m_Subgraph = 0;                 // a group node of this sub-graph (m_Prefab: the Group type)
            std::shared_ptr<const node>     m_pType;                        // its pins: the sub-graph's interface
        };

        // The step type an entry makes: a node type, or a group node with its sub-graph's pins
        const node* TypeOf(const entry& E) const noexcept
        {
            if (E.m_pType) return E.m_pType.get();
            auto It = m_Doc.m_Graph.m_PrefabNodes.find(xmaterial_graph::node_guid{ E.m_Prefab });
            return It == m_Doc.m_Graph.m_PrefabNodes.end() ? nullptr : It->second.get();
        }

        // The sub-graphs a group node here can call, as step types with their interfaces (read once per search: a file each). A sub-graph never
        // offers itself or one that uses it.
        void ReadSubgraphTypes() noexcept
        {
            m_SubgraphTypes.clear();
            const auto Self = m_Doc.m_Graph.m_bSubgraph ? m_Doc.m_Guid.m_Instance.m_Value : 0;
            for (auto& S : cmds::ListSubgraphs())
            {
                xmaterial_graph::graph Sub;
                if (S.m_Guid == Self || !xmaterial_graph::LoadSubgraph(Sub, S.m_Path) || (Self && xmaterial_graph::UsesSubgraph(Sub, Self, cmds::SubgraphPath))) continue;
                auto T = std::make_shared<node>(*m_Doc.m_Graph.m_PrefabNodes.at(xmaterial_graph::group_call_prefab_v));
                T->m_Name = S.m_Name;
                xmaterial_graph::CallPins(m_Doc.m_Graph, *T, Sub);          // the prefab's id as the group's: no instance pin matches, nothing is touched
                m_SubgraphTypes.push_back({ S.m_Guid, std::move(T) });
            }
        }

        void OpenSearch(search::kind K, ImVec2 AtWorld, std::uint64_t Pin, std::uint64_t Connection = 0) noexcept
        {
            m_Search = {};
            m_Search.m_Kind = K; m_Search.m_Pin = Pin; m_Search.m_Conn = Connection; m_Search.m_At = AtWorld;
            if (K == search::kind::REPLACE_ALL)
                for (auto& [Id, C] : m_Doc.m_Graph.m_Connections) if (C->m_OutputPinGuid.m_Value == Pin) m_Search.m_Uses.push_back(C->m_InputPinGuid.m_Value);
            m_Picked.reset();
            m_HoverEntry = m_HoverPort = -1;
            m_Open = "##Search";
            ReadSubgraphTypes();
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
            for (auto& [Guid, pN] : G.m_PrefabNodes) if (!pN->isCommentNode() && !pN->isGroupCallNode() && xmaterial_graph::IsAllowedIn(G, *pN)) Types.push_back(pN.get());     // a group node: one entry per sub-graph (below)
            std::sort(Types.begin(), Types.end(), [](auto* a, auto* b) { return a->m_Name < b->m_Name; });
            if (m_Search.m_Kind != k::INSERT) for (auto& [Sub, pT] : m_SubgraphTypes) Types.push_back(pT.get());          // InsertOnConnection makes a type's step: not a group's
            for (auto* pT : Types)
            {
                if (!Keep(pT->m_Name)) continue;
                entry E{ pT->m_Name, pT->m_Guid.m_Value, 0, {} };
                for (auto& [Sub, pS] : m_SubgraphTypes) if (pS.get() == pT) { E.m_Subgraph = Sub; E.m_pType = pS; E.m_Label += "  (sub-graph)"; }
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
            const auto* pType = TypeOf(E);
            if (!pType) return;
            const auto& T = *pType;
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
            if (E.m_Subgraph) Cmds.push_back(std::format("SetSubgraph -Node {:016X} -Subgraph {:016X}", Node, E.m_Subgraph));     // a group node: its pins, before anything connects to them
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
            const auto* pType = TypeOf(*pE);
            if (!pType) return;
            const auto& T = *pType;
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
                const auto* pType = TypeOf(*m_Picked);
                if (!pType) { m_Picked.reset(); ImGui::EndPopup(); return; }
                const int Side = Varies(m_Picked->m_Ports);
                ImGui::TextUnformatted(std::format("Which {} of {}?", Side == 3 ? "input and output" : Side == 1 ? "input" : "output", m_Picked->m_Label).c_str());
                m_HoverPort = -1;
                for (int i = 0; i < static_cast<int>(m_Picked->m_Ports.size()); ++i)
                {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(PortLabel(*pType, Side, m_Picked->m_Ports[i]).c_str())) { Commit(*m_Picked, m_Picked->m_Ports[i]); ImGui::CloseCurrentPopup(); }
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
                auto* pN   = m_Doc.FindNode(m_SelStep);
                auto* pBox = pN ? nullptr : SectionOf(m_SelStep);
                if (pN || pBox)
                {
                    ImGui::TextDisabled("%s", pN ? cmds::StepLabel(*pN).c_str() : pBox->m_Name.c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Trace upstream",   "U", m_Trace == trace::UP))   Trace(m_Trace == trace::UP   ? trace::NONE : trace::UP);
                    if (ImGui::MenuItem("Trace downstream", "D", m_Trace == trace::DOWN)) Trace(m_Trace == trace::DOWN ? trace::NONE : trace::DOWN);
                }
                if (pN)
                {
                    if (!pN->m_OutputPins.empty() && ImGui::MenuItem("Preview on the mesh", "P")) PreviewSelected();
                    const auto Sel = Selection();
                    const auto Block = Closure(Sel);
                    if (ImGui::MenuItem(std::format("Collapse {} step{}", Block.size(), Block.size() == 1 ? "" : "s").c_str(), "Ctrl+click adds")) Collapse(Sel);
                    if (ImGui::IsItemHovered()) m_GroupHintNext = Block;                // the steps it would take, outlined
                    if (ImGui::MenuItem(std::format("Make sub-graph of {} step{}...", Block.size(), Block.size() == 1 ? "" : "s").c_str(), "Ctrl+G")) AskSubgraphName();
                    if (ImGui::IsItemHovered()) m_GroupHintNext = Block;
                    if (pN->isGroupCallNode())
                    {
                        ImGui::Separator();
                        if (ImGui::MenuItem("Go into the sub-graph", "Tab")) NoteIf(cmds::EnterGroup(m_Doc, pN->m_Guid.m_Value), ToWorld(ImGui::GetIO().MousePos));
                        if (ImGui::MenuItem("Ungroup (its steps here)")) if (!xeditor::RunGroup(m_Undo, "Ungroup", { std::format("UngroupStep -Node {:016X}", pN->m_Guid.m_Value) })) Note("not ungrouped", ToWorld(ImGui::GetIO().MousePos));
                    }
                }
                if (pBox && ImGui::MenuItem("Expand")) Expand(pBox->m_Guid.m_Value);
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
                    if (HasComponents(c->m_OutputPinGuid.m_Value) && ImGui::MenuItem("Components...", "."))
                        OpenComponents(c->m_OutputPinGuid.m_Value, c->m_InputPinGuid.m_Value, Id, ImGui::GetIO().MousePos);
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
                if (ImGui::MenuItem("Show previews in steps", "T", m_bPreviews)) m_bPreviews = !m_bPreviews;
                if (ImGui::MenuItem("Clean graph", nullptr, false, !m_Doc.m_Graph.m_Stack.m_Places.empty())) xeditor::Run(m_Undo, "CleanGraph");
                ImGui::EndPopup();
            }

            if (ImGui::BeginPopup("##MakeSubgraph"))                                 // Make sub-graph: its name, then one undo step here
            {
                ImGui::TextDisabled("New sub-graph of %d step%s", static_cast<int>(m_MakeSteps.size()), m_MakeSteps.size() == 1 ? "" : "s");
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                ImGui::SetNextItemWidth(240);
                const bool bEnter = ImGui::InputText("##Name", m_MakeName, sizeof(m_MakeName), ImGuiInputTextFlags_EnterReturnsTrue);
                if ((bEnter || ImGui::Button("Make")) && m_MakeName[0])
                {
                    std::string Ids;
                    for (auto S : m_MakeSteps) Ids += std::format("{}{:016X}", Ids.empty() ? "" : ",", S);
                    const bool bMade = xeditor::RunGroup(m_Undo, "Make sub-graph", { std::format("MakeSubgraph -Nodes {} -Name {} -Subgraph {:016X} -Node {:016X}", Ids, xeditor::Quote(m_MakeName), xresource::guid_generator::Instance64(), xresource::guid_generator::Type64()) });
                    if (!bMade) Note("the sub-graph was not made (see the log)", m_MenuAt);
                    m_Group.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            DrawSearch();
            DrawComponents();
        }

        // Make sub-graph (subgraphs_plan.md 2.9): the steps of the selection closed over its paths, then their name
        void AskSubgraphName() noexcept
        {
            m_MakeSteps = Closure(Selection());
            if (m_MakeSteps.empty()) return;
            const auto* pFirst = m_Doc.FindNode(m_MakeSteps.front());
            std::snprintf(m_MakeName, sizeof(m_MakeName), "%s Group", pFirst ? cmds::StepLabel(*pFirst).c_str() : "New");
            m_MenuAt = ToWorld(ImGui::GetIO().MousePos);
            m_Open = "##MakeSubgraph";
        }

        // Clean graph (top right): the layout places every step again, forgetting where the person put them (one undo step)
        void DrawCleanButton() noexcept
        {
            const ImVec2 Keep = ImGui::GetCursorScreenPos();
            const char*  pLabel = "Clean graph";
            ImGui::SetCursorScreenPos({ m_Origin.x + m_Avail.x - ImGui::CalcTextSize(pLabel).x - ImGui::GetStyle().FramePadding.x * 2 - 8, m_Origin.y + 6 });
            ImGui::BeginDisabled(m_Doc.m_Graph.m_Stack.m_Places.empty());
            if (ImGui::SmallButton(pLabel)) xeditor::Run(m_Undo, "CleanGraph");
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) xeditor::hint::Text("Lay out every step again, forgetting where you put them (Ctrl+Z brings them back)");
            ImGui::SetCursorScreenPos(Keep);
        }

        // A sub-graph entered from a group: the graphs above it, each a way back (Tab goes to the last)
        void DrawBreadcrumb() noexcept
        {
            auto It = cmds::GroupTrails().find(m_Doc.m_Guid.m_Instance.m_Value);
            if (It == cmds::GroupTrails().end() || It->second.empty()) return;
            const ImVec2 Keep = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos({ ImGui::GetWindowPos().x + 8, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - 6 });     // bottom left: the layout starts at the top
            const int n = static_cast<int>(It->second.size());
            for (int i = 0; i < n; ++i)
            {
                ImGui::PushID(i);
                if (ImGui::SmallButton(It->second[i].m_Name.c_str())) NoteIf(cmds::ExitGroup(m_Doc, n - i), m_MenuAt);
                if (ImGui::IsItemHovered()) xeditor::hint::Text(i == n - 1 ? "Back to the graph this one was entered from (Tab)" : "Back to this graph");
                ImGui::PopID();
                ImGui::SameLine(); ImGui::TextDisabled(">"); ImGui::SameLine();
            }
            ImGui::TextUnformatted(m_Doc.getDisplayName().c_str());
            ImGui::SetCursorScreenPos(Keep);
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
        std::vector<std::pair<std::uint64_t, std::shared_ptr<const node>>> m_SubgraphTypes;    // the sub-graphs the search offers (ReadSubgraphTypes)
        std::vector<std::uint64_t>                  m_MakeSteps;        // what Make sub-graph takes
        char                                        m_MakeName[96] = {};
        int                                         m_HoverEntry = -1, m_HoverPort = -1;
        std::vector<std::uint64_t>                  m_Group;            // steps added to the selection with Ctrl+click
        std::vector<std::uint64_t>                  m_GroupHint, m_GroupHintNext;     // what the menu's Collapse would take (hovered)
        std::uint64_t                               m_DragStep = 0;     // a step being dragged to a column and a height
        float                                       m_DragDy   = 0;     // the grab: mouse minus the step's top, world
        bool                                        m_bDragCancelled = false;          // Esc during a drag: nothing until the button is released
        move_plan                                   m_Drag;
        std::uint64_t                               m_PreviewPin = 0;   // the output previewed on the mesh (0: the material)
        std::string                                 m_PreviewStatus;
        bool                                        m_bPreviews = true; // in-node previews shown (T, the background menu)
    public:
        struct rendered { const void* m_pTexture = nullptr; ImVec2 m_UV0, m_UV1; std::string m_Why; };
        std::function<bool(const node&)>            m_Renders;          // the session's rendered previews (none headless): whether a step shows one
        std::function<rendered(const node&)>        m_Rendered;         //   and its picture (none yet) or why it has none
    private:
        std::unordered_map<std::uint64_t, ImVec2>   m_SuffixAt;         // connection guid -> its chip's component suffix centre on screen, last frame
        std::unordered_map<std::uint64_t, ImVec2>   m_ArrowAt;          // output pin guid -> its open arrow's centre on screen, last frame
        std::uint64_t                               m_Fan = 0;          // while an input is dragged: the vector output whose components fan out
        struct components { std::uint64_t m_Out = 0, m_In = 0, m_Conn = 0; ImVec2 m_At = {}; char m_Text[8] = {}; bool m_bFocus = true; };
        components                                  m_Comp;             // the component picker: whole output, input, connection (0: to make)
    };

    namespace cmds
    {
        //---- SelectStep: view only, a step or a route of the Material Stack selected (its routes lit), as a click does
        struct select_step_cmd : xundo::query_command_base
        {
            graph_document& m_Doc;
            stack_canvas&   m_Canvas;
            select_step_cmd(xundo::system& System, graph_document& Doc, stack_canvas& Canvas) noexcept : query_command_base(System, "SelectStep", nullptr), m_Doc(Doc), m_Canvas(Canvas) { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "View only: selects a step (its input and consumer routes lit) or a connection (its route lit) on the Material Stack, or nothing (no argument); replies where it is on screen. Usage: SelectStep [-Node hexguid|@StepName|collapsed section guid | -Connection hexguid]"; }
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
                    if (!ResolveNodeText(m_Doc, Text, Id, &Err) && !(ParseHex(Text, Id) && m_Canvas.HasStep(Id))) return "SelectStep: " + Err;      // a collapsed section's box: its guid
                    if (!m_Canvas.HasStep(Id)) return "SelectStep: not a step on the canvas";
                    m_Canvas.Select(Id, 0);
                    auto* pN = m_Doc.FindNode(Id);
                    return "selected " + (pN ? StepLabel(*pN) : std::string("a collapsed section")) + "  " + m_Canvas.Where(Id, 0);
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

        //---- PreviewStep: view only, one step's output on the Mesh Preview instead of the material (P / Esc on the canvas)
        struct preview_step_cmd : xundo::query_command_base
        {
            graph_document& m_Doc;
            stack_canvas&   m_Canvas;
            preview_step_cmd(xundo::system& System, graph_document& Doc, stack_canvas& Canvas) noexcept : query_command_base(System, "PreviewStep", nullptr), m_Doc(Doc), m_Canvas(Canvas) { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "View only: shows a step's output (its first, or -Output name) on the Mesh Preview, compiled from a temporary copy of the material; without -Node: back to the material. Usage: PreviewStep [-Node hexguid|@StepName [-Output name]]"; }
            void RegisterArguments() noexcept override
            {
                m_hNode   = m_Parser.addOption("Node",   "The step", false, 1);
                m_hOutput = m_Parser.addOption("Output", "Its output's name (default: the first)", false, 1);
            }
            std::string Query() noexcept override
            {
                std::string Text, Out, Err;
                std::uint64_t Id = 0;
                if (!GetArg(m_Parser, m_hNode, Text)) { m_Canvas.Preview(0); return "previewing the material"; }
                if (!ResolveNodeText(m_Doc, Text, Id, &Err)) return "PreviewStep: " + Err;
                auto* pN = m_Doc.FindNode(Id);
                if (!pN || pN->m_OutputPins.empty()) return "PreviewStep: that step has no output";
                const bool bNamed = GetArg(m_Parser, m_hOutput, Out);
                for (auto& O : pN->m_OutputPins)
                    if (!bNamed || NoSpace(O.m_Name) == NoSpace(Out)) { m_Canvas.Preview(O.m_PinGUID.m_Value); return "previewing " + ChipText(m_Doc.m_Graph, O.m_PinGUID); }
                return "PreviewStep: no output named " + Out;
            }
            xcmdline::parser::handle m_hNode, m_hOutput;
        };
    }
}

#endif // XMATERIAL_STACK_CANVAS_H
