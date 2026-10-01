#include "Scene/SequencePlayer.h"

#include "Core/Log.h"
#include "Core/Reflection/TypeInfo.h"
#include "Scene/AnimGraph.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	using FValue = FSequenceEvalState::FValue;

	bool IsAnimatableType(EPropertyType Type)
	{
		switch (Type)
		{
		case EPropertyType::Bool:
		case EPropertyType::Int32:
		case EPropertyType::UInt32:
		case EPropertyType::Float:
		case EPropertyType::Vector2:
		case EPropertyType::Vector3:
		case EPropertyType::Vector4: return true;
		default:                     return false;
		}
	}

	FValue ReadProperty(const FPropertyInfo& Property, const void* Object)
	{
		switch (Property.Type)
		{
		case EPropertyType::Bool:    return Property.GetRef<bool>(Object);
		case EPropertyType::Int32:   return *static_cast<const int32*>(Property.GetPtr(Object)); // enum 포함
		case EPropertyType::UInt32:  return Property.GetRef<uint32>(Object);
		case EPropertyType::Float:   return Property.GetRef<float>(Object);
		case EPropertyType::Vector2: return Property.GetRef<FVector2>(Object);
		case EPropertyType::Vector3: return Property.GetRef<FVector3>(Object);
		case EPropertyType::Vector4: return Property.GetRef<FVector4>(Object);
		case EPropertyType::Quat:    return Property.GetRef<FQuat>(Object);
		case EPropertyType::String:  return Property.GetRef<std::string>(Object);
		default:                     return false;
		}
	}

	void WriteProperty(const FPropertyInfo& Property, void* Object, const FValue& Value)
	{
		std::visit(
			[&](const auto& Item) {
				using T = std::decay_t<decltype(Item)>;
				if constexpr (std::is_same_v<T, int32>)
				{
					if (Property.Type == EPropertyType::Int32)
					{
						*static_cast<int32*>(Property.GetPtr(Object)) = Item;
					}
				}
				else
				{
					if (TPropertyTypeOf<T>::Value == Property.Type)
					{
						Property.GetRef<T>(Object) = Item;
					}
				}
			},
			Value);
	}

	// 평가 값(최대 4성분) → 프로퍼티 타입 값
	FValue MakeValue(EPropertyType Type, const FVector4& Value)
	{
		switch (Type)
		{
		case EPropertyType::Bool:    return Value.X >= 0.5f;
		case EPropertyType::Int32:   return static_cast<int32>(std::lround(Value.X));
		case EPropertyType::UInt32:  return static_cast<uint32>(std::max(0l, std::lround(Value.X)));
		case EPropertyType::Float:   return Value.X;
		case EPropertyType::Vector2: return FVector2(Value.X, Value.Y);
		case EPropertyType::Vector3: return FVector3(Value.X, Value.Y, Value.Z);
		default:                     return Value;
		}
	}

	// 처음 쓰는 프로퍼티면 원래 값을 기억한다. 컴포넌트가 없으면 nullptr, 있으면 컴포넌트 주소
	void* Capture(FSequenceEvalState& State, FScene& Scene, FEntity Entity, const FTypeInfo& Type, const FPropertyInfo& Property)
	{
		void* Component = Type.GetComponent ? Type.GetComponent(Scene.GetRegistry(), Entity) : nullptr;
		if (Component == nullptr)
		{
			return nullptr;
		}
		const bool bKnown = std::any_of(State.Saved.begin(), State.Saved.end(), [&](const FSequenceEvalState::FSaved& Saved) {
			return Saved.Entity == Entity && Saved.Property == &Property;
		});
		if (!bKnown)
		{
			State.Saved.push_back({ Entity, &Type, &Property, ReadProperty(Property, Component) });
		}
		return Component;
	}

	// Entity의 프로퍼티 하나 기록을 되돌리고 지운다
	void RestoreProperty(FSequenceEvalState& State, FScene& Scene, FEntity Entity, const FPropertyInfo* Property)
	{
		FRegistry& Registry = Scene.GetRegistry();
		for (auto It = State.Saved.begin(); It != State.Saved.end(); ++It)
		{
			if (It->Entity == Entity && It->Property == Property)
			{
				if (void* Component = Registry.IsValid(Entity) ? It->Type->GetComponent(Registry, Entity) : nullptr)
				{
					WriteProperty(*Property, Component, It->Value);
				}
				State.Saved.erase(It);
				return;
			}
		}
	}

	// 카메라 컷 해제: 컷이 바꾼 주 카메라/우선순위만 되돌린다 (시야각 등 프로퍼티 트랙 값은 그대로)
	void ReleaseCameraCut(FSequenceEvalState& State, FScene& Scene)
	{
		if (const FTypeInfo* CameraType = FTypeRegistry::Get().Find<FCameraComponent>(); CameraType != nullptr && State.ActiveCamera.IsValid())
		{
			RestoreProperty(State, Scene, State.ActiveCamera, CameraType->FindProperty("Primary"));
			RestoreProperty(State, Scene, State.ActiveCamera, CameraType->FindProperty("Priority"));
		}
		State.ActiveCamera = NullEntity;
	}

	std::string_view GetName(const FScene& Scene, FEntity Entity)
	{
		const FNameComponent* Name = Scene.GetRegistry().TryGet<FNameComponent>(Entity);
		return Name != nullptr ? std::string_view(Name->Name) : std::string_view();
	}

	// 이름 경로 (끝부분 일치): Segments 마지막 = 엔티티 이름, 그 앞 = 부모들
	bool MatchesPath(const FScene& Scene, FEntity Entity, const std::vector<std::string_view>& Segments)
	{
		FEntity Current = Entity;
		for (auto It = Segments.rbegin(); It != Segments.rend(); ++It)
		{
			if (!Current.IsValid() || !Scene.GetRegistry().IsValid(Current) || GetName(Scene, Current) != *It)
			{
				return false;
			}
			Current = Scene.GetParent(Current);
		}
		return true;
	}

	FEntity FindInSubtree(const FScene& Scene, FEntity Root, const std::vector<std::string_view>& Segments)
	{
		if (MatchesPath(Scene, Root, Segments))
		{
			return Root;
		}
		for (const FEntity Child : Scene.GetChildren(Root))
		{
			if (const FEntity Found = FindInSubtree(Scene, Child, Segments); Found.IsValid())
			{
				return Found;
			}
		}
		return NullEntity;
	}

	FEntity Resolve(FScene& Scene, FEntity Context, const std::string& Path, FEntity& Cached)
	{
		if (!Cached.IsValid() || !Scene.GetRegistry().IsValid(Cached))
		{
			Cached = FSequenceSystem::ResolveBinding(Scene, Context, Path);
		}
		return Cached;
	}

	void WarnOnce(FSequenceEvalState& State, size_t Track, const std::string& Message)
	{
		if (State.WarnedTracks.size() <= Track)
		{
			State.WarnedTracks.resize(Track + 1, false);
		}
		if (!State.WarnedTracks[Track])
		{
			State.WarnedTracks[Track] = true;
			E_LOG(LogScene, Warning, "시퀀스 트랙 {}: {}", Track, Message);
		}
	}

	void CollectAllEvents(const FSequenceAsset& Asset, float From, float To, bool bIncludeFrom, std::vector<std::string>& OutEvents)
	{
		std::vector<int32> Indices;
		for (const FSequenceTrack& Track : Asset.Tracks)
		{
			if (Track.Type != ESequenceTrackType::Event || Track.bMuted)
			{
				continue;
			}
			Indices.clear();
			SequenceMath::CollectEvents(Track.Events, From, To, bIncludeFrom, Indices);
			for (int32 Index : Indices)
			{
				OutEvents.push_back(Track.Events[static_cast<size_t>(Index)].Name);
			}
		}
	}
} // namespace

