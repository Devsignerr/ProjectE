# 데모 서브맵 "Alley"(비 그친 밤의 도시 뒷골목) 생성: 키트 조립 glTF + 그래프 머티리얼 + 데칼 텍스처 + 파티클 + 씬(Scenes/Demo/Alley.escene)
#   실행: python Tools/DemoMap/BuildAlley.py  (먼저 Scripts/FetchDemoAssets.ps1로 Poly Haven 에셋을 받는다 — 조립 glTF가 원본 .bin/텍스처를 참조)
#   배치를 바꿀 때는 씬 파일이 아니라 이 스크립트를 고치고 다시 실행한다 (결정적 — 고정 시드)
#   좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 골목은 +X 방향(길이 36m), 왼쪽 건물(아파트) 벽면 Y = -300, 오른쪽(공장) Y = +300.
#   골목 끝(X = 3600)은 철망 울타리, 그 너머 교차로 가로등 빛이 안개 속 빛줄기로 보인다. 시작(X = 0)은 벽돌 벽 + Hub 포털
import json
import math
import os
import random
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from GltfKit import FGltfKitComposer, EngineToGltf, RotateX, Translate  # noqa: E402
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTENT = os.path.join(ROOT, "Projects", "Sample", "Content")
PH      = "Asset/PolyHaven"
KIT_OUT = "Asset/DemoKits/Alley"   # 조립 glTF (커밋 — 원본 키트를 상대 경로로 참조)
MAT     = "Materials/Demo/Alley"
APT     = "modular_urban_apartments_facade"
FAC     = "modular_factory_facade"

ALLEY_LENGTH = 3600.0
HALF_WIDTH   = 300.0
FLOOR        = 300.0   # 키트 벽 조각 높이 (cm)
ROOM_DEPTH   = 150.0   # 파사드 뒤 실내 상자 (유리창 너머가 비지 않게)

# 임포트 설정(.eimport MaxTriangles)은 쓰지 않는다 — 이 맵에서 새로 쓰는 에셋은 모두 3만 삼각형 이하 (2026-10-04 glTF 실측)


def Model(Id):
	return f"{PH}/{Id}/{Id}.gltf"


def WriteJson(Path, Doc, Compact=False):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=None if Compact else 2, ensure_ascii=False)
		File.write("\n")


# ---- 키트 조립 (파사드·홈통·배관·철망·셔터) --------------------------------------------------------------------------
# 파사드 띠: 로컬 +x로 3m 칸이 이어지고 +y 위, 앞(골목 쪽) = +z. 조각 i는 x ∈ [3i, 3i+3] (키트 조각이 [-3, 0]이라 노드 x = 3i + 3)
APT_GROUND = {
	"std":      [("wall_standard_standard_01", None)],
	"door_s":   [("wall_door_centered_small_01", "door_centered_small_01")],
	"door_l":   [("wall_door_centered_large_01", "door_centered_large_01")],
	"door_off": [("wall_door_offset_small_01", "door_offset_small_01")],
}
APT_UPPER_VARIANTS = {  # 층(1부터) → 키트 행 변형 번호
	"centered_large": [1, 2, 3], "centered_small": [1, 2, 3], "centered_double": [1, 2, 3], "offset_small": [1, 3, 5],
}
FAC_GROUND = {
	"std":        ("wall_standard_standard_01", None, 1),
	"door_c_l":   ("wall_door_centered_large_01", "door_centered_large_01", 1),
	"door_c_s":   ("wall_door_centered_small_01", "door_centered_small_01", 1),
	"door_rec_l": ("wall_door_recessed_large_01", "door_recessed_large_01", 1),
	"garage":     ("wall_door_garage_door_01", "door_garage_door_01", 2),
}
FAC_UPPER_VARIANTS = {"centered_large": [1, 2, 3], "centered_medium": [1, 2, 3], "centered_small": [1, 2, 3], "centered_double": [1, 2, 3]}


class FFacade:
	# 파사드 띠 하나 (glTF 기준 행렬 Base) — 건물 여럿을 이어 붙인다
	def __init__(self, Kit, Base):
		self.Kit, self.Base = Kit, Base
		self.Lit = []  # (칸, 층) 불 켜진 창 — 실내 발광 판을 둔다
		self.Doors = []  # (칸 가운데 x, 종류)

	def At(self, X, Y, Z=0.0):
		return self.Base @ Translate(X, Y, Z)


def BuildApartments(K, F, StartCol, Ground, Upper, Floors):
	# 아파트 건물 하나: 1층 Ground[칸], 2층부터 Upper[칸] 창 종류, 층 사이 코니스, 꼭대기 크라운
	Cols = len(Ground)
	for C in range(Cols):
		X = 3.0 * (StartCol + C + 1)
		for Wall, Fill in APT_GROUND[Ground[C]]:
			K.Add(APT, Wall, F.At(X, 0))
			if Fill:
				K.Add(APT, Fill, F.At(X, 0))
				F.Doors.append((X - 1.5, Ground[C]))
		if Ground[C] == "std":
			K.Add(APT, "base_standard_01", F.At(X, 0))
		for Floor in range(1, Floors):
			Type = Upper[C]
			V = APT_UPPER_VARIANTS[Type][min(Floor - 1, 2)]
			K.Add(APT, f"wall_window_{Type}_{V:02d}", F.At(X, Floor * 3.0))
			K.Add(APT, f"window_{Type}_{V:02d}", F.At(X, Floor * 3.0))
			K.Add(APT, "cornice_standard_standard_01", F.At(X, Floor * 3.0))
		K.Add(APT, "crown_standard_standard_01", F.At(X, Floors * 3.0))
	X0, X1 = 3.0 * StartCol, 3.0 * (StartCol + Cols)
	K.Add(APT, "crown_end_01", F.At(X0, Floors * 3.0))
	K.Add(APT, "crown_end_02", F.At(X1, Floors * 3.0))
	for X in (X0, X1):
		K.Add(APT, "wall_pier_standard_01", F.At(X, 0))
		for Floor in range(1, Floors):
			K.Add(APT, "wall_pier_standard_01", F.At(X, Floor * 3.0))


def BuildFactory(K, F, StartCol, Ground, Upper, Floors):
	Col = StartCol
	for Type in Ground:
		Wall, Fill, Span = FAC_GROUND[Type]
		X = 3.0 * (Col + Span)
		K.Add(FAC, Wall, F.At(X, 0))
		if Fill:
			K.Add(FAC, Fill, F.At(X, 0))
			F.Doors.append((X - 1.5 * Span, Type))
		if Type == "std":
			K.Add(FAC, "base_standard_standard_01", F.At(X, 0))
		Col += Span
	Cols = Col - StartCol
	for C in range(Cols):
		X = 3.0 * (StartCol + C + 1)
		for Floor in range(1, Floors):
			Type = Upper[C]
			V = FAC_UPPER_VARIANTS[Type][min(Floor - 1, 2)]
			K.Add(FAC, f"wall_window_{Type}_{V:02d}", F.At(X, Floor * 3.0))
			K.Add(FAC, f"window_{Type}_{V:02d}", F.At(X, Floor * 3.0))
			K.Add(FAC, "cornice03_standard_standard_01", F.At(X, Floor * 3.0))
		K.Add(FAC, "cornice01_standard_standard_01", F.At(X, Floors * 3.0 - 0.15))
		K.Add(FAC, "crown_standard_standard_01", F.At(X, Floors * 3.0))
	X0, X1 = 3.0 * StartCol, 3.0 * (StartCol + Cols)
	K.Add(FAC, "crown_end_01", F.At(X0, Floors * 3.0))
	K.Add(FAC, "crown_end_02", F.At(X1, Floors * 3.0))
	for X in (X0, X1):
		for Floor in range(Floors):
			K.Add(FAC, "wall_pier_standard_01", F.At(X, Floor * 3.0))


