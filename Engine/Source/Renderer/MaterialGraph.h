#pragma once

#include "Core/Math/Math.h"
#include "Renderer/TextureCompression.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// 머티리얼 그래프 (Phase 49 사이드): .emat의 "Parameters" + "Graph" → HLSL 함수 EvaluateMaterial 하나.
//
// ---- 에셋 형식 (.emat 안, 그래프 머티리얼만 — 없으면 기존 고정 PBR 경로)
//   "Parameters": [ { "Name": "Speed", "Type": "Scalar", "Value": 0.2 },
//                   { "Name": "Tint", "Type": "Vector", "Value": [1, 0.5, 0.2, 1] },
//                   { "Name": "Albedo", "Type": "Texture", "Value": "../Rock.png", "Usage": "Color" },   // Usage = Color|Linear|Normal|Mask
//                   { "Name": "UseDetail", "Type": "StaticSwitch", "Value": true } ],                     // 정적: 셰이더 변형이 바뀐다
//   "Graph": { "Nodes": [ { "Id": "uv", "Type": "TexCoord", "Tiling": [2, 2] },
//                         { "Id": "rock", "Type": "TextureSample", "Texture": "Albedo", "Inputs": { "UV": "uv" } },
//                         { "Id": "tint", "Type": "Multiply", "Inputs": { "A": "rock:1", "B": [1, 0.8, 0.7] } } ],
//              "Output": { "BaseColor": "tint", "Roughness": 0.7 } }
//   입력 값 = 연결 "노드Id"(출력 0) / "노드Id:출력번호" 또는 상수(숫자 = float1, 배열 2~4개 = float2~4).
//   "EditorPosition": [x, y]는 노드 편집기(Phase 51) 정보 — 실행·코드 생성에 쓰지 않는다.
//   텍스처 파라미터 경로는 .emat 폴더 기준 상대 경로(기존 텍스처 키와 같음 → 콘텐츠 브라우저 이동 시 참조 갱신 대상).
//   머티리얼 인스턴스(Parent)는 Parameters에 덮어쓸 값만 적는다(이름으로 찾음, 타입은 부모 것). 그래프는 항상 체인 맨 위(일반 머티리얼) 것.
//
// ---- 타입 규칙 (float1~4)
//   노드 입력: 같은 너비 또는 float1(모든 성분으로 확장)만 허용 — 그 밖의 너비 차이는 오류(자르지 않는다).
//   예외(자르기 허용): 머티리얼 출력 핀(float4 → BaseColor float3 = .xyz 등)과 UV 입력(TextureSample/Panner UV는 앞 2성분).
//   이항 산술(+−×÷, Min/Max, Power)의 결과 너비 = 두 입력 중 큰 너비. Dot/Length → float1, Cross → float3(두 입력 float3),
//   Append → 너비 합(≤ 4), ComponentMask → 채널 수, Split → 출력 k = 성분 k(입력 너비보다 큰 k는 오류).
//
// ---- 컴파일 규칙 (FMaterialGraphCompiler::Compile)
//   전체 그래프 검사: 중복 Id, 알 수 없는 노드 종류/핀/연결 대상, 순환(쓰지 않는 노드 포함) → 오류.
//   출력에서 닿는 노드만 코드로 만든다(사용 안 하는 노드 제거). 정적 스위치는 고른 쪽 입력만 따라간다.
//   같은 계산(노드 종류 + 설정 + 입력 식이 같음)은 지역 변수 하나를 다시 쓴다(공통식 재사용 — 다른 노드여도).
//   지역 변수 이름은 방문 순서 번호(Local0, Local1, ...)라 노드 Id와 무관 → 같은 그래프 의미 → 같은 HLSL 문자열 → 같은 해시.
//   파라미터 레이아웃: 출력에서 닿는 파라미터만, 파라미터 목록(선언) 순서로 — 벡터는 레지스터(float4) 하나씩 먼저,
//   스칼라는 그 뒤 레지스터에 4개씩 x,y,z,w 순으로, 텍스처는 칸 0부터. 정적 스위치는 상수 버퍼에 넣지 않는다(코드에 박힘).
//
// 새 노드를 추가하려면 MaterialGraphCompiler.cpp의 노드 표(GetNodeTable)에 핀과 코드 생성 함수를 넣고,
// MaterialGraphTests에 타입/코드 케이스를 추가한다(노드 Type 이름은 JSON에 그대로 저장되므로 바꾸지 않는다).

