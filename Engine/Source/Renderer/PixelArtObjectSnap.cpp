// 픽셀 아트 물체 도트 스냅 규칙
//   대상: 최상위 루트(부모 없음) 중 한 번이라도 월드 위치가 바뀐 것. 멈춘 뒤에도 계속 스냅한다(멈출 때 반 도트 튐 방지).
//         씬에 정적/이동 구분이 없으므로 움직인 적 없는 물체는 건드리지 않는다 → 정적 타일마다 따로 반올림되어 생기는 이음새 없음
//   이동량: 루트 위치를 카메라와 같은 격자(Right/Up, 텍셀 크기)에 맞추는 값 하나를 하위 트리 전체(스킨 뼈 포함)에 더한다
//   소켓 부착 루트: 계층 부모를 무시하고 대상 모델의 최상위 루트 이동량을 따른다 (들고 있는 물체가 캐릭터와 따로 떨지 않게)
//   직교 카메라 전용(호출자 판단). 렌더 중 GPU 파티클 계산이 보는 이미터 위치도 텍셀 반 개 이하로 함께 밀린다
#include "Renderer/PixelArtObjectSnap.h"

#include "Renderer/PixelArtMath.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

namespace
{
	constexpr float MoveThresholdSquared = 0.01f * 0.01f; // cm²: 이보다 작게 바뀌면 움직이지 않은 것으로 본다

	FEntity FindTopRoot(const FScene& Scene, FEntity Entity)
	{
		for (FEntity Parent = Scene.GetParent(Entity); Parent.IsValid(); Parent = Scene.GetParent(Entity))
		{
			Entity = Parent;
		}
		return Entity;
	}
} // namespace

void FPixelArtObjectSnap::Apply(FScene& Scene, const FVector3& Right, const FVector3& Up, float TexelWorldSize)
{
	Saved.clear();
	if (&Scene != TrackedScene)
	{
		Tracked.clear();
		TrackedScene = &Scene;
	}

	FRegistry&                            Registry = Scene.GetRegistry();
	std::unordered_map<FEntity, FTracked> Next;
	std::unordered_map<FEntity, FVector3> RootDeltas;
	std::vector<FEntity>                  SocketRoots;
	for (const FEntity Root : Scene.GetRootEntities())
	{
		const FTransformComponent* Transform = Registry.TryGet<FTransformComponent>(Root);
		if (Transform == nullptr)
		{
			continue;
		}
		if (Scene.IsSocketAttached(Root))
		{
			SocketRoots.push_back(Root);
			continue;
		}
		const FVector3 Position = Transform->GetWorldPosition();
		FTracked       State{ Position, false };
		if (const auto It = Tracked.find(Root); It != Tracked.end())
		{
			State.bMoved = It->second.bMoved || (Position - It->second.LastPosition).LengthSquared() > MoveThresholdSquared;
		}
		Next.emplace(Root, State);
		if (State.bMoved)
		{
			RootDeltas.emplace(Root, FPixelArtMath::ComputeObjectSnapDelta(Position, Right, Up, TexelWorldSize));
		}
	}
	Tracked = std::move(Next);

	for (const auto& [Root, Delta] : RootDeltas)
	{
		OffsetSubtree(Scene, Root, Delta);
	}
	for (const FEntity Root : SocketRoots)
	{
		const FSocketAttachmentComponent& Socket = Registry.Get<FSocketAttachmentComponent>(Root);
		if (const auto It = RootDeltas.find(FindTopRoot(Scene, Socket.Target)); It != RootDeltas.end())
		{
			OffsetSubtree(Scene, Root, It->second);
		}
	}
}

void FPixelArtObjectSnap::Restore(FScene& Scene)
{
	FRegistry& Registry = Scene.GetRegistry();
	for (auto It = Saved.rbegin(); It != Saved.rend(); ++It)
	{
		if (FTransformComponent* Transform = Registry.TryGet<FTransformComponent>(It->Entity))
		{
			Transform->WorldMatrix.M[3][0] = It->Origin.X;
			Transform->WorldMatrix.M[3][1] = It->Origin.Y;
			Transform->WorldMatrix.M[3][2] = It->Origin.Z;
		}
	}
	Saved.clear();
}

void FPixelArtObjectSnap::OffsetSubtree(FScene& Scene, FEntity Entity, const FVector3& Delta)
{
	if (FTransformComponent* Transform = Scene.GetRegistry().TryGet<FTransformComponent>(Entity))
	{
		Saved.push_back({ Entity, Transform->WorldMatrix.GetOrigin() });
		Transform->WorldMatrix.M[3][0] += Delta.X;
		Transform->WorldMatrix.M[3][1] += Delta.Y;
		Transform->WorldMatrix.M[3][2] += Delta.Z;
	}
	for (const FEntity Child : Scene.GetChildren(Entity))
	{
		OffsetSubtree(Scene, Child, Delta);
	}
}
