#include "hlsl_compile.h"

#include <pch.h>
#include <dxc_compiler.h>

#include <cstring>
#include <mutex>

std::vector<uint32_t> CompileHlslToSpirv(const std::string& source, bool isPixelShader)
{
    // DXC instances are not thread safe
    static std::mutex dxcMutex;
    std::lock_guard lock(dxcMutex);
    static DxcCompiler dxc;
    std::vector<uint32_t> spirv;
    IDxcBlob* blob = dxc.compile(source, isPixelShader, false, true);
    if (blob == nullptr)
        return spirv;
    spirv.resize(blob->GetBufferSize() / 4);
    memcpy(spirv.data(), blob->GetBufferPointer(), spirv.size() * 4);
    blob->Release();
    return spirv;
}
