#pragma once

#include "Renderer/ShaderTypes.h"
#include "Scene/ResourceHandles.h"

#include <string>

// Blinn-Phong 머티리얼 (텍스처 핸들 + 상수). 리소스 관리자가 소유한다.
// alignas(16) 상수를 첫 멤버로 두어 구조체 패딩 경고(C4324)를 피한다.
struct FMaterial
{
	FMaterialConstants Constants;
	std::string        Name;
	FTextureHandle     BaseColorTexture; // 무효면 흰색 텍스처 사용
};
