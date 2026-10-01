#include "Scene/Sequence.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Scene/Prefab.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <fstream>

E_DECLARE_LOG_CATEGORY(LogScene)

namespace
{
	using nlohmann::json;

	constexpr const char* TrackTypeNames[] = { "Transform", "Property", "CameraCut", "Animation", "Event" };
	constexpr const char* InterpNames[]    = { "Constant", "Linear", "Smooth" };

	template <typename TEnum, size_t N>
	TEnum ParseEnum(const std::string& Text, const char* const (&Names)[N], TEnum Fallback)
	{
		for (size_t Index = 0; Index < N; ++Index)
		{
			if (Text == Names[Index])
			{
				return static_cast<TEnum>(Index);
			}
		}
		return Fallback;
	}

	FVector3 ReadVector3(const json& Node, const char* Key, const FVector3& Fallback)
	{
		const auto Found = Node.find(Key);
		if (Found == Node.end() || !Found->is_array() || Found->size() != 3)
		{
			return Fallback;
		}
		return FVector3((*Found)[0].get<float>(), (*Found)[1].get<float>(), (*Found)[2].get<float>());
	}

	FVector4 ReadValue(const json& Node)
	{
		FVector4   Value;
		const auto Found = Node.find("Value");
		if (Found == Node.end())
		{
			return Value;
		}
		if (Found->is_boolean())
		{
			Value.X = Found->get<bool>() ? 1.0f : 0.0f;
		}
		else if (Found->is_number())
		{
			Value.X = Found->get<float>();
		}
		else if (Found->is_array())
		{
			float* Components[] = { &Value.X, &Value.Y, &Value.Z, &Value.W };
			for (size_t Index = 0; Index < Found->size() && Index < 4; ++Index)
			{
				*Components[Index] = (*Found)[Index].is_number() ? (*Found)[Index].get<float>() : 0.0f;
			}
		}
		return Value;
	}

	json WriteVector3(const FVector3& Value) { return json::array({ Value.X, Value.Y, Value.Z }); }

	// 이전 키 시각 이하인 마지막 키 번호 (Time이 첫 키 앞이면 -1)
	template <typename TKey>
	int32 FindSegment(const std::vector<TKey>& Keys, float Time)
	{
		const auto Upper = std::upper_bound(Keys.begin(), Keys.end(), Time, [](float Value, const TKey& Key) { return Value < Key.Time; });
		return static_cast<int32>(Upper - Keys.begin()) - 1;
	}

	// 키 열의 한 성분 (Get(키) → float)을 Time에서 보간
	template <typename TKey, typename TGet>
	float EvaluateComponent(const std::vector<TKey>& Keys, int32 Segment, float Time, bool bStepOnly, TGet&& Get)
	{
		if (Segment < 0)
		{
			return Get(Keys.front());
		}
		if (Segment + 1 >= static_cast<int32>(Keys.size()))
		{
			return Get(Keys.back());
		}
		const TKey& K0   = Keys[static_cast<size_t>(Segment)];
		const TKey& K1   = Keys[static_cast<size_t>(Segment + 1)];
		const float Span = K1.Time - K0.Time;
		if (Span <= FMath::SmallNumber)
		{
			return Get(K1);
		}
		const float           Alpha    = FMath::Clamp((Time - K0.Time) / Span, 0.0f, 1.0f);
		const bool            bHasPrev = Segment > 0;
		const bool            bHasNext = Segment + 2 < static_cast<int32>(Keys.size());
		const TKey&           Prev     = bHasPrev ? Keys[static_cast<size_t>(Segment - 1)] : K0;
		const TKey&           Next     = bHasNext ? Keys[static_cast<size_t>(Segment + 2)] : K1;
		const ESequenceInterp Interp   = bStepOnly ? ESequenceInterp::Constant : K0.Interp;
		return SequenceMath::Interpolate(Interp, Alpha, K0.Time, Get(K0), K1.Time, Get(K1), bHasPrev, Prev.Time, Get(Prev), bHasNext, Next.Time, Get(Next));
	}

	float UnwrapNear(float Degrees, float Reference)
	{
		return Degrees + 360.0f * std::round((Reference - Degrees) / 360.0f);
	}
} // namespace

const char* ToString(ESequenceTrackType Type)
{
	return TrackTypeNames[static_cast<size_t>(Type)];
}

