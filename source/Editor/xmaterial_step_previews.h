#ifndef XMATERIAL_STEP_PREVIEWS_H
#define XMATERIAL_STEP_PREVIEWS_H
#pragma once

// What a step makes, rendered (stack_canvas.md 10.4, 13). A temporary copy of the material whose colour output takes one step's output is
// compiled by the material compiler as a side resource (preview_compile): P on the stack canvas shows it on the mesh (the session), and the
// in-node previews (step_previews) draw it on a small flat quad, one step at a time, and keep the picture until what feeds the step changes.
#include "plugins/xmaterial.plugin/source/Editor/xmaterial_stack_commands.h"
#include "source/Tools/Editor/xeditor_mesh_preview.h"
#include "source/Tools/Editor/xeditor_thumbnail_cache.h"
#include "plugins/xmaterial.plugin/source/xmaterial_xgpu_rsc_loader.h"
#include "plugins/xmaterial.plugin/source/xmaterial_runtime.h"

#include <atomic>
#include <optional>
#include <thread>
#include <unordered_set>

namespace xmaterial_editor
{
    // The colour input a preview copy feeds: the first vecN input of an output node
    inline std::pair<xmaterial_graph::node*, int> ColorInput(const xmaterial_graph::graph& G) noexcept
    {
        for (auto& [Id, pN] : G.m_InstanceNodes)
            if (pN->isOutputNode())
                for (int k = 0; k < static_cast<int>(pN->m_InputPins.size()); ++k)
                    if (auto* t = G.findType(pN->m_InputPins[k].m_TypeGUID); t && t->m_CodeString.starts_with("vec")) return { pN.get(), k };
        return { nullptr, -1 };
    }

    // How a value of type Out reaches a colour input of type In: directly (the same GLSL type: a Color is a vec4 as a Vector4 is; a float
    // splats, the compiler's rule), or through *ppConv, a prefab with one input of Out's GLSL type and an output of In's (Vec4 To Vec3). False: it cannot.
    inline bool ColorFeed(const xmaterial_graph::graph& G, xmaterial_graph::type_guid Out, xmaterial_graph::type_guid In, const xmaterial_graph::node** ppConv) noexcept
    {
        *ppConv = nullptr;
        const auto *tOut = G.findType(Out), *tIn = G.findType(In);
        if (!tOut || !tIn) return false;
        if (tOut->m_CodeString == tIn->m_CodeString || tOut->m_CodeString == "float") return true;
        auto SameGlsl = [&](xmaterial_graph::type_guid a, xmaterial_graph::type_guid b) { auto *ta = G.findType(a), *tb = G.findType(b); return ta && tb && ta->m_CodeString == tb->m_CodeString; };
        for (auto& [Id, pT] : G.m_PrefabNodes)
            if (!pT->isInputNode() && !pT->isOutputNode() && pT->m_InputPins.size() == 1 && !pT->m_OutputPins.empty()
                && SameGlsl(pT->m_InputPins[0].m_TypeGUID, Out) && SameGlsl(pT->m_OutputPins[0].m_TypeGUID, In)) { *ppConv = pT.get(); return true; }
        return false;
    }

