#ifndef XMATERIAL_EDITOR_H
#define XMATERIAL_EDITOR_H
#pragma once

// The Material editor: opens a material from the asset browser in its own window. The material is a graph of nodes edited on two canvases over the
// same document and undo: the Material Stack (sections and steps placed by the system) and the Material Graph (free nodes). Every edit is an undoable
// command, so an AI drives it from the console; the compiled material is previewed on a mesh and the shader the compiler made is shown as text.
// Hosts include this header and open editors through xeditor::open_resource_editors.
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_graph_canvas.h"
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_stack_canvas.h"
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_subgraph_commands.h"
#include "source/Tools/Editor/xeditor_mesh_preview.h"
#include "source/Tools/Editor/xeditor_text_widget.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_resources.h"
#include "plugins/xmaterial.plugin/source/xmaterial_xgpu_rsc_loader.h"
#include "plugins/xmaterial.plugin/source/xmaterial_runtime.h"
#include "plugins/xmaterial.plugin/source/xmaterial_xgpu_rsc_loader.cpp"        // the resource loader: compiled once, in the host's translation unit

#include "plugins/xmaterial.plugin/source/Editor/xmaterial_step_previews.h"
#include "plugins/xmaterial.plugin/source/Graph/xmaterial_step_names.h"
#include <algorithm>

namespace xmaterial_editor
{
    struct session : xeditor::document_editor<graph_document>
    {
        cmds::create_node_cmd           m_CreateNode;
        cmds::delete_node_cmd           m_DeleteNode;
        cmds::connect_cmd               m_Connect;
        cmds::disconnect_cmd            m_Disconnect;
        cmds::set_input_swizzle_cmd     m_SetInputSwizzle;
        cmds::set_shader_file_cmd       m_SetShaderFile;
        cmds::move_node_cmd             m_MoveNode;
        cmds::set_node_property_cmd     m_SetNodeProperty;
        cmds::list_node_types_cmd       m_ListNodeTypes;
        cmds::list_nodes_cmd            m_ListNodes;
        cmds::node_properties_cmd       m_NodeProperties;
        cmds::place_steps_cmd           m_PlaceSteps;
        cmds::rename_step_cmd           m_RenameStep;
        cmds::create_section_cmd        m_CreateSection;
        cmds::rename_section_cmd        m_RenameSection;
        cmds::move_section_cmd          m_MoveSection;
        cmds::delete_section_cmd        m_DeleteSection;
        cmds::set_section_collapsed_cmd m_SetSectionCollapsed;
        cmds::expose_param_cmd          m_ExposeParam;
        cmds::unexpose_param_cmd        m_UnexposeParam;
        cmds::set_param_info_cmd        m_SetParamInfo;
        cmds::list_params_cmd           m_ListParams;
        cmds::set_param_default_cmd     m_SetParamDefault;
        cmds::retype_param_cmd          m_RetypeParam;
        cmds::purge_unused_params_cmd   m_PurgeUnusedParams;
        cmds::insert_on_connection_cmd  m_InsertOnConnection;
        cmds::duplicate_step_cmd        m_DuplicateStep;
        cmds::list_stack_cmd            m_ListStack;
        cmds::sources_cmd               m_Sources;
        cmds::find_uses_cmd             m_FindUses;
        cmds::validate_placement_cmd    m_ValidatePlacement;
        cmds::stack_view                m_StackView;            // how the stack canvas measures (view state, never saved)
        cmds::layout_stats_cmd          m_LayoutStats;
        cmds::route_info_cmd            m_RouteInfo;
        cmds::repack_routes_cmd         m_RepackRoutes;
        cmds::stack_self_test_cmd       m_StackSelfTest;
        cmds::add_group_pin_cmd         m_AddGroupPin    { m_Undo, m_Document };      // a sub-graph's interface (subgraphs_plan.md 2.2)
        cmds::remove_group_pin_cmd      m_RemoveGroupPin { m_Undo, m_Document };
        cmds::rename_group_pin_cmd      m_RenameGroupPin { m_Undo, m_Document };
        cmds::retype_group_pin_cmd      m_RetypeGroupPin { m_Undo, m_Document };
        cmds::list_group_pins_cmd       m_ListGroupPins  { m_Undo, m_Document };
        cmds::set_subgraph_cmd          m_SetSubgraph    { m_Undo, m_Document };      // group nodes (subgraphs_plan.md 2.3)
        cmds::list_subgraphs_cmd        m_ListSubgraphs  { m_Undo };
        cmds::ungroup_step_cmd          m_UngroupStep    { m_Undo, m_Document };
        cmds::make_subgraph_cmd         m_MakeSubgraph   { m_Undo, m_Document };
        cmds::enter_group_cmd           m_EnterGroup     { m_Undo, m_Document };
        cmds::exit_group_cmd            m_ExitGroup      { m_Undo, m_Document };