def AddDownpipe(K, F, X, Top):
	# 빗물 홈통: 바닥 토출구 + 1m 관 + 꼭대기 깔때기 (로컬 x, 벽 앞 z = 0.12)
	Z = 0.12
	K.Add("modular_metal_gutter", "modular_metal_gutter_outlet", F.At(X, 0.27, Z))
	Y = 0.4
	while Y + 1.0 <= Top - 0.4:
		K.Add("modular_metal_gutter", "modular_metal_gutter_section", F.At(X, Y, Z))
		Y += 1.0
	K.Add("modular_metal_gutter", "modular_metal_gutter_funnel", F.At(X, Y, Z))


def AddDuct(K, F, X, Bottom, Top):
	# 원형 덕트 세로 배관 (키트 덕트 축 +z → RotateX(-90)으로 +y)
	Z = 0.3
	Y = Bottom
	while Y + 1.42 <= Top:
		K.Add("modular_airduct_circular_01", "modular_airduct_circular_triple", F.At(X, Y, Z) @ RotateX(-90))
		K.Add("modular_airduct_circular_01", "modular_airduct_circular_brace", F.At(X, Y + 0.7, Z) @ RotateX(-90))
		Y += 1.42
	K.Add("modular_airduct_circular_01", "modular_airduct_circular_fan", F.At(X, Y, Z) @ RotateX(-90))


def AddChainlinkFence(K, Base):
	# 골목 끝 철망: 기둥 0/2/4/6m, 2m 철망 두 칸 + 가운데 문틀(+문) — 로컬 +x 방향 6m
	Fence = "modular_chainlink_fence"
	K.Add(Fence, "modular_chainlink_fence_end_01", Base @ Translate(0.0, 0, 0))
	K.Add(Fence, "modular_chainlink_fence_double", Base @ Translate(2.0, 0, 0))
	K.Add(Fence, "modular_chainlink_post_middle", Base @ Translate(2.0, 0, 0))
	K.Add(Fence, "modular_chainlink_fence_door_frame", Base @ Translate(2.0, 0, 0))
	K.Add(Fence, "modular_chainlink_fence_door_gate", Base @ Translate(2.55, 1.08, 0))
	K.Add(Fence, "modular_chainlink_fence_door_latch", Base @ Translate(3.28, 1.14, 0))
	K.Add(Fence, "modular_chainlink_post_middle", Base @ Translate(4.0, 0, 0))
	K.Add(Fence, "modular_chainlink_fence_double", Base @ Translate(6.0, 0, 0))
	K.Add(Fence, "modular_chainlink_fence_end_02", Base @ Translate(6.0, 0, 0))


# 건물 구성 (칸 = 3m)
L1 = dict(StartCol=0, Floors=4, Ground=["std", "door_s", "std", "std", "door_l", "std"],
		  Upper=["centered_small", "centered_large", "centered_double", "centered_large", "centered_double", "centered_small"])
L2 = dict(StartCol=6, Floors=3, Ground=["std", "door_off", "std", "std", "std", "door_s"],
		  Upper=["offset_small", "centered_large", "centered_large", "centered_small", "centered_double", "offset_small"])
R1 = dict(StartCol=0, Floors=4, Ground=["door_c_l", "std", "door_c_s", "std", "std", "std", "garage", "std", "door_rec_l", "std", "std"],
		  Upper=["centered_medium", "centered_medium", "centered_double", "centered_large", "centered_large", "centered_small",
				 "centered_medium", "centered_medium", "centered_double", "centered_large", "centered_small", "centered_medium"])
BACK = dict(StartCol=0, Floors=3, Ground=["std", "door_c_l", "std", "std", "door_c_s", "std"],
			Upper=["centered_large", "centered_double", "centered_large", "centered_large", "centered_double", "centered_large"])
LIT_WINDOWS = {"L": [(1, 2), (3, 1), (4, 3), (7, 1), (10, 2)], "R": [(2, 1), (5, 2), (9, 3), (10, 1)], "B": [(0, 1), (3, 2), (5, 1)]}

# 파사드 기준: 왼쪽 띠는 엔진 (0, -300)에서 +X로(엔진 Yaw -90), 오른쪽 띠는 (3600, +300)에서 -X로(Yaw +90),
# 교차로 너머 배경 건물은 (4800, -900)에서 +Y로, 앞(-X)을 본다(Yaw 0)
FACADE_LEFT  = EngineToGltf(0.0, -HALF_WIDTH, 0.0, -90.0)
FACADE_RIGHT = EngineToGltf(ALLEY_LENGTH, HALF_WIDTH, 0.0, 90.0)
BACKDROP_X   = 4800.0
FACADE_BACK  = EngineToGltf(BACKDROP_X, -900.0, 0.0, 0.0)


def LeftLocalToEngine(X, Y, Z=0.0):   # 왼쪽 띠 로컬(m) → 엔진 cm
	return (X * 100.0, -HALF_WIDTH + Z * 100.0, Y * 100.0)


def RightLocalToEngine(X, Y, Z=0.0):
	return (ALLEY_LENGTH - X * 100.0, HALF_WIDTH - Z * 100.0, Y * 100.0)


def BuildArchitecture():
	K = FGltfKitComposer(os.path.join(CONTENT, PH), os.path.join(CONTENT, KIT_OUT, "AlleyArchitecture.gltf"))
	Left, Right, Back = FFacade(APT, FACADE_LEFT), FFacade(FAC, FACADE_RIGHT), FFacade(FAC, FACADE_BACK)
	BuildApartments(K, Left, **L1)
	BuildApartments(K, Left, **L2)
	BuildFactory(K, Right, **R1)
	BuildFactory(K, Back, **BACK)
	# 홈통: 왼쪽 두 건물 사이, 오른쪽 공장 두 곳
	AddDownpipe(K, Left, 18.0 - 0.25, 9.0)
	AddDownpipe(K, Right, 9.0 + 0.3, 12.0)
	AddDownpipe(K, Right, 27.0 + 0.3, 12.0)
	# 공장 벽 덕트 + 환풍구(증기)
	AddDuct(K, Right, 16.5, 0.6, 12.0)
	K.Add("modular_airduct_circular_01", "modular_airduct_rectangular_converter", Right.At(4.5, 1.6, 0.0))
	K.Add("modular_airduct_circular_01", "modular_airduct_rectangular_vent_fan", Right.At(4.5, 1.6, 0.32))
	# 작은 문 구멍에는 셔터 (구멍 폭 1.26m, 높이 2.21m — 셔터 1.08 × 2.4를 맞춘다)
	for Facade in (Right, Back):
		for Index, (X, Type) in enumerate(Facade.Doors):
			if Type == "door_c_s":
				Name = "rollershutter_door_graffiti" if Index % 2 == 0 else "rollershutter_door"
				K.Add("rollershutter_door", Name, Facade.At(X, 0.0, -0.02) @ np.diag([1.16, 0.92, 1.0, 1.0]))
	AddChainlinkFence(K, EngineToGltf(ALLEY_LENGTH, -HALF_WIDTH, 0.0, 0.0))
	Count = K.Save()
	return Count, Left, Right, Back