    // What a step's picture depends on: the step and every step upstream of it (their prefabs, literals and connections) and the declared
    // parameters. An edit anywhere else leaves it as it is.
    // ponytail: a texture or shader file compiled again is not seen (same guid); the picture stays until the step's inputs change
    inline std::uint64_t StepKey(const xmaterial_graph::graph& G, const xmaterial_graph::node& Step) noexcept
    {
        using type = xmaterial_graph::node_param::type;
        std::uint64_t H = 14695981039346656037ull;
        auto Mix  = [&](const void* p, std::size_t n) { for (auto* b = static_cast<const unsigned char*>(p); n--; ++b) H = (H ^ *b) * 1099511628211ull; };
        auto MixV = [&](const auto& v) { Mix(&v, sizeof(v)); };
        auto MixS = [&](const auto& s) { MixV(s.size()); Mix(s.data(), s.size() * sizeof(s[0])); };
        for (auto& D : G.m_Params) { MixV(D.m_ID); MixV(D.m_Type); MixV(D.m_Default); MixV(D.m_DefaultTexture); }

        std::unordered_set<std::uint64_t>         Seen;
        std::vector<const xmaterial_graph::node*> Todo{ &Step };
        while (!Todo.empty())
        {
            const auto& N = *Todo.back();
            Todo.pop_back();
            if (!Seen.insert(N.m_Guid.m_Value).second) continue;
            MixV(N.m_Guid.m_Value); MixV(N.m_PrefabGuid.m_Value);
            for (auto& P : N.m_Params)
            {
                MixV(P.m_Type); MixV(P.m_ParamID);
                switch (P.m_Type)
                {
                case type::FLOAT:            MixV(P.m_Value.get<float>());                 break;
                case type::INT:              MixV(P.m_Value.get<int>());                   break;
                case type::STRING:           MixS(P.m_Value.get<std::string>());           break;
                case type::FILE:             MixS(P.m_Value.get<std::wstring>());          break;
                case type::TEXTURE_RESOURCE: MixV(P.m_Value.get<xresource::full_guid>());  break;
                default:                                                                   break;
                }
            }
            for (auto& In : N.m_InputPins)
            {
                auto It = G.m_Connections.find(In.m_ConnectionGUID);
                if (!In.m_ConnectionGUID.m_Value || It == G.m_Connections.end()) continue;
                const auto& C = *It->second;
                auto* pSrc = C.m_InputPinGuid == In.m_PinGUID ? G.findNodeByPin(C.m_OutputPinGuid) : nullptr;
                if (!pSrc) continue;
                MixV(In.m_PinGUID.m_Value); MixV(C.m_OutputPinGuid.m_Value); MixS(C.m_Swizzle);
                Todo.push_back(pSrc);
            }
        }
        return H;
    }

    //------------------------------------------------------------------------------------------
    // One preview copy: written to Cache\Descriptors\Material\XX\YY\<guid>.desc (where the pipeline keeps generated descriptors), compiled by
    // the plugin's compiler with the library's command line on a thread, loaded by guid through g_Mgr. The asset, its descriptor, its compiled
    // resource and the library are never touched; Stop removes the temporary files.
    //------------------------------------------------------------------------------------------
    struct preview_compile
    {
        xrsc::material_ref m_Ref;                       // the compiled copy, once Poll said ""

        ~preview_compile() noexcept { Stop(); }

