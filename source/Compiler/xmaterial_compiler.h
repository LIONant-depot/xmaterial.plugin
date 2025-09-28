#ifndef SHADER_COMPILER_H
#define SHADER_COMPILER_H
#pragma once
#include "source/Graph/xmaterial_graph.h"
#include "dependencies/xresource_pipeline_v2/source/xresource_pipeline.h"

namespace xmaterial_compiler
{
    enum state : std::uint8_t
    { OK
    , FAILURE
    };

    struct instance : xresource_pipeline::compiler::base
    {
        static std::unique_ptr<instance> Create(void);
    };
}

#endif