const char* ToString(ESequenceInterp Interp)
{
	return InterpNames[static_cast<size_t>(Interp)];
}

// ---------------------------------------------------------------- 트랙

size_t FSequenceTrack::GetKeyCount() const
{
	switch (Type)
	{
	case ESequenceTrackType::Transform: return TransformKeys.size();
	case ESequenceTrackType::Property:  return ValueKeys.size();
	case ESequenceTrackType::CameraCut: return Cuts.size();
	case ESequenceTrackType::Animation: return Sections.size();
	case ESequenceTrackType::Event:     return Events.size();
	}
	return 0;
}

float FSequenceTrack::GetKeyTime(size_t Index) const
{
	switch (Type)
	{
	case ESequenceTrackType::Transform: return TransformKeys[Index].Time;
	case ESequenceTrackType::Property:  return ValueKeys[Index].Time;
	case ESequenceTrackType::CameraCut: return Cuts[Index].Time;
	case ESequenceTrackType::Animation: return Sections[Index].Start;
	case ESequenceTrackType::Event:     return Events[Index].Time;
	}
	return 0.0f;
}

void FSequenceTrack::SortKeys()
{
	const auto ByTime = [](const auto& A, const auto& B) { return A.Time < B.Time; };
	std::stable_sort(TransformKeys.begin(), TransformKeys.end(), ByTime);
	std::stable_sort(ValueKeys.begin(), ValueKeys.end(), ByTime);
	std::stable_sort(Cuts.begin(), Cuts.end(), ByTime);
	std::stable_sort(Events.begin(), Events.end(), ByTime);
	std::stable_sort(Sections.begin(), Sections.end(), [](const FSequenceAnimSection& A, const FSequenceAnimSection& B) { return A.Start < B.Start; });
}

// ---------------------------------------------------------------- 에셋

bool FSequenceAsset::FromJsonString(const std::string& Text, FSequenceAsset& Out, std::string* OutError)
{
	const json Root = json::parse(Text, nullptr, false);
	if (!Root.is_object())
	{
		if (OutError != nullptr)
		{
			*OutError = "JSON 객체가 아닙니다";
		}
		return false;
	}
	FSequenceAsset Asset;
	Asset.Duration  = std::max(0.01f, Root.value("Duration", 5.0f));
	Asset.FrameRate = std::clamp(Root.value("FrameRate", 30.0f), 1.0f, 240.0f);
	if (const auto Tracks = Root.find("Tracks"); Tracks != Root.end() && Tracks->is_array())
	{
		for (const json& Node : *Tracks)
		{
			if (!Node.is_object())
			{
				continue;
			}
			FSequenceTrack Track;
			Track.Type      = ParseEnum(Node.value("Type", std::string("Transform")), TrackTypeNames, ESequenceTrackType::Transform);
			Track.Name      = Node.value("Name", std::string());
			Track.Target    = Node.value("Target", std::string());
			Track.Component = Node.value("Component", std::string());
			Track.Property  = Node.value("Property", std::string());
			Track.bMuted    = Node.value("Muted", false);
			const auto Keys = Node.find("Keys");
			const bool bKeys = Keys != Node.end() && Keys->is_array();
			switch (Track.Type)
			{
			case ESequenceTrackType::Transform:
				for (const json& KeyNode : bKeys ? *Keys : json::array())
				{
					FSequenceTransformKey Key;
					Key.Time     = KeyNode.value("Time", 0.0f);
					Key.Position = ReadVector3(KeyNode, "Position", FVector3());
					Key.Rotation = ReadVector3(KeyNode, "Rotation", FVector3());
					Key.Scale    = ReadVector3(KeyNode, "Scale", FVector3(1.0f));
					Key.Interp   = ParseEnum(KeyNode.value("Interp", std::string("Smooth")), InterpNames, ESequenceInterp::Smooth);
					Track.TransformKeys.push_back(Key);
				}
				break;
			case ESequenceTrackType::Property:
				for (const json& KeyNode : bKeys ? *Keys : json::array())
				{
					FSequenceValueKey Key;
					Key.Time   = KeyNode.value("Time", 0.0f);
					Key.Value  = ReadValue(KeyNode);
					Key.Interp = ParseEnum(KeyNode.value("Interp", std::string("Linear")), InterpNames, ESequenceInterp::Linear);
					Track.ValueKeys.push_back(Key);
				}
				break;
			case ESequenceTrackType::CameraCut:
				for (const json& KeyNode : bKeys ? *Keys : json::array())
				{
					Track.Cuts.push_back({ KeyNode.value("Time", 0.0f), KeyNode.value("Camera", std::string()) });
				}
				break;
			case ESequenceTrackType::Animation:
				for (const json& KeyNode : bKeys ? *Keys : json::array())
				{
					FSequenceAnimSection Section;
					Section.Start  = KeyNode.value("Start", 0.0f);
					Section.End    = std::max(Section.Start, KeyNode.value("End", Section.Start + 1.0f));
					Section.Clip   = KeyNode.value("Clip", std::string());
					Section.Offset = KeyNode.value("Offset", 0.0f);
					Section.Rate   = KeyNode.value("Rate", 1.0f);
					Section.bLoop  = KeyNode.value("Loop", true);
					Track.Sections.push_back(Section);
				}
				break;
			case ESequenceTrackType::Event:
				for (const json& KeyNode : bKeys ? *Keys : json::array())
				{
					Track.Events.push_back({ KeyNode.value("Time", 0.0f), KeyNode.value("Name", std::string()) });
				}
				break;
			}
			Track.SortKeys();
			Asset.Tracks.push_back(std::move(Track));
		}
	}
	Out = std::move(Asset);
	return true;
}

