#pragma once

#include <filesystem>

class FResourceManager;
class FScene;

// 씬 로드 후 에셋 참조 문자열을 런타임 리소스 핸들로 복원한다.
//   - FStaticMeshComponent: MeshAsset("primitive:cube") → 메시 핸들, MaterialAsset(.emat 상대 경로) → 머티리얼 핸들
//   - FModelComponent: 자식이 없으면 glTF를 로드해 하위 노드 엔티티 생성
//   - FParticleSystemComponent: Asset(.eparticle) → 공유 이미터 설정
// 이미 유효한 핸들은 건드리지 않으므로 여러 번 호출해도 안전하다.
struct FSceneAssetResolver
{
	static void Resolve(FScene& Scene, FResourceManager& Resources, const std::filesystem::path& ContentDirectory);

	// 파티클 컴포넌트의 Asset 경로 → 공유 이미터 설정. 경로가 바뀐 컴포넌트만 다시 해석하므로 매 프레임 호출해도 싸다
	static void ResolveParticles(FScene& Scene, FResourceManager& Resources, const std::filesystem::path& ContentDirectory);
};