        graph_canvas                    m_Canvas;
        stack_canvas                    m_StackCanvas;
        cmds::select_step_cmd           m_SelectStep;
        cmds::trace_route_cmd           m_TraceRoute;
        cmds::preview_step_cmd          m_PreviewStep;
        std::uint64_t                   m_GraphSelected = 0, m_StackSelected = 0, m_Selected = 0;     // the canvas whose selection changed last decides what Node Properties shows
        xeditor::inspector_panel        m_NodeInspector{ "Node Properties" };
        std::uint64_t                   m_InspectedNode = 0;
        xeditor::mesh_preview           m_Preview;
        xeditor::mesh_preview_cmds      m_PreviewCmds;
        TextEditor                      m_Shader;
        xrsc::material_ref              m_MaterialRef;
        std::uint64_t                   m_PreviewPin = 0;                        // the output on the mesh instead of the material
        preview_compile                 m_PreviewJob;                            // its temporary copy
        step_previews                   m_StepPreviews;                          // the outputs rendered in the steps
        cmds::step_previews_cmd         m_StepPreviewsCmd;

        session(xresource::full_guid Guid, xresource_editor::library::guid LibraryGuid, xgpu::device* pDevice) noexcept
            : document_editor(Guid.m_Type == xmaterial_graph::subgraph_type_guid_v ? "Material Subgraph" : "Material", Guid, LibraryGuid, pDevice)
            , m_PreviewCmds(m_Undo, m_Preview)
            , m_CreateNode(m_Undo, m_Document), m_DeleteNode(m_Undo, m_Document), m_Connect(m_Undo, m_Document), m_Disconnect(m_Undo, m_Document), m_SetInputSwizzle(m_Undo, m_Document)
            , m_SetShaderFile(m_Undo, m_Document), m_MoveNode(m_Undo, m_Document), m_SetNodeProperty(m_Undo, m_Document)
            , m_ListNodeTypes(m_Undo, m_Document), m_ListNodes(m_Undo, m_Document), m_NodeProperties(m_Undo, m_Document)
            , m_PlaceSteps(m_Undo, m_Document), m_RenameStep(m_Undo, m_Document), m_CreateSection(m_Undo, m_Document), m_RenameSection(m_Undo, m_Document)
            , m_MoveSection(m_Undo, m_Document), m_DeleteSection(m_Undo, m_Document), m_SetSectionCollapsed(m_Undo, m_Document)
            , m_ExposeParam(m_Undo, m_Document), m_UnexposeParam(m_Undo, m_Document), m_SetParamInfo(m_Undo, m_Document), m_ListParams(m_Undo, m_Document)
            , m_SetParamDefault(m_Undo, m_Document), m_RetypeParam(m_Undo, m_Document), m_PurgeUnusedParams(m_Undo, m_Document)
            , m_InsertOnConnection(m_Undo, m_Document), m_DuplicateStep(m_Undo, m_Document), m_ListStack(m_Undo, m_Document), m_Sources(m_Undo, m_Document)
            , m_FindUses(m_Undo, m_Document), m_ValidatePlacement(m_Undo, m_Document)
            , m_LayoutStats(m_Undo, m_Document, m_StackView), m_RouteInfo(m_Undo, m_Document, m_StackView)
            , m_RepackRoutes(m_Undo, m_StackView), m_StackSelfTest(m_Undo)
            , m_Canvas(m_Document, m_Undo), m_StackCanvas(m_Document, m_Undo, m_StackView)
            , m_SelectStep(m_Undo, m_Document, m_StackCanvas), m_TraceRoute(m_Undo, m_Document, m_StackCanvas), m_PreviewStep(m_Undo, m_Document, m_StackCanvas)
            , m_StepPreviewsCmd(m_Undo, m_StepPreviews)
        {
            xresource_editor::WireResourcePickerCallbacks(m_NodeInspector.m_Inspector);
            m_Shader.SetLanguageDefinition(TextEditor::LanguageDefinition::GLSL());
            m_Shader.SetImGuiChildIgnored(true);                // it draws in its panel's window
            m_Shader.SetReadOnly(true);                         // a view of the shader the graph makes (or of the node's shader file)

            const bool bMaterial = Guid.m_Type != xmaterial_graph::subgraph_type_guid_v;     // a sub-graph compiles to no shader: no mesh, no shader text, no step renders; only the stack
            if (bMaterial) AddPanel("Mesh Preview",    dock::left,        [this] { UpdateStepPreview(); m_Preview.Render(); DrawPreviewBanner(); });
            AddPanel("Node Properties", dock::left_bottom, [this] { RenderNodeProperties(); });
            if (bMaterial) AddPanel("Material Graph",  dock::center,      [this] { m_Canvas.Render(); });       // ponytail: still editable until the stack canvas edits everything (stack_canvas.md task 12)
            AddPanel("Material Stack",  dock::center,      [this] { m_StepPreviews.Update(m_Document); m_StackCanvas.Render(); }, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);   // after the graph: it joins the graph's dock as a tab
            if (bMaterial) AddPanel("Shader",          dock::bottom,      [this] { m_Shader.Render("Shader", ImVec2(0, 0), true, [] {}); }, ImGuiWindowFlags_HorizontalScrollbar);

            LoadShaderText(false);
            m_Document.m_OnRead = [](xmaterial_graph::graph& G) { cmds::RefreshGroupNodes(G); };    // group nodes follow their sub-graphs' interfaces
            cmds::RefreshGroupNodes(m_Document.m_Graph);                                            // (the document was read before this hook existed)
            if (pDevice && bMaterial && m_Preview.Init(*pDevice) && std::filesystem::exists(m_Document.m_ResourcePath)) ReloadPreview();
            if (pDevice && bMaterial && m_StepPreviews.Init(*pDevice))
            {
                m_StackCanvas.m_Renders  = [this](const xmaterial_graph::node& N) { return m_StepPreviews.Shows(m_Document.m_Graph, N); };
                m_StackCanvas.m_Rendered = [this](const xmaterial_graph::node& N)
                {
                    stack_canvas::rendered R;
                    const auto P = m_StepPreviews.Picture(m_Document.m_Graph, N, R.m_Why);
                    R.m_pTexture = P.m_pTexture; R.m_UV0 = P.m_UV0; R.m_UV1 = P.m_UV1;
                    return R;
                };
            }
        }