// ---------------------------------------------------------------- 적용 상태

bool FSequenceEvalState::HasStaleEntities(const FScene& Scene) const
{
	return std::any_of(Saved.begin(), Saved.end(), [&](const FSaved& Item) { return !Scene.GetRegistry().IsValid(Item.Entity); });
}

void FSequenceEvalState::SwapWithScene(FScene& Scene)
{
	FRegistry& Registry = Scene.GetRegistry();
	for (FSaved& Item : Saved)
	{
		void* Component = Registry.IsValid(Item.Entity) ? Item.Type->GetComponent(Registry, Item.Entity) : nullptr;
		if (Component != nullptr)
		{
			FValue Current = ReadProperty(*Item.Property, Component);
			WriteProperty(*Item.Property, Component, Item.Value);
			Item.Value = std::move(Current);
		}
	}
}

void FSequenceEvalState::Forget()
{
	Saved.clear();
	Bindings.clear();
	CutCameras.clear();
	ActiveCamera = NullEntity;
}

// ---------------------------------------------------------------- 바인딩

FEntity FSequenceSystem::ResolveBinding(const FScene& Scene, FEntity Context, std::string_view Path)
{
	if (Path.empty())
	{
		return Scene.GetRegistry().IsValid(Context) ? Context : NullEntity;
	}
	std::vector<std::string_view> Segments;
	for (size_t Start = 0; Start <= Path.size();)
	{
		const size_t Slash = Path.find('/', Start);
		const size_t End   = Slash == std::string_view::npos ? Path.size() : Slash;
		if (End > Start)
		{
			Segments.push_back(Path.substr(Start, End - Start));
		}
		Start = End + 1;
	}
	if (Segments.empty())
	{
		return NullEntity;
	}
	// 재생 엔티티 하위 먼저 (프리팹 인스턴스로 만든 연출이 자기 부품을 찾게), 그다음 씬 전체
	if (Scene.GetRegistry().IsValid(Context))
	{
		for (const FEntity Child : Scene.GetChildren(Context))
		{
			if (const FEntity Found = FindInSubtree(Scene, Child, Segments); Found.IsValid())
			{
				return Found;
			}
		}
	}
	for (const FEntity Root : Scene.GetRootEntities())
	{
		if (const FEntity Found = FindInSubtree(Scene, Root, Segments); Found.IsValid())
		{
			return Found;
		}
	}
	return NullEntity;
}

