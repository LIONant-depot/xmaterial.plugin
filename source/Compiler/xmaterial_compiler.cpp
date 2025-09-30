#include "xmaterial_compiler.h"
#include <regex>
#include <unordered_set>

#include "dependencies/shaderc/include/shaderc/shaderc.hpp"
#include "dependencies/xstrtool/source/xstrtool.h"
#include <fstream>
#include <cerrno>   // for errno
#include <iostream>

#include "source/xmaterial_data_file.h"

#pragma comment(lib, "../../dependencies/shaderc/lib/shaderc_combined.lib")

#include <process.h>

namespace xmaterial_compiler
{
    struct implementation final : xmaterial_compiler::instance
    {
        std::string PinToVarName(const node& n)
        {
            return "N_" + std::to_string(n.m_Guid.m_Value);
        }

        std::string GetInputExpr(const graph& g, const node& n, int pinIndex, int paramIndex, bool requireConnectionOnly)
        {
            if (pinIndex < 0 || pinIndex >= (int)n.m_InputPins.size())
                return "/*bad pin index*/";

            const auto& ip = n.m_InputPins[pinIndex];

            //1. if connected == resolve upstream
            if (ip.m_ConnectionGUID.m_Value != 0) //will there be casing where the guid just = 0?
            {
                auto it = g.m_Connections.find(ip.m_ConnectionGUID);
                if (it != g.m_Connections.end())
                {
                    const auto& conn = *it->second;
                    if (auto* srcNode = g.FindNodeByPin(conn.m_OutputPinGuid))
                    {
                        int idx, sub;
                        bool dummy;
                        const pin* outPin = g.FindPinConst(*srcNode, conn.m_OutputPinGuid, dummy, idx, sub);
                        if (outPin)
                        {

                            if (!outPin->m_DefaultExpr.empty())
                            {
                                // Direct GLSL expr (like "In.Color" or "In.UV")
                                if (sub >= 0 && sub < 4)
                                {
                                    static const char* swizzle[4] = { "x","y","z","w" };
                                    return outPin->m_DefaultExpr + "." + swizzle[sub];
                                }
                                return outPin->m_DefaultExpr;
                            }
                            auto baseVar = PinToVarName(*srcNode);
                            if (sub >= 0 && sub < 4) // sub-output like .x/.y/.z/.w
                            {
                                static const char* swizzle[4] = { "x","y","z","w" };
                                return baseVar + "." + swizzle[sub];
                            }
                            return baseVar;
                        }
                    }
                }
            }

            // 2. Not connected
            if (requireConnectionOnly)
            {
                // strict mode → must be connected
                return "/* ERROR: Node '" + n.m_Name +
                    "' requires input[" + std::to_string(pinIndex) +
                    "] to be connected */";
            }

            // 3. use param fallback
            if (paramIndex >= 0 && paramIndex < (int)n.m_Params.m_Properties.size())
            {
                const auto& prop = n.m_Params.m_Properties[paramIndex];
                if (prop.m_Value.m_pType)
                {
                    auto guid = prop.m_Value.m_pType->m_GUID;
                    if (guid == xproperty::settings::var_type<float>::guid_v)
                        return std::to_string(prop.m_Value.get<float>());
                    if (guid == xproperty::settings::var_type<int>::guid_v)
                        return std::to_string(prop.m_Value.get<int>());
                    if (guid == xproperty::settings::var_type<std::string>::guid_v)
                    {
                        g.m_shaderDetail.m_Textures.push_back(prop.m_Value.get<std::string>());
                        return std::to_string(g.m_shaderDetail.m_Textures.size() - 1);
                    }


                    //extend with vec2/vec3/vec4 serialization here
                }
            }

            return "/*missing input*/";
        }

        void ExtractSection(std::string& src, const std::string& marker, std::string& out)
        {
            size_t start = src.find(marker);
            if (start != std::string::npos)
            {
                size_t open = src.find("<!", start);
                size_t close = src.find("!>", open);
                if (open != std::string::npos && close != std::string::npos && close > open)
                {
                    out += src.substr(open + 2, close - open - 2);
                    src.erase(start, close - start + 2);
                }
            }
        }

        void DFS(const graph& g, const node& n, std::unordered_set<node_guid>& visited, std::vector<const node*>& ordered)
        {
            if (visited.contains(n.m_Guid)) return;
            visited.insert(n.m_Guid);

            for (const auto& ip : n.m_InputPins)
            {
                if (ip.m_ConnectionGUID.m_Value != 0)
                {
                    auto it = g.m_Connections.find(ip.m_ConnectionGUID);
                    if (it != g.m_Connections.end())
                    {
                        if (auto* dep = g.FindNodeByPin(it->second->m_OutputPinGuid))
                            DFS(g, *dep, visited, ordered);
                    }
                }
            }
            ordered.push_back(&n);
        }