std::string FSequenceAsset::ToJsonString() const
{
	json Root         = json::object();
	Root["Version"]   = Version;
	Root["Duration"]  = Duration;
	Root["FrameRate"] = FrameRate;
	json TrackArray   = json::array();
	for (const FSequenceTrack& Track : Tracks)
	{
		json Node    = json::object();
		Node["Type"] = ToString(Track.Type);
		if (!Track.Name.empty())
		{
			Node["Name"] = Track.Name;
		}
		if (Track.Type != ESequenceTrackType::CameraCut && Track.Type != ESequenceTrackType::Event)
		{
			Node["Target"] = Track.Target;
		}
		if (Track.Type == ESequenceTrackType::Property)
		{
			Node["Component"] = Track.Component;
			Node["Property"]  = Track.Property;
		}
		if (Track.bMuted)
		{
			Node["Muted"] = true;
		}
		json Keys = json::array();
		switch (Track.Type)
		{
		case ESequenceTrackType::Transform:
			for (const FSequenceTransformKey& Key : Track.TransformKeys)
			{
				Keys.push_back({ { "Time", Key.Time }, { "Position", WriteVector3(Key.Position) }, { "Rotation", WriteVector3(Key.Rotation) },
				                 { "Scale", WriteVector3(Key.Scale) }, { "Interp", ToString(Key.Interp) } });
			}
			break;
		case ESequenceTrackType::Property:
			for (const FSequenceValueKey& Key : Track.ValueKeys)
			{
				Keys.push_back({ { "Time", Key.Time }, { "Value", json::array({ Key.Value.X, Key.Value.Y, Key.Value.Z, Key.Value.W }) },
				                 { "Interp", ToString(Key.Interp) } });
			}
			break;
		case ESequenceTrackType::CameraCut:
			for (const FSequenceCameraCut& Cut : Track.Cuts)
			{
				Keys.push_back({ { "Time", Cut.Time }, { "Camera", Cut.Camera } });
			}
			break;
		case ESequenceTrackType::Animation:
			for (const FSequenceAnimSection& Section : Track.Sections)
			{
				Keys.push_back({ { "Start", Section.Start }, { "End", Section.End }, { "Clip", Section.Clip }, { "Offset", Section.Offset },
				                 { "Rate", Section.Rate }, { "Loop", Section.bLoop } });
			}
			break;
		case ESequenceTrackType::Event:
			for (const FSequenceEventKey& Event : Track.Events)
			{
				Keys.push_back({ { "Time", Event.Time }, { "Name", Event.Name } });
			}
			break;
		}
		Node["Keys"] = std::move(Keys);
		TrackArray.push_back(std::move(Node));
	}
	Root["Tracks"] = std::move(TrackArray);
	return Root.dump(2) + "\n";
}

bool FSequenceAsset::SaveToFile(const std::filesystem::path& Path) const
{
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		return false;
	}
	const std::string Text = ToJsonString();
	File.write(Text.data(), static_cast<std::streamsize>(Text.size()));
	return File.good();
}

