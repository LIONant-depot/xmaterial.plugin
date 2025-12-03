#include "xmaterial_compiler.h"
#include <string>

int main(int argc, const char* argv[])
{
    //for debug purpose
    if constexpr (false)
    {
        static const char* pDebugArgs[] =
        { "D:\\xgpu_test\\xGPU\\example.lionprj\\Cache\\Plugins\\xmaterial.plugin\\Build\\xmaterial_compiler.vs2022\\Release\\xmaterial_compiler.exe" 
        , "-PROJECT"
        , "D:\\xgpu_test\\xGPU\\example.lionprj" 
        , "-OPTIMIZATION"
        , "O1"
        , "-DEBUG"
        , "D0"
        , "-DESCRIPTOR"
        , "Descriptors\\Material\\03\\C0\\8D45F86DC5F2C003.desc" 
        , "-OUTPUT"
        , "D:\\xgpu_test\\xGPU\\example.lionprj\\Cache\\Resources\\Platforms\\WINDOWS"
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