// 머티리얼 텍스처 테이블 칸 상한 (그래프 텍스처 파라미터 수 상한). 셰이더는 공간 2 t0~t(N-1)
constexpr uint32 MaterialTextureMax = 16;

enum class EMaterialParameterType : uint8
{
	Scalar,       // float
	Vector,       // float4 (색은 선형 값)
	Texture,      // Texture2D (+ 용도)
	StaticSwitch, // bool — 컴파일 시점 분기 (값이 다르면 다른 셰이더)
	Count
};

const char*            GetMaterialParameterTypeName(EMaterialParameterType Type);
bool                   ParseMaterialParameterType(std::string_view Name, EMaterialParameterType& OutType);
const char*            GetTextureUsageName(ETextureUsage Usage);
bool                   ParseTextureUsage(std::string_view Name, ETextureUsage& OutUsage);

struct FMaterialParameter
{
	std::string            Name;
	EMaterialParameterType Type = EMaterialParameterType::Scalar;
	FVector4               Value = FVector4::ZeroVector; // Scalar = X, Vector = XYZW, StaticSwitch = X != 0
	std::string            Texture;                       // Texture: .emat 폴더 기준 상대 경로 (비면 기본 텍스처 — Normal 용도는 평면 노멀, 나머지 흰색)
	ETextureUsage          Usage = ETextureUsage::Color;  // Texture: 쿠킹 형식/색공간 + 노멀이면 샘플이 탄젠트 노멀(XY → Z 재구성)로 풀린다

	bool GetBool() const { return Value.X != 0.0f; }
};

// 노드 입력 하나: 연결(노드 Id + 출력 번호) 또는 상수
struct FMaterialGraphInput
{
	std::string Pin;            // 핀 이름 ("A", "UV", ...) — 머티리얼 출력이면 출력 이름 ("BaseColor", ...)
	std::string Node;           // 비어 있으면 상수
	uint32      Output = 0;     // 연결된 노드의 출력 번호
	FVector4    Constant = FVector4::ZeroVector;
	uint32      ConstantWidth = 1; // 1~4

	bool IsLink() const { return !Node.empty(); }
};

struct FMaterialGraphNode
{
	std::string                      Id;
	std::string                      Type;
	std::vector<FMaterialGraphInput> Inputs;
	// 종류별 설정 (JSON 키는 종류마다 다르다 — MaterialGraph.cpp 읽기/쓰기 참고)
	//   Constant: Value/ValueWidth ("Value": 숫자 또는 배열)
	//   ScalarParameter/VectorParameter/StaticSwitch: Name ("Parameter"), TextureSample: Name ("Texture") + Option ("Sampler": Wrap|Clamp)
	//   TexCoord: Value.XY ("Tiling", 기본 [1,1]) + Index ("Channel", 0만), ComponentMask: Option ("Channels": "xyz" 등), Compare: Option ("Op")
	FVector4    Value = FVector4::ZeroVector;
	uint32      ValueWidth = 1;
	std::string Name;
	std::string Option;
	int32       Index = 0;
	FVector2    EditorPosition = FVector2::ZeroVector; // 노드 편집기 전용 (코드 생성에 쓰지 않음)

	const FMaterialGraphInput* FindInput(std::string_view Pin) const;
};

struct FMaterialGraph
{
	std::vector<FMaterialGraphNode>  Nodes;
	std::vector<FMaterialGraphInput> Outputs; // Pin = 머티리얼 출력 이름