void FSequenceAsset::SortKeys()
{
	for (FSequenceTrack& Track : Tracks)
	{
		Track.SortKeys();
	}
}

FSequenceAsset FSequenceAsset::MakeDefault()
{
	FSequenceAsset Asset;
	FSequenceTrack Events;
	Events.Type = ESequenceTrackType::Event;
	Events.Name = "이벤트";
	Asset.Tracks.push_back(std::move(Events));
	return Asset;
}

// ---------------------------------------------------------------- 순수 계산

namespace SequenceMath
{
	float Interpolate(ESequenceInterp Interp, float Alpha, float T0, float V0, float T1, float V1, bool bHasPrev, float TPrev, float VPrev, bool bHasNext,
	                  float TNext, float VNext)
	{
		switch (Interp)
		{
		case ESequenceInterp::Constant: return Alpha >= 1.0f ? V1 : V0;
		case ESequenceInterp::Linear:   return V0 + (V1 - V0) * Alpha;
		case ESequenceInterp::Smooth:   break;
		}
		// 3차 에르미트: 기울기 = 이웃 키를 잇는 기울기 (Catmull-Rom, 시각 간격이 달라도 맞게 초당 값으로), 양 끝 키는 0
		const float Span   = T1 - T0;
		const float Slope0 = bHasPrev && T1 - TPrev > FMath::SmallNumber ? (V1 - VPrev) / (T1 - TPrev) : 0.0f;
		const float Slope1 = bHasNext && TNext - T0 > FMath::SmallNumber ? (VNext - V0) / (TNext - T0) : 0.0f;
		const float A2     = Alpha * Alpha;
		const float A3     = A2 * Alpha;
		const float H00    = 2.0f * A3 - 3.0f * A2 + 1.0f;
		const float H10    = A3 - 2.0f * A2 + Alpha;
		const float H01    = -2.0f * A3 + 3.0f * A2;
		const float H11    = A3 - A2;
		return H00 * V0 + H10 * Span * Slope0 + H01 * V1 + H11 * Span * Slope1;
	}

	FVector4 EvaluateValueKeys(const std::vector<FSequenceValueKey>& Keys, float Time, const FVector4& Fallback, bool bStepOnly)
	{
		if (Keys.empty())
		{
			return Fallback;
		}
		const int32 Segment = FindSegment(Keys, Time);
		return FVector4(EvaluateComponent(Keys, Segment, Time, bStepOnly, [](const FSequenceValueKey& Key) { return Key.Value.X; }),
		                EvaluateComponent(Keys, Segment, Time, bStepOnly, [](const FSequenceValueKey& Key) { return Key.Value.Y; }),
		                EvaluateComponent(Keys, Segment, Time, bStepOnly, [](const FSequenceValueKey& Key) { return Key.Value.Z; }),
		                EvaluateComponent(Keys, Segment, Time, bStepOnly, [](const FSequenceValueKey& Key) { return Key.Value.W; }));
	}

	bool EvaluateTransformKeys(const std::vector<FSequenceTransformKey>& Keys, float Time, FVector3& OutPosition, FQuat& OutRotation, FVector3& OutScale)
	{
		if (Keys.empty())
		{
			return false;
		}
		const int32 Segment = FindSegment(Keys, Time);
		const auto  Vector  = [&](auto&& Get) {
            return FVector3(EvaluateComponent(Keys, Segment, Time, false, [&](const FSequenceTransformKey& Key) { return Get(Key).X; }),
                            EvaluateComponent(Keys, Segment, Time, false, [&](const FSequenceTransformKey& Key) { return Get(Key).Y; }),
                            EvaluateComponent(Keys, Segment, Time, false, [&](const FSequenceTransformKey& Key) { return Get(Key).Z; }));
		};
		OutPosition             = Vector([](const FSequenceTransformKey& Key) -> const FVector3& { return Key.Position; });
		const FVector3 Rotation = Vector([](const FSequenceTransformKey& Key) -> const FVector3& { return Key.Rotation; });
		OutScale                = Vector([](const FSequenceTransformKey& Key) -> const FVector3& { return Key.Scale; });
		OutRotation             = FQuat::FromEuler(Rotation.X, Rotation.Y, Rotation.Z).GetNormalized();
		return true;
	}

