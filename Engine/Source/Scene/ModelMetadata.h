#pragma once

#include "Core/Math/Math.h"
#include "Scene/AnimNotify.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// 모델 부착 지점 (언리얼 소켓). 뼈(노드) 기준 상대 트랜스폼. Bone이 비면 모델 루트 기준
struct FModelSocket
{
	std::string Name;
	std::string Bone;
	FVector3    Position;
	FQuat       Rotation;
	FVector3    Scale = FVector3::OneVector;

	FMatrix4x4 GetLocalMatrix() const { return FMatrix4x4::MakeTransform(Position, Rotation, Scale); }
};

// 클립 하나의 노티파이 목록 (클립 이름으로 연결 — 다시 가져오며 클립 이름이 바뀌면 연결이 끊긴다)
struct FClipNotifies
{
	std::string              Clip;
	std::vector<FAnimNotify> Notifies;
};

// 리타기팅 본 매핑 수동 지정 하나 (Scene/AnimRetarget.h): 휴머노이드 리그 뼈 이름(예 "LeftUpperArm") → 모델 노드 이름.
// Node가 비면 "매핑하지 않음". 목록에 없는 리그 뼈는 자동 추정을 쓴다
struct FRetargetBoneOverride
{
	std::string Bone;
	std::string Node;
};

// 모델 원본 옆 사이드카(<원본>.emeta, JSON)에 저장하는 편집 데이터: 클립별 노티파이 + 소켓 + 리타기팅 본 매핑.
// 임포트 설정(.eimport)과 달리 쿠킹 입력이 아니므로 고쳐도 모델을 다시 가져오지 않는다.
// 런타임은 FModelResources가 공유하는 인스턴스를 읽는다 (편집기가 고치면 열린 씬에 바로 반영 — 리타기팅 결과 캐시는
// FAnimRetargetLibrary::Invalidate(모델 경로)로 다시 만든다).
// 형식 버전: 1 = 노티파이 + 소켓, 2 = "Retarget": {"Bones": {"<리그 뼈>": "<노드 이름>", ...}} 추가 (1도 읽는다)
struct FModelMetadata
{
	static constexpr const wchar_t* Extension = L".emeta";
	static constexpr int32          Version   = 2;

	std::vector<FClipNotifies>         Clips;
	std::vector<FModelSocket>          Sockets;
	std::vector<FRetargetBoneOverride> RetargetBones;

	const std::vector<FAnimNotify>* FindNotifies(std::string_view Clip) const;
	std::vector<FAnimNotify>&       GetOrAddNotifies(std::string_view Clip);
	const FModelSocket*             FindSocket(std::string_view Name) const;
	const FRetargetBoneOverride*    FindRetargetBone(std::string_view Bone) const;
	// Node가 nullptr이면 수동 지정을 지운다 (자동 추정으로)
	void                            SetRetargetBone(std::string_view Bone, const std::string* Node);
	bool                            IsEmpty() const;

	std::string ToJsonString() const;
	bool        FromJsonString(const std::string& Json);

	static std::filesystem::path GetSidecarPath(const std::filesystem::path& SourcePath);
	// 파일이 없거나 읽지 못하면 빈 데이터
	static FModelMetadata LoadForSource(const std::filesystem::path& SourcePath);
	// 비어 있으면 사이드카 파일을 지운다
	bool SaveForSource(const std::filesystem::path& SourcePath) const;
};