        ~session() noexcept override { StopStepPreview(); xresource::g_Mgr.ReleaseRef(m_MaterialRef); }

        void OnDocumentReplaced() noexcept override { m_InspectedNode = 0; m_NodeInspector.Clear(); }

        void OnCompiled() noexcept override
        {
            ClearErrors();
            ReloadPreview();
            if (m_PreviewPin) StopStepPreview();                // previewing a step: compile it again from the new material (next frame)
            LoadShaderText(true);
        }

        // The compiler's log names the nodes it did not like (their quoted GLSL names, see StepVarName): they turn red and say why.
        void OnCompileFailed() noexcept override
        {
            ClearErrors();
            std::string Text;
            if (m_CompilationLog)
            {
                xcontainer::lock::scope Lock(*m_CompilationLog);
                Text = m_CompilationLog->get().m_Log;
            }
            std::size_t Start = 0;
            while (Start < Text.size())
            {
                auto End = Text.find('\n', Start);
                if (End == std::string::npos) End = Text.size();
                const std::string Line = Text.substr(Start, End - Start);
                Start = End + 1;

                if (xstrtool::findI(Line, "ERROR:") == std::string::npos) continue;
                for (auto& [Id, pNode] : m_Document.m_Graph.m_InstanceNodes)
                    for (int Out = 0; Out < std::max<int>(1, (int)pNode->m_OutputPins.size()); ++Out)
                    {
                        // The name is quoted by the compiler, so it must end there: 'uv' or 'uv.x', not 'uv2'
                        const std::string Var = "'" + xmaterial_graph::StepVarName(*pNode, Out);
                        for (auto At = Line.find(Var); At != std::string::npos; At = Line.find(Var, At + 1))
                        {
                            const auto Stop = At + Var.size();
                            if (Stop < Line.size() && (std::isalnum((unsigned char)Line[Stop]) || Line[Stop] == '_')) continue;
                            pNode->m_HasErrMsg = true; pNode->m_ErrMsg = Line; break;
                        }
                    }
            }
        }