        // "" or why not. The copy: the colour input fed by Pin (a float splats; another type through a conversion step that takes it).
        std::string Start(graph_document& Doc, std::uint64_t Pin) noexcept
        {
            using namespace xmaterial_graph;
            Stop();
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
            m_Guid = G;
            if (Ec || !Doc.WriteToFile(File)) return "could not write the preview's descriptor";

            graph Copy;
            Copy.CreateGraph(Copy);
            Copy.serialize(Copy, true, File);
            auto* pSrc = Copy.findNodeByPin(pin_guid{ Pin });
            bool bIn; int i, Sub;
            const pin* pOut = pSrc ? Copy.findPinConst(*pSrc, pin_guid{ Pin }, bIn, i, Sub) : nullptr;
            auto [pFinal, iColor] = ColorInput(Copy);
            if (!pOut || !pFinal) return "nothing to preview";
            const auto Color = pFinal->m_InputPins[iColor];
            const node* pConv;
            if (!ColorFeed(Copy, pOut->m_TypeGUID, Color.m_TypeGUID, &pConv))
            {
                auto* t = Copy.findType(pOut->m_TypeGUID);
                return std::format("a {} cannot show on the mesh", t ? t->m_Name : std::string("value"));
            }
            if (Color.m_ConnectionGUID.m_Value) Copy.RemoveConnection(Color.m_ConnectionGUID);
            std::uint64_t Feed = Pin;
            if (pConv)
            {
                auto& Conv = Copy.CreateNode(pConv->m_Guid);
                cmds::MakeLink(Copy, xresource::guid_generator::Type64(), pin_guid{ Pin }, Conv.m_InputPins[0].m_PinGUID);
                Feed = Conv.m_OutputPins[0].m_PinGUID.m_Value;
            }
            cmds::MakeLink(Copy, xresource::guid_generator::Type64(), pin_guid{ Feed }, Color.m_PinGUID);

            // Only what the output reads now stays: a broken step elsewhere does not stop the preview, and the copy compiles faster
            std::unordered_set<std::uint64_t> Keep;
            std::vector<const node*>          Todo{ pFinal };
            while (!Todo.empty())
            {
                const auto& N = *Todo.back();
                Todo.pop_back();
                if (!Keep.insert(N.m_Guid.m_Value).second) continue;
                for (auto& In : N.m_InputPins)
                    if (auto It = Copy.m_Connections.find(In.m_ConnectionGUID); In.m_ConnectionGUID.m_Value && It != Copy.m_Connections.end())
                        if (auto* pS = Copy.findNodeByPin(It->second->m_OutputPinGuid)) Todo.push_back(pS);
            }
            std::vector<node_guid> Drop;
            for (auto& [Id, pN] : Copy.m_InstanceNodes) if (!Keep.contains(Id.m_Value)) Drop.push_back(Id);
            for (auto Id : Drop) Copy.RemoveNode(Id);
            Copy.serialize(Copy, false, File);

            const std::wstring Cmd = std::format(LR"("{}" -PROJECT "{}" -OPTIMIZATION {} -DEBUG {} -DESCRIPTOR "{}" -OUTPUT "{}\Cache\Resources\Platforms\WINDOWS")"
                , Exe, LM.m_ProjectPath, LM.m_Compilation.m_Settings.getOptimizationLevelString(), LM.m_Compilation.m_Settings.getDebugLevelString(), Rel, LM.m_ProjectPath);
            m_Log   = std::make_shared<xresource_editor::compilation::historical_entry::log>();
            m_bDone = false;
            m_Thread = std::thread([this, Cmd, Log = m_Log] { xresource_editor::compilation::RunCommandLine(Cmd, *Log, false); m_bDone = true; });
            m_Ref = {};
            m_Ref.m_Instance.m_Value = G;
            return {};
        }

        bool isRunning() const noexcept { return m_Thread.joinable(); }

        // Once the compile ended: "" (m_Ref loads the copy) or the compiler's first complaint. Nothing while it runs.
        std::optional<std::string> Poll() noexcept
        {
            if (!m_Thread.joinable() || !m_bDone) return std::nullopt;
            m_Thread.join();
            xcontainer::lock::scope Lock(*m_Log);
            const auto& L = m_Log->get();
            using result = xresource_editor::compilation::historical_entry::result;
            if (L.m_Result == result::SUCCESS || L.m_Result == result::SUCCESS_WARNINGS) return std::string{};
            const auto At = L.m_Log.find("ERROR");
            return At == std::string::npos ? std::string("the preview did not compile") : L.m_Log.substr(At, std::min<std::size_t>(L.m_Log.find('\n', At) - At, 120));
        }

        // Back to nothing: a running compile ends, its resource is let go of, its temporary files removed
        void Stop() noexcept
        {
            if (m_Thread.joinable())
            {
                xresource_editor::compilation::RequestCancel(*m_Log);
                m_Thread.join();
            }
            if (m_Ref.m_Instance.m_Value) xresource::g_Mgr.ReleaseRef(m_Ref);
            m_Ref = {};
            if (const auto G = std::exchange(m_Guid, 0))
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
        }

    private:
        std::uint64_t                                                           m_Guid = 0;     // the temporary resource
        std::thread                                                             m_Thread;       // its compile
        std::atomic<bool>                                                       m_bDone = false;
        std::shared_ptr<xresource_editor::compilation::historical_entry::log>   m_Log;
    };