# ---- 머티리얼 ---------------------------------------------------------------------------------------------------------
def TexturePath(Id, Map):
	return f"../../../{PH}/{Id}/{Id}_{Map}_2k.jpg"


def WorldUvGraph(Id, Scale, Wall, BaseScale, RoughScale, Wet=False, Tint=(1.0, 1.0, 1.0)):
	# 월드 좌표 UV 그래프 머티리얼 (내장 큐브는 면마다 UV 0~1이라 상자 크기와 무관하게 텍스처 밀도를 맞춘다).
	#   바닥: UV = 월드 XY × Scale, 벽: UV = (X + Y, -Z) × Scale (축 정렬 벽 — 한 축은 벽면에서 일정)
	#   Wet: 큰 배율로 한 번 더 읽은 거칠기 맵 G를 젖음 마스크로 써서 어둡고 매끈한 얼룩(물 고임)을 섞는다
	Nodes = [
		{"Id": "pos", "Type": "WorldPosition"},
		{"Id": "scale", "Type": "ScalarParameter", "Parameter": "WorldScale"},
		{"Id": "p", "Type": "Multiply", "Inputs": {"A": "pos", "B": "scale"}},
	]
	if Wall:
		Nodes += [
			{"Id": "ps", "Type": "Split", "Inputs": {"A": "p"}},
			{"Id": "u", "Type": "Add", "Inputs": {"A": "ps:0", "B": "ps:1"}},
			{"Id": "v", "Type": "Multiply", "Inputs": {"A": "ps:2", "B": -1.0}},
			{"Id": "uv", "Type": "Append", "Inputs": {"A": "u", "B": "v"}},
		]
	else:
		Nodes.append({"Id": "uv", "Type": "ComponentMask", "Channels": "xy", "Inputs": {"A": "p"}})
	Nodes += [
		{"Id": "col", "Type": "TextureSample", "Texture": "Albedo", "Inputs": {"UV": "uv"}},
		{"Id": "arm", "Type": "TextureSample", "Texture": "Arm", "Inputs": {"UV": "uv"}},
		{"Id": "nrm", "Type": "TextureSample", "Texture": "Normal", "Inputs": {"UV": "uv"}},
		{"Id": "tint", "Type": "VectorParameter", "Parameter": "Tint"},
		{"Id": "dry", "Type": "Multiply", "Inputs": {"A": "col:1", "B": "tint:1"}},
		{"Id": "dryBase", "Type": "Multiply", "Inputs": {"A": "dry", "B": BaseScale}},
		{"Id": "dryRough", "Type": "Multiply", "Inputs": {"A": "arm:3", "B": RoughScale}},
	]
	Output = {"BaseColor": "dryBase", "Roughness": "dryRough", "Normal": "nrm:1", "AmbientOcclusion": "arm:2", "Metallic": 0.0}
	if Wet:
		Nodes += [
			{"Id": "uvBig", "Type": "Multiply", "Inputs": {"A": "uv", "B": 0.11}},
			{"Id": "maskTex", "Type": "TextureSample", "Texture": "Arm", "Inputs": {"UV": "uvBig"}},
			{"Id": "maskRaw", "Type": "Subtract", "Inputs": {"A": "maskTex:3", "B": 0.5}},
			{"Id": "maskSharp", "Type": "Multiply", "Inputs": {"A": "maskRaw", "B": 6.0}},
			{"Id": "wet", "Type": "Saturate", "Inputs": {"A": "maskSharp"}},
			{"Id": "wetBase", "Type": "Multiply", "Inputs": {"A": "dryBase", "B": 0.55}},
			{"Id": "base", "Type": "Lerp", "Inputs": {"A": "dryBase", "B": "wetBase", "Alpha": "wet"}},
			{"Id": "rough", "Type": "Lerp", "Inputs": {"A": "dryRough", "B": 0.05, "Alpha": "wet"}},
			{"Id": "normal", "Type": "Lerp", "Inputs": {"A": "nrm:1", "B": [0.0, 0.0, 1.0], "Alpha": "wet"}},
		]
		Output.update({"BaseColor": "base", "Roughness": "rough", "Normal": "normal"})
	return {
		"BlendMode": "Opaque",
		"Parameters": [
			{"Name": "Albedo", "Type": "Texture", "Value": TexturePath(Id, "diff"), "Usage": "Color"},
			{"Name": "Arm", "Type": "Texture", "Value": TexturePath(Id, "arm"), "Usage": "Linear"},
			{"Name": "Normal", "Type": "Texture", "Value": TexturePath(Id, "nor_gl"), "Usage": "Normal"},
			{"Name": "WorldScale", "Type": "Scalar", "Value": Scale},
			{"Name": "Tint", "Type": "Vector", "Value": list(Tint) + [1.0]},
		],
		"Graph": {"Nodes": Nodes, "Output": Output},
	}


def PlainMaterial(Name, Base, Rough, Emissive=(0.0, 0.0, 0.0), Blend=None, Texture="", TwoSided=False):
	Mat = {"Name": Name}
	if Blend:
		Mat["BlendMode"] = Blend
	if TwoSided:
		Mat["TwoSided"] = True
	Mat.update({
		"BaseColorFactor": list(Base), "EmissiveFactor": list(Emissive), "Metallic": 0.0, "Roughness": Rough,
		"NormalScale": 1.0, "OcclusionStrength": 1.0, "BaseColorTexture": Texture, "MetallicRoughnessTexture": "",
		"NormalTexture": "", "OcclusionTexture": "", "EmissiveTexture": "",
	})
	return Mat