	int32 FindCameraCut(const std::vector<FSequenceCameraCut>& Cuts, float Time)
	{
		return Cuts.empty() ? -1 : FindSegment(Cuts, Time);
	}

	int32 FindAnimSection(const std::vector<FSequenceAnimSection>& Sections, float Time, float& OutClipTime)
	{
		int32 Found = -1;
		for (size_t Index = 0; Index < Sections.size(); ++Index)
		{
			if (Sections[Index].Start <= Time)
			{
				Found = static_cast<int32>(Index);
			}
		}
		if (Found < 0)
		{
			OutClipTime = 0.0f;
			return -1;
		}
		const FSequenceAnimSection& Section = Sections[static_cast<size_t>(Found)];
		OutClipTime                         = Section.Offset + (std::min(Time, Section.End) - Section.Start) * Section.Rate;
		return Found;
	}

	void CollectEvents(const std::vector<FSequenceEventKey>& Events, float From, float To, bool bIncludeFrom, std::vector<int32>& OutIndices)
	{
		for (size_t Index = 0; Index < Events.size(); ++Index)
		{
			const float Time = Events[Index].Time;
			if ((Time > From || (bIncludeFrom && Time >= From)) && Time <= To)
			{
				OutIndices.push_back(static_cast<int32>(Index));
			}
		}
	}

	FVector3 QuatToEulerNear(const FQuat& Rotation, const FVector3& Reference)
	{
		float Pitch = 0.0f;
		float Yaw   = 0.0f;
		float Roll  = 0.0f;
		Rotation.GetNormalized().ToEuler(Pitch, Yaw, Roll);
		// 같은 회전의 두 오일러 표현 (Pitch, Yaw, Roll) ~ (180 - Pitch, Yaw + 180, Roll + 180) 중 기준에 가까운 쪽
		const FVector3 A(UnwrapNear(Pitch, Reference.X), UnwrapNear(Yaw, Reference.Y), UnwrapNear(Roll, Reference.Z));
		const FVector3 B(UnwrapNear(180.0f - Pitch, Reference.X), UnwrapNear(Yaw + 180.0f, Reference.Y), UnwrapNear(Roll + 180.0f, Reference.Z));
		const auto     Distance = [&](const FVector3& Value) {
            return FMath::Abs(Value.X - Reference.X) + FMath::Abs(Value.Y - Reference.Y) + FMath::Abs(Value.Z - Reference.Z);
		};
		return Distance(B) + 1.0e-3f < Distance(A) ? B : A;
	}
} // namespace SequenceMath

// ---------------------------------------------------------------- 라이브러리

FSequenceLibrary& FSequenceLibrary::Get()
{
	static FSequenceLibrary Instance;
	return Instance;
}

std::wstring FSequenceLibrary::MakeKey(const std::string& AssetPath)
{
	std::wstring Key = FPrefabLibrary::Get().ResolveAssetPath(AssetPath).lexically_normal().generic_wstring();
	std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
	return Key;
}

void FSequenceLibrary::Invalidate()
{
	Cache.clear();
	++Generation;
}

void FSequenceLibrary::Invalidate(const std::string& AssetPath)
{
	Cache.erase(MakeKey(AssetPath));
	++Generation;
}

std::shared_ptr<const FSequenceAsset> FSequenceLibrary::Load(const std::string& AssetPath)
{
	const std::wstring Key = MakeKey(AssetPath);
	if (const auto Found = Cache.find(Key); Found != Cache.end())
	{
		return Found->second;
	}
	std::shared_ptr<const FSequenceAsset> Result;
	std::string                           Text;
	if (!FFileSystem::ReadTextFile(FPrefabLibrary::Get().ResolveAssetPath(AssetPath), Text))
	{
		E_LOG(LogScene, Warning, "시퀀스를 읽을 수 없습니다: {}", AssetPath);
	}
	else
	{
		auto        Asset = std::make_shared<FSequenceAsset>();
		std::string Error;
		if (FSequenceAsset::FromJsonString(Text, *Asset, &Error))
		{
			Result = std::move(Asset);
		}
		else
		{
			E_LOG(LogScene, Warning, "시퀀스 형식 오류 ({}): {}", AssetPath, Error);
		}
	}
	Cache[Key] = Result;
	return Result;
}
