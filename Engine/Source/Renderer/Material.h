#pragma once

#include "Renderer/ShaderTypes.h"
#include "Scene/ResourceHandles.h"

#include <string>

// Blinn-Phong 머티리얼 (텍스처 핸들 + 상수). 리소스 관리자가 소유한다.
struct FMaterial
{
	std::string        Name;
	FTextureHandle     BaseColorTexture; // 무효면 흰색 텍스처 사용
	FMaterialConstants Constants;
};