def WriteMaterials():
	Folder = os.path.join(CONTENT, MAT)
	Graphs = {
		"AlleyAsphalt": WorldUvGraph("asphalt_02", 1.0 / 260.0, False, 0.75, 0.6, Wet=True),
		# 배수로 바닥: 밝은 콘크리트 + 젖음 얼룩(거칠기 0.05)이 높은 시점에서 골목 위 하늘을 비춰 밝은 회색 띠로 떴다 → 어둡고 거친 콘크리트.
		#   얇은 물 상자(Channel_Water, 4cm)는 흡수가 거의 없어 바닥이 그대로 비친다 (물의 안개 이중 적용은 Water.hlsl에서 고침)
		"AlleyConcrete": WorldUvGraph("concrete_floor_worn_001", 1.0 / 200.0, False, 0.08, 1.0, Tint=(0.9, 0.95, 1.0)),
		"AlleyBrick": WorldUvGraph("dark_brick_wall", 1.0 / 220.0, True, 0.8, 0.9),
	}
	for Name, Graph in Graphs.items():
		WriteJson(os.path.join(Folder, f"{Name}.emat"), {"Name": Name, **Graph})
	Plain = {
		"AlleyInterior": PlainMaterial("AlleyInterior", (0.035, 0.032, 0.03, 1.0), 0.9),
		"AlleyWindowGlow": PlainMaterial("AlleyWindowGlow", (0.0, 0.0, 0.0, 1.0), 1.0, (2.4, 1.35, 0.6)),
		"AlleyWindowGlowCool": PlainMaterial("AlleyWindowGlowCool", (0.0, 0.0, 0.0, 1.0), 1.0, (0.7, 1.1, 1.6)),
		"AlleyNeonPink": PlainMaterial("AlleyNeonPink", (0.0, 0.0, 0.0, 1.0), 1.0, (14.0, 1.2, 6.0)),
		"AlleyNeonCyan": PlainMaterial("AlleyNeonCyan", (0.0, 0.0, 0.0, 1.0), 1.0, (0.8, 9.0, 14.0)),
		"AlleyBulbWarm": PlainMaterial("AlleyBulbWarm", (0.0, 0.0, 0.0, 1.0), 1.0, (30.0, 18.0, 8.0)),
		"AlleyBulbSodium": PlainMaterial("AlleyBulbSodium", (0.0, 0.0, 0.0, 1.0), 1.0, (40.0, 18.0, 5.0)),
		"AlleyBulbCool": PlainMaterial("AlleyBulbCool", (0.0, 0.0, 0.0, 1.0), 1.0, (20.0, 24.0, 30.0)),
		# 데칼 (텍스처는 이 스크립트가 만든 알파 마스크)
		"DecalPuddleA": PlainMaterial("DecalPuddleA", (0.03, 0.033, 0.036, 0.92), 0.02, Texture="PuddleA.png"),
		"DecalPuddleB": PlainMaterial("DecalPuddleB", (0.03, 0.033, 0.036, 0.92), 0.02, Texture="PuddleB.png"),
		"DecalDamp": PlainMaterial("DecalDamp", (0.012, 0.011, 0.009, 1.0), 0.22, Texture="Damp.png"),
		"DecalOil": PlainMaterial("DecalOil", (0.012, 0.012, 0.014, 0.85), 0.08, Texture="PuddleB.png"),
	}
	for Name, Mat in Plain.items():
		WriteJson(os.path.join(Folder, f"{Name}.emat"), Mat)


# ---- 데칼 텍스처 (절차적 알파 마스크, 결정적) ---------------------------------------------------------------------------
def _Noise(Size, Scale, Seed, Octaves=5):
	Rng = np.random.default_rng(Seed)
	Y, X = np.mgrid[0:Size, 0:Size] / Size
	Sum = np.zeros((Size, Size))
	Amp, Norm = 1.0, 0.0
	for Octave in range(Octaves):
		Cells = int(Scale * 2 ** Octave)
		Grid = Rng.random((Cells + 1, Cells + 1))
		FX, FY = X * Cells, Y * Cells
		IX, IY = np.floor(FX).astype(int), np.floor(FY).astype(int)
		TX, TY = FX - IX, FY - IY
		TX, TY = TX * TX * (3 - 2 * TX), TY * TY * (3 - 2 * TY)
		IX1, IY1 = np.minimum(IX + 1, Cells), np.minimum(IY + 1, Cells)
		Top = Grid[IY, IX] * (1 - TX) + Grid[IY, IX1] * TX
		Bot = Grid[IY1, IX] * (1 - TX) + Grid[IY1, IX1] * TX
		Sum += (Top * (1 - TY) + Bot * TY) * Amp
		Norm += Amp
		Amp *= 0.5
	return Sum / Norm


def _SaveMask(Path, Alpha):
	A = np.clip(Alpha * 255.0 + 0.5, 0, 255).astype(np.uint8)
	Rgba = np.dstack([np.full_like(A, 255)] * 3 + [A])
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	Image.fromarray(Rgba, "RGBA").save(Path, optimize=False)


def WriteDecalTextures():
	Folder = os.path.join(CONTENT, MAT)
	Size = 512
	Y, X = (np.mgrid[0:Size, 0:Size] + 0.5) / Size * 2.0 - 1.0
	for Name, Seed, Stretch in (("PuddleA", 3, 1.0), ("PuddleB", 9, 1.6)):
		R = np.sqrt((X * Stretch) ** 2 + Y ** 2)
		Field = (1.0 - R) + (_Noise(Size, 3, Seed) - 0.5) * 1.1
		Edge = np.clip((Field - 0.15) / 0.07, 0.0, 1.0)  # 좁은 가장자리 (물 경계)
		Edge *= np.clip((0.98 - np.maximum(np.abs(X), np.abs(Y))) / 0.05, 0.0, 1.0)  # 상자 경계에서 0
		_SaveMask(os.path.join(Folder, f"{Name}.png"), Edge)
	# 벽 밑동 습기 얼룩: 아래(텍스처 V = 1)가 진하고 위쪽 경계가 불규칙
	V = (np.mgrid[0:Size, 0:Size][0] + 0.5) / Size
	U = (np.mgrid[0:Size, 0:Size][1] + 0.5) / Size
	Line = 0.45 + (_Noise(Size, 4, 21) - 0.5) * 0.5
	Damp = np.clip((V - Line) / 0.18, 0.0, 1.0) * (0.55 + 0.45 * _Noise(Size, 8, 22))
	Damp *= np.clip(np.minimum(U, 1.0 - U) / 0.12, 0.0, 1.0)
	_SaveMask(os.path.join(Folder, "Damp.png"), Damp)


# ---- 파티클 / 스크립트 -------------------------------------------------------------------------------------------------
def WriteParticles():
	Folder = os.path.join(CONTENT, "Particles", "Demo")
	Base = {"Duration": 1.0, "Loop": True, "BurstCount": 0, "Shape": 0, "ShapeExtent": [10.0, 10.0, 10.0],
			"Direction": [0.0, 0.0, 1.0], "RandomRotation": True, "Texture": ""}
	Systems = {
		"AlleySteam": dict(SpawnRate=9.0, MaxParticles=64, ShapeRadius=12.0, LifetimeMin=3.0, LifetimeMax=4.2, ConeAngle=18.0,
						   SpeedMin=35.0, SpeedMax=70.0, RotationSpeedMin=-20.0, RotationSpeedMax=20.0, SizeStart=18.0, SizeEnd=150.0,
						   ColorStart=[0.7, 0.72, 0.76, 0.12], ColorEnd=[0.7, 0.72, 0.76, 0.0], Acceleration=[0.0, 0.0, 12.0],
						   Drag=0.4, BlendMode=0, Seed=31),
		"AlleyStoveSmoke": dict(SpawnRate=7.0, MaxParticles=64, ShapeRadius=18.0, LifetimeMin=4.0, LifetimeMax=5.5, ConeAngle=12.0,
								SpeedMin=55.0, SpeedMax=90.0, RotationSpeedMin=-25.0, RotationSpeedMax=25.0, SizeStart=30.0, SizeEnd=210.0,
								ColorStart=[0.22, 0.21, 0.2, 0.38], ColorEnd=[0.3, 0.3, 0.3, 0.0], Acceleration=[8.0, 0.0, 14.0],
								Drag=0.5, BlendMode=0, Seed=37),
		"AlleyStoveFire": dict(SpawnRate=45.0, MaxParticles=96, ShapeRadius=16.0, LifetimeMin=0.45, LifetimeMax=0.9, ConeAngle=22.0,
							   SpeedMin=45.0, SpeedMax=95.0, RotationSpeedMin=-120.0, RotationSpeedMax=120.0, SizeStart=26.0, SizeEnd=6.0,
							   ColorStart=[2.6, 1.05, 0.28, 1.0], ColorEnd=[1.2, 0.2, 0.03, 0.0], Acceleration=[0.0, 0.0, 60.0],
							   Drag=1.0, BlendMode=1, Seed=41),
	}
	for Name, Params in Systems.items():
		WriteJson(os.path.join(Folder, f"{Name}.eparticle"), {"Name": Name, **Base, **Params})


