#include "port/vk_shader.h"

// glslang keeps its HLSL front end behind this (deprecated there, still built)
#ifndef ENABLE_HLSL
#define ENABLE_HLSL 1
#endif

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/log.h"

namespace gpu::vk {

namespace {

std::once_flag g_init;

// ATTR<n> as the input's semantic, or its name v<n> (glslang keeps either, depending on the version)
int AttrRegister(const glslang::TObjectReflection& in) {
    const char* name = in.name.c_str();
    if (const char* v = std::strrchr(name, '.')) name = v + 1;
    if (name[0] == 'v' && name[1] >= '0' && name[1] <= '9') return std::atoi(name + 1);
    return -1;
}

}  // namespace

bool CompileHlsl(const std::string& src, const char* entry, Stage stage, Spirv& out) {
    std::call_once(g_init, [] { glslang::InitializeProcess(); });
    EShLanguage lang = stage == Stage::Vertex ? EShLangVertex : EShLangFragment;
    glslang::TShader sh(lang);
    const char* text = src.c_str();
    sh.setStrings(&text, 1);
    sh.setEntryPoint(entry);
    sh.setSourceEntryPoint(entry);
    sh.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    sh.setHlslIoMapping(true);
    sh.setAutoMapLocations(true);
    sh.setShiftBinding(glslang::EResUbo, kBindingUbo);
    sh.setShiftBinding(glslang::EResTexture, kBindingTexture);
    sh.setShiftBinding(glslang::EResSampler, kBindingSampler);
    // the HLSL semantics (ATTR<n>) kept in the SPIR-V
    sh.setEnvTargetHlslFunctionality1();
    auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!sh.parse(GetDefaultResources(), 100, false, messages)) {
        LOGE("vk: %s shader: %s", stage == Stage::Vertex ? "vertex" : "pixel", sh.getInfoLog());
        LOGE("%s", src.c_str());
        return false;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(messages) || !prog.mapIO()) {
        LOGE("vk: shader link: %s", prog.getInfoLog());
        return false;
    }
    std::memset(out.locations, -1, sizeof(out.locations));
    if (stage == Stage::Vertex && prog.buildReflection(EShReflectionAllIOVariables)) {
        for (int i = 0; i < prog.getNumPipeInputs(); i++) {
            const glslang::TObjectReflection& in = prog.getPipeInput(i);
            int r = AttrRegister(in);
            unsigned loc = in.layoutLocation();
            if (r >= 0 && r < 16 && loc < 16) out.locations[r] = int8_t(loc);
        }
    }
    glslang::SpvOptions opt;
    opt.disableOptimizer = false;
    opt.validate = false;
    out.code.clear();
    glslang::GlslangToSpv(*prog.getIntermediate(lang), out.code, &opt);
    return !out.code.empty();
}

}  // namespace gpu::vk
