#pragma once

#include "Core/Containers/Handle.h"

// 렌더 리소스 핸들. 실제 리소스는 Renderer 모듈의 FResourceManager가 소유한다.
struct FMeshHandleTag {};
struct FTextureHandleTag {};
struct FMaterialHandleTag {};

using FMeshHandle     = THandle<FMeshHandleTag>;
using FTextureHandle  = THandle<FTextureHandleTag>;
using FMaterialHandle = THandle<FMaterialHandleTag>;
