#ifndef XMATERIAL_GRAPH_CANVAS_H
#define XMATERIAL_GRAPH_CANVAS_H
#pragma once

// The material graph's canvas: nodes, pins, links, the widgets of unconnected inputs, the create/delete menus. It only draws and asks: every edit it
// makes is a command run on the editor's undo system (xmaterial_graph_editing.h), so what a person does here an AI does from the console.
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_graph_editing.h"
#include "dependencies/xresource_pipeline_v2/source/editor/E10_InspectorPickers.h"
#include "dependencies/imgui-node-editor/imgui_node_editor.h"
#include "dependencies/imgui/imgui_internal.h"

#include <commdlg.h>

namespace xmaterial_editor
{
    namespace ed = ax::NodeEditor;

    class graph_canvas
    {
    public:
        graph_canvas(graph_document& Doc, xundo::system& Undo) noexcept : m_Doc(Doc), m_Undo(Undo)
        {
            ed::Config Config;
            Config.SettingsFile = "";                       // no .json file for the node editor
            m_pEditor = ed::CreateEditor(&Config);
        }

        ~graph_canvas() noexcept { ed::DestroyEditor(m_pEditor); }
        graph_canvas(const graph_canvas&) = delete;

        // The node that is the only one selected (0 when none or several): what the properties panel shows.
        std::uint64_t SelectedNode() const noexcept { return m_Selected; }

        void Render() noexcept
        {
            ed::SetCurrentEditor(m_pEditor);
            Draw();
            std::array<ed::NodeId, 2> Selected;
            const int nSelected = ed::GetSelectedNodes(Selected.data(), static_cast<int>(Selected.size()));
            m_Selected = nSelected == 1 && m_Doc.FindNode(Selected[0].Get()) ? Selected[0].Get() : 0;
            ed::SetCurrentEditor(nullptr);
        }

    private:
        //------------------------------------------------------------------------------------------
        static ImColor ToImColor(const xmaterial_graph::ColorRGBA& C) noexcept { return ImColor(C.r, C.g, C.b, C.a); }

        ImColor IconColor(const xmaterial_graph::type_guid& Type) const noexcept
        {
            const auto* pType = m_Doc.m_Graph.findType(Type);
            return pType ? ToImColor(pType->m_Color) : ImColor(255, 255, 255);
        }

        bool IsPinConnected(const xmaterial_graph::pin_guid& Pin) const noexcept
        {
            for (auto& [Id, Conn] : m_Doc.m_Graph.m_Connections)
                if (Conn->m_InputPinGuid == Pin || Conn->m_OutputPinGuid == Pin) return true;
            return false;
        }

        void DrawPinCircle(const xmaterial_graph::type_guid& Type, const xmaterial_graph::pin_guid& Pin, ImVec2 Size = ImVec2{ 10, 10 }) const noexcept
        {
            const ImColor Color   = IconColor(Type);
            const auto    Cursor  = ImGui::GetCursorScreenPos();
            const auto    Draw    = ImGui::GetWindowDrawList();
            const float   Radius  = Size.x * 0.5f;
            const ImVec2  Center  = ImVec2(Cursor.x + Radius, Cursor.y + Radius);

            if (IsPinConnected(Pin)) Draw->AddCircleFilled(Center, Radius, Color);
            else                     Draw->AddCircle(Center, Radius, Color);
            ImGui::Dummy(Size);
        }

        static void NodeFillColor(xmaterial_graph::node& N, ImVec2 Pos, ImVec2 Size, ImU32 Color, float Rounding = 0, ImDrawFlags Flags = ImDrawFlags_None, bool bBorderOnly = false, ImU32 BorderColor = IM_COL32(200, 200, 200, 200)) noexcept
        {
            auto* pDraw = ed::GetNodeBackgroundDrawList(N.m_Guid.m_Value);
            const ImVec2 Max = ImVec2(Pos.x + Size.x, Pos.y + Size.y);
            pDraw->AddRectFilled(Pos, Max, Color, Rounding, Flags);
            // imgui 1.92.8 swapped AddRect's last two parameters (thickness now comes before flags); with
            // the old order the corner flags silently become a 48-240px border thickness.
#if IMGUI_VERSION_NUM >= 19280
            if (bBorderOnly) pDraw->AddRect(Pos, Max, BorderColor, Rounding, 1.0f, Flags);
#else
            if (bBorderOnly) pDraw->AddRect(Pos, Max, BorderColor, Rounding, Flags, 1.0f);
#endif
        }

