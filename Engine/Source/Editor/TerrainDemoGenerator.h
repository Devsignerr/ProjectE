#pragma once

#include <filesystem>

// 지형 데모 에셋 생성 (에디터 --generate-terrain-demo): 절차적 높이맵/레이어 가중치(.eterrain), 풀·흙·바위 텍스처(PNG)와
// 머티리얼(.emat), 데모 씬 Scenes/Demo_Terrain.escene. 결정적이라 다시 실행해도 같은 결과 (에셋을 다시 만들 때)
bool GenerateTerrainDemo(const std::filesystem::path& ContentDirectory);
