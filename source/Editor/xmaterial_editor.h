#ifndef XMATERIAL_EDITOR_H
#define XMATERIAL_EDITOR_H
#pragma once

// The Material editor: opens a material from the asset browser in its own window. The material is a graph of nodes edited on two canvases over the
// same document and undo: the Material Stack (sections and steps placed by the system) and the Material Graph (free nodes). Every edit is an undoable
// command, so an AI drives it from the console; the compiled material is previewed on a mesh and the shader the compiler made is shown as text.
// Hosts include this header and open editors through xeditor::open_resource_editors.
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_graph_canvas.h"
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_stack_canvas.h"
#include "source/Tools/Editor/xeditor_mesh_preview.h"
#include "source/Tools/Editor/xeditor_text_widget.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_resources.h"
#include "plugins/xmaterial.plugin/source/xmaterial_xgpu_rsc_loader.h"
#include "plugins/xmaterial.plugin/source/xmaterial_runtime.h"
#include "plugins/xmaterial.plugin/source/xmaterial_xgpu_rsc_loader.cpp"        // the resource loader: compiled once, in the host's translation unit

#include "plugins/xmaterial.plugin/source/Graph/xmaterial_step_names.h"
#include <algorithm>
#include <atomic>
#include <thread>

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
        std::uint64_t                   m_PreviewPin = 0, m_PreviewGuid = 0;     // the output on the mesh instead of the material, its temporary resource
        xrsc::material_ref              m_PreviewRef;
        std::thread                     m_PreviewThread;                         // its compile
        std::atomic<bool>               m_bPreviewDone = false;
        std::shared_ptr<xresource_editor::compilation::historical_entry::log> m_PreviewLog;

        session(xresource::full_guid Guid, xresource_editor::library::guid LibraryGuid, xgpu::device* pDevice) noexcept
            : document_editor("Material", Guid, LibraryGuid, pDevice)
            , m_PreviewCmds(m_Undo, m_Preview)
            , m_CreateNode(m_Undo, m_Document), m_DeleteNode(m_Undo, m_Document), m_Connect(m_Undo, m_Document), m_Disconnect(m_Undo, m_Document)
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
        {
            xresource_editor::WireResourcePickerCallbacks(m_NodeInspector.m_Inspector);
            m_Shader.SetLanguageDefinition(TextEditor::LanguageDefinition::GLSL());
            m_Shader.SetImGuiChildIgnored(true);                // it draws in its panel's window
            m_Shader.SetReadOnly(true);                         // a view of the shader the graph makes (or of the node's shader file)

            AddPanel("Mesh Preview",    dock::left,        [this] { UpdateStepPreview(); m_Preview.Render(); DrawPreviewBanner(); });
            AddPanel("Node Properties", dock::left_bottom, [this] { RenderNodeProperties(); });
            AddPanel("Material Graph",  dock::center,      [this] { m_Canvas.Render(); });       // ponytail: still editable until the stack canvas edits everything (stack_canvas.md task 12)
            AddPanel("Material Stack",  dock::center,      [this] { m_StackCanvas.Render(); }, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);   // after the graph: it joins the graph's dock as a tab
            AddPanel("Shader",          dock::bottom,      [this] { m_Shader.Render("Shader", ImVec2(0, 0), true, [] {}); }, ImGuiWindowFlags_HorizontalScrollbar);

            LoadShaderText(false);
            if (pDevice && m_Preview.Init(*pDevice) && std::filesystem::exists(m_Document.m_ResourcePath)) ReloadPreview();
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
        // output colour takes it is compiled by the material compiler as a temporary resource with its descriptor in Cache\Descriptors (where
        // the pipeline keeps generated descriptors) and shown on the mesh. The asset, its descriptor, its compiled resource and the library
        // are never touched; the temporary files go when the preview ends. Esc (canvas or Mesh Preview) returns to the material.
        //------------------------------------------------------------------------------------------
        void UpdateStepPreview() noexcept
        {
            if (const auto Want = m_StackCanvas.PreviewPin(); Want != m_PreviewPin)
            {
                ReloadPreview();                                                // the material first: the mesh never draws a released shader
                StopStepPreview();
                m_PreviewPin = Want;
                if (!Want) return;
                const auto Err = StartStepPreview(Want);
                m_StackCanvas.PreviewStatus(Err.empty() ? "compiling..." : Err);
            }
            if (!m_PreviewThread.joinable() || !m_bPreviewDone) return;
            m_PreviewThread.join();
            std::string Log; bool bOk;
            {
                xcontainer::lock::scope Lock(*m_PreviewLog);
                Log = m_PreviewLog->get().m_Log;
                bOk = m_PreviewLog->get().m_Result == xresource_editor::compilation::historical_entry::result::SUCCESS || m_PreviewLog->get().m_Result == xresource_editor::compilation::historical_entry::result::SUCCESS_WARNINGS;
            }
            if (bOk && ShowMaterial(m_PreviewRef)) { m_StackCanvas.PreviewStatus({}); return; }
            const auto At = Log.find("ERROR");                                 // the compiler's first complaint says why
            m_StackCanvas.PreviewStatus(At == std::string::npos ? std::string("the preview did not compile") : Log.substr(At, std::min<std::size_t>(Log.find('\n', At) - At, 120)));
        }

        // "" or why not. The copy: Output's colour input fed by the output (a float splats; another type through a conversion step that takes it).
        std::string StartStepPreview(std::uint64_t Pin) noexcept
        {
            using namespace xmaterial_graph;
            auto& LM = xresource_editor::g_LibMgr;
            auto* pPlugin = LM.m_AssetPluginsDB.find(xrsc::material_type_guid_v);
            if (!pPlugin) return "no material compiler";
            std::wstring Exe;
            {
                std::scoped_lock Lk(LM.m_Compilation.m_Settings.m_Mutex);
                Exe = (LM.m_Compilation.m_Settings.m_bUseDebugCompiler && !pPlugin->m_DebugCompiler.empty()) || pPlugin->m_ReleaseCompiler.empty() ? pPlugin->m_DebugCompiler : pPlugin->m_ReleaseCompiler;
            }
            if (Exe.empty()) return "no material compiler";
            Exe = std::filesystem::path(std::format(L"{}\\{}", LM.m_ProjectPath, Exe)).lexically_normal().wstring();

            const std::uint64_t G = xresource::guid_generator::Instance64();
            const std::wstring  Rel = std::format(L"Cache\\Descriptors\\Material\\{:02X}\\{:02X}\\{:X}.desc", G & 0xff, (G >> 8) & 0xff, G);
            const std::wstring  Dir = std::format(L"{}\\{}", LM.m_ProjectPath, Rel);
            std::error_code Ec;
            std::filesystem::create_directories(Dir, Ec);
            const std::wstring File = Dir + L"\\Descriptor.txt";
            if (Ec || !m_Document.WriteToFile(File)) return "could not write the preview's descriptor";
            m_PreviewGuid = G;

            graph Copy;
            Copy.CreateGraph(Copy);
            Copy.serialize(Copy, true, File);
            auto* pSrc = Copy.findNodeByPin(pin_guid{ Pin });
            bool bIn; int i, Sub;
            const pin* pOut = pSrc ? Copy.findPinConst(*pSrc, pin_guid{ Pin }, bIn, i, Sub) : nullptr;
            node* pFinal = nullptr; int iColor = -1;
            for (auto& [Id, pN] : Copy.m_InstanceNodes)
                if (pN->isOutputNode())
                    for (int k = 0; k < static_cast<int>(pN->m_InputPins.size()) && iColor < 0; ++k)
                        if (auto* t = Copy.findType(pN->m_InputPins[k].m_TypeGUID); t && t->m_CodeString.starts_with("vec")) { pFinal = pN.get(); iColor = k; }
            if (!pOut || !pFinal) return "nothing to preview";
            const auto Color = pFinal->m_InputPins[iColor];
            const auto* tOut = Copy.findType(pOut->m_TypeGUID);
            const auto* tIn  = Copy.findType(Color.m_TypeGUID);
            if (!tOut || !tIn) return "nothing to preview";
            if (Color.m_ConnectionGUID.m_Value) Copy.RemoveConnection(Color.m_ConnectionGUID);
            std::uint64_t Feed = Pin;
            if (tOut->m_GUID != tIn->m_GUID && tOut->m_CodeString != "float")
            {
                const node* pConv = nullptr;                                        // e.g. Vec4 To Vec3
                for (auto& [Id, pT] : Copy.m_PrefabNodes)
                    if (!pT->isInputNode() && !pT->isOutputNode() && pT->m_InputPins.size() == 1 && !pT->m_OutputPins.empty()
                        && pT->m_InputPins[0].m_TypeGUID == pOut->m_TypeGUID && pT->m_OutputPins[0].m_TypeGUID == Color.m_TypeGUID) { pConv = pT.get(); break; }
                if (!pConv) return std::format("a {} cannot show on the mesh", tOut->m_Name);
                auto& Conv = Copy.CreateNode(pConv->m_Guid);
                cmds::MakeLink(Copy, xresource::guid_generator::Type64(), pin_guid{ Pin }, Conv.m_InputPins[0].m_PinGUID);
                Feed = Conv.m_OutputPins[0].m_PinGUID.m_Value;
            }
            cmds::MakeLink(Copy, xresource::guid_generator::Type64(), pin_guid{ Feed }, Color.m_PinGUID);
            Copy.serialize(Copy, false, File);

            const std::wstring Cmd = std::format(LR"("{}" -PROJECT "{}" -OPTIMIZATION {} -DEBUG {} -DESCRIPTOR "{}" -OUTPUT "{}\Cache\Resources\Platforms\WINDOWS")"
                , Exe, LM.m_ProjectPath, LM.m_Compilation.m_Settings.getOptimizationLevelString(), LM.m_Compilation.m_Settings.getDebugLevelString(), Rel, LM.m_ProjectPath);
            m_PreviewLog   = std::make_shared<xresource_editor::compilation::historical_entry::log>();
            m_bPreviewDone = false;
            m_PreviewThread = std::thread([this, Cmd, Log = m_PreviewLog] { xresource_editor::compilation::RunCommandLine(Cmd, *Log, false); m_bPreviewDone = true; });
            m_PreviewRef = {};
            m_PreviewRef.m_Instance.m_Value = G;
            return {};
        }

        // Back to nothing: a running preview compile ends, its resource is let go of, its temporary files removed
        void StopStepPreview() noexcept
        {
            if (m_PreviewThread.joinable())
            {
                xresource_editor::compilation::RequestCancel(*m_PreviewLog);
                m_PreviewThread.join();
            }
            if (m_PreviewRef.m_Instance.m_Value) xresource::g_Mgr.ReleaseRef(m_PreviewRef);
            m_PreviewRef = {};
            if (const auto G = std::exchange(m_PreviewGuid, 0))
            {
                auto& LM = xresource_editor::g_LibMgr;
                const std::wstring Sub = std::format(L"Material\\{:02X}\\{:02X}\\{:X}", G & 0xff, (G >> 8) & 0xff, G);
                std::error_code Ec;
                for (const std::filesystem::path P : { std::format(L"{}\\Cache\\Descriptors\\{}.desc", LM.m_ProjectPath, Sub), std::format(L"{}\\Cache\\Resources\\Platforms\\WINDOWS\\{}", LM.m_ProjectPath, Sub), std::format(L"{}\\Cache\\Resources\\Logs\\{}.log", LM.m_ProjectPath, Sub) })
                {
                    std::filesystem::remove_all(P, Ec);
                    for (auto D = P.parent_path(); D.filename() != L"Cache" && std::filesystem::remove(D, Ec); D = D.parent_path()) {}     // its folders, while they hold nothing else
                }
            }
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
}

#endif // XMATERIAL_EDITOR_H
