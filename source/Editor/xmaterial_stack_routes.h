#ifndef XMATERIAL_STACK_ROUTES_H
#define XMATERIAL_STACK_ROUTES_H
#pragma once

// The material stack's router is xgraph's (dependencies/xgraph/source/xgraph_routes.h; render graph stage F1): its names here, and below
// the material's own part - the graph read as the router's input (FromGraph), rows, components
#include "xmaterial_stack_layout.h"
#include "dependencies/xgraph/source/xgraph_routes.h"

namespace xmaterial_editor::stack_routes { using namespace xgraph::routes; }

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
                         , const std::unordered_set<std::uint64_t>* pOpen = nullptr, const std::function<float(const xmaterial_graph::node&)>* pNoteW = nullptr) noexcept      // pNoteW: a step's note row (the output's passes): wide enough for it
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
                const float TextW = std::max({ Measure(N.m_StepName.empty() ? N.m_Name : N.m_StepName + "  " + N.m_Name), InW + OutW + (nIn && nOut ? sl::geo::MID_GAP : 0), pNoteW && *pNoteW ? (*pNoteW)(N) : 0.0f });
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
        std::function<float(const xmaterial_graph::node&)> m_NoteW;                                                          // a step's note row's width (the output's passes)
        std::unordered_set<std::uint64_t>                m_Open;            // the vector outputs the person opened on the canvas: their components show (ShowsComponents)
    };

    // The layout the canvas shows
    inline stack_routes::result StackLayoutOf(const graph_document& Doc, const stack_view& View, stack_routes::graph_input& In, double& Ms) noexcept
    {
        In = stack_routes::FromGraph(Doc.m_Graph, View.m_Measure, View.m_InputW, View.m_LitW, View.m_PreviewRows, nullptr, &View.m_Open, &View.m_NoteW);
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
