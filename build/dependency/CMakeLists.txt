include(${CMAKE_BINARY_DIR}/_deps/xcmake_tools/Common.cmake)

# Fetch dependencies
FetchAndPopulate("https://github.com/LIONant-depot/xserializer.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xresource_pipeline_v2.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xresource_mgr.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xstrtool.git" "main" )
FetchAndPopulate("https://github.com/LIONant-depot/xscheduler.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xmath.git" "main")

#
# Get shaderC
# We are trying to get it in the most efficient way possible since it is pretty large
#
# Download Shaderc zip if it doesn't exist
if(WIN32)
  set(SHADERC_ZIP "${CMAKE_SOURCE_DIR}/dependencies/Shaderc.zip")
  set(SHADERC_EXTRACT_DIR "${CMAKE_SOURCE_DIR}/dependencies")
  set(SHADERC_INSTALL_DIR "${SHADERC_EXTRACT_DIR}/shaderc")
  # Gate on the fully-extracted install dir, not the zip: a failed/interrupted download can
  # leave a stale or 0-byte zip behind, which would otherwise block every future retry.
  if(NOT EXISTS "${SHADERC_INSTALL_DIR}")

      # Define paths
      set(SHADERC_URL "https://storage.googleapis.com/shaderc/artifacts/prod/graphics_shader_compiler/shaderc/windows-vs2022-amd64-release/continuous/36/20260731-122731/install.zip")

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
else()
  # Linux: shaderc from the distribution (Ubuntu: libshaderc-dev). Its libshaderc_combined.a leaves glslang/SPIRV-Tools
  # out (they are system libraries there), so the shared libshaderc.so is linked, which brings them in.
  find_path(XMATERIAL_SHADERC_INCLUDE shaderc/shaderc.hpp REQUIRED)
  find_library(XMATERIAL_SHADERC_LIB NAMES shaderc_shared shaderc REQUIRED)
  cmake_policy(SET CMP0079 NEW)   # links the plugin's executable, defined in the plugin's top directory
  target_link_libraries(${TARGET_PROJECT} PRIVATE ${XMATERIAL_SHADERC_LIB})
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
  "**Others"
  "source/xmaterial_xgpu_rsc_loader.h"
  "source/xmaterial_rsc_descriptor.h"
  "source/xmaterial_data_file.h"
)
if(xmaterial_compiler__CREATED)
   AppendComponentIncludes(xmaterial_compiler_ "${CMAKE_SOURCE_DIR}/dependencies/shaderc/include"
)
endif()