	bool IsEmpty() const { return Nodes.empty() && Outputs.empty(); }
	const FMaterialGraphNode* FindNode(std::string_view Id) const;
};

// 머티리얼 출력 (생성 함수의 FMaterialSurface 필드와 같은 이름·순서)
enum class EMaterialOutput : uint8
{
	BaseColor,        // float3, 기본 0.5
	Metallic,         // float1, 기본 0 (셰이더가 saturate)
	Roughness,        // float1, 기본 0.5
	Normal,           // float3 탄젠트 공간 (+Z = 표면 법선), 기본 (0,0,1) — 메시 패스가 TBN으로 월드로 보내 정규화
	AmbientOcclusion, // float1, 기본 1
	Emissive,         // float3 (HDR 선형), 기본 0
	Opacity,          // float1 (Translucent/Additive 알파), 기본 1
	OpacityMask,      // float1 (Masked: AlphaCutoff보다 작으면 버림), 기본 1
	Count
};
const char* GetMaterialOutputName(EMaterialOutput Output);
uint32      GetMaterialOutputWidth(EMaterialOutput Output);

// 파라미터 하나의 셰이더 위치
struct FMaterialParameterSlot
{
	std::string            Name;
	EMaterialParameterType Type      = EMaterialParameterType::Scalar;
	uint32                 Register  = 0; // Scalar/Vector: MaterialParams[Register] (헤더 다음 float4), Texture: 테이블 칸
	uint32                 Component = 0; // Scalar: 0~3 (x~w)
	ETextureUsage          Usage     = ETextureUsage::Color;
};

// 그래프 머티리얼의 상수 버퍼(헤더 16B + float4 × ConstantRegisters)와 텍스처 테이블(TextureCount칸) 레이아웃
struct FMaterialParameterLayout
{
	std::vector<FMaterialParameterSlot> Slots; // 레지스터/칸 순서
	uint32                              ConstantRegisters = 0;
	uint32                              TextureCount      = 0;

	const FMaterialParameterSlot* Find(std::string_view Name) const;
	// 파라미터 값 → MaterialParams[] (헤더 제외). 목록에 없는 이름은 0
	std::vector<FVector4> BuildConstants(const std::vector<FMaterialParameter>& Parameters) const;
	// 텍스처 칸 → 파라미터 (없으면 nullptr)
	const FMaterialParameter* FindTextureParameter(uint32 Slot, const std::vector<FMaterialParameter>& Parameters) const;
};

// 컴파일된 그래프 머티리얼 셰이더 (해시가 같으면 같은 HLSL — 셰이더 변형·PSO 공유 키)
struct FMaterialShader
{
	uint64                   Hash = 0;
	std::string              Hlsl;   // 가상 포함 파일 MaterialGraph.generated.hlsli 내용
	FMaterialParameterLayout Layout;
};

struct FMaterialGraphCompileResult
{
	bool                     bSuccess = false;
	std::vector<std::string> Errors; // "[노드 Id] 메시지"
	std::shared_ptr<const FMaterialShader> Shader;

	std::string JoinErrors() const;
};

class FMaterialGraphCompiler
{
public:
	// 생성 HLSL을 담는 가상 포함 파일 이름 (Mesh.hlsl/Shadow.hlsl이 E_MATERIAL_GRAPH일 때 #include)
	static constexpr const char* GeneratedFileName = "MaterialGraph.generated.hlsli";

	static FMaterialGraphCompileResult Compile(const FMaterialGraph& Graph, const std::vector<FMaterialParameter>& Parameters);

	// 노드 종류 목록 (편집기/테스트용, 표 순서)
	static std::vector<std::string> GetNodeTypes();
	// 64비트 FNV-1a (HLSL 문자열 → 해시, 0이면 1)
	static uint64 HashText(std::string_view Text);
};