        void ClearErrors() noexcept
        {
            for (auto& [Id, pNode] : m_Document.m_Graph.m_InstanceNodes) { pNode->m_HasErrMsg = false; pNode->m_ErrMsg.clear(); }
        }

        // The selected node's properties: an edit is a command, like everything else.
        void RenderNodeProperties() noexcept
        {
            if (const auto S = m_Canvas.SelectedNode();      S != m_GraphSelected) m_Selected = m_GraphSelected = S;
            if (const auto S = m_StackCanvas.SelectedNode(); S != m_StackSelected) m_Selected = m_StackSelected = S;
            const std::uint64_t Selected = m_Selected && m_Document.FindNode(m_Selected) ? m_Selected : 0;
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
            if (auto* pNode = m_InspectedNode ? m_Document.FindNode(m_InspectedNode) : nullptr)         // the inputs that read a parameter (ExposeParam)
                if (const auto Links = cmds::ParamLinks(m_Document.m_Graph, *pNode); !Links.empty()) ImGui::TextDisabled("%s", Links.c_str());
        }

        void OnNodePropertyChange(xproperty::inspector&, const xproperty::ui::undo::cmd& Cmd) noexcept
        {
            m_Document.m_bDirty = true;
            auto* pNode = m_Document.FindNode(m_InspectedNode);
            if (!pNode || !Cmd.m_NewValue.m_pType || !Cmd.m_Original.m_pType || !xeditor::cmd_util::IsAtomicType(Cmd.m_NewValue.getTypeGuid())) return;
            xeditor::Run(m_Undo, std::format("SetNodeProperty -Node {:016X} -Path {} -Value {} -Before {}", pNode->m_Guid.m_Value, xeditor::Quote(Cmd.m_Name)
                , xeditor::Quote(xeditor::cmd_util::FormatValue(Cmd.m_NewValue)), xeditor::Quote(xeditor::cmd_util::FormatValue(Cmd.m_Original))));
        }

        // What the preview draws: the compiled material, its default textures.
        void ReloadPreview() noexcept
        {
            xresource::g_Mgr.ReleaseRef(m_MaterialRef);
            if (m_Document.m_Graph.m_bSubgraph) return;
            m_MaterialRef.m_Instance = m_Document.m_Guid.m_Instance;
            if (!ShowMaterial(m_MaterialRef)) m_Preview.ClearMaterial();
        }

