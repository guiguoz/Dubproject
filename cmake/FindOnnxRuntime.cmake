# ─────────────────────────────────────────────────────────────────────────────
# FindOnnxRuntime.cmake
#
# Downloads pre-built ONNX Runtime binaries (CPU, Windows x64) via
# FetchContent and exposes an IMPORTED target: OnnxRuntime::OnnxRuntime
#
# Usage:
#   include(cmake/FindOnnxRuntime.cmake)
#   target_link_libraries(MyTarget PRIVATE OnnxRuntime::OnnxRuntime)
# ─────────────────────────────────────────────────────────────────────────────

include(FetchContent)

set(ORT_VERSION "1.24.4" CACHE STRING "ONNX Runtime version")

FetchContent_Declare(
    onnxruntime_prebuilt
    URL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-x64-${ORT_VERSION}.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(onnxruntime_prebuilt)

# Locate the extracted directory
set(ORT_ROOT "${onnxruntime_prebuilt_SOURCE_DIR}")

# Create IMPORTED target
if(NOT TARGET OnnxRuntime::OnnxRuntime)
    add_library(OnnxRuntime::OnnxRuntime SHARED IMPORTED GLOBAL)

    set_target_properties(OnnxRuntime::OnnxRuntime PROPERTIES
        IMPORTED_IMPLIB "${ORT_ROOT}/lib/onnxruntime.lib"
        IMPORTED_LOCATION "${ORT_ROOT}/lib/onnxruntime.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${ORT_ROOT}/include"
    )
endif()

# Copy DLL next to executables at build time
function(ort_copy_dll target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ORT_ROOT}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMENT "Copying onnxruntime.dll for ${target_name}"
    )
endfunction()
