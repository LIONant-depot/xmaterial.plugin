#include "xmaterial_compiler.h"
#include <string>

int main(int argc, const char* argv[])
{
    //for debug purpose
    if constexpr (false)
    {
        static const char* pDebugArgs[] =
        { "ShaderCompiler"
        , "-PROJECT"
        , "C:\\computergraphicsCS250\\xGPU\\dependencies\\shader.plugin\\bin\\Shader.lion_project"
        , "-DEBUG"
        , "D1"
        , "-DESCRIPTOR"
        , "Descriptors\\Shader\\D7\\00\\189E8EEFD88400D7.desc"
        , "-OUTPUT"
        , "C:\\computergraphicsCS250\\xGPU\\dependencies\\shader.plugin\\bin\\Shader.lion_project\\Cache\\Resources\\Platforms"
        };

        argv = pDebugArgs;
        argc = static_cast<int>(sizeof(pDebugArgs) / sizeof(pDebugArgs[0]));
    }

    auto shaderCompilerPipeline = xmaterial_compiler::instance::Create();
    

    if (auto Err = shaderCompilerPipeline->Parse(argc, argv); Err)
    {
        Err.ForEachInChain([&](xerr Error)
        {
            xstrtool::print("ERROR: {}\n", Err.getMessage());
            if (auto Hint = Err.getHint(); Hint.empty() == false)
                xstrtool::print("- HINT: {}\n", Hint);
        });
        return 1;
    }

    if (auto Err = shaderCompilerPipeline->Compile(); Err)
    {
        xstrtool::print("{}\nERROR: Fail to compile(2)\n", Err.getMessage());
        if (auto Hint = Err.getHint(); Hint.empty() == false)
            xstrtool::print("- HINT: {}\n", Hint);
        return Err.getStateUID();
    }
    
    return 0;
}