        bool ShowMaterial(xrsc::material_ref& Ref) noexcept
        {
            auto* pMaterial = xresource::g_Mgr.getResource(Ref);
            if (!pMaterial) return false;

            std::vector<xgpu::pipeline_instance::sampler_binding> Bindings;
            for (int i = 0; i < pMaterial->m_nDefaultTextures; ++i)
            {
                auto& Entry = pMaterial->m_pDefaultTextures[i];
                if (!Entry.empty())
                    if (auto* pTexture = xresource::g_Mgr.getResource(Entry)) { Bindings.emplace_back(*pTexture); continue; }
                Bindings.emplace_back(m_Preview.DefaultTexture());
            }
            m_Preview.SetMaterial(pMaterial->getShader(), Bindings, { pMaterial->m_pDefaultBlock, std::size_t(pMaterial->m_BlockBytes) / 4 });
            return true;
        }

        //------------------------------------------------------------------------------------------
        // One intermediate preview (stack_canvas.md 10.4): P on the stack canvas (or PreviewStep) asks for an output; a copy of the graph whose
        // output colour takes it (preview_compile) is shown on the mesh. Esc (canvas or Mesh Preview) returns to the material.
        //------------------------------------------------------------------------------------------
        void UpdateStepPreview() noexcept
        {
            if (const auto Want = m_StackCanvas.PreviewPin(); Want != m_PreviewPin)
            {
                ReloadPreview();                                                // the material first: the mesh never draws a released shader
                StopStepPreview();
                m_PreviewPin = Want;
                if (!Want) return;
                const auto Err = m_PreviewJob.Start(m_Document, Want);
                m_StackCanvas.PreviewStatus(Err.empty() ? "compiling..." : Err);
            }
            const auto R = m_PreviewJob.Poll();
            if (!R) return;
            if (R->empty() && ShowMaterial(m_PreviewJob.m_Ref)) { m_StackCanvas.PreviewStatus({}); return; }
            m_StackCanvas.PreviewStatus(R->empty() ? std::string("the preview did not load") : *R);
        }

        void StopStepPreview() noexcept
        {
            m_PreviewJob.Stop();
            m_PreviewPin = 0;
        }

        void DrawPreviewBanner() noexcept                                 // the same banner over the mesh: what it shows is not the material
        {
            const auto B = m_StackCanvas.PreviewBanner();
            if (B.empty()) return;
            const ImVec2 P = ImGui::GetWindowPos(), S = ImGui::GetWindowSize();
            stack_style::Banner(ImGui::GetWindowDrawList(), ImGui::GetFont(), ImGui::GetFontSize(), { P.x + S.x * 0.5f, P.y + ImGui::GetFrameHeight() + 6 }, B);
            if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_StackCanvas.Preview(0);
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
                File = std::format(L"{}/{}", xresource_editor::g_LibMgr.m_ProjectPath, Param);
            }
            else File = std::format(L"{}/shader.txt", m_Document.m_LogPath);

            std::ifstream In{ std::filesystem::path(File) };
            if (!In.is_open()) return;
            std::stringstream Text; Text << In.rdbuf();
            m_Shader.SetText(Text.str());
        }
    };

    inline const xeditor::auto_register_resource_editor g_Registration
    { xrsc::material_type_guid_v
    , [](xresource::full_guid Guid, xresource_editor::library::guid LibraryGuid, xgpu::device* pDevice) -> std::unique_ptr<xeditor::resource_editor>
      { return std::make_unique<session>(Guid, LibraryGuid, pDevice); }
    };

    // A sub-graph is edited by the same editor: the same graph file, its interface instead of material outputs
    inline const xeditor::auto_register_resource_editor g_SubgraphRegistration
    { xmaterial_graph::subgraph_type_guid_v
    , [](xresource::full_guid Guid, xresource_editor::library::guid LibraryGuid, xgpu::device* pDevice) -> std::unique_ptr<xeditor::resource_editor>
      { return std::make_unique<session>(Guid, LibraryGuid, pDevice); }
    };
}

#endif // XMATERIAL_EDITOR_H
