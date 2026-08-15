// QNX MHI2Q is a fixed GLES2 target.  The full desktop shader translator
// pulls glslang and SPIRV-Cross into the core even though Vulkan/D3D cannot be
// selected.  Native GLES2 shaders do not need translation.

#include "ppsspp_config.h"

#include "Common/GPU/ShaderTranslation.h"

void ShaderTranslationInit() {
}

void ShaderTranslationShutdown() {
}

bool TranslateShader(std::string *dest, ShaderLanguage destLang,
		const ShaderLanguageDesc &, TranslatedShaderMetadata *, std::string src,
		ShaderLanguage srcLang, ShaderStage, std::string *errorMessage) {
	if (srcLang == destLang) {
		*dest = std::move(src);
		return true;
	}

	if (errorMessage) {
		*errorMessage = "Shader translation is unavailable in the QNX GLES2 build";
	}
	return false;
}