# ---- 씬 배치 --------------------------------------------------------------------------------------------------------
def WallYaw(Side):
	# 벽에 붙이는 모델(앞 = 모델 -X)의 Yaw: 왼쪽 벽(법선 +Y) -90, 오른쪽 벽(법선 -Y) +90
	return -90.0 if Side == "L" else 90.0


def BuildScene(Left, Right):
	Rng = random.Random(11)
	S = FScene()

	def Box(Name, Center, Size, Material, Collide=True, Parent=-1):
		Comps = {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": Material}}
		if Collide:
			Comps["BoxColliderComponent"] = {"HalfExtents": [50.0, 50.0, 50.0]}
		return S.Add(Name, Comps, Center, None, tuple(V / 100.0 for V in Size), Parent)  # 내장 큐브 = 100cm

	def Collider(Name, Center, Size):
		return S.Add(Name, {"BoxColliderComponent": {"HalfExtents": [Size[0] * 0.5, Size[1] * 0.5, Size[2] * 0.5]}}, Center)

	def Spot(Name, Pos, Pitch, Yaw, Color, Intensity, Radius, Inner, Outer, Shadows=False, Flicker=None):
		Comps = {"SpotLightComponent": {"Color": list(Color), "Intensity": Intensity, "Radius": Radius,
										"InnerConeAngle": Inner, "OuterConeAngle": Outer, "CastShadows": Shadows}}
		if Flicker:
			Comps["ScriptComponent"] = FlickerScript(Intensity, **Flicker)
		return S.Add(Name, Comps, Pos, QuatFromEuler(Pitch=Pitch, Yaw=Yaw))

	def Point(Name, Pos, Color, Intensity, Radius, Shadows=False, Flicker=None, Parent=-1):
		Comps = {"PointLightComponent": {"Color": list(Color), "Intensity": Intensity, "Radius": Radius, "CastShadows": Shadows}}
		if Flicker:
			Comps["ScriptComponent"] = FlickerScript(Intensity, **Flicker)
		return S.Add(Name, Comps, Pos, Parent=Parent)

	def Bulb(Name, Pos, Material, Size=6.0, Parent=-1):
		return S.Add(Name, {"StaticMeshComponent": {"MeshAsset": "primitive:sphere", "MaterialAsset": f"{MAT}/{Material}.emat"}},
					 Pos, None, (Size / 100.0, Size / 100.0, Size / 100.0), Parent)

	# ---- 환경: 밤 (22:30, 달빛) + 짙은 볼류메트릭 안개 + 약한 하늘광
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.96, 0.9], "Intensity": 3.2}}, (0, 0, 3000), QuatFromEuler(Pitch=-30, Yaw=40))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {"MoonIntensity": 0.45, "MoonColor": [0.55, 0.68, 1.0], "NightSkyColor": [0.01, 0.015, 0.032], "StarIntensity": 0.6},
		"TimeOfDayComponent": {"TimeOfDay": 22.5, "DayLengthMinutes": 0.0, "MaxSunElevation": 58.0, "NorthAzimuth": 200.0, "AnimateInEditor": False},
		"SkyLightComponent": {"Intensity": 1.5},
		"HeightFogComponent": {
			"Color": [0.018, 0.022, 0.032], "Density": 0.012, "HeightFalloff": 0.1, "StartDistance": 0.0, "MaxOpacity": 0.85,
			"DirectionalInscatteringColor": [0.0, 0.0, 0.0], "Volumetric": True, "VolumetricDistance": 5200.0,
			"VolumetricAlbedo": [0.9, 0.92, 0.95], "VolumetricExtinctionScale": 1.0, "VolumetricAnisotropy": 0.35,
			"VolumetricDirectionalScale": 0.3, "VolumetricLocalLightScale": 0.6},
	})

	# ---- 바닥: 젖은 아스팔트 (가운데 배수로 양쪽) + 배수로 콘크리트 + 배수로 물
	Gutter = 16.0
	for Side in (-1, 1):
		Y0, Y1 = Side * Gutter, Side * (HALF_WIDTH + ROOM_DEPTH)
		Box(f"Ground_{'L' if Side < 0 else 'R'}", (ALLEY_LENGTH * 0.5 - 50.0, (Y0 + Y1) * 0.5, -25.0), (ALLEY_LENGTH + 100.0, abs(Y1 - Y0), 50.0),
			f"{MAT}/AlleyAsphalt.emat")
	Box("Ground_Channel", (ALLEY_LENGTH * 0.5 - 50.0, 0.0, -31.0), (ALLEY_LENGTH + 100.0, Gutter * 2.0, 50.0), f"{MAT}/AlleyConcrete.emat")
	S.Add("Channel_Water", {"WaterBodyComponent": {
		"Size": [ALLEY_LENGTH + 100.0, Gutter * 2.0, 6.0], "ScatterColor": [0.01, 0.012, 0.014], "Absorption": [0.6, 0.45, 0.4],
		"NormalStrength": 0.25, "WaveScale": 120.0, "WaveSpeed": 6.0, "FlowDirection": 0.0, "FlowSpeed": 18.0,
		"FoamIntensity": 0.0, "FoamDistance": 1.0, "RefractionStrength": 0.02, "ReflectionIntensity": 0.3, "Roughness": 0.04}},
		(ALLEY_LENGTH * 0.5 - 50.0, 0.0, -5.0))
	# 교차로(울타리 너머) 바닥
	Box("Ground_Cross", (BACKDROP_X - 600.0, 0.0, -25.0), (1300.0, 3000.0, 50.0), f"{MAT}/AlleyAsphalt.emat", Collide=False)

	# ---- 건물: 조립 파사드 모델 (엔진 원점) + 파사드 뒤 실내 상자/층 바닥 + 벽 충돌
	S.Model("Architecture", f"{KIT_OUT}/AlleyArchitecture.gltf", (0, 0, 0))
	Interior = f"{MAT}/AlleyInterior.emat"
	Buildings = [("L1", "L", 0.0, 1800.0, L1["Floors"]), ("L2", "L", 1800.0, 3600.0, L2["Floors"]), ("R1", "R", 0.0, 3600.0, R1["Floors"])]
	for Name, Side, X0, X1, Floors in Buildings:
		Sign = -1.0 if Side == "L" else 1.0
		Height = Floors * FLOOR
		WallY = Sign * (HALF_WIDTH + ROOM_DEPTH + 10.0)
		Box(f"{Name}_Interior", ((X0 + X1) * 0.5, WallY, Height * 0.5), (X1 - X0, 20.0, Height), Interior, Collide=False)
		for Floor in range(1, Floors + 1):
			Box(f"{Name}_Slab_{Floor}", ((X0 + X1) * 0.5, Sign * (HALF_WIDTH + ROOM_DEPTH * 0.5), Floor * FLOOR - 15.0),
				(X1 - X0, ROOM_DEPTH + 20.0, 30.0), Interior, Collide=False)
		for X in (X0, X1):
			Box(f"{Name}_Partition", (X, Sign * (HALF_WIDTH + ROOM_DEPTH * 0.5), Height * 0.5), (20.0, ROOM_DEPTH, Height), Interior, Collide=False)
		Collider(f"{Name}_WallCollision", ((X0 + X1) * 0.5, Sign * (HALF_WIDTH + 20.0), Height * 0.5), (X1 - X0, 40.0, Height))
	# 높은 L1이 낮은 L2 위로 드러나는 옆벽 (교차로 쪽에서 비스듬히 보임)
	Box("L1_SideWall", (1810.0, -HALF_WIDTH - 150.0, (L2["Floors"] * FLOOR + L1["Floors"] * FLOOR + 75.0) * 0.5),
		(20.0, 300.0, (L1["Floors"] - L2["Floors"]) * FLOOR + 75.0), f"{MAT}/AlleyBrick.emat", Collide=False)
	# 배경 건물 실내
	Box("Back_Interior", (BACKDROP_X + ROOM_DEPTH + 10.0, 0.0, BACK["Floors"] * FLOOR * 0.5), (20.0, 1800.0, BACK["Floors"] * FLOOR), Interior, Collide=False)
	for Floor in range(1, BACK["Floors"] + 1):
		Box(f"Back_Slab_{Floor}", (BACKDROP_X + ROOM_DEPTH * 0.5, 0.0, Floor * FLOOR - 15.0), (ROOM_DEPTH + 20.0, 1800.0, 30.0), Interior, Collide=False)

	# 불 켜진 창: 창 뒤 발광 판 (따뜻한 색 위주, 몇 개는 푸른 TV 빛)
	def LitPanel(Name, Pos, Size, Index):
		Material = "AlleyWindowGlowCool" if Index % 4 == 3 else "AlleyWindowGlow"
		Box(Name, Pos, Size, f"{MAT}/{Material}.emat", Collide=False)
	for Index, (Col, Floor) in enumerate(LIT_WINDOWS["L"]):
		X, _, Z = LeftLocalToEngine(3.0 * Col + 1.5, Floor * 3.0 + 1.4)
		LitPanel(f"L_LitWindow_{Index}", (X, -HALF_WIDTH - 60.0, Z), (240.0, 4.0, 230.0), Index)
	for Index, (Col, Floor) in enumerate(LIT_WINDOWS["R"]):
		X, _, Z = RightLocalToEngine(3.0 * Col + 1.5, Floor * 3.0 + 1.4)
		LitPanel(f"R_LitWindow_{Index}", (X, HALF_WIDTH + 60.0, Z), (240.0, 4.0, 230.0), Index)
	for Index, (Col, Floor) in enumerate(LIT_WINDOWS["B"]):
		LitPanel(f"B_LitWindow_{Index}", (BACKDROP_X + 60.0, -900.0 + 300.0 * Col + 150.0, Floor * FLOOR + 140.0), (4.0, 240.0, 230.0), Index)

	# ---- 시작 쪽 벽돌 벽 + Hub 포털 (Hub와 같은 구성: 성문 + 랜턴 + 화분 + 빛 상자)
	Box("StartWall", (-60.0, 0.0, 450.0), (120.0, 2 * HALF_WIDTH + 2 * ROOM_DEPTH, 900.0), f"{MAT}/AlleyBrick.emat")
	Root = S.Add("Portal_Hub", {
		"ScriptComponent": {"ScriptAsset": "Scripts/Demo/DemoPortal.lua", "ExecutionLocation": 0,
			"PropertyOverrides": json.dumps({"TargetScene": {"Asset": "Scenes/Demo/Hub.escene"}, "Label": "Hub로", "Ready": True,
											 "RememberReturn": False}, ensure_ascii=False)}},
		(25.0, 0.0, 0.0), QuatFromEuler(Yaw=0.0))
	S.Model("Portal_Hub_Door", Model("large_castle_door"), (0, 0, -3), 0.0, 1.0, Parent=Root)
	S.Model("Portal_Hub_Planter", Model("planter_box_01"), (60, 0, 0), 90.0, 1.0, Parent=Root)
	for Side in (-1, 1):
		S.Model("Portal_Hub_Post", Model("tree_stump_01"), (40, Side * 150, -25), Rng.uniform(0, 360), 0.45, Parent=Root)
		S.Model("Portal_Hub_Lantern", Model("wooden_lantern_01"), (40, Side * 150, 8), Rng.uniform(0, 360), 1.3, Parent=Root)
	S.Add("Portal_Hub_Glow", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Demo/PortalGlow.emat"}},
		  (18, 0, 148), None, (0.02, 1.85, 2.85), Parent=Root)
	S.Add("Portal_Hub_Light", {"PointLightComponent": {"Color": [1.0, 0.7, 0.4], "Intensity": 3.0, "Radius": 500.0}}, (80, 0, 220), Parent=Root)

	# ---- 플레이어 (포털 앞에서 골목 안쪽 +X를 본다)
	PlayerIndex = len(S.Entities)
	S.Add("Player", {
		"PrefabInstanceComponent": {"Asset": "Prefabs/Demo/DemoPlayer.eprefab", "Overrides": ""},
		"PrefabLinkComponent": {"Id": "1", "Root": PlayerIndex}},
		(700.0, 60.0, 110.0), QuatFromEuler(Yaw=0.0))

	# ---- 끝: 철망(충돌) + 교차로 가로등 (안개 속 빛줄기, 철망 그림자)
	Collider("Fence_Collision", (ALLEY_LENGTH, 0.0, 130.0), (30.0, 2 * HALF_WIDTH, 260.0))
	S.Model("CrossLamp", Model("street_lamp_01"), (4150.0, -120.0, 0.0), 90.0, 1.0)
	Spot("CrossLamp_Light", (4150.0, -120.0, 372.0), -90.0, 0.0, (1.0, 0.45, 0.12), 90.0, 1600.0, 28.0, 58.0, Shadows=True)
	Bulb("CrossLamp_Bulb", (4150.0, -120.0, 376.0), "AlleyBulbSodium", 9.0)
	S.Model("Back_WallLamp", Model("industrial_wall_lamp"), (BACKDROP_X - 2.0, -420.0, 320.0), 0.0, 1.2)
	Spot("Back_WallLamp_Light", (BACKDROP_X - 30.0, -420.0, 315.0), -65.0, 180.0, (0.7, 0.85, 1.0), 25.0, 900.0, 25.0, 60.0)
	S.Model("Cross_Barrier", Model("concrete_road_barrier_02"), (3950.0, -150.0, 0.0), 8.0, 1.0)
	S.Model("Cross_Car", Model("covered_car"), (4350.0, -520.0, 0.0), 82.0, 1.0)

	# ---- 왼쪽 벽 (아파트): 비상계단, 벽등, 네온, 쓰레기통, 전기함
	S.Model("FireEscape", Model("modular_fire_escape"), (807.0, -HALF_WIDTH + 80.0, 617.0), WallYaw("L"), 1.0)
	S.Model("PullChainSocket", Model("pull_chain_light_socket"), (900.0, -HALF_WIDTH + 95.0, 318.0), 0.0, 1.4)
	Point("PullChain_Light", (900.0, -HALF_WIDTH + 95.0, 296.0), (1.0, 0.68, 0.36), 7.0, 550.0, Flicker={"Style": "Broken", "Seed": 3})
	Bulb("PullChain_Bulb", (900.0, -HALF_WIDTH + 95.0, 297.0), "AlleyBulbWarm", 6.0)
	for Name, X, Color, Bulb_, Shadows, Flicker, Power in (("WallLamp_L1", 450.0, (0.72, 0.85, 1.0), "AlleyBulbCool", True, None, 9.0),
													("WallLamp_L2", 2550.0, (1.0, 0.72, 0.42), "AlleyBulbWarm", False, {"Style": "Broken", "Seed": 7}, 16.0)):
		S.Model(Name, Model("industrial_wall_lamp"), (X, -HALF_WIDTH + 2.0, 330.0), WallYaw("L"), 1.2)
		Spot(f"{Name}_Light", (X, -HALF_WIDTH + 25.0, 325.0), -62.0, 90.0, Color, Power, 1000.0, 22.0, 55.0, Shadows=Shadows, Flicker=Flicker)
		Bulb(f"{Name}_Bulb", (X, -HALF_WIDTH + 14.0, 326.0), Bulb_, 7.0)
	# 네온 (분홍, 문 위 가로 관 두 줄)
	DoorX = LeftLocalToEngine(Left.Doors[1][0], 0)[0] if len(Left.Doors) > 1 else 1350.0
	for Row, Z in enumerate((292.0, 280.0)):
		S.Add(f"Neon_Pink_{Row}", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MAT}/AlleyNeonPink.emat"}},
			  (DoorX, -HALF_WIDTH + 12.0, Z), None, (1.5 - Row * 0.4, 0.03, 0.03))
	Point("Neon_Pink_Light", (DoorX, -HALF_WIDTH + 45.0, 270.0), (1.0, 0.12, 0.55), 9.0, 650.0, Flicker={"Style": "Neon", "Seed": 5})
	S.Model("Trash_L", Model("metal_trash_can"), (1650.0, -HALF_WIDTH + 55.0, 0.0), -90.0, 1.0)
	S.Model("Trashbag_L1", Model("trashbag"), (1530.0, -HALF_WIDTH + 50.0, 0.0), 30.0, 1.1)
	S.Model("Trashbag_L2", Model("trashbag"), (1490.0, -HALF_WIDTH + 85.0, 0.0), 170.0, 0.95)
	S.Model("Cardboard_L", Model("cardboard_box_01"), (1760.0, -HALF_WIDTH + 70.0, 0.0), 12.0, 1.3)
	S.Model("UtilityBox_L", Model("utility_box_02"), (2150.0, -HALF_WIDTH + 25.0, 0.0), WallYaw("L"), 1.0)
	S.Model("Aircon_L", Model("exterior_aircon_unit"), (2900.0, -HALF_WIDTH + 40.0, 32.0), WallYaw("L"), 1.0)
	S.Model("Crate_L", Model("plastic_crate_01"), (3300.0, -HALF_WIDTH + 60.0, 0.0), 75.0, 1.0)
	S.Model("Crate_L2", Model("plastic_crate_01"), (3305.0, -HALF_WIDTH + 58.0, 26.0), 70.0, 1.0)
	S.Model("Tyre_L", Model("old_tyre"), (3450.0, -HALF_WIDTH + 30.0, 30.0), WallYaw("L") + 12.0, 1.0)
	Collider("Trash_L_Collision", (1650.0, -HALF_WIDTH + 55.0, 45.0), (70.0, 190.0, 90.0))

	# ---- 오른쪽 벽 (공장): 보안등, 가로등 팔, 네온, 차, 화로, 바리케이드
	SecX = RightLocalToEngine(R1_DOOR_X(Right, "door_c_l"), 0)[0]
	S.Model("SecurityLight", Model("security_light"), (SecX, HALF_WIDTH - 2.0, 300.0), WallYaw("R"), 1.2)
	Spot("SecurityLight_Light", (SecX, HALF_WIDTH - 40.0, 290.0), -55.0, -90.0, (0.85, 0.92, 1.0), 40.0, 1100.0, 25.0, 55.0, Shadows=True)
	Bulb("SecurityLight_Bulb", (SecX, HALF_WIDTH - 30.0, 296.0), "AlleyBulbCool", 8.0)
	S.Model("StreetLampArm_R", Model("street_lamp_02"), (2250.0, HALF_WIDTH - 2.0, 360.0), WallYaw("R"), 1.0)
	Spot("StreetLampArm_R_Light", (2250.0, HALF_WIDTH - 72.0, 342.0), -90.0, 0.0, (1.0, 0.6, 0.28), 34.0, 1000.0, 30.0, 62.0, Shadows=True)
	Bulb("StreetLampArm_R_Bulb", (2250.0, HALF_WIDTH - 72.0, 346.0), "AlleyBulbSodium", 7.0)
	NeonX = 1500.0
	S.Add("Neon_Cyan", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MAT}/AlleyNeonCyan.emat"}},
		  (NeonX, HALF_WIDTH - 10.0, 250.0), None, (0.025, 0.025, 1.3))
	S.Add("Neon_Cyan_Top", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MAT}/AlleyNeonCyan.emat"}},
		  (NeonX + 30.0, HALF_WIDTH - 10.0, 314.0), None, (0.6, 0.025, 0.025))
	Point("Neon_Cyan_Light", (NeonX, HALF_WIDTH - 45.0, 260.0), (0.1, 0.75, 1.0), 9.0, 650.0)
	GarageX = RightLocalToEngine(R1_DOOR_X(Right, "garage"), 0)[0]
	S.Model("Car_R", Model("covered_car"), (GarageX + 40.0, HALF_WIDTH - 115.0, 0.0), 0.0, 1.0)
	Collider("Car_R_Collision", (GarageX + 40.0, HALF_WIDTH - 115.0, 70.0), (440.0, 185.0, 140.0))
	StoveX, StoveY = 3020.0, HALF_WIDTH - 110.0
	S.Model("BarrelStove", Model("barrel_stove"), (StoveX, StoveY, 0.0), 30.0, 1.0)
	Collider("BarrelStove_Collision", (StoveX, StoveY, 43.0), (60.0, 60.0, 86.0))
	S.Add("BarrelStove_Fire", {"ParticleSystemComponent": {"Asset": "Particles/Demo/AlleyStoveFire.eparticle", "Playing": True, "Speed": 1.0}},
		  (StoveX, StoveY, 80.0))
	S.Add("BarrelStove_Smoke", {"ParticleSystemComponent": {"Asset": "Particles/Demo/AlleyStoveSmoke.eparticle", "Playing": True, "Speed": 1.0}},
		  (StoveX, StoveY, 110.0))
	Point("BarrelStove_Light", (StoveX, StoveY, 120.0), (1.0, 0.45, 0.14), 16.0, 700.0, Flicker={"Style": "Fire", "Seed": 9})
	S.Model("Tyre_R1", Model("old_tyre"), (3220.0, HALF_WIDTH - 40.0, 30.0), WallYaw("R") - 8.0, 1.0)
	S.Model("Tyre_R2", Model("old_tyre"), (3290.0, HALF_WIDTH - 45.0, 30.0), WallYaw("R") + 15.0, 1.0)
	S.Model("Barrier_R", Model("concrete_road_barrier_02"), (3450.0, 150.0, 0.0), 12.0, 1.0)
	Collider("Barrier_R_Collision", (3450.0, 150.0, 55.0), (50.0, 160.0, 110.0))
	S.Model("UtilityBox_R", Model("utility_box_01"), (650.0, HALF_WIDTH - 25.0, 0.0), WallYaw("R"), 1.0)
	S.Model("SecurityCamera", Model("security_camera_01"), (1150.0, HALF_WIDTH - 5.0, 420.0), WallYaw("R"), 1.0)
	S.Model("Manhole", Model("water_manhole_cover"), (2600.0, -120.0, 0.0), 20.0, 1.0)
	# 공장 벽 환풍구 증기 (로컬 x 4.5m, 높이 1.6m)
	VentX, _, VentZ = RightLocalToEngine(4.5, 1.6 + 0.2)
	S.Add("Vent_Steam", {"ParticleSystemComponent": {"Asset": "Particles/Demo/AlleySteam.eparticle", "Playing": True, "Speed": 1.0}},
		  (VentX, HALF_WIDTH - 45.0, VentZ), QuatFromEuler(Pitch=-50.0, Yaw=-90.0))
	# 맨홀 증기
	S.Add("Manhole_Steam", {"ParticleSystemComponent": {"Asset": "Particles/Demo/AlleySteam.eparticle", "Playing": True, "Speed": 0.8}},
		  (2600.0, -120.0, 6.0))

	# ---- 데칼: 웅덩이(매끈·어두움), 기름 얼룩, 벽 밑동 습기
	Puddles = [(700, 140, 260, 180, "A"), (1250, -170, 300, 200, "B"), (1950, 120, 240, 170, "A"), (2400, -60, 360, 230, "B"),
			   (2800, 190, 220, 150, "A"), (3250, -150, 280, 190, "B"), (520, -200, 200, 140, "B"), (1600, 60, 180, 120, "A")]
	for Index, (X, Y, SX, SY, Kind) in enumerate(Puddles):
		S.Add(f"Decal_Puddle_{Index}", {"DecalComponent": {
			"MaterialAsset": f"{MAT}/DecalPuddle{Kind}.emat", "Size": [float(SX), float(SY), 40.0], "Opacity": 1.0, "SortOrder": 1,
			"AffectBaseColor": True, "AffectNormal": True, "AffectRoughness": True, "FadeStartDistance": 0.0, "FadeEndDistance": 0.0}},
			(X, Y, 0.0), QuatFromEuler(Yaw=Rng.uniform(0, 360)))
	for Index, (X, Y) in enumerate(((2050.0, 210.0), (3050.0, 60.0))):
		S.Add(f"Decal_Oil_{Index}", {"DecalComponent": {
			"MaterialAsset": f"{MAT}/DecalOil.emat", "Size": [140.0, 90.0, 40.0], "Opacity": 0.8, "SortOrder": 0,
			"AffectBaseColor": True, "AffectNormal": True, "AffectRoughness": True, "FadeStartDistance": 0.0, "FadeEndDistance": 0.0}},
			(X, Y, 0.0), QuatFromEuler(Yaw=Rng.uniform(0, 360)))
	for Index in range(8):
		Side = "L" if Index % 2 == 0 else "R"
		X = 250.0 + Index * 420.0 + Rng.uniform(-60, 60)
		Y = -HALF_WIDTH if Side == "L" else HALF_WIDTH
		# 데칼 축(DecalMath.h): 로컬 +Z = 표면 바깥 법선(벽 법선 ±Y), 텍스처 위(V = 0) = 로컬 +X → 월드 위(+Z)
		Normal = (0.0, 1.0, 0.0) if Side == "L" else (0.0, -1.0, 0.0)
		S.Add(f"Decal_Damp_{Index}", {"DecalComponent": {
			"MaterialAsset": f"{MAT}/DecalDamp.emat", "Size": [230.0, Rng.uniform(220, 380), 60.0], "Opacity": 1.0, "SortOrder": 0,
			"AffectBaseColor": True, "AffectNormal": False, "AffectRoughness": True, "FadeStartDistance": 0.0, "FadeEndDistance": 0.0}},
			(X, Y, 115.0), QuatFromAxes((0.0, 0.0, 1.0), Normal))
	return S