        std::string GenerateGLSL(const graph& g)
        {
            // Header (always)
            std::ostringstream ss;
            ss << "#version 450\n";
            ss << "#extension GL_ARB_separate_shader_objects  : enable\n";
            ss << "#extension GL_ARB_shading_language_420pack : enable\n\n";


            // Validate final output
            std::vector<const node*> fragOutNodes;
            for (auto& [id, nPtr] : g.m_InstanceNodes)
                if (nPtr->isOutputNode())
                    fragOutNodes.push_back(nPtr.get());

            if (fragOutNodes.empty())
                return "/* ERROR: Graph must have a Final FragOut node */\n";

            std::string topCode, botCode;

            // Traverse only from Final FragOut nodes
            std::unordered_set<node_guid> visited;
            std::vector<const node*> ordered;
            for (auto* fragNode : fragOutNodes)
                DFS(g, *fragNode, visited, ordered);

            // simple traversal for now
            for (auto* np : ordered)
            {
                const node& n = *np;
                std::string code = n.m_Code;

                // replace $var with node variable name
                code = std::regex_replace(code, std::regex("\\$var"), PinToVarName(n));


                // replace $input[pin]_prop[param] with expression
                std::regex reProp(R"(\$input\[(\d+)\]_prop\[(\d+)\])");
                std::smatch m;

                while (std::regex_search(code, m, reProp))
                {
                    int pinIdx = std::stoi(m[1].str());
                    int paramIdx = std::stoi(m[2].str());
                    std::string repl = GetInputExpr(g, n, pinIdx, paramIdx, false);
                    code.replace(m.position(0), m.length(0), repl);
                }

                // --- Replace plain $input[pin] (fallback paramIndex = same as pin) ---
                std::regex rePlain(R"(\$input\[(\d+)\])");//texture($input[0], $input[1])
                while (std::regex_search(code, m, rePlain))
                {
                    int pinIdx = std::stoi(m[1].str());
                    std::string repl = GetInputExpr(g, n, pinIdx, -1, true);
                    code.replace(m.position(0), m.length(0), repl);
                }


                // --- Replace plain $input[pin] (fallback paramIndex = same as pin) ---
                std::regex reTex(R"(\$tex\[(\d+)\])");
                while (std::regex_search(code, m, reTex))
                {
                    int pinIdx = std::stoi(m[1].str());
                    std::string repl = GetInputExpr(g, n, pinIdx, n.m_InputPins[pinIdx].m_ParamIndex, false);
                    code.replace(m.position(0), m.length(0), repl);
                }


                // extract [TOP]/[BOT]
                ExtractSection(code, "[TOP]", topCode);
                ExtractSection(code, "[BOT]", botCode);

                // whatever remains goes to botCode (main)
                if (!code.empty())
                    botCode += code;
            }

            ss << topCode << "\n";
            ss << "void main() \n{\n";
            ss << botCode;
            ss << "}\n";

            return ss.str();
        }

        xerr onCompile(void) override
        {
            //
            // Load the descriptor 
            //
            auto DescriptorFileName = std::format( L"{}/{}/Descriptor.txt", m_ProjectPaths.m_Project, m_InputSrcDescriptorPath);

            graph m_graph;
            m_graph.CreateGraph(m_graph);
            m_graph.serialize(m_graph, true, DescriptorFileName);
            
            const auto& shaderString = GenerateGLSL(m_graph);

            // Print the generated shader
            LogMessage( xresource_pipeline::msg_type::INFO, shaderString);

            //
            // Generate the sprv shader
            //
            displayProgressBar("Compiling Shader", 0.0f);
            shaderc::Compiler       compiler;
            shaderc::CompileOptions options;

            options.SetOptimizationLevel(shaderc_optimization_level_performance);
            options.SetTargetSpirv(shaderc_spirv_version_1_0);

            auto result = compiler.CompileGlslToSpv( shaderString
                                                   , shaderc_glsl_default_fragment_shader
                                                   , "shader.glsl"
                                                   , options
                                                   );

            if (const auto Err = result.GetCompilationStatus(); Err != shaderc_compilation_status_success ) 
            {
                switch (Err)
                {
                case shaderc_compilation_status_invalid_stage:          return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_invalid_stage">();
                case shaderc_compilation_status_compilation_error:      return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_compilation_error">();
                case shaderc_compilation_status_internal_error:         return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_internal_error">();
                case shaderc_compilation_status_null_result_object:     return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_null_result_object">();
                case shaderc_compilation_status_invalid_assembly:       return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_invalid_assembly">();
                case shaderc_compilation_status_validation_error:       return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_validation_error">();
                case shaderc_compilation_status_transformation_error:   return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_transformation_error">();
                case shaderc_compilation_status_configuration_error:    return xerr::create_f<state, "Compilation for spirv fail - shaderc_compilation_status_configuration_error">();
                default:                                                return xerr::create_f<state, "Compilation for spirv fail">();
                }
            }
            displayProgressBar("Compiling Shader", 1.0f);

            //
            // Export the compiled material
            //
            std::vector<uint32_t> spirv(result.cbegin(), result.cend());
            LogMessage(xresource_pipeline::msg_type::INFO, std::format("Shader compiled successfully! SPIR-V size: {} words.\n", spirv.size()) );


            //
            // Create the actual material
            //
            xmaterial::data_file MaterialDataFile;

            MaterialDataFile.m_pShader          = spirv.data();
            MaterialDataFile.m_ShaderSize       = static_cast<std::uint32_t>(spirv.size());
            MaterialDataFile.m_Flags.m_bAlpha   = false;

            //
            // Serialize Final xBitmap
            //
            int Count = 0;
            for (auto& T : m_Target)
            {
                displayProgressBar("Serializing", Count++ / static_cast<float>(m_Target.size()));

                if (T.m_bValid)
                {
                    xserializer::stream Stream;
                    if ( auto Err = Stream.Save(T.m_DataPath, MaterialDataFile, xserializer::compression_level::HIGH); Err )
                        return Err;
                }
            }
            displayProgressBar("Serializing", 1);

            //
            // Save the general information in case the viewer needs it...
            //
            if ( auto Err = m_graph.m_shaderDetail.serializeShaderDetails(false, std::format(L"{}/{}.log/shader_detail.txt", m_ProjectPaths.m_ResourcesLogs, m_ResourcePartialPath )); Err )
                return xerr::create_f<xmaterial_compiler::state, "Fail to serialize the information required for the editor">(Err);

            return {};
        }
    };
}

//
// The creation function
//
std::unique_ptr<xmaterial_compiler::instance> xmaterial_compiler::instance::Create(void)
{
    return std::make_unique<implementation>();
}