    //------------------------------------------------------------------------------------------
    // In-node rendered previews: the steps on the screen ask for their picture each frame (Picture); Update, once a frame, works on one step
    // at a time: compiles its copy, draws it on a flat quad (the mesh preview's own drawing, the PLANE3D model, no background) into a small
    // target, reads it back and writes it into the step's cell of one atlas page. A step keeps its picture, the old one shown until the new is
    // ready, until it has not been asked for in a while and its cell is wanted. Memory: the page (4 MB), one target, one compile at a time.
    //------------------------------------------------------------------------------------------
    struct step_previews
    {
        static constexpr int SIZE = 64, COLS = 16, CELLS = COLS * COLS;        // a picture's pixels; the page: 16 x 16 pictures (1024 px)

        struct picture { const void* m_pTexture = nullptr; ImVec2 m_UV0, m_UV1; };

        bool m_bOn = true;                                                      // view state (StepPreviews -On): off, no step shows one

        ~step_previews() noexcept
        {
            m_Job.Stop();
            xeditor::DestroyGpu(m_pDevice, m_Page, m_Pass, m_Color, m_Depth);
        }

        bool Init(xgpu::device& Device) noexcept
        {
            m_pDevice = &Device;
            if (!m_Renderer.Init(Device)) return false;
            m_Renderer.m_Model = e19::mesh_manager::model::PLANE3D;
            *m_Renderer.Camera().m_pDistance = 1;                               // the quad fills the target
            std::vector<std::byte> Clear(std::size_t(SIZE) * COLS * SIZE * COLS * 4);
            if (Device.Create(m_Page,  { .m_Format = xgpu::texture::format::R8G8B8A8_UNORM, .m_Width = SIZE * COLS, .m_Height = SIZE * COLS, .m_AllFacesData = Clear, .m_isGamma = false })) return false;
            if (Device.Create(m_Color, { .m_Format = xgpu::texture::format::R8G8B8A8_UNORM, .m_Width = SIZE, .m_Height = SIZE, .m_isGamma = false })) return false;
            if (Device.Create(m_Depth, { .m_Format = xgpu::texture::format::DEPTH_F32, .m_Width = SIZE, .m_Height = SIZE, .m_isGamma = false })) return false;
            std::array<xgpu::renderpass::attachment, 2> Attachments{ { { m_Color }, { m_Depth } } };
            if (Device.Create(m_Pass, { .m_Attachments = Attachments, .m_ClearColorR = 0, .m_ClearColorG = 0, .m_ClearColorB = 0, .m_ClearColorA = 1 })) return false;
            for (int i = CELLS - 1; i >= 0; --i) m_FreeCells.push_back(i);
            m_bReady = true;
            return true;
        }

        // Whether a step shows a rendered preview: it has an output that can reach the material's colour
        bool Shows(const xmaterial_graph::graph& G, const xmaterial_graph::node& N) const noexcept
        {
            if (!m_bReady || !m_bOn || N.m_OutputPins.empty()) return false;
            auto [pOut, i] = ColorInput(G);
            const xmaterial_graph::node* pConv;
            return pOut && ColorFeed(G, N.m_OutputPins[0].m_TypeGUID, pOut->m_InputPins[i].m_TypeGUID, &pConv);
        }

        // The step's picture (none until its first is ready; the older one while a newer is made) and why it has none. Asking keeps it wanted.
        // ponytail: the key is worked out for each step on the screen each frame (a walk upstream); a graph revision number would spare it
        picture Picture(const xmaterial_graph::graph& G, const xmaterial_graph::node& N, std::string& Why) noexcept
        {
            auto& E = m_Steps[N.m_Guid.m_Value];
            E.m_Want      = StepKey(G, N);
            E.m_LastFrame = ImGui::GetFrameCount();
            if (E.m_ErrorKey == E.m_Want) Why = E.m_Error;
            if (E.m_Cell < 0) return {};
            const float s = 1.0f / COLS, u = (E.m_Cell % COLS) * s, v = (E.m_Cell / COLS) * s;
            return { &m_Page, { u, v + s }, { u + s, v } };                    // read back bottom row first
        }