def QuatFromAxes(LocalX, LocalZ):
	# 로컬 X/Z 축이 갈 월드 방향 → 쿼터니언 [X, Y, Z, W] (해밀턴 q v q*, FQuat과 같은 성분 규약). Y = Z × X
	X, Z = np.array(LocalX, dtype=float), np.array(LocalZ, dtype=float)
	Y = np.cross(Z, X)
	M = np.column_stack([X, Y, Z])
	W = math.sqrt(max(0.0, 1.0 + M[0, 0] + M[1, 1] + M[2, 2])) * 0.5
	if W > 1e-4:
		return [(M[2, 1] - M[1, 2]) / (4 * W), (M[0, 2] - M[2, 0]) / (4 * W), (M[1, 0] - M[0, 1]) / (4 * W), W]
	I = int(np.argmax([M[0, 0], M[1, 1], M[2, 2]]))
	J, K = (I + 1) % 3, (I + 2) % 3
	R = math.sqrt(max(0.0, 1.0 + M[I, I] - M[J, J] - M[K, K]))
	Q = [0.0, 0.0, 0.0, (M[K, J] - M[J, K]) / (2 * R)]
	Q[I], Q[J], Q[K] = R * 0.5, (M[J, I] + M[I, J]) / (2 * R), (M[K, I] + M[I, K]) / (2 * R)
	return Q


