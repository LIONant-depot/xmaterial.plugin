include(${CMAKE_BINARY_DIR}/_deps/xcmake_tools/Common.cmake)

# Fetch dependencies
FetchAndPopulate("https://github.com/LIONant-depot/xserializer.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xresource_pipeline_v2.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xstrtool.git" "main" )
FetchAndPopulate("https://github.com/LIONant-depot/xscheduler.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xmath.git" "main")

#
# Get shaderC
# We are trying to get it in the most efficient way possible since it is pretty large
#
# Download Shaderc zip if it doesn't exist
set(SHADERC_ZIP "${CMAKE_SOURCE_DIR}/dependencies/Shaderc.zip")
if(NOT EXISTS "${SHADERC_ZIP}")

    # Define paths
    set(SHADERC_URL "https://storage.googleapis.com/shaderc/artifacts/prod/graphics_shader_compiler/shaderc/windows/continuous_release_2017/371/20210722-133829/install.zip")
    set(SHADERC_EXTRACT_DIR "${CMAKE_SOURCE_DIR}/dependencies")
    set(SHADERC_INSTALL_DIR "${SHADERC_EXTRACT_DIR}/shaderc")

    message(STATUS "Downloading Shaderc...")
    file(DOWNLOAD "${SHADERC_URL}" "${SHADERC_ZIP}" STATUS download_status)
    list(GET download_status 0 status_code)
    if(status_code)
        message(FATAL_ERROR "Failed to download Shaderc: ${download_status}")
    endif()

    # Unzip the Shaderc archive
    message(STATUS "Unzipping Shaderc...")
    file(ARCHIVE_EXTRACT INPUT "${SHADERC_ZIP}" DESTINATION "${SHADERC_EXTRACT_DIR}")
    file(RENAME "${SHADERC_EXTRACT_DIR}/install" "${SHADERC_INSTALL_DIR}")
    if(NOT EXISTS "${SHADERC_INSTALL_DIR}")
       message(FATAL_ERROR "Failed to extract Shaderc zip")
    endif()
endif()


DefineInterfaceComponent(xmaterial_compiler_ "dependencies"
  "source/Compiler/xmaterial_compiler.cpp"
  "source/Compiler/xmaterial_compiler.h"
  "**Graph"
  "source/Graph/xmaterial_graph_node.h"
  "source/Graph/xmaterial_graph_node.cpp"
  "source/Graph/xmaterial_graph.h"
  "source/Graph/xmaterial_graph.cpp"
  "source/Graph/xmaterial_graph_node_guid.h"
)
if(xmaterial_compiler__CREATED)
   AppendComponentIncludes(xmaterial_compiler_ "${CMAKE_SOURCE_DIR}/dependencies/shaderc/include"
)
endif()