#ifndef XMATERIAL_GRAPH_WIDGETS_H
#define XMATERIAL_GRAPH_WIDGETS_H
#pragma once

// The widgets of an unconnected input, shared by the graph canvas and the stack canvas: a number, a texture, a shader file. They draw at the
// cursor (where, how big and how decorated is the caller's) and every edit is a command on the editor's undo system: SetNodeProperty,
// SetShaderFile. A hovered button returns its full text; the caller shows the hint (the node editor needs ed::Suspend around it).
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_graph_editing.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_inspector_pickers.h"
#include "dependencies/imgui/imgui.h"

#include <commdlg.h>

namespace xmaterial_editor::graph_widgets
{
    inline std::string ParamPath(int iParam, const char* pMember) noexcept { return std::format("node/Params[G:{}]/{}", iParam, pMember); }

    // Runs a command for an edit the widget has already applied: Before is what the value was.
    inline void CommitProperty(xundo::system& Undo, const xmaterial_graph::node& N, const std::string& Path, const std::string& After, const std::string& Before) noexcept
    {
        xeditor::Run(Undo, std::format("SetNodeProperty -Node {:016X} -Path {} -Value {} -Before {}", N.m_Guid.m_Value, xeditor::Quote(Path), xeditor::Quote(After), xeditor::Quote(Before)));
    }

    // A float or int: dragging edits the value live, one command is sent when the drag ends. Before holds the value the drag began with.
    inline void NumberDrag(xundo::system& Undo, xmaterial_graph::node& N, int iParam, const std::string& Id, std::string& Before) noexcept
    {
        auto& Param = N.m_Params[iParam];
        if (Param.m_Type == xmaterial_graph::node_param::type::FLOAT)
        {
            float& Value = Param.m_Value.get<float>();
            const float Pre = Value;
            ImGui::DragFloat(Id.c_str(), &Value, 0.01f);
            if (ImGui::IsItemActivated()) Before = std::format("{:.9g}", Pre);
            if (ImGui::IsItemDeactivatedAfterEdit()) CommitProperty(Undo, N, ParamPath(iParam, "Float"), std::format("{:.9g}", Value), Before);
        }
        else
        {
            int& Value = Param.m_Value.get<int>();
            const int Pre = Value;
            ImGui::DragInt(Id.c_str(), &Value, 0.01f);
            if (ImGui::IsItemActivated()) Before = std::to_string(Pre);
            if (ImGui::IsItemDeactivatedAfterEdit()) CommitProperty(Undo, N, ParamPath(iParam, "Int"), std::to_string(Value), Before);
        }
    }

    // What a texture button says: the texture's name, or "textures" when there is none
    inline std::string TextureLabel(const xmaterial_graph::node_param& Param) noexcept
    {
        std::string Name;
        xresource_editor::RemapGUIDToString(Name, Param.m_Value.get<xresource::full_guid>());
        return (Name.empty() || Name == "None") ? "textures" : Name;
    }

    inline std::string FileLabel(const xmaterial_graph::node_param& Param) noexcept { return xstrtool::To(xstrtool::PathFileName(Param.m_Value.get<std::wstring>())); }

    // A button with the texture's name; pressing it opens the asset browser as a popup and the pick becomes a command.
    inline std::string TextureButton(xundo::system& Undo, xmaterial_graph::node& N, int iParam, ImVec2 Size) noexcept
    {
        auto& Param = N.m_Params[iParam];
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 32, 32, 200));
        auto& Guid = Param.m_Value.get<xresource::full_guid>();

        bool bOpen = false;
        xresource::full_guid NewGuid = {};
        const std::string Name = TextureLabel(Param);
        if (ImGui::Button(std::format("{}##{}", Name, (void*)&Param.m_Value).c_str(), Size)) bOpen = true;
        const std::string Hint = ImGui::IsItemHovered() ? Name : std::string{};
        static constexpr auto Filters = std::array{ xrsc::texture_type_guid_v };
        xresource_editor::ResourceBrowserPopup(&Guid, bOpen, NewGuid, Filters);

        if (!NewGuid.empty() && NewGuid.m_Type == xrsc::texture_type_guid_v && NewGuid != Guid)
            xeditor::Run(Undo, std::format("SetNodeProperty -Node {:016X} -Path {} -Value {}", N.m_Guid.m_Value, xeditor::Quote(ParamPath(iParam, "TextureRef"))
                , xeditor::Quote(std::format("{:X}, {:X}", NewGuid.m_Instance.m_Value, NewGuid.m_Type.m_Value))));
        ImGui::PopStyleColor();
        return Hint;
    }

    // A button with the file's name; pressing it asks for a shader file (SetShaderFile reads it).
    inline std::string FileButton(xundo::system& Undo, xmaterial_graph::node& N, int iParam, ImVec2 Size) noexcept
    {
        auto& Param = N.m_Params[iParam];
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 32, 32, 200));

        const std::wstring FileName = Param.m_Value.get<std::wstring>();
        if (ImGui::Button(std::format("{}##{}", FileLabel(Param), (void*)&Param.m_Value).c_str(), Size))
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
                xeditor::Run(Undo, std::format("SetShaderFile -Node {:016X} -File {}", N.m_Guid.m_Value, xeditor::Quote(xstrtool::To(std::wstring(Buffer)))));
        }
        const std::string Hint = ImGui::IsItemHovered() ? xstrtool::To(FileName) : std::string{};
        ImGui::PopStyleColor();
        return Hint;
    }
}

#endif // XMATERIAL_GRAPH_WIDGETS_H