        // Runs a command for an edit the widgets have already applied: Before is what the value was.
        void CommitProperty(const xmaterial_graph::node& N, const std::string& Path, const std::string& After, const std::string& Before) noexcept
        {
            xeditor::Run(m_Undo, std::format("SetNodeProperty -Node {:016X} -Path {} -Value {} -Before {}", N.m_Guid.m_Value
                , xeditor::Base64Encode(Path), xeditor::Base64Encode(After), xeditor::Base64Encode(Before)));
        }

        static std::string ParamPath(int iParam, const char* pMember) noexcept { return std::format("node/Params[G:{}]/{}", iParam, pMember); }

        //------------------------------------------------------------------------------------------
        // The widgets of an unconnected input: a number, a texture, a shader file
        //------------------------------------------------------------------------------------------
        void DrawParamWidget(xmaterial_graph::node& N, const xmaterial_graph::input_pin& Pin, float OffsetX, float OffsetY, const ImU32 BorderColor) noexcept
        {
            auto& Param  = N.m_Params[Pin.m_ParamIndex];
            const int iParam = Pin.m_ParamIndex;
            const ImVec2 WidgetPos = ImVec2(N.m_Pos.m_X - OffsetX, N.m_Pos.m_Y + OffsetY);
            ImGui::SetCursorScreenPos(WidgetPos);

            if (Param.m_Type == xmaterial_graph::node_param::type::FLOAT || Param.m_Type == xmaterial_graph::node_param::type::INT)
            {
                NodeFillColor(N, { WidgetPos.x - 23.f, WidgetPos.y + 1.3f }, { 85, 18 }, IM_COL32(96, 96, 96, 200), ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersNone, true, BorderColor);

                const float TextHeight = ImGui::GetTextLineHeight();
                const float DragHeight = ImGui::GetFrameHeight();
                const float Offset     = (DragHeight - TextHeight) * 0.5f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 16.f);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Offset + 2.f);
                ImGui::Text("%s", std::format("{}", Pin.m_Name[0]).c_str());