def R1_DOOR_X(Facade, Type):
	for X, Kind in Facade.Doors:
		if Kind == Type:
			return X
	raise KeyError(Type)


def FlickerScript(Intensity, Style="Fire", Seed=1):
	return {"ScriptAsset": "Scripts/Demo/FlickerLight.lua", "ExecutionLocation": 2,
			"PropertyOverrides": json.dumps({"Style": Style, "BaseIntensity": Intensity, "Seed": Seed}, ensure_ascii=False)}


def Main():
	Count, Left, Right, _ = BuildArchitecture()
	WriteMaterials()
	WriteDecalTextures()
	WriteParticles()
	Scene = BuildScene(Left, Right)
	Scene.Save(os.path.join(CONTENT, "Scenes", "Demo", "Alley.escene"))
	# 확인용 고정 카메라 변형 (커밋하지 않음): python BuildAlley.py --camera X,Y,Z,Pitch,Yaw[,Fov] → Scenes/Demo/_AlleyCamera.escene
	if len(sys.argv) > 2 and sys.argv[1] == "--camera":
		X, Y, Z, Pitch, Yaw, *Rest = [float(V) for V in sys.argv[2].split(",")]
		Scene.Add("_VerifyCamera", {"CameraComponent": {"FovYDegrees": Rest[0] if Rest else 70.0, "NearZ": 5.0, "FarZ": 200000.0, "Primary": True,
															   "Priority": 100}}, (X, Y, Z), QuatFromEuler(Pitch=Pitch, Yaw=Yaw))
		Scene.Save(os.path.join(CONTENT, "Scenes", "Demo", "_AlleyCamera.escene"))
	print(f"Alley 생성: 엔티티 {len(Scene.Entities)}개, 조립 조각 {Count}개")


if __name__ == "__main__":
	Main()
