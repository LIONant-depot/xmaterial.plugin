#include "xmaterial_compiler.h"
#include <regex>
#include <unordered_set>
#include <filesystem>

#include "dependencies/shaderc/include/shaderc/shaderc.hpp"
#include "dependencies/xstrtool/source/xstrtool.h"
#include <fstream>
#include <cerrno>   // for errno
#include <iostream>


#include "source/xmaterial_data_file.h"

#include "plugins/xmaterial_instance.plugin/source/xmaterial_intance_descriptor.h"

#pragma comment(lib, "../../dependencies/shaderc/lib/shaderc_combined.lib")

#include <process.h>

//
// force to create the property registrations for these types
//
#include "dependencies/xproperty/source/xcore/my_properties.cpp"
#include "dependencies/xmath/source/bridge/xmath_to_xproperty.h"

void xresource::loader< xrsc::material_type_guid_v >::Destroy(xresource::mgr& Mgr, data_type&& Data, const full_guid& GUID){}
void xresource::loader< xrsc::texture_type_guid_v >::Destroy(xresource::mgr& Mgr, data_type&& Data, const full_guid& GUID) {}

namespace xmaterial_compiler
{
#include <shaderc/shaderc.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <array>  // Or use struct for container

    class CustomIncluder : public shaderc::CompileOptions::IncluderInterface
    {
    public:
        shaderc_include_result* GetInclude(const char* requested_source,
            shaderc_include_type type,
            const char* requesting_source,
            size_t include_depth) override {
            auto result = new shaderc_include_result;
            auto container = new std::array<std::string, 2>;  // [0]: source_name, [1]: content or error
            result->user_data = container;

            try {
                std::filesystem::path requested_path(requested_source);
                if (type == shaderc_include_type_relative) {
                    std::filesystem::path requesting_path(requesting_source);
                    if (!requesting_path.empty()) {
                        requested_path = requesting_path.parent_path() / requested_path;
                    }
                }
                auto full_path = std::filesystem::absolute(requested_path);

                std::ifstream file(full_path, std::ios::in | std::ios::binary);
                if (!file) {
                    throw std::runtime_error("File not found: " + full_path.string());
                }

                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

                (*container)[0] = full_path.string();
                (*container)[1] = std::move(content);

                result->source_name = (*container)[0].data();
                result->source_name_length = (*container)[0].size();
                result->content = (*container)[1].data();
                result->content_length = (*container)[1].size();
            }
            catch (const std::exception& e) {
                (*container)[0] = "";
                (*container)[1] = e.what();

                result->source_name = (*container)[0].data();
                result->source_name_length = 0;
                result->content = (*container)[1].data();
                result->content_length = (*container)[1].size();
            }

            return result;
        }

        void ReleaseInclude(shaderc_include_result* data) override {
            delete static_cast<std::array<std::string, 2>*>(data->user_data);
            delete data;
        }
    };


    using namespace xmaterial_graph;
    struct implementation final : xmaterial_compiler::instance
    {
        std::string PinToVarName(const node& n)
        {
            return "N_" + std::to_string(n.m_Guid.m_Value);
        }