        // Once a frame, before the canvas draws: moves the one step being worked on along
        void Update(graph_document& Doc) noexcept
        {
            auto* pWindow = xeditor::g_ThumbnailCache.Window();
            if (!m_bReady || !pWindow) return;
            switch (m_Stage)
            {
            case stage::IDLE:
            {
                const int Frame = ImGui::GetFrameCount();
                for (auto It = m_Steps.begin(); It != m_Steps.end(); )           // the steps gone: their cells are free
                {
                    if (Doc.FindNode(It->first)) { ++It; continue; }
                    if (It->second.m_Cell >= 0) m_FreeCells.push_back(It->second.m_Cell);
                    It = m_Steps.erase(It);
                }
                for (auto& [Id, E] : m_Steps)                                   // one on the screen whose picture is not of what it makes now
                {
                    if (E.m_LastFrame < Frame - 1 || E.m_Key == E.m_Want || E.m_ErrorKey == E.m_Want) continue;
                    auto* pN = Doc.FindNode(Id);
                    m_JobStep = Id;
                    m_JobKey  = E.m_Want;
                    if (auto Err = m_Job.Start(Doc, pN->m_OutputPins[0].m_PinGUID.m_Value); !Err.empty()) { Fail(Err); return; }
                    m_Stage = stage::COMPILING;
                    return;
                }
                return;
            }
            case stage::COMPILING:
            {
                auto It = m_Steps.find(m_JobStep);
                if (It == m_Steps.end() || It->second.m_Want != m_JobKey) { m_Job.Stop(); m_Stage = stage::IDLE; return; }   // gone or changed: start again from what it is now
                const auto R = m_Job.Poll();
                if (!R) return;
                if (!R->empty()) { Fail(*R); return; }
                auto* pMaterial = xresource::g_Mgr.getResource(m_Job.m_Ref);
                if (!pMaterial) { Fail("the preview did not load"); return; }
                std::vector<xgpu::pipeline_instance::sampler_binding> Bindings;
                for (int i = 0; i < pMaterial->m_nDefaultTextures; ++i)
                {
                    auto& Entry = pMaterial->m_pDefaultTextures[i];
                    if (!Entry.empty()) if (auto* pTexture = xresource::g_Mgr.getResource(Entry)) { Bindings.emplace_back(*pTexture); continue; }
                    Bindings.emplace_back(m_Renderer.DefaultTexture());
                }
                if (!m_Renderer.SetMaterial(pMaterial->getShader(), Bindings, { pMaterial->m_pDefaultBlock, std::size_t(pMaterial->m_BlockBytes) / 4 })) { Fail("the preview did not draw"); return; }
                {
                    auto CmdBuffer = pWindow->StartRenderPass(m_Pass);          // its destructor ends the pass, before the readback
                    m_Renderer.Draw(CmdBuffer, float(SIZE), float(SIZE), false);
                }
                m_bReadbackDone = false;
                if (!pWindow->ReadbackTexture(m_Color, m_Pixels, m_ReadW, m_ReadH, m_bReadbackDone)) { Fail("the preview could not be read back"); return; }
                m_Stage = stage::READBACK;
                return;
            }
            case stage::READBACK:
            {
                if (!m_bReadbackDone) return;
                m_Stage = stage::IDLE;
                m_Renderer.ClearMaterial();
                m_Job.Stop();
                auto It = m_Steps.find(m_JobStep);
                if (It == m_Steps.end() || m_ReadW != SIZE || m_ReadH != SIZE) return;
                auto& E = It->second;
                if (E.m_Cell < 0 && (E.m_Cell = ClaimCell()) < 0) return;
                for (auto& p : m_Pixels) p |= 0xff000000u;                     // the colour only: alpha is the material's business
                if (m_pDevice->UpdateTexture(m_Page, (E.m_Cell % COLS) * SIZE, (E.m_Cell / COLS) * SIZE, SIZE, SIZE, std::as_bytes(std::span{ m_Pixels }))) return;
                E.m_Key = m_JobKey;
                ++E.m_Renders;
                return;
            }
            }
        }