std::string FSequenceSystem::MakeBindingPath(const FScene& Scene, FEntity Context, FEntity Target)
{
	if (!Scene.GetRegistry().IsValid(Target) || Target == Context)
	{
		return {};
	}
	std::string Path(GetName(Scene, Target));
	for (FEntity Parent = Scene.GetParent(Target);; Parent = Scene.GetParent(Parent))
	{
		if (ResolveBinding(Scene, Context, Path) == Target || !Parent.IsValid() || Parent == Context)
		{
			return Path;
		}
		Path = std::string(GetName(Scene, Parent)) + "/" + Path;
	}
}

// ---------------------------------------------------------------- 적용

void FSequenceSystem::Evaluate(FScene& Scene, FEntity Context, const FSequenceAsset& Asset, float Time, FSequenceEvalState& State)
{
	FRegistry&           Registry   = Scene.GetRegistry();
	FTypeRegistry&       Types      = FTypeRegistry::Get();
	const FTypeInfo*     Transform  = Types.Find<FTransformComponent>();
	const FTypeInfo*     CameraType = Types.Find<FCameraComponent>();
	const FTypeInfo*     AnimType   = Types.Find<FAnimationComponent>();
	if (State.Bindings.size() != Asset.Tracks.size())
	{
		State.Bindings.assign(Asset.Tracks.size(), NullEntity);
	}
	bool bCameraTrack = false;
	for (size_t TrackIndex = 0; TrackIndex < Asset.Tracks.size(); ++TrackIndex)
	{
		const FSequenceTrack& Track = Asset.Tracks[TrackIndex];
		if (Track.bMuted || Track.GetKeyCount() == 0)
		{
			continue;
		}
		switch (Track.Type)
		{
		case ESequenceTrackType::Transform:
		{
			const FEntity Target = Resolve(Scene, Context, Track.Target, State.Bindings[TrackIndex]);
			if (!Target.IsValid() || Transform == nullptr)
			{
				WarnOnce(State, TrackIndex, "트랜스폼 대상을 찾을 수 없습니다: '" + Track.Target + "'");
				break;
			}
			FVector3 Position;
			FQuat    Rotation;
			FVector3 Scale;
			SequenceMath::EvaluateTransformKeys(Track.TransformKeys, Time, Position, Rotation, Scale);
			for (const char* Name : { "Position", "Rotation", "Scale" })
			{
				Capture(State, Scene, Target, *Transform, *Transform->FindProperty(Name));
			}
			FTransformComponent& Component = Scene.GetTransform(Target);
			Component.Position             = Position;
			Component.Rotation             = Rotation;
			Component.Scale                = Scale;
			break;
		}
		case ESequenceTrackType::Property:
		{
			const FEntity        Target   = Resolve(Scene, Context, Track.Target, State.Bindings[TrackIndex]);
			const FTypeInfo*     Type     = Types.Find(Track.Component);
			const FPropertyInfo* Property = Type != nullptr ? Type->FindProperty(Track.Property) : nullptr;
			if (!Target.IsValid() || Property == nullptr || !IsAnimatableType(Property->Type))
			{
				WarnOnce(State, TrackIndex, "프로퍼티 대상을 찾을 수 없거나 키를 넣을 수 없는 타입입니다: '" + Track.Target + "' " + Track.Component + "." + Track.Property);
				break;
			}
			void* Component = Capture(State, Scene, Target, *Type, *Property);
			if (Component == nullptr)
			{
				WarnOnce(State, TrackIndex, "대상에 컴포넌트가 없습니다: " + Track.Component);
				break;
			}
			const bool     bStep = Property->Type == EPropertyType::Bool || Property->Type == EPropertyType::Int32 || Property->Type == EPropertyType::UInt32;
			const FVector4 Value = SequenceMath::EvaluateValueKeys(Track.ValueKeys, Time, FVector4(), bStep);
			WriteProperty(*Property, Component, MakeValue(Property->Type, Value));
			break;
		}
		case ESequenceTrackType::CameraCut:
		{
			if (bCameraTrack || CameraType == nullptr)
			{
				break; // 카메라 컷은 첫 트랙 하나만
			}
			bCameraTrack = true;
			if (State.CutCameras.size() != Track.Cuts.size())
			{
				State.CutCameras.assign(Track.Cuts.size(), NullEntity);
			}
			const int32 Cut    = SequenceMath::FindCameraCut(Track.Cuts, Time);
			FEntity     Camera = Cut >= 0 ? Resolve(Scene, Context, Track.Cuts[static_cast<size_t>(Cut)].Camera, State.CutCameras[static_cast<size_t>(Cut)])
			                              : NullEntity;
			if (Camera.IsValid() && !Registry.Has<FCameraComponent>(Camera))
			{
				WarnOnce(State, TrackIndex, "컷 대상에 카메라 컴포넌트가 없습니다: " + Track.Cuts[static_cast<size_t>(Cut)].Camera);
				Camera = NullEntity;
			}
			if (Camera != State.ActiveCamera)
			{
				ReleaseCameraCut(State, Scene);
				if (Camera.IsValid())
				{
					Capture(State, Scene, Camera, *CameraType, *CameraType->FindProperty("Primary"));
					Capture(State, Scene, Camera, *CameraType, *CameraType->FindProperty("Priority"));
					FCameraComponent& Component = Registry.Get<FCameraComponent>(Camera);
					Component.bPrimary          = true;
					Component.Priority          = CameraCutPriority;
					State.ActiveCamera          = Camera;
				}
			}
			break;
		}
		case ESequenceTrackType::Animation:
		{
			const FEntity        Target    = Resolve(Scene, Context, Track.Target, State.Bindings[TrackIndex]);
			FAnimationComponent* Animation = Target.IsValid() ? Registry.TryGet<FAnimationComponent>(Target) : nullptr;
			if (Animation == nullptr || !Animation->Runtime.Set || AnimType == nullptr)
			{
				WarnOnce(State, TrackIndex, "애니메이션 대상(모델 루트)을 찾을 수 없습니다: '" + Track.Target + "'");
				break;
			}
			if (Registry.Has<FAnimGraphComponent>(Target))
			{
				WarnOnce(State, TrackIndex, "애니메이션 그래프로 재생 중인 모델에는 클립 트랙을 적용하지 않습니다: '" + Track.Target + "'");
				break;
			}
			float       ClipTime = 0.0f;
			const int32 Section  = SequenceMath::FindAnimSection(Track.Sections, Time, ClipTime);
			if (Section < 0)
			{
				break; // 첫 구간 전: 원래 재생 그대로
			}
			const FSequenceAnimSection& Item = Track.Sections[static_cast<size_t>(Section)];
			const int32                 Clip = Animation->Runtime.Set->FindClip(Item.Clip);
			if (Clip < 0)
			{
				WarnOnce(State, TrackIndex, "모델에 클립이 없습니다: " + Item.Clip);
				break;
			}
			Capture(State, Scene, Target, *AnimType, *AnimType->FindProperty("Clip"));
			Capture(State, Scene, Target, *AnimType, *AnimType->FindProperty("Playing"));
			const float Duration = Animation->Runtime.Set->Clips[static_cast<size_t>(Clip)].Duration;
			if (Item.bLoop && Duration > FMath::SmallNumber)
			{
				ClipTime = ClipTime - std::floor(ClipTime / Duration) * Duration;
			}
			Animation->Clip     = Item.Clip;
			Animation->bPlaying = false; // 시각은 시퀀스가 정한다 (표시 틱 Update(0)이 그 포즈를 쓴다)
			FAnimationSystem::SetTime(Scene, Target, ClipTime);
			break;
		}
		case ESequenceTrackType::Event: break;
		}
	}
	if (!bCameraTrack && State.ActiveCamera.IsValid())
	{
		ReleaseCameraCut(State, Scene); // 카메라 컷 트랙이 꺼졌거나 지워짐
	}
}

