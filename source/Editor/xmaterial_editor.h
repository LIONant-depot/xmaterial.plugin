#ifndef XMATERIAL_EDITOR_H
#define XMATERIAL_EDITOR_H
#pragma once

// The Material editor: opens a material from the asset browser in its own window. The material is a graph of nodes edited on a canvas (every edit is
// an undoable command, so an AI drives it from the console); the compiled material is previewed on a mesh and the shader the compiler made is shown as
// text. Hosts include this header and open editors through xeditor::open_resource_editors.
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_graph_canvas.h"
#include "source/Tools/Editor/xeditor_mesh_preview.h"
#include "source/Tools/Editor/xeditor_text_widget.h"
#include "dependencies/xresource_pipeline_v2/source/editor/E10_Resources.h"
#include "plugins/xmaterial.plugin/source/xmaterial_xgpu_rsc_loader.h"
#include "plugins/xmaterial.plugin/source/xmaterial_runtime.h"
#include "plugins/xmaterial.plugin/source/xmaterial_xgpu_rsc_loader.cpp"        // the resource loader: compiled once, in the host's translation unit

#include <regex>

namespace xmaterial_editor
{
    struct session : xeditor::document_editor<graph_document>
    {
        cmds::create_node_cmd           m_CreateNode;
        cmds::delete_node_cmd           m_DeleteNode;
        cmds::connect_cmd               m_Connect;
        cmds::disconnect_cmd            m_Disconnect;
        cmds::set_shader_file_cmd       m_SetShaderFile;
        cmds::move_node_cmd             m_MoveNode;
        cmds::set_node_property_cmd     m_SetNodeProperty;
        cmds::list_node_types_cmd       m_ListNodeTypes;
        cmds::list_nodes_cmd            m_ListNodes;
        cmds::node_properties_cmd       m_NodeProperties;

        graph_canvas                    m_Canvas;
        xeditor::inspector_panel        m_NodeInspector{ "Node Properties" };
        std::uint64_t                   m_InspectedNode = 0;
        xeditor::mesh_preview           m_Preview;
        xeditor::mesh_preview_cmds      m_PreviewCmds;
        TextEditor                      m_Shader;
        xrsc::material_ref              m_MaterialRef;

        session(xresource::full_guid Guid, e10::library::guid LibraryGuid, xgpu::device* pDevice) noexcept
            : document_editor("Material", Guid, LibraryGuid, pDevice)
            , m_PreviewCmds(m_Undo, m_Preview)
            , m_CreateNode(m_Undo, m_Document), m_DeleteNode(m_Undo, m_Document), m_Connect(m_Undo, m_Document), m_Disconnect(m_Undo, m_Document)
            , m_SetShaderFile(m_Undo, m_Document), m_MoveNode(m_Undo, m_Document), m_SetNodeProperty(m_Undo, m_Document)
            , m_ListNodeTypes(m_Undo, m_Document), m_ListNodes(m_Undo, m_Document), m_NodeProperties(m_Undo, m_Document)
            , m_Canvas(m_Document, m_Undo)
        {
            e10::WireResourcePickerCallbacks(m_NodeInspector.m_Inspector);
            m_Shader.SetLanguageDefinition(TextEditor::LanguageDefinition::GLSL());
            m_Shader.SetImGuiChildIgnored(true);                // it draws in its panel's window
            m_Shader.SetReadOnly(true);                         // a view of the shader the graph makes (or of the node's shader file)

            AddPanel("Mesh Preview",    dock::left,        [this] { m_Preview.Render(); });
            AddPanel("Node Properties", dock::left_bottom, [this] { RenderNodeProperties(); });
            AddPanel("Material Graph",  dock::center,      [this] { m_Canvas.Render(); });
            AddPanel("Shader",          dock::bottom,      [this] { m_Shader.Render("Shader", ImVec2(0, 0), true, [] {}); }, ImGuiWindowFlags_HorizontalScrollbar);

            LoadShaderText(false);
            if (pDevice && m_Preview.Init(*pDevice) && std::filesystem::exists(m_Document.m_ResourcePath)) ReloadPreview();
        }

        ~session() noexcept override { xresource::g_Mgr.ReleaseRef(m_MaterialRef); }

        void OnDocumentReplaced() noexcept override { m_InspectedNode = 0; m_NodeInspector.Clear(); }

        void OnCompiled() noexcept override
        {
            ClearErrors();
            ReloadPreview();
            LoadShaderText(true);
        }