        // What the steps asked for lately have (StepPreviews): "<step guid> current|stale|none|failed <renders>[ <why>]", one line each
        std::string Report() const noexcept
        {
            std::vector<std::pair<std::uint64_t, const step*>> Sorted;
            for (auto& [Id, E] : m_Steps) Sorted.emplace_back(Id, &E);
            std::ranges::sort(Sorted);
            std::string Out;
            for (auto& [Id, pE] : Sorted)
            {
                const char* State = pE->m_ErrorKey == pE->m_Want ? "failed" : pE->m_Cell < 0 ? "none" : pE->m_Key == pE->m_Want ? "current" : "stale";
                Out += std::format("{:016X} {} {}{}\n", Id, State, pE->m_Renders, pE->m_ErrorKey == pE->m_Want ? " " + pE->m_Error : std::string{});
            }
            return Out;
        }

    private:
        enum class stage : std::uint8_t { IDLE, COMPILING, READBACK };
        struct step
        {
            std::uint64_t   m_Want = 0, m_Key = 0, m_ErrorKey = 0;              // what it makes now, what its picture shows, what failed
            std::string     m_Error;
            int             m_Cell = -1, m_LastFrame = 0, m_Renders = 0;
        };

        void Fail(const std::string& Why) noexcept
        {
            m_Job.Stop();
            m_Renderer.ClearMaterial();
            m_Stage = stage::IDLE;
            if (auto It = m_Steps.find(m_JobStep); It != m_Steps.end()) { It->second.m_ErrorKey = m_JobKey; It->second.m_Error = Why; }
        }

        // A free cell, else the one of the step asked for longest ago (not on the screen); -1 when every cell is on the screen
        int ClaimCell() noexcept
        {
            if (m_FreeCells.empty())
            {
                step* pOld = nullptr;
                for (auto& [Id, E] : m_Steps) if (E.m_Cell >= 0 && E.m_LastFrame < ImGui::GetFrameCount() - 1 && (!pOld || E.m_LastFrame < pOld->m_LastFrame)) pOld = &E;
                if (!pOld) return -1;
                m_FreeCells.push_back(std::exchange(pOld->m_Cell, -1));
                pOld->m_Key = 0;
            }
            const int Cell = m_FreeCells.back();
            m_FreeCells.pop_back();
            return Cell;
        }

        xgpu::device*                               m_pDevice = nullptr;
        bool                                        m_bReady  = false;
        xeditor::mesh_preview                       m_Renderer;
        xgpu::texture                               m_Page, m_Color, m_Depth;
        xgpu::renderpass                            m_Pass;
        std::vector<int>                            m_FreeCells;
        std::unordered_map<std::uint64_t, step>     m_Steps;                    // by node guid

        stage                                       m_Stage = stage::IDLE;      // the one step being worked on
        std::uint64_t                               m_JobStep = 0, m_JobKey = 0;
        preview_compile                             m_Job;
        std::vector<std::uint32_t>                  m_Pixels;
        int                                         m_ReadW = 0, m_ReadH = 0;
        bool                                        m_bReadbackDone = false;
    };

    namespace cmds
    {
        //---- StepPreviews: view only, the state of the in-node rendered previews
        struct step_previews_cmd : xundo::query_command_base
        {
            step_previews& m_Previews;
            step_previews_cmd(xundo::system& System, step_previews& Previews) noexcept : query_command_base(System, "StepPreviews", nullptr), m_Previews(Previews) { RegisterArguments(); }
            const char* getCommandHelp() const noexcept override { return "View only: the rendered previews of the steps shown lately, one line each: step guid, current (shows what the step makes now), stale (an older picture while a new one is made), none (not made yet) or failed (and why), and how many times it was made. -On false: steps show none (their layout as without them); true: back. Usage: StepPreviews [-On true|false]"; }
            void RegisterArguments() noexcept override { m_hOn = m_Parser.addOption("On", "true or false: steps show rendered previews", false, 1); }
            std::string Query() noexcept override
            {
                if (std::string On; GetArg(m_Parser, m_hOn, On))
                {
                    if (On != "true" && On != "false") return "StepPreviews: -On takes true or false";
                    m_Previews.m_bOn = On == "true";
                    return std::format("StepPreviews: {}", On == "true" ? "on" : "off");
                }
                return m_Previews.Report();
            }
            xcmdline::parser::handle m_hOn;
        };
    }
}

#endif // XMATERIAL_STEP_PREVIEWS_H