void FSequenceSystem::Restore(FScene& Scene, FSequenceEvalState& State)
{
	FRegistry& Registry = Scene.GetRegistry();
	for (auto It = State.Saved.rbegin(); It != State.Saved.rend(); ++It)
	{
		if (Registry.IsValid(It->Entity))
		{
			if (void* Component = It->Type->GetComponent(Registry, It->Entity))
			{
				WriteProperty(*It->Property, Component, It->Value);
			}
		}
	}
	State.Forget();
}

// ---------------------------------------------------------------- 재생

void FSequenceSystem::Update(FScene& Scene, float DeltaSeconds)
{
	std::vector<FEntity> Players;
	Scene.GetRegistry().View<FSequencePlayerComponent>().Each([&](FEntity Entity, FSequencePlayerComponent&) { Players.push_back(Entity); });
	FSequenceLibrary& Library = FSequenceLibrary::Get();
	for (const FEntity Entity : Players)
	{
		FSequencePlayerComponent* Component = Scene.GetRegistry().TryGet<FSequencePlayerComponent>(Entity);
		if (Component == nullptr)
		{
			continue;
		}
		FSequencePlayerRuntime& Runtime = Component->Runtime;
		Runtime.Events.clear();
		Runtime.bFinishedThisUpdate = false;

		// 에셋 해석 (경로 변경, 라이브러리 무효화 = 편집기 저장)
		if (!Runtime.bResolved || Runtime.ResolvedPath != Component->Sequence || Runtime.ResolvedGeneration != Library.GetGeneration())
		{
			Runtime.bResolved                                  = true;
			Runtime.ResolvedPath                               = Component->Sequence;
			Runtime.ResolvedGeneration                         = Library.GetGeneration();
			std::shared_ptr<const FSequenceAsset> NewAsset = Component->Sequence.empty() ? nullptr : Library.Load(Component->Sequence);
			if (NewAsset != Runtime.Asset)
			{
				Runtime.Asset = std::move(NewAsset);
				Runtime.State.Bindings.clear();
				Runtime.State.CutCameras.clear();
				Runtime.State.WarnedTracks.clear();
			}
		}
		if (!Runtime.Asset)
		{
			continue;
		}
		if (!Runtime.bAutoPlayChecked)
		{
			Runtime.bAutoPlayChecked = true;
			if (Component->bAutoPlay && !Runtime.bPlaying)
			{
				Runtime.bPlaying      = true;
				Runtime.bPendingStart = true;
				Runtime.Time          = 0.0f;
			}
		}
		if (!Runtime.bPlaying)
		{
			continue;
		}

		const FSequenceAsset& Asset        = *Runtime.Asset;
		const float           Duration     = Asset.Duration;
		const bool            bIncludeFrom = Runtime.bPendingStart;
		const float           From         = FMath::Clamp(Runtime.Time, 0.0f, Duration);
		float                 To           = bIncludeFrom ? From : From + DeltaSeconds * FMath::Max(Component->PlayRate, 0.0f);
		Runtime.bPendingStart              = false;
		bool bFinished                     = false;
		if (To >= Duration && !bIncludeFrom)
		{
			if (Component->bLoop && Duration > FMath::SmallNumber)
			{
				CollectAllEvents(Asset, From, Duration, false, Runtime.Events);
				To = std::fmod(To, Duration);
				CollectAllEvents(Asset, 0.0f, To, true, Runtime.Events);
			}
			else
			{
				To        = Duration;
				bFinished = true;
				CollectAllEvents(Asset, From, To, false, Runtime.Events);
			}
		}
		else
		{
			CollectAllEvents(Asset, From, To, bIncludeFrom, Runtime.Events);
		}
		Runtime.Time = To;
		Evaluate(Scene, Entity, Asset, Runtime.Time, Runtime.State);
		if (bFinished)
		{
			Runtime.bPlaying            = false;
			Runtime.bFinishedThisUpdate = true;
			if (Component->bRestoreState)
			{
				Restore(Scene, Runtime.State);
			}
			else
			{
				ReleaseCameraCut(Runtime.State, Scene); // 끝나면 원래 카메라로 (문 열림 같은 값은 유지)
			}
		}
	}
}

