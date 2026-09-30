#include "Scene/SceneReflection.h"

#include "Core/Reflection/TypeInfo.h"
#include "Scene/Components.h"
#include "Scene/Particles.h"
#include "Scene/Prefab.h"

void RegisterSceneTypes()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	FTypeRegistry& Registry = FTypeRegistry::Get();

	// 이름/계층은 인스펙터와 직렬화가 별도로 다루므로 목록에서 숨기고 제거 불가
	Registry.RegisterType<FNameComponent>("NameComponent", "이름")
		.Property(&FNameComponent::Name, "Name", "이름")
		.AsComponent(false)
		.Hide();

	// 계층은 복제 생성 메시지(부모 NetId)로, 생성됨 표식은 편집 전용이라 복제하지 않는다
	Registry.RegisterType<FHierarchyComponent>("HierarchyComponent", "계층")
		.AsComponent(false)
		.Hide()
		.NoReplicate();

	Registry.RegisterType<FTransientComponent>("TransientComponent", "생성됨")
		.AsComponent(false)
		.Hide()
		.NoReplicate();

	// WorldMatrix는 파생 값이므로 등록하지 않는다 (직렬화 제외)
	Registry.RegisterType<FTransformComponent>("TransformComponent", "트랜스폼")
		.Property(&FTransformComponent::Position, "Position", "위치").Range(-1.0e7f, 1.0e7f, 1.0f) // cm
		.Property(&FTransformComponent::Rotation, "Rotation", "회전")
		.Property(&FTransformComponent::Scale, "Scale", "스케일").Range(0.001f, 1000.0f, 0.02f)
		.AsComponent(false);

	// 런타임 핸들은 Transient, 에셋 참조 문자열이 직렬화 대상
	Registry.RegisterType<FStaticMeshComponent>("StaticMeshComponent", "스태틱 메시")
		.Property(&FStaticMeshComponent::Mesh, "Mesh", "메시", PF_Transient | PF_ReadOnly)
		.Property(&FStaticMeshComponent::Material, "Material", "머티리얼", PF_Transient | PF_ReadOnly)
		.Property(&FStaticMeshComponent::bVisible, "Visible", "표시")
		.Property(&FStaticMeshComponent::MeshAsset, "MeshAsset", "메시 에셋", PF_ReadOnly)
		.Property(&FStaticMeshComponent::MaterialAsset, "MaterialAsset", "머티리얼 에셋", PF_ReadOnly).AssetFilter(".emat")
		.AsComponent();

	Registry.RegisterType<FModelComponent>("ModelComponent", "모델")
		.Property(&FModelComponent::AssetPath, "AssetPath", "에셋 경로", PF_ReadOnly)
		.AsComponent();

	// 소켓 부착: 대상 모델과 소켓은 인스펙터 전용 UI(목록 선택)로 고른다
	Registry.RegisterType<FSocketAttachmentComponent>("SocketAttachmentComponent", "소켓 부착")
		.Property(&FSocketAttachmentComponent::Target, "Target", "대상 모델", PF_Hidden)
		.Property(&FSocketAttachmentComponent::Socket, "Socket", "소켓", PF_Hidden)
		.AsComponent();

	Registry.RegisterType<FDirectionalLightComponent>("DirectionalLightComponent", "방향광")
		.Property(&FDirectionalLightComponent::Color, "Color", "색", PF_Color)
		.Property(&FDirectionalLightComponent::Intensity, "Intensity", "강도").Range(0.0f, 50.0f, 0.05f)
		.AsComponent();

	// 런타임 상태(Runtime)는 등록하지 않는다. FSkinComponent는 런타임 전용이라 등록하지 않음
	Registry.RegisterType<FAnimationComponent>("AnimationComponent", "애니메이션")
		.Property(&FAnimationComponent::Clip, "Clip", "클립")
		.Property(&FAnimationComponent::Speed, "Speed", "속도").Range(-5.0f, 5.0f, 0.01f)
		.Property(&FAnimationComponent::BlendTime, "BlendTime", "블렌드 시간").Range(0.0f, 5.0f, 0.01f)
		.Property(&FAnimationComponent::bPlaying, "Playing", "재생")
		.Property(&FAnimationComponent::bLoop, "Loop", "반복")
		.Property(&FAnimationComponent::bRootMotion, "RootMotion", "루트 모션")
		.AsComponent();

	Registry.RegisterType<FCameraComponent>("CameraComponent", "카메라")
		.Property(&FCameraComponent::FovYDegrees, "FovYDegrees", "시야각 (도)").Range(5.0f, 170.0f, 0.5f)
		.Property(&FCameraComponent::NearZ, "NearZ", "근평면 (cm)").Range(0.1f, 10000.0f, 0.5f)
		.Property(&FCameraComponent::FarZ, "FarZ", "원평면 (cm)").Range(100.0f, 10000000.0f, 100.0f)
		.Property(&FCameraComponent::bPrimary, "Primary", "주 카메라")
		.Property(&FCameraComponent::bOrthographic, "Orthographic", "직교 투영")
		.Property(&FCameraComponent::OrthoHeight, "OrthoHeight", "직교 높이 (cm)").Range(1.0f, 100000.0f, 1.0f)
		.AsComponent();

	Registry.RegisterType<FPixelArtComponent>("PixelArtComponent", "픽셀 아트")
		.Property(&FPixelArtComponent::bEnabled, "Enabled", "사용")
		.Property(&FPixelArtComponent::PixelSize, "PixelSize", "도트 크기 (px)").Range(1.0f, 16.0f)
		.Property(&FPixelArtComponent::bSnapCamera, "SnapCamera", "카메라 도트 스냅")
		.Property(&FPixelArtComponent::OutlineStrength, "OutlineStrength", "외곽선").Range(0.0f, 1.0f, 0.01f)
		.Property(&FPixelArtComponent::HighlightStrength, "HighlightStrength", "모서리 하이라이트").Range(0.0f, 2.0f, 0.01f)
		.Property(&FPixelArtComponent::DepthThreshold, "DepthThreshold", "외곽선 깊이 차 (cm)").Range(0.1f, 1000.0f, 0.5f)
		.Property(&FPixelArtComponent::ColorLevels, "ColorLevels", "색 단계 (0 = 끔)").Range(0.0f, 32.0f)
		.Property(&FPixelArtComponent::DitherStrength, "DitherStrength", "디더").Range(0.0f, 1.0f, 0.01f)
		.AsComponent();

	// 스크립트 Properties 오버라이드는 인스펙터가 스크립트 선언을 읽어 전용 UI로 편집한다
	Registry.RegisterType<FScriptComponent>("ScriptComponent", "스크립트")
		.Property(&FScriptComponent::ScriptAsset, "ScriptAsset", "스크립트").AssetFilter(".lua")
		.Property(&FScriptComponent::PropertyOverrides, "PropertyOverrides", "프로퍼티 오버라이드", PF_Hidden)
		.Property(&FScriptComponent::ExecutionLocation, "ExecutionLocation", "실행 위치 (0 서버, 1 클라이언트, 2 양쪽)").Range(0.0f, 2.0f, 1.0f)
		.AsComponent();

	// 파티클: 에셋(.eparticle)은 콘텐츠 브라우저의 파티클 편집기에서 고친다
	Registry.RegisterType<FParticleSystemComponent>("ParticleSystemComponent", "파티클")
		.Property(&FParticleSystemComponent::Asset, "Asset", "파티클 에셋").AssetFilter(".eparticle")
		.Property(&FParticleSystemComponent::bPlaying, "Playing", "재생")
		.Property(&FParticleSystemComponent::Speed, "Speed", "속도").Range(0.0f, 10.0f, 0.01f)
		.AsComponent();

	// 프리팹: 인스턴스 루트 표식 + 엔티티 연결. 인스펙터는 전용 머리글로 보여 주고(목록에서 숨김), 연결 해제로만 없앤다.
	// 복제는 프리팹 생성 메시지(경로 + 링크 ID)로 클라이언트가 직접 만들므로 이 컴포넌트들은 보내지 않는다
	Registry.RegisterType<FPrefabInstanceComponent>("PrefabInstanceComponent", "프리팹 인스턴스")
		.Property(&FPrefabInstanceComponent::Asset, "Asset", "프리팹", PF_ReadOnly).AssetFilter(".eprefab")
		.Property(&FPrefabInstanceComponent::Overrides, "Overrides", "오버라이드", PF_Hidden | PF_ReadOnly)
		.AsComponent(false)
		.Hide()
		.NoReplicate();

	Registry.RegisterType<FPrefabLinkComponent>("PrefabLinkComponent", "프리팹 연결")
		.Property(&FPrefabLinkComponent::Id, "Id", "프리팹 안 ID", PF_ReadOnly)
		.Property(&FPrefabLinkComponent::Root, "Root", "인스턴스 루트", PF_ReadOnly)
		.AsComponent(false)
		.Hide()
		.NoReplicate();
}