        // The compiler's log names the nodes it did not like (N_<node id>): they turn red and say why.
        void OnCompileFailed() noexcept override
        {
            ClearErrors();
            std::string Text;
            if (m_CompilationLog)
            {
                xcontainer::lock::scope Lock(*m_CompilationLog);
                Text = m_CompilationLog->get().m_Log;
            }
            static const std::regex NodeName(R"(N_(\d+))");
            std::size_t Start = 0;
            while (Start < Text.size())
            {
                auto End = Text.find('\n', Start);
                if (End == std::string::npos) End = Text.size();
                const std::string Line = Text.substr(Start, End - Start);
                Start = End + 1;

                std::smatch Match;
                if (xstrtool::findI(Line, "ERROR:") == std::string::npos || !std::regex_search(Line, Match, NodeName)) continue;
                for (auto& [Id, pNode] : m_Document.m_Graph.m_InstanceNodes)
                    if (std::to_string(Id.m_Value) == Match[1].str()) { pNode->m_HasErrMsg = true; pNode->m_ErrMsg = Line; }
            }
        }

        void ClearErrors() noexcept
        {
            for (auto& [Id, pNode] : m_Document.m_Graph.m_InstanceNodes) { pNode->m_HasErrMsg = false; pNode->m_ErrMsg.clear(); }
        }

        // The selected node's properties: an edit is a command, like everything else.
        void RenderNodeProperties() noexcept
        {
            const std::uint64_t Selected = m_Canvas.SelectedNode();
            if (Selected != m_InspectedNode)
            {
                m_InspectedNode = Selected;
                m_NodeInspector.Clear();
                if (auto* pNode = Selected ? m_Document.FindNode(Selected) : nullptr)
                {
                    m_NodeInspector.m_Inspector.AppendEntity();
                    m_NodeInspector.AppendComponent(*xproperty::getObject(*pNode), pNode);
                    m_NodeInspector.m_Inspector.m_OnChangeEvent.m_Delegates.clear();
                    m_NodeInspector.m_Inspector.m_OnChangeEvent.Register<&session::OnNodePropertyChange>(*this);
                }
            }
            m_NodeInspector.Show();
        }

        void OnNodePropertyChange(xproperty::inspector&, const xproperty::ui::undo::cmd& Cmd) noexcept
        {
            m_Document.m_bDirty = true;
            auto* pNode = m_Document.FindNode(m_InspectedNode);
            if (!pNode || !Cmd.m_NewValue.m_pType || !Cmd.m_Original.m_pType || !xeditor::cmd_util::IsAtomicType(Cmd.m_NewValue.getTypeGuid())) return;
            xeditor::Run(m_Undo, std::format("SetNodeProperty -Node {:016X} -Path {} -Value {} -Before {}", pNode->m_Guid.m_Value, xeditor::Base64Encode(Cmd.m_Name)
                , xeditor::Base64Encode(xeditor::cmd_util::FormatValue(Cmd.m_NewValue)), xeditor::Base64Encode(xeditor::cmd_util::FormatValue(Cmd.m_Original))));
        }

        // What the preview draws: the compiled material, its default textures.
        void ReloadPreview() noexcept
        {
            xresource::g_Mgr.ReleaseRef(m_MaterialRef);
            m_MaterialRef.m_Instance = m_Document.m_Guid.m_Instance;
            auto* pMaterial = xresource::g_Mgr.getResource(m_MaterialRef);
            if (!pMaterial) { m_Preview.ClearMaterial(); return; }

            std::vector<xgpu::pipeline_instance::sampler_binding> Bindings;
            for (int i = 0; i < pMaterial->m_nDefaultTextures; ++i)
            {
                auto& Entry = pMaterial->m_pDefaultTextures[i];
                if (!Entry.empty())
                    if (auto* pTexture = xresource::g_Mgr.getResource(Entry)) { Bindings.emplace_back(*pTexture); continue; }
                Bindings.emplace_back(m_Preview.DefaultTexture());
            }
            m_Preview.SetMaterial(pMaterial->getShader(), Bindings);
        }

        // The shader text: the file of a shader-file node, or what the compiler made from the graph (left in its log).
        void LoadShaderText(bool bAfterCompile) noexcept
        {
            std::wstring File;
            if (auto* pNode = m_Document.m_Graph.findFullShaderNode())
            {
                if (bAfterCompile) return;
                const auto& Param = pNode->m_Params[0].m_Value.get<std::wstring>();
                if (Param.empty()) return;
                File = std::format(L"{}/{}", e10::g_LibMgr.m_ProjectPath, Param);
            }
            else File = std::format(L"{}/shader.txt", m_Document.m_LogPath);

            std::ifstream In(File);
            if (!In.is_open()) return;
            std::stringstream Text; Text << In.rdbuf();
            m_Shader.SetText(Text.str());
        }
    };

    inline const xeditor::auto_register_resource_editor g_Registration
    { xrsc::material_type_guid_v
    , [](xresource::full_guid Guid, e10::library::guid LibraryGuid, xgpu::device* pDevice) -> std::unique_ptr<xeditor::resource_editor>
      { return std::make_unique<session>(Guid, LibraryGuid, pDevice); }
    };
}

#endif // XMATERIAL_EDITOR_H