bool FSequenceSystem::Play(FScene& Scene, FEntity Entity, std::string_view Asset, float StartTime)
{
	FRegistry& Registry = Scene.GetRegistry();
	if (!Registry.IsValid(Entity))
	{
		return false;
	}
	FSequencePlayerComponent* Component = Registry.TryGet<FSequencePlayerComponent>(Entity);
	if (Component == nullptr)
	{
		if (Asset.empty())
		{
			return false;
		}
		Component            = &Registry.Emplace<FSequencePlayerComponent>(Entity);
		Component->bAutoPlay = false;
	}
	if (!Asset.empty() && Component->Sequence != Asset)
	{
		Component->Sequence = std::string(Asset);
	}
	FSequencePlayerRuntime& Runtime = Component->Runtime;
	Runtime.bAutoPlayChecked        = true;
	Runtime.bPlaying                = true;
	if (StartTime >= 0.0f)
	{
		Runtime.Time          = StartTime;
		Runtime.bPendingStart = true;
	}
	return true;
}

void FSequenceSystem::Stop(FScene& Scene, FEntity Entity)
{
	FSequencePlayerComponent* Component = Scene.GetRegistry().IsValid(Entity) ? Scene.GetRegistry().TryGet<FSequencePlayerComponent>(Entity) : nullptr;
	if (Component == nullptr)
	{
		return;
	}
	FSequencePlayerRuntime& Runtime = Component->Runtime;
	Runtime.bAutoPlayChecked        = true;
	Runtime.bPlaying                = false;
	Runtime.Time                    = 0.0f;
	if (Component->bRestoreState)
	{
		Restore(Scene, Runtime.State);
	}
	else
	{
		ReleaseCameraCut(Runtime.State, Scene);
	}
}