        std::string GetInputExpr(const graph& g, const node& n, int pinIndex, int paramIndex, bool requireConnectionOnly)
        {
            if (pinIndex < 0 || pinIndex >= static_cast<int>(n.m_InputPins.size()))
                return "/*bad pin index*/";

            const auto& ip = n.m_InputPins[pinIndex];

            //1. if connected == resolve upstream
            if (ip.m_ConnectionGUID.m_Value != 0) //will there be casing where the guid just = 0?
            {
                auto it = g.m_Connections.find(ip.m_ConnectionGUID);
                if (it != g.m_Connections.end())
                {
                    const auto& conn = *it->second;
                    if (auto* srcNode = g.findNodeByPin(conn.m_OutputPinGuid))
                    {
                        int idx, sub;
                        bool dummy;
                        const pin* outPin = g.findPinConst(*srcNode, conn.m_OutputPinGuid, dummy, idx, sub);
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
            if (paramIndex >= 0 && paramIndex < static_cast<int>(n.m_Params.size()))
            {
                const auto& prop = n.m_Params[paramIndex];
                if (prop.m_Value.m_pType)
                {
                    auto guid = prop.m_Value.m_pType->m_GUID;
                    if (guid == xproperty::settings::var_type<float>::guid_v)
                        return std::to_string(prop.m_Value.get<float>());
                    if (guid == xproperty::settings::var_type<int>::guid_v)
                        return std::to_string(prop.m_Value.get<int>());
                    if (guid == xproperty::settings::var_type<std::string>::guid_v)
                        return prop.m_Value.get<std::string>();
                    if (guid == xproperty::settings::var_type<xresource::full_guid>::guid_v)
                    {
                        g.m_FinalTextureNodes.m_Textures.emplace_back(&n, paramIndex);
                        return std::to_string(g.m_FinalTextureNodes.m_Textures.size() - 1);
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
                        if (auto* dep = g.findNodeByPin(it->second->m_OutputPinGuid))
                            DFS(g, *dep, visited, ordered);
                    }
                }
            }
            ordered.push_back(&n);
        }


        std::string HandleShaderOnlyNode(graph& g, node& n )
        {
            std::string         ss;
            std::wstring        ShaderFileName;
            std::string         Shader;

            ShaderFileName = std::format( L"{}/{}", m_ProjectPaths.m_Project, n.m_Params[0].m_Value.get<std::wstring>() );

            if ( auto Err = RefreshShaderOnlyNode(ShaderFileName, n, Shader); Err )
            {
                Err.ForEachInChain([&](xerr Error)
                {
                    xstrtool::print("ERROR: {}\n", Err.getMessage());
                    if (auto Hint = Err.getHint(); Hint.empty() == false)
                        xstrtool::print("- HINT: {}\n", Hint);
                });
                return{"ERROR"};
            }

            for( int i=1; i<n.m_InputPins.size(); ++i)
            {
                auto& E = n.m_InputPins[i];

                g.m_FinalTextureNodes.m_Textures.emplace_back( &n, i );
            }

            return Shader;
        }

        std::string HandleShaderOnlyGraphs( graph& g)
        {
            if ( auto pNode = g.findFullShaderNode(); pNode )
            {
                return HandleShaderOnlyNode(g, *pNode);
            }

            return {};
        }

        std::string GenerateGLSL( graph& g)
        {
            // handle shader only nodes
            if (auto str = HandleShaderOnlyGraphs(g); not str.empty())
                return str;


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

            graph Graph;
            Graph.CreateGraph(Graph);
            Graph.serialize(Graph, true, DescriptorFileName);

            const auto& shaderString = GenerateGLSL(Graph);

            LogMessage(xresource_pipeline::msg_type::INFO, std::format("Setting the working path to : {}", xstrtool::To(m_ProjectPaths.m_Project)));
            std::filesystem::current_path(m_ProjectPaths.m_Project);

            //
            // Generate the sprv shader
            //
            displayProgressBar("Compiling Shader", 0.0f);
            shaderc::Compiler       compiler;
            shaderc::CompileOptions options;
            options.SetIncluder(std::make_unique<CustomIncluder>());

            options.SetOptimizationLevel(shaderc_optimization_level_performance);
            options.SetTargetSpirv(shaderc_spirv_version_1_0);
            auto result = compiler.CompileGlslToSpv( shaderString
                                                   , shaderc_glsl_default_fragment_shader
                                                   , "shader.glsl"
                                                   , options
                                                   );

            if (const auto Err = result.GetCompilationStatus(); Err != shaderc_compilation_status_success ) 
            {
                LogMessage(xresource_pipeline::msg_type::ERROR, result.GetErrorMessage().c_str());
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
            // Print the generated shader
            //
            LogMessage(xresource_pipeline::msg_type::INFO, shaderString);

            //
            // Add the dependencies
            //
            for (auto& E : Graph.m_FinalTextureNodes.m_Textures )
            {
                auto Ref = E.m_pNode->m_Params[ E.m_iParam ].m_Value.get<xresource::full_guid>();
                m_Dependencies.m_Resources.push_back(Ref);
            }

            // Check if it is a FullShaderNode if so add the shader as an asset
            if ( auto pNode = Graph.findFullShaderNode(); pNode )
            {
                m_Dependencies.m_Assets.push_back( pNode->m_Params[0].m_Value.get<std::wstring>() );
            }

            //
            // Export the compiled material
            //
            std::vector<uint32_t> spirv(result.cbegin(), result.cend());
            LogMessage(xresource_pipeline::msg_type::INFO, std::format("Shader compiled successfully! SPIR-V size: {} words.\n", spirv.size()) );


            //
            // Create the actual material
            //

            // Allocate and collect the textures
            auto Textures = std::make_unique<xrsc::texture_ref[]>(Graph.m_FinalTextureNodes.m_Textures.size());
            for (auto& E : Graph.m_FinalTextureNodes.m_Textures)
            {
                const int Index = static_cast<int>(&E - Graph.m_FinalTextureNodes.m_Textures.data());
                auto Ref = E.m_pNode->m_Params[E.m_iParam].m_Value.get<xresource::full_guid>();
                Textures[Index].m_Instance = Ref.m_Instance;
            }

            // Setup the structure
            xmaterial::data_file MaterialDataFile;
            MaterialDataFile.m_pShader          = spirv.data();
            MaterialDataFile.m_ShaderSize       = static_cast<std::uint32_t>(spirv.size());
            MaterialDataFile.m_Flags.m_bAlpha   = false;
            MaterialDataFile.m_pDefaultTextures = Graph.m_FinalTextureNodes.m_Textures.empty()? nullptr : Textures.get();
            MaterialDataFile.m_nDefaultTextures = static_cast<std::uint8_t>(Graph.m_FinalTextureNodes.m_Textures.size());

            //
            // Save the material instance template
            //
            {
                displayProgressBar("default material instance", 0.0f);

                xmaterial_instance::descriptor MaterialInstance;

                // Set the material reference
                MaterialInstance.m_MaterialRef.m_Instance = m_ResourceGuid;

                // Allocate all the memory for the final textures
                MaterialInstance.m_lFinalTextures.resize(Graph.m_FinalTextureNodes.m_Textures.size());

                // Set all the textures
                for (auto& E : Graph.m_FinalTextureNodes.m_Textures)
                {
                    const int       Index = static_cast<int>(&E - Graph.m_FinalTextureNodes.m_Textures.data());
                    const auto&     Param = E.m_pNode->m_Params[E.m_iParam];

                    if (Param.m_bCanExpose && Param.m_bExpose )
                    {
                        auto        Ref   = Param.m_Value.get<xresource::full_guid>();
                        auto&       Entry = MaterialInstance.m_lTextureDefaults.emplace_back();

                        // Double check that the file is unique
                        for (auto& O : MaterialInstance.m_lTextureDefaults)
                        {
                            if (O.m_Name == Param.m_ExposeName)
                            {
                                LogMessage(xresource_pipeline::msg_type::ERROR, std::format("Found an Material Texture Expose Parameter duplication [{}]", O.m_Name) );
                                return xerr::create_f<state, "Found an Material Texture Expose Parameter duplication">();
                            }
                        }

                        // Set up the entry
                        Entry.m_Name        = Param.m_ExposeName;
                        Entry.m_Index       = Index;
                        Entry.m_GUID        = E.m_pNode->m_Guid.m_Value;

                        // Set the default... let the material instance override it
                        MaterialInstance.m_lFinalTextures[Index].m_TextureRef.m_Instance = Ref.m_Instance;
                    }
                    else
                    {
                        // In the future we need to differentiate between Hardcoded textures and system textures...
                        auto Ref = Param.m_Value.get<xresource::full_guid>();
                        MaterialInstance.m_lFinalTextures[Index].m_TextureRef.m_Instance = Ref.m_Instance;
                    }
                }

                xtextfile::stream Stream;
                if ( auto Err = Stream.Open(false, std::format(L"{}/{}.log/MaterialInstance.txt", m_ProjectPaths.m_ResourcesLogs, m_ResourcePartialPath), xtextfile::file_type::TEXT); Err )
                    return xerr::create_f<state, "Fail to open the file to save the material instance template">(Err);

                xproperty::settings::context C;
                if ( auto Err = xproperty::sprop::serializer::Stream( Stream, MaterialInstance, C); Err )
                    return xerr::create_f<state, "Fail to save the material instance template">(Err);

                displayProgressBar("default material instance", 1.0f);
            }

            //
            // Serialize Final xMaterial
            //
            int Count = 0;
            for (auto& T : m_Target)
            {
                displayProgressBar("Serializing", Count++ / static_cast<float>(m_Target.size()));

                if (T.m_bValid)
                {
                    xserializer::stream Stream;
                    if (auto Err = Stream.Save(T.m_DataPath, MaterialDataFile, xserializer::compression_level::HIGH); Err)
                        return Err;
                }
            }
            displayProgressBar("Serializing", 1);

            //
            // Serialize the actual shader (If it is not a full-shader...)
            //
            if (auto pNode = Graph.findFullShaderNode(); pNode == nullptr)
            {
                std::ofstream out(std::format(L"{}/{}.log/Shader.txt", m_ProjectPaths.m_ResourcesLogs, m_ResourcePartialPath));
                if (!out) { std::cerr << "Failed to open file for writing.\n";}
                else
                {
                    out << shaderString << "\n";
                    out.close();
                }
            }

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