                ImGui::SameLine();
                ImGui::SetNextItemWidth(45);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 2.f);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 1.6f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(32, 32, 32, 200));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 1.f));

                // Dragging edits the value live; one command is sent when the drag ends
                const std::string Id = "##" + Pin.m_Name + std::to_string(N.m_Guid.m_Value);
                if (Param.m_Type == xmaterial_graph::node_param::type::FLOAT)
                {
                    float& Value = Param.m_Value.get<float>();
                    const float Pre = Value;
                    ImGui::DragFloat(Id.c_str(), &Value, 0.01f);
                    if (ImGui::IsItemActivated()) m_Before = std::format("{:.9g}", Pre);
                    if (ImGui::IsItemDeactivatedAfterEdit()) CommitProperty(N, ParamPath(iParam, "Float"), std::format("{:.9g}", Value), m_Before);
                }
                else
                {
                    int& Value = Param.m_Value.get<int>();
                    const int Pre = Value;
                    ImGui::DragInt(Id.c_str(), &Value, 0.01f);
                    if (ImGui::IsItemActivated()) m_Before = std::to_string(Pre);
                    if (ImGui::IsItemDeactivatedAfterEdit()) CommitProperty(N, ParamPath(iParam, "Int"), std::to_string(Value), m_Before);
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();

                auto* pDraw = ImGui::GetWindowDrawList();
                const ImVec2 Center = ImVec2(WidgetPos.x + 53, WidgetPos.y + 10);
                const float  Radius = 5.f;
                pDraw->AddCircleFilled(Center, Radius, IM_COL32(64, 200, 64, 255));
                pDraw->AddLine(ImVec2(Center.x + Radius, Center.y), ImVec2(Center.x + 25.0f, Center.y), IM_COL32(64, 200, 64, 255), 2.0f);
            }
            else if (Param.m_Type == xmaterial_graph::node_param::type::TEXTURE_RESOURCE || Param.m_Type == xmaterial_graph::node_param::type::FILE)
            {
                const bool bTexture = Param.m_Type == xmaterial_graph::node_param::type::TEXTURE_RESOURCE;
                if (bTexture)
                {
                    NodeFillColor(N, { WidgetPos.x - 63.f - 30, WidgetPos.y + 0.3f }, { 125 + 30, 19 }, IM_COL32(96, 96, 96, 255), ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersNone, true, BorderColor);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 60.f - 25);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.f);
                    ImGui::Text("Rsc");
                    ImGui::SameLine(0, 5);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.f);
                    DrawTextureButton(N, iParam, Param);
                }
                else
                {
                    NodeFillColor(N, { WidgetPos.x - 63.f - 35, WidgetPos.y + 0.3f }, { 125 + 35, 19 }, IM_COL32(96, 96, 96, 255), ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersNone, true, BorderColor);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 60.f - 32);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.f);
                    ImGui::Text("File");
                    ImGui::SameLine(0, 68 - 5);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5.f);
                    DrawFileButton(N, Param);
                }

                auto* pDraw = ImGui::GetWindowDrawList();
                const ImVec2 Center = ImVec2(WidgetPos.x + 53, WidgetPos.y + 10);
                const float  Radius = 5.f;
                pDraw->AddCircleFilled(Center, Radius, IM_COL32(64, 200, 64, 255));
                pDraw->AddLine(ImVec2(Center.x + Radius, Center.y), ImVec2(Center.x + 25.0f, Center.y), IM_COL32(64, 200, 64, 255), 2.0f);
            }
        }

        // A button with the texture's name; pressing it opens the asset browser as a popup and the pick becomes a command.
        void DrawTextureButton(xmaterial_graph::node& N, int iParam, xmaterial_graph::node_param& Param) noexcept
        {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 32, 32, 200));
            auto& Guid = Param.m_Value.get<xresource::full_guid>();

            bool bOpen = false;
            xresource::full_guid NewGuid = {};
            std::string Name;
            e10::RemapGUIDToString(Name, Guid);
            if (ImGui::Button(std::format("{}##{}", (Name.empty() || Name == "None") ? "textures" : Name, (void*)&Param.m_Value).c_str(), ImVec2(100, 14))) bOpen = true;
            static constexpr auto Filters = std::array{ xrsc::texture_type_guid_v };
            e10::ResourceBrowserPopup(&Guid, bOpen, NewGuid, Filters);
            ed::Suspend();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Name.c_str());
            ed::Resume();

            if (!NewGuid.empty() && NewGuid.m_Type == xrsc::texture_type_guid_v && NewGuid != Guid)
                xeditor::Run(m_Undo, std::format("SetNodeProperty -Node {:016X} -Path {} -Value {}", N.m_Guid.m_Value, xeditor::Base64Encode(ParamPath(iParam, "TextureRef"))
                    , xeditor::Base64Encode(std::format("{:X}, {:X}", NewGuid.m_Instance.m_Value, NewGuid.m_Type.m_Value))));
            ImGui::PopStyleColor();
        }

        // A button with the file's name; pressing it asks for a shader file (SetShaderFile reads it).
        void DrawFileButton(xmaterial_graph::node& N, xmaterial_graph::node_param& Param) noexcept
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 60.f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.f);
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 32, 32, 200));

            const std::wstring FileName = Param.m_Value.get<std::wstring>();
            if (ImGui::Button(std::format("{}##{}", xstrtool::To(xstrtool::PathFileName(FileName)), (void*)&Param.m_Value).c_str(), ImVec2(102, 14)))
            {
                wchar_t Buffer[MAX_PATH] = { 0 };
                OPENFILENAMEW Ofn = { 0 };
                Ofn.lStructSize     = sizeof(Ofn);
                Ofn.lpstrFilter     = xmaterial_graph::node_param::file_filter_v;
                Ofn.lpstrFile       = Buffer;
                Ofn.nMaxFile        = MAX_PATH;
                Ofn.Flags           = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
                Ofn.lpstrDefExt     = L"txt";
                Ofn.lpstrInitialDir = xproperty::member_ui<std::wstring>::g_CurrentPath.c_str();
                if (GetOpenFileNameW(&Ofn))
                    xeditor::Run(m_Undo, std::format("SetShaderFile -Node {:016X} -File {}", N.m_Guid.m_Value, xeditor::Base64Encode(xstrtool::To(std::wstring(Buffer)))));
            }
            ed::Suspend();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", xstrtool::To(FileName).c_str());
            ed::Resume();
            ImGui::PopStyleColor();
        }

        //------------------------------------------------------------------------------------------
        void DrawComment(xmaterial_graph::node& N, ImU32 BorderColor) noexcept
        {
            (void)BorderColor;
            if (!ed::BeginGroupHint(N.m_Guid.m_Value)) { ed::EndGroupHint(); return; }

            const auto BgAlpha = static_cast<int>(ImGui::GetStyle().Alpha * 255);
            const auto Min = ed::GetGroupMin();
            ImGui::SetCursorScreenPos({ Min.x - 8, Min.y - ImGui::GetTextLineHeightWithSpacing() + 8 });
            ImGui::BeginGroup();
            ImGui::PushID(static_cast<int>(N.m_Guid.m_Value));
            ImGui::SetNextItemWidth(100);

            auto& Comment = N.m_Params[0].m_Value.get<std::string>();
            const auto CommentSize = ed::GetNodeSize(N.m_Guid.m_Value);                 // the size follows the canvas: layout, not an edit
            N.m_Params[1].m_Value.get<float>() = CommentSize.x;
            N.m_Params[2].m_Value.get<float>() = CommentSize.y;

            char Buffer[256];
            strncpy_s(Buffer, Comment.c_str(), sizeof(Buffer));
            Buffer[sizeof(Buffer) - 1] = '\0';
            if (ImGui::InputTextMultiline("##Comment", Buffer, IM_ARRAYSIZE(Buffer), ImVec2(100, 50), ImGuiInputTextFlags_AllowTabInput)) Comment = Buffer;
            const bool bActivated = ImGui::IsItemActivated();
            const bool bDone      = ImGui::IsItemDeactivatedAfterEdit();
            const bool bActive    = ImGui::IsItemActive();
            ImGui::PopID();
            ImGui::EndGroup();

            if (bActivated) m_Before = m_CommentBefore;
            if (bDone)      CommitProperty(N, ParamPath(0, "String"), Comment, m_Before);
            if (!bActive) m_CommentBefore = Comment;

            auto* pDraw = ImGui::GetForegroundDrawList();
            ImRect Frame(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            Frame.Min.x -= 8; Frame.Min.y -= 4; Frame.Max.x += 8; Frame.Max.y += 4;
            pDraw->AddRectFilled(Frame.GetTL(), Frame.GetBR(), IM_COL32(255, 255, 255, 64 * BgAlpha / 255), 4.0f);
            pDraw->AddRect(Frame.GetTL(), Frame.GetBR(), IM_COL32(255, 255, 255, 128 * BgAlpha / 255), 4.0f);
            ed::EndGroupHint();
        }

        //------------------------------------------------------------------------------------------
        void Draw() noexcept
        {
            auto& G = m_Doc.m_Graph;
            ed::Begin("Material Graph Editor");

            const auto BorderOutline = IM_COL32(40, 40, 40, 255);
            ed::PushStyleVar(ed::StyleVar_NodeRounding, 3.5f);
            ed::PushStyleColor(ed::StyleColor_NodeBorder, ImColor(BorderOutline));

            // The nodes moved behind the canvas's back (a load, an undo, a command): move them here
            bool bJustMoved = false;
            if (m_Doc.m_bPositionsChanged)
            {
                m_Doc.m_bPositionsChanged = false;
                bJustMoved = true;
                for (auto& [Id, pNode] : G.m_InstanceNodes)
                {
                    ed::SetNodePosition(Id.m_Value, ImVec2(pNode->m_Pos.m_X, pNode->m_Pos.m_Y));
                    m_Committed[Id.m_Value] = ImVec2(pNode->m_Pos.m_X, pNode->m_Pos.m_Y);
                }
            }

            const float CharacterWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, "A").x;

            for (auto& [Id, pNode] : G.m_InstanceNodes)
            {
                if (pNode == nullptr) continue;
                auto& N = *pNode;
                if (N.isCommentNode())
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.75f);
                    ed::PushStyleColor(ed::StyleColor_NodeBg, ImColor(255, 255, 255, 64));
                }

                ed::BeginNode(N.m_Guid.m_Value);
                ImGui::TextUnformatted(N.m_Name.c_str());
                ImGui::Dummy({ 0.f, 5.f });                                     // space between the header and the content

                const float LineWidth1 = N.m_OutputPins.empty() ? std::max(N.m_Name.length(), N.m_MaxInputChars + 4ull) * CharacterWidth : (N.m_MaxInputChars + 6ull) * CharacterWidth;
                const float LineWidth2 = [&]
                {
                    if (N.m_InputPins.empty()) return std::max(N.m_Name.length() - 3ull, N.m_MaxOutputChars + 4ull) * CharacterWidth;
                    const auto L = (N.m_MaxOutputChars + 6ull) * CharacterWidth;
                    const auto S = N.m_Name.length() * CharacterWidth - LineWidth1;
                    return S > 0 ? (L - S + (0.5f * CharacterWidth)) : LineWidth1 > L ? L + (1 * CharacterWidth) : L - (2.5f * CharacterWidth);
                }();

                if (N.isCommentNode()) ed::Group(ImVec2(N.m_Params[1].m_Value.get<float>(), N.m_Params[2].m_Value.get<float>()));

                if (N.m_InputPins.size())
                {
                    ImGui::BeginGroup();
                    for (auto& Ip : N.m_InputPins)
                    {
                        ed::BeginPin(Ip.m_PinGUID.m_Value, ed::PinKind::Input);
                        ed::PinPivotAlignment(ImVec2(0.f, 0.5f));
                        DrawPinCircle(Ip.m_TypeGUID, Ip.m_PinGUID);
                        ImGui::SameLine();
                        ImGui::Text("%s", Ip.m_Name.c_str());
                        ed::EndPin();
                        ImGui::Dummy({ 0.f, 1.f });
                    }
                    ImGui::EndGroup();
                }

                if (N.m_OutputPins.size())
                {
                    if (!N.m_InputPins.empty()) ImGui::SameLine(LineWidth2);
                    ImGui::BeginGroup();
                    auto DrawOutput = [&](const xmaterial_graph::pin& Op)
                    {
                        const ImVec2 ScreenPos = ImGui::GetCursorScreenPos();
                        ImGui::SetCursorScreenPos({ ScreenPos.x + LineWidth2 - (Op.m_Name.length()) * CharacterWidth, ScreenPos.y });
                        ed::BeginPin(Op.m_PinGUID.m_Value, ed::PinKind::Output);
                        ImGui::TextUnformatted(Op.m_Name.c_str());
                        ImGui::SameLine();
                        ed::PinPivotAlignment(ImVec2(1.f, 0.5f));
                        DrawPinCircle(Op.m_TypeGUID, Op.m_PinGUID);
                        ed::EndPin();
                        ImGui::Dummy({ 0.f, 1.f });
                    };
                    for (auto& Op : N.m_OutputPins)
                    {
                        DrawOutput(Op);
                        for (auto& Sub : Op.m_SubElements) DrawOutput(Sub);
                    }
                    ImGui::EndGroup();
                }

                ed::EndNode();
                if (N.isCommentNode())
                {
                    ImGui::PopStyleVar();                   // the alpha pushed above
                    ed::PopStyleColor(1);
                }

                if (!bJustMoved)
                {
                    const auto NodePos = ed::GetNodePosition(Id.m_Value);
                    N.m_Pos = { NodePos.x, NodePos.y };
                }

                // The header and body colours
                const float HeaderHeight = 26.f;
                {
                    const auto Size = ed::GetNodeSize(N.m_Guid.m_Value);
                    const auto Pos  = ed::GetNodePosition(N.m_Guid.m_Value);
                    if (N.isFunctionNode())
                    {
                        const bool bExpose = std::ranges::any_of(N.m_Params, [](auto& P) { return P.m_bExpose; });
                        NodeFillColor(N, Pos, { Size.x, HeaderHeight + 1 }, bExpose ? IM_COL32(200, 200, 96, 128) : IM_COL32(100, 100, 100, 128), ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersTop, true, BorderOutline);
                    }
                    else if (N.isInputNode())  NodeFillColor(N, Pos, { Size.x, HeaderHeight + 1 }, IM_COL32(22, 128, 22, 128),  ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersTop, true, BorderOutline);
                    else if (N.isOutputNode()) NodeFillColor(N, Pos, { Size.x, HeaderHeight + 1 }, IM_COL32(148, 48, 148, 128), ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersTop, true, BorderOutline);
                    else if (N.isCommentNode()) DrawComment(N, BorderOutline);
                }
                if (!N.isCommentNode())
                {
                    auto Pos  = ed::GetNodePosition(N.m_Guid.m_Value);
                    auto Size = ed::GetNodeSize(N.m_Guid.m_Value);
                    Pos.y += HeaderHeight;
                    if (N.m_OutputPins.empty())     NodeFillColor(N, { Pos.x, Pos.y }, { Size.x, Size.y - HeaderHeight }, IM_COL32(96, 96, 96, 128), ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersBottom, true, BorderOutline);
                    else if (N.m_InputPins.empty()) NodeFillColor(N, { Pos.x, Pos.y }, { Size.x, Size.y - HeaderHeight }, IM_COL32(32, 32, 32, 128), ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersBottom, true, BorderOutline);
                    else if (N.isFunctionNode())
                    {
                        const float End = LineWidth1;
                        NodeFillColor(N, { Pos.x, Pos.y }, { End + 1, Size.y - HeaderHeight }, IM_COL32(96, 96, 96, 128), ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersBottomLeft, true, BorderOutline);
                        NodeFillColor(N, { Pos.x + End, Pos.y }, { Size.x - End, Size.y - HeaderHeight }, IM_COL32(32, 32, 32, 128), ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersBottomRight, true, BorderOutline);
                    }
                }

                // A node the compiler complained about
                if (N.m_HasErrMsg)
                    NodeFillColor(N, ed::GetNodePosition(N.m_Guid.m_Value), ed::GetNodeSize(N.m_Guid.m_Value), IM_COL32(255, 0, 0, 150), ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersAll);
                ed::Suspend();
                if (N.m_HasErrMsg && !N.m_ErrMsg.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", N.m_ErrMsg.c_str());
                ed::Resume();

                // The widgets floating over the node for its unconnected inputs
                float OffsetY = 28.f;
                for (auto& Ip : N.m_InputPins)
                {
                    if (!IsPinConnected(Ip.m_PinGUID) && Ip.m_ParamIndex >= 0 && Ip.m_ParamIndex < static_cast<int>(N.m_Params.size()))
                        DrawParamWidget(N, Ip, 70.f, OffsetY, BorderOutline);
                    OffsetY += 21.f;
                }
            }

            ed::PopStyleVar();
            ed::PopStyleColor(1);

            // Links, the selected node's in yellow
            for (auto& [Cid, pConn] : G.m_Connections)
            {
                const auto& C = *pConn;
                auto* pOutNode = G.findNodeByPin(C.m_OutputPinGuid);
                if (!pOutNode) continue;

                int Index, Sub; bool bIsInput;
                const auto* pOutPin = G.findPinConst(*pOutNode, C.m_OutputPinGuid, bIsInput, Index, Sub);
                ImColor Color = pOutPin ? IconColor(pOutPin->m_TypeGUID) : ImColor(200, 200, 200);

                bool bHighlight = false;
                if (m_Selected != 0)
                    if (auto* pSelected = m_Doc.FindNode(m_Selected)) bHighlight = pSelected == G.findNodeByPin(C.m_InputPinGuid) || pSelected == G.findNodeByPin(C.m_OutputPinGuid);
                if (bHighlight) ed::Link(Cid.m_Value, C.m_OutputPinGuid.m_Value, C.m_InputPinGuid.m_Value, ImColor(255, 255, 0), 3.0f);
                else            ed::Link(Cid.m_Value, C.m_OutputPinGuid.m_Value, C.m_InputPinGuid.m_Value, Color);
            }

            // Creating a link: drag from one pin to another
            if (ed::BeginCreate())
            {
                ed::PinId A, B;
                if (ed::QueryNewLink(&A, &B))
                {
                    const xmaterial_graph::pin_guid PinA{ A.Get() }, PinB{ B.Get() };
                    auto* pNodeA = G.findNodeByPin(PinA);
                    auto* pNodeB = G.findNodeByPin(PinB);
                    bool bAIsInput = false, bBIsInput = false;
                    int Index = -1, Sub = -1;
                    const xmaterial_graph::pin* pA = pNodeA ? G.findPinConst(*pNodeA, PinA, bAIsInput, Index, Sub) : nullptr;
                    const xmaterial_graph::pin* pB = pNodeB ? G.findPinConst(*pNodeB, PinB, bBIsInput, Index, Sub) : nullptr;

                    const xmaterial_graph::pin* pOut = nullptr; const xmaterial_graph::pin* pIn = nullptr;
                    if (pA && pB)
                    {
                        if (!bAIsInput && bBIsInput)      { pOut = pA; pIn = pB; }
                        else if (!bBIsInput && bAIsInput) { pOut = pB; pIn = pA; }
                    }

                    if (pOut && pIn)
                    {
                        if (IsPinCompatible(*pOut, *pIn, G))
                        {
                            if (ed::AcceptNewItem(ImColor(0, 255, 0), 2.0f))
                                xeditor::Run(m_Undo, std::format("Connect -Output {:016X} -Input {:016X} -Connection {:016X}", pOut->m_PinGUID.m_Value, pIn->m_PinGUID.m_Value, xresource::guid_generator::Type64()));
                        }
                        else ed::RejectNewItem(ImColor(255, 0, 0), 2.0f);
                    }
                }
            }
            ed::EndCreate();

            // Deleting a link
            if (ed::BeginDelete())
            {
                ed::LinkId Lid;
                while (ed::QueryDeletedLink(&Lid))
                    if (ed::AcceptDeletedItem()) xeditor::Run(m_Undo, std::format("Disconnect -Connection {:016X}", Lid.Get()));
            }
            ed::EndDelete();

            // Moving nodes: the drag edits the positions live, one command per node when it ends
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !bJustMoved)
            {
                std::vector<std::string> Moves;
                for (auto& [Id, pNode] : G.m_InstanceNodes)
                {
                    auto& Was = m_Committed[Id.m_Value];
                    if (std::abs(Was.x - pNode->m_Pos.m_X) > 0.5f || std::abs(Was.y - pNode->m_Pos.m_Y) > 0.5f)
                    {
                        Moves.push_back(std::format("MoveNode -Node {:016X} -X {:.3f} -Y {:.3f} -BeforeX {:.3f} -BeforeY {:.3f}", Id.m_Value, pNode->m_Pos.m_X, pNode->m_Pos.m_Y, Was.x, Was.y));
                        Was = ImVec2(pNode->m_Pos.m_X, pNode->m_Pos.m_Y);
                    }
                }
                if (!xeditor::RunGroup(m_Undo, "Move Nodes", Moves)) m_Doc.m_bPositionsChanged = true;
            }

            DrawMenus();
            ed::End();
        }

        void DrawMenus() noexcept
        {
            auto& G = m_Doc.m_Graph;
            const auto PopUpPos = ImGui::GetMousePos();
            ed::Suspend();

            if (ImGui::IsWindowHovered())
            {
                if (ed::ShowBackgroundContextMenu())                 { m_MenuCanvasPos = ed::ScreenToCanvas(PopUpPos); ImGui::OpenPopup("CreateNodePopup"); }
                else if (ed::ShowNodeContextMenu(&m_ContextNode))    ImGui::OpenPopup("Node Context Menu");
            }

            if (ImGui::BeginPopup("Node Context Menu"))
            {
                if (auto* pNode = m_Doc.FindNode(m_ContextNode.Get()))
                {
                    ImGui::TextUnformatted(pNode->m_Name.c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete Node"))
                    {
                        if (m_Selected == m_ContextNode.Get()) ed::ClearSelection();
                        xeditor::Run(m_Undo, std::format("DeleteNode -Node {:016X}", m_ContextNode.Get()));
                        ImGui::CloseCurrentPopup();
                    }
                }
                else ImGui::Text("Unknown node: %p", m_ContextNode.AsPointer());
                ImGui::EndPopup();
            }

            if (ImGui::BeginPopup("CreateNodePopup"))
            {
                for (auto& [PrefabId, pPrefab] : G.m_PrefabNodes)
                    if (ImGui::MenuItem(pPrefab->m_Name.c_str()))
                        xeditor::Run(m_Undo, std::format("CreateNode -Prefab {:016X} -Node {:016X} -X {:.3f} -Y {:.3f}", PrefabId.m_Value, xresource::guid_generator::Type64(), m_MenuCanvasPos.x, m_MenuCanvasPos.y));
                ImGui::EndPopup();
            }
            ed::Resume();
        }

        graph_document&                                 m_Doc;
        xundo::system&                                  m_Undo;
        ed::EditorContext*                              m_pEditor = nullptr;
        std::uint64_t                                   m_Selected = 0;
        ed::NodeId                                      m_ContextNode = 0;
        ImVec2                                          m_MenuCanvasPos = {};
        std::unordered_map<std::uint64_t, ImVec2>       m_Committed;            // where each node was when the last move command ran
        std::string                                     m_Before;               // the value of the widget being edited, when the edit began
        std::string                                     m_CommentBefore;
    };
}

#endif // XMATERIAL_GRAPH_CANVAS_H