void FSequenceSystem::Pause(FScene& Scene, FEntity Entity)
{
	if (FSequencePlayerComponent* Component = Scene.GetRegistry().IsValid(Entity) ? Scene.GetRegistry().TryGet<FSequencePlayerComponent>(Entity) : nullptr)
	{
		Component->Runtime.bAutoPlayChecked = true;
		Component->Runtime.bPlaying         = false;
	}
}

bool FSequenceSystem::IsPlaying(const FScene& Scene, FEntity Entity)
{
	const FSequencePlayerComponent* Component =
		Scene.GetRegistry().IsValid(Entity) ? Scene.GetRegistry().TryGet<FSequencePlayerComponent>(Entity) : nullptr;
	return Component != nullptr && Component->Runtime.bPlaying;
}

float FSequenceSystem::GetTime(const FScene& Scene, FEntity Entity)
{
	const FSequencePlayerComponent* Component =
		Scene.GetRegistry().IsValid(Entity) ? Scene.GetRegistry().TryGet<FSequencePlayerComponent>(Entity) : nullptr;
	return Component != nullptr ? Component->Runtime.Time : 0.0f;
}

void FSequenceSystem::SetTime(FScene& Scene, FEntity Entity, float Seconds)
{
	if (FSequencePlayerComponent* Component = Scene.GetRegistry().IsValid(Entity) ? Scene.GetRegistry().TryGet<FSequencePlayerComponent>(Entity) : nullptr)
	{
		Component->Runtime.Time = FMath::Max(Seconds, 0.0f);
	}
}

float FSequenceSystem::GetDuration(FScene& Scene, FEntity Entity)
{
	FSequencePlayerComponent* Component = Scene.GetRegistry().IsValid(Entity) ? Scene.GetRegistry().TryGet<FSequencePlayerComponent>(Entity) : nullptr;
	if (Component == nullptr || Component->Sequence.empty())
	{
		return 0.0f;
	}
	const std::shared_ptr<const FSequenceAsset> Asset = Component->Runtime.Asset ? Component->Runtime.Asset : FSequenceLibrary::Get().Load(Component->Sequence);
	return Asset ? Asset->Duration : 0.0f;
}
