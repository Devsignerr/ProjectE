# 엔진 비교 벤치 씬 "BenchSquare"(해 질 녘 도시 광장) 생성: 배치 설명 하나 → ProjectE 씬(.escene) + Godot 씬(.tscn)을 함께 쓴다.
#   실행: python Tools/DemoMap/BuildBenchSquare.py [--godot <Godot 프로젝트 폴더>] [--no-godot]
#   목적: 두 엔진에 "같은 장면, 같은 부하"를 주기 위해 두 엔진이 같은 의미로 지원하는 기능만 쓴다 —
#         glTF 모델(정적 PBR + 스킨 애니메이션), 방향광 CSM, 점광원/스포트라이트(일부 그림자), 절차적 하늘 환경광.
#         지형·물·대기·구름·면광원·데칼·파티클·그래프 머티리얼·DDGI/RT는 쓰지 않는다(Godot에 같은 기능이 없거나 식이 다름).
#   좌표: 엔진 = 왼손 Z-up cm, Godot = glTF 축(+Y 위, m). 엔진 (X, Y, Z) = (-gz, gx, gy) × 100 (FGltfLoader::ConvertPosition)
#         → Godot g = (Y, Z, -X) / 100. 엔진 Yaw θ = Godot Y축 회전 -θ (GltfKit.EngineToGltf와 같은 규약)
#   움직임: 캐릭터 = 엔진 StressWalkerComponent(SampleGame) ↔ Godot bench_square.gd (같은 식), 카메라 = 같은 궤도 식
#           (Scripts/Bench/BenchCamera.lua ↔ bench_square.gd). 시간은 고정 dt(--fixed-delta 60 / --fixed-fps 60)로 같은 화면 순서.
#   측정 스크립트: Scripts/EngineCompare.ps1. 결정적(고정 시드).
import argparse
import json
import math
import os
import random
import shutil
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import ModelBounds  # noqa: E402
from BuildAlley import APT, FAC, BuildApartments, BuildFactory, FFacade  # noqa: E402
from GltfKit import EngineToGltf, FGltfKitComposer  # noqa: E402
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTENT = os.path.join(ROOT, "Projects", "Sample", "Content")
PH      = "Asset/PolyHaven"
KIT_OUT = "Asset/DemoKits/Bench"
SCENE   = "Scenes/Bench/BenchSquare.escene"
CAMERA_SCRIPT = "Scripts/Bench/BenchCamera.lua"
DEFAULT_GODOT = r"E:\Godot\TestProject\test-project"

HALF       = 2400.0  # 광장 반 크기 (cm) — 네 변 파사드 면
COLS       = 16      # 변마다 3m 칸 수 (가운데 2칸은 골목 입구)
UNIT_COUNT = 256
CHARACTERS = ["Knight", "Barbarian", "Mage", "Rogue", "SkeletonMinion", "SkeletonWarrior", "SkeletonRogue", "SkeletonMage"]
WALK_SPEED, RUN_SPEED = 160.0, 345.0  # cm/s (KayKit Walking_A / Running_A 발 속도 — BuildStress와 같음)

# 빛 (엔진 단위). Godot 에너지 = 엔진 강도 × 아래 배율 — 화면 밝기를 맞추기 위한 값(부하에는 영향 없음)
SUN       = dict(Color=[1.0, 0.78, 0.55], Intensity=1.6, Pitch=-20.0, Yaw=-35.0)
SKY_LIGHT = 0.7
LAMP      = dict(Color=[1.0, 0.78, 0.5], Intensity=14.0, Radius=1000.0)
SPOT      = dict(Color=[0.85, 0.9, 1.0], Intensity=45.0, Radius=2600.0, Inner=28.0, Outer=40.0)
LANTERN   = dict(Color=[1.0, 0.62, 0.3], Intensity=3.0, Radius=450.0)
GODOT_DIRECTIONAL_SCALE = 1.0 / math.pi
GODOT_LOCAL_SCALE       = 1.0 / math.pi
# CSM: 엔진 FShadowSettings 기본값(4장, 2048, 60m, λ 0.75)을 Godot PSSM 4 분할로 옮긴다 (분할 식 = ShadowMath::ComputeCascadeSplits)
CAMERA_NEAR, SHADOW_DISTANCE, SPLIT_LAMBDA = 10.0, 6000.0, 0.75


def Model(Id):
	return f"{PH}/{Id}/{Id}.gltf"


# ---- 생성 glTF (바닥 타일, 실내 판) ----------------------------------------------------------------------------------
def WriteQuadGltf(Path, Positions, Normal, Uvs, Material):
	# 사각형 하나(삼각형 2개, glTF CCW 앞면). 탄젠트는 두 엔진이 각자 만든다 (엔진 ComputeTangents / Godot ensure_tangents)
	Indices = [0, 1, 2, 0, 2, 3]
	Blob = b"".join(struct.pack("<3f", *P) for P in Positions)
	Blob += b"".join(struct.pack("<3f", *Normal) for _ in Positions)
	Blob += b"".join(struct.pack("<2f", *T) for T in Uvs)
	Blob += struct.pack("<6H", *Indices) + b"\0\0"
	Lo = [min(P[A] for P in Positions) for A in range(3)]
	Hi = [max(P[A] for P in Positions) for A in range(3)]
	BinName = os.path.splitext(os.path.basename(Path))[0] + ".bin"
	Doc = {
		"asset": {"version": "2.0", "generator": "ProjectE Tools/DemoMap/BuildBenchSquare.py"},
		"scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": os.path.splitext(os.path.basename(Path))[0], "mesh": 0}],
		"meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
		"buffers": [{"uri": BinName, "byteLength": len(Blob)}],
		"bufferViews": [
			{"buffer": 0, "byteOffset": 0, "byteLength": 48, "target": 34962},
			{"buffer": 0, "byteOffset": 48, "byteLength": 48, "target": 34962},
			{"buffer": 0, "byteOffset": 96, "byteLength": 32, "target": 34962},
			{"buffer": 0, "byteOffset": 128, "byteLength": 12, "target": 34963},
		],
		"accessors": [
			{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": Lo, "max": Hi},
			{"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
			{"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
			{"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
		],
		"materials": [Material["Material"]],
	}
	if Material.get("Images"):
		Doc["images"] = [{"uri": Uri} for Uri in Material["Images"]]
		Doc["textures"] = [{"source": Index, "sampler": 0} for Index in range(len(Material["Images"]))]
		Doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(os.path.join(os.path.dirname(Path), BinName), "wb") as File:
		File.write(Blob)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=1)
		File.write("\n")


def WriteGeneratedModels():
	Tex = "../../PolyHaven/cobblestone_floor_04/cobblestone_floor_04"
	# 바닥: 6m 타일, UV 0~2 (텍스처 3m 반복). Poly Haven arm = AO(R)/거칠기(G)/금속(B) = glTF 금속거칠기 + 오클루전 그대로
	WriteQuadGltf(os.path.join(CONTENT, KIT_OUT, "GroundTile.gltf"),
		[(-3, 0, -3), (-3, 0, 3), (3, 0, 3), (3, 0, -3)], (0, 1, 0), [(0, 0), (0, 2), (2, 2), (2, 0)],
		{"Images": [f"{Tex}_diff_2k.jpg", f"{Tex}_arm_2k.jpg", f"{Tex}_nor_gl_2k.jpg"],
		 "Material": {"name": "BenchCobblestone", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicRoughnessTexture": {"index": 1},
			"metallicFactor": 1.0, "roughnessFactor": 1.0}, "normalTexture": {"index": 2}, "occlusionTexture": {"index": 1}}})
	# 실내 판: 1m 사각형(로컬 x 0~1, y 0~1, 앞 +z) — 파사드 1.5m 뒤에 두어 유리창 너머가 비지 않게 (엔티티 스케일로 크기)
	WriteQuadGltf(os.path.join(CONTENT, KIT_OUT, "InteriorWall.gltf"),
		[(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)], (0, 0, 1), [(0, 1), (1, 1), (1, 0), (0, 0)],
		{"Material": {"name": "BenchInterior", "pbrMetallicRoughness": {"baseColorFactor": [0.05, 0.045, 0.04, 1.0], "metallicFactor": 0.0,
			"roughnessFactor": 0.9}}})


# ---- 건물 (모듈 파사드 조립 glTF) ------------------------------------------------------------------------------------
APT_GROUND_A = ["std", "door_s", "std", "std", "door_l", "std", "door_off"]
APT_GROUND_B = ["door_off", "std", "std", "door_s", "std", "std", "std"]
APT_UPPER    = ["centered_small", "centered_large", "centered_double", "centered_large", "centered_double", "centered_small", "offset_small"]
FAC_GROUND_A = ["door_c_l", "std", "door_c_s", "std", "garage", "std"]
FAC_GROUND_B = ["std", "door_rec_l", "std", "std", "door_c_s", "std", "std"]
FAC_UPPER    = ["centered_medium", "centered_double", "centered_large", "centered_large", "centered_small", "centered_medium", "centered_double"]
# 변: (파사드 시작 엔진 위치, Yaw — 띠는 로컬 +x로 이어지고 앞 +z가 광장 안쪽), 건물 A(칸 0~6)/B(칸 9~15) = (키트, 층수, 1층 구성)
SIDES = [
	((-HALF, -HALF), -90.0, ("APT", 4, APT_GROUND_A), ("APT", 3, APT_GROUND_B)),  # 남쪽: +X로, 앞 +Y
	((HALF, -HALF), 0.0, ("FAC", 3, FAC_GROUND_A), ("FAC", 4, FAC_GROUND_B)),     # 동쪽: +Y로, 앞 -X
	((HALF, HALF), 90.0, ("APT", 3, APT_GROUND_B), ("FAC", 4, FAC_GROUND_A)),     # 북쪽: -X로, 앞 -Y
	((-HALF, HALF), 180.0, ("FAC", 4, FAC_GROUND_B), ("APT", 4, APT_GROUND_A)),   # 서쪽: -Y로, 앞 +X
]


def FacadeToEngine(Base, X, Y, Z):
	# 파사드 로컬(m, glTF) → 엔진 cm
	G = Base @ np.array([X, Y, Z, 1.0])
	return (-G[2] * 100.0, G[0] * 100.0, G[1] * 100.0)


def BuildArchitecture():
	K = FGltfKitComposer(os.path.join(CONTENT, PH), os.path.join(CONTENT, KIT_OUT, "BenchSquareArchitecture.gltf"))
	Backings = []  # (엔진 위치, Yaw, 폭 m, 높이 m)
	for (SX, SY), Yaw, *Buildings in SIDES:
		Base = EngineToGltf(SX, SY, 0.0, Yaw)
		Facade = FFacade(None, Base)
		for StartCol, (Kit, Floors, Ground) in zip((0, 9), Buildings):
			Cols = sum(2 if G == "garage" else 1 for G in Ground)
			if Kit == "APT":
				BuildApartments(K, Facade, StartCol=StartCol, Ground=Ground, Upper=APT_UPPER[:Cols], Floors=Floors)
			else:
				BuildFactory(K, Facade, StartCol=StartCol, Ground=Ground, Upper=FAC_UPPER[:Cols], Floors=Floors)
			Backings.append((FacadeToEngine(Base, 3.0 * StartCol, 0.0, -1.5), Yaw, 3.0 * Cols, 3.0 * Floors + 0.6))
	return K.Save(), Backings


# ---- 배치 ------------------------------------------------------------------------------------------------------------
class FBounds:
	# 모델 경계(엔진 cm, 스케일 1) 캐시 — 위에 올려놓기·바닥 맞추기
	def __init__(self):
		ModelBounds._CONVERT = lambda P: (-P[2] * 100.0, P[0] * 100.0, P[1] * 100.0)
		self.Cache = {}

	def Get(self, Asset):
		if Asset not in self.Cache:
			self.Cache[Asset] = ModelBounds.ComputeBounds(os.path.join(CONTENT, *Asset.split("/")))
		return self.Cache[Asset]

	def Top(self, Asset, Scale=1.0):
		return self.Get(Asset)[1][2] * Scale

	def Bottom(self, Asset, Scale=1.0):
		return self.Get(Asset)[0][2] * Scale


class FPlacement:
	# 두 엔진 공용 배치 목록 (엔진 좌표)
	def __init__(self):
		self.Models = []   # dict(Name, Asset, Pos, Yaw, Scale(3), Extra)
		self.Lights = []   # dict(Name, Type, Pos, Pitch, Yaw, Params)
		self.Bounds = FBounds()

	def Add(self, Name, Asset, Pos, Yaw=0.0, Scale=1.0, Walker=None, Anim=None, Ground=True):
		S = tuple(Scale) if isinstance(Scale, (list, tuple)) else (Scale, Scale, Scale)
		Z = Pos[2] - (self.Bounds.Bottom(Asset, S[2]) if Ground else 0.0)  # 모델 바닥을 Pos 높이에
		self.Models.append(dict(Name=Name, Asset=Asset, Pos=(Pos[0], Pos[1], Z), Yaw=Yaw, Scale=S, Walker=Walker, Anim=Anim))
		return Z + self.Bounds.Top(Asset, S[2])  # 윗면 높이

	def Light(self, Name, Type, Pos, Params, Pitch=0.0, Yaw=0.0):
		self.Lights.append(dict(Name=Name, Type=Type, Pos=Pos, Pitch=Pitch, Yaw=Yaw, Params=Params))


def SideFrame(Side):
	# 변 기준 좌표계: 원점 = 광장 가운데, (u = 변을 따라, v = 가운데 → 벽 방향 거리) → 엔진 XY. 안쪽을 보는 Yaw
	Angle = [-90.0, 0.0, 90.0, 180.0][Side]  # 남/동/북/서 벽 방향 (엔진 Yaw 기준 +X에서)
	R = math.radians(Angle)
	Out = (math.cos(R), math.sin(R))   # 벽 방향 (가운데 → 벽)
	Along = (-Out[1], Out[0])
	def ToEngine(U, V, Z=0.0):
		return (Along[0] * U + Out[0] * V, Along[1] * U + Out[1] * V, Z)
	return ToEngine, Angle + 180.0  # 벽에서 안쪽을 보는 Yaw


SMALL_GOODS = ["ceramic_vase_01", "ceramic_vase_02", "ceramic_vase_03", "ceramic_vase_04", "brass_pot_01", "brass_vase_02", "brass_vase_03",
			   "jug_01", "wicker_basket_01", "wicker_basket_02", "brass_goblets", "CheeseBox_01", "pot_enamel_01", "antique_ceramic_vase_01",
			   "wooden_bucket_01"]
STALL_SIDE   = ["Barrel_01", "Barrel_02", "barrel_03", "wine_barrel_01", "wooden_crate_01", "wooden_crate_02", "plastic_crate_01",
				"cardboard_box_01", "cement_bag", "wooden_stool_01", "folding_wooden_stool"]
WALL_CLUTTER = ["Barrel_01", "barrel_03", "wooden_crate_02", "cardboard_box_01", "trashbag", "metal_trash_can", "propane_tank", "old_tyre",
				"wooden_barrels_01", "cement_bag", "plastic_crate_01", "utility_box_01", "utility_box_02"]
CRATES       = ["wooden_crate_01", "wooden_crate_02", "cardboard_box_01", "plastic_crate_01"]


def BuildPlacement():
	Rng = random.Random(2026)
	P = FPlacement()
	# 바닥: 6m 타일 14 × 14 (84m — 골목 입구 너머까지)
	for IX in range(14):
		for IY in range(14):
			P.Add(f"Ground_{IX:02d}_{IY:02d}", f"{KIT_OUT}/GroundTile.gltf", (-3900 + IX * 600, -3900 + IY * 600, 0.0), Ground=False)
	P.Add("Architecture", f"{KIT_OUT}/BenchSquareArchitecture.gltf", (0, 0, 0), Ground=False)

	# 가운데 분수 받침 + 고래 조각 + 둘레 화분/벤치
	Top = P.Add("Fountain", Model("stone_fire_pit"), (0, 0, 0), 0.0, 3.2)
	P.Add("FountainStatue", Model("bronze_whale_statue"), (0, 0, Top - 25.0), 30.0, 2.4)
	for Index in range(8):
		A = math.radians(Index * 45.0 + 22.5)
		R = 420.0
		Pos = (math.cos(A) * R, math.sin(A) * R, 0.0)
		if Index % 2 == 0:
			PlanterTop = P.Add(f"Planter_{Index}", Model("planter_box_01"), Pos, math.degrees(A) + 90.0)
			for K in (-1, 1):
				Off = (math.cos(A + math.pi / 2) * 25.0 * K, math.sin(A + math.pi / 2) * 25.0 * K)
				P.Add(f"Planter_{Index}_Plant{K}", Model("potted_plant_04"), (Pos[0] + Off[0], Pos[1] + Off[1], PlanterTop - 20.0), Rng.uniform(0, 360))
		else:
			P.Add(f"Bench_{Index}", Model("painted_wooden_bench"), Pos, math.degrees(A) + 90.0)

	# 캐릭터: 반지름 550~1450 고리, 고리마다 걷기/달리기·방향 교대 (BuildStress 규칙)
	Placed, Ring = 0, 0
	while Placed < UNIT_COUNT:
		Radius = 550.0 + Ring * 115.0
		Count = min(int(2 * math.pi * Radius / 200.0), UNIT_COUNT - Placed)
		bRun = Ring % 2 == 1
		Direction = 1.0 if Ring % 4 < 2 else -1.0
		Phase = Rng.uniform(0, 2 * math.pi)
		for Index in range(Count):
			Angle = Phase + 2 * math.pi * Index / Count
			Character = CHARACTERS[(Ring * 3 + Index) % len(CHARACTERS)]
			P.Add(f"Unit_{Placed:03d}", f"Asset/KayKit/Characters/{Character}.glb", (math.cos(Angle) * Radius, math.sin(Angle) * Radius, 0.0),
				0.0, 1.0, Ground=False,
				Walker=dict(Speed=(RUN_SPEED if bRun else WALK_SPEED) * Direction, YawOffset=180.0),
				Anim=dict(Clip="Running_A" if bRun else "Walking_A", Speed=round(Rng.uniform(0.95, 1.05), 4)))
			Placed += 1
		Ring += 1

	# 가로등 16개 (반지름 1650) + 점광원 (4개 그림자)
	Lamp = Model("street_lamp_01")
	for Index in range(16):
		A = math.radians(Index * 22.5 + 11.25)
		Pos = (math.cos(A) * 1650.0, math.sin(A) * 1650.0, 0.0)
		Top = P.Add(f"Lamp_{Index:02d}", Lamp, Pos, math.degrees(A))
		P.Light(f"LampLight_{Index:02d}", "Point", (Pos[0], Pos[1], Top - 45.0), dict(LAMP, CastShadows=Index % 4 == 0))

	for Side in range(4):
		ToEngine, InYaw = SideFrame(Side)
		# 벽 스포트 2개 (그림자): 파사드 높이 6m에서 광장 안쪽 아래로
		for U in (-1200.0, 1200.0):
			Pos = ToEngine(U, HALF - 40.0, 600.0)
			P.Add(f"Floodlight_{Side}_{int(U)}", Model("security_light"), Pos, InYaw, 1.6, Ground=False)
			P.Light(f"Spot_{Side}_{int(U)}", "Spot", ToEngine(U, HALF - 70.0, 590.0), dict(SPOT, CastShadows=True), Pitch=-42.0, Yaw=InYaw)

		# 노점 4개: 탁자 + 위 물건 12개 + 옆 상자/통 6개 + 랜턴(점광원, 그림자 없음)
		for StallIndex, U in enumerate((-1750.0, -950.0, 950.0, 1750.0)):
			Base = ToEngine(U, 1950.0)
			Name = f"Stall_{Side}_{StallIndex}"
			Table = Model("wooden_picnic_table")
			TableTop = P.Add(Name, Table, Base, InYaw + 90.0)
			for Item in range(12):
				LU, LV = Rng.uniform(-110.0, 110.0), Rng.uniform(-30.0, 30.0)  # 탁자 긴 방향(변을 따라) / 짧은 방향
				Pos = ToEngine(U + LU, 1950.0 + LV, TableTop - 4.0)
				P.Add(f"{Name}_Good{Item:02d}", Model(Rng.choice(SMALL_GOODS)), Pos, Rng.uniform(0, 360))
			LanternPos = ToEngine(U, 1950.0, TableTop - 4.0)
			LanternTop = P.Add(f"{Name}_Lantern", Model("wooden_lantern_01"), LanternPos, InYaw)
			P.Light(f"{Name}_LanternLight", "Point", (LanternPos[0], LanternPos[1], LanternTop + 15.0), dict(LANTERN, CastShadows=False))
			for Item in range(6):
				LU = (-1 if Item < 3 else 1) * Rng.uniform(170.0, 230.0)
				LV = Rng.uniform(-90.0, 90.0)
				Asset = Model(Rng.choice(STALL_SIDE))
				Top = P.Add(f"{Name}_Side{Item}", Asset, ToEngine(U + LU, 1950.0 + LV), Rng.uniform(0, 360))
				if "crate" in Asset or "box" in Asset:
					P.Add(f"{Name}_Side{Item}_Top", Model(Rng.choice(SMALL_GOODS)), ToEngine(U + LU, 1950.0 + LV, Top - 2.0), Rng.uniform(0, 360))

		# 벽 앞 잡동사니 (골목 입구·문 앞 제외 없이 55cm 간격, 건물 칸 범위만)
		Index = 0
		U = -2250.0
		while U < 2250.0:
			if abs(U) > 500.0:
				Asset = Model(Rng.choice(WALL_CLUTTER))
				V = HALF - (45.0 if "utility_box" in Asset else Rng.uniform(70.0, 120.0))
				P.Add(f"Clutter_{Side}_{Index:03d}", Asset, ToEngine(U, V), InYaw + Rng.uniform(-25, 25))
				Index += 1
			U += 55.0

		# 골목 입구 바깥: 덮개 씌운 차 2대 + 도로 방벽 3개
		for K, U in enumerate((-160.0, 170.0)):
			P.Add(f"Car_{Side}_{K}", Model("covered_car"), ToEngine(U, HALF + 900.0 + K * 700.0), InYaw + 90.0 + Rng.uniform(-6, 6))
		for K in range(3):
			P.Add(f"Barrier_{Side}_{K}", Model("concrete_road_barrier_02"), ToEngine(-200.0 + K * 200.0, HALF + 250.0), InYaw)

	# 모퉁이 4곳 상자 야적장: 5 × 5 격자, 일부 2단 + 위 물건
	for CX in (-1, 1):
		for CY in (-1, 1):
			for IX in range(5):
				for IY in range(5):
					X = CX * (1750.0 + IX * 120.0)
					Y = CY * (1750.0 + IY * 120.0)
					Name = f"Yard_{'E' if CX > 0 else 'W'}{'N' if CY > 0 else 'S'}_{IX}{IY}"
					Asset = Model(Rng.choice(CRATES))
					Top = P.Add(Name, Asset, (X, Y, 0.0), Rng.choice((0, 90, 180, 270)) + Rng.uniform(-5, 5))
					if Rng.random() < 0.5:
						Asset2 = Model(Rng.choice(CRATES))
						Top = P.Add(f"{Name}_B", Asset2, (X, Y, Top - 1.0), Rng.choice((0, 90, 180, 270)) + Rng.uniform(-5, 5))
					if Rng.random() < 0.6:
						P.Add(f"{Name}_Top", Model(Rng.choice(SMALL_GOODS)), (X, Y, Top - 2.0), Rng.uniform(0, 360))
	return P


# ---- 카메라 궤도 (Lua와 GDScript가 같은 식 — 바꾸면 셋 다) ----------------------------------------------------------
#   a = 2π t / 20, 위치 = (1150 cos a, 1150 sin a, 420 + 70 sin 2a), 바라보는 점 = (-1500 cos(a + 0.5), -1500 sin(a + 0.5), 380)
CAMERA_LUA = """-- 엔진 비교 벤치(BenchSquare) 카메라 궤도 — Tools/DemoMap/BuildBenchSquare.py가 생성 (Godot bench_square.gd와 같은 식)
--   20초에 한 바퀴. 고정 dt(--fixed-delta 60)로 돌리면 두 엔진이 프레임마다 같은 시점을 그린다
local BenchCamera = {
	Properties = {},
}

function BenchCamera:OnStart()
	self.Time = 0.0
end

function BenchCamera:OnUpdate(dt)
	self.Time = self.Time + dt
	local A = 2.0 * math.pi * self.Time / 20.0
	local From = Vector3(1150.0 * math.cos(A), 1150.0 * math.sin(A), 420.0 + 70.0 * math.sin(2.0 * A))
	local At = Vector3(-1500.0 * math.cos(A + 0.5), -1500.0 * math.sin(A + 0.5), 380.0)
	self.entity:SetPosition(From)
	self.entity:SetRotation(Quat.LookRotation(At - From))
end

return BenchCamera
"""


def CascadeSplitFractions():
	# ShadowMath::ComputeCascadeSplits와 같은 실용 분할 → Godot split_1~3 (최대 거리 비율)
	N, F = CAMERA_NEAR, SHADOW_DISTANCE
	Out = []
	for I in range(1, 4):
		T = I / 4.0
		Log = N * (F / N) ** T
		Uni = N + (F - N) * T
		Out.append((Uni + (Log - Uni) * SPLIT_LAMBDA) / F)
	return Out


# ---- ProjectE 씬 -----------------------------------------------------------------------------------------------------
def WriteEngineScene(P, Backings):
	S = FScene()
	S.Add("Sun", {"DirectionalLightComponent": {"Color": SUN["Color"], "Intensity": SUN["Intensity"]}}, (0, 0, 2000),
		QuatFromEuler(Pitch=SUN["Pitch"], Yaw=SUN["Yaw"]))
	S.Add("SkyLight", {"SkyLightComponent": {"Intensity": SKY_LIGHT}})
	for Index, (Pos, Yaw, Width, Height) in enumerate(Backings):
		S.Model(f"Interior_{Index}", f"{KIT_OUT}/InteriorWall.gltf", Pos, Yaw, (1.0, Width, Height))
	for M in P.Models:
		Extra = {}
		if M["Anim"]:
			Extra["AnimationComponent"] = {"BlendTime": 0.25, "Clip": M["Anim"]["Clip"], "Loop": True, "Playing": True, "Speed": M["Anim"]["Speed"]}
		if M["Walker"]:
			Extra["StressWalkerComponent"] = {"Center": [0.0, 0.0, 0.0], "Speed": M["Walker"]["Speed"], "YawOffset": M["Walker"]["YawOffset"]}
		S.Model(M["Name"], M["Asset"], M["Pos"], M["Yaw"], M["Scale"], Extra=Extra or None)
	for L in P.Lights:
		Params = L["Params"]
		if L["Type"] == "Point":
			Comp = {"PointLightComponent": {"Color": Params["Color"], "Intensity": Params["Intensity"], "Radius": Params["Radius"],
				"CastShadows": Params["CastShadows"]}}
		else:
			Comp = {"SpotLightComponent": {"Color": Params["Color"], "Intensity": Params["Intensity"], "Radius": Params["Radius"],
				"InnerConeAngle": Params["Inner"], "OuterConeAngle": Params["Outer"], "CastShadows": Params["CastShadows"]}}
		S.Add(L["Name"], Comp, L["Pos"], QuatFromEuler(Pitch=L["Pitch"], Yaw=L["Yaw"]))
	S.Add("Camera", {"CameraComponent": {"Primary": True, "FovYDegrees": 60.0, "NearZ": CAMERA_NEAR, "FarZ": 100000.0},
		"ScriptComponent": {"ScriptAsset": CAMERA_SCRIPT, "PropertyOverrides": "{}"}}, (1150, 0, 420), QuatFromEuler(Yaw=180.0))
	Path = os.path.join(CONTENT, *SCENE.split("/"))
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	S.Save(Path)
	ScriptPath = os.path.join(CONTENT, *CAMERA_SCRIPT.split("/"))
	os.makedirs(os.path.dirname(ScriptPath), exist_ok=True)
	with open(ScriptPath, "w", encoding="utf-8", newline="\n") as File:
		File.write(CAMERA_LUA)
	return len(S.Entities)


# ---- Godot 씬 --------------------------------------------------------------------------------------------------------
def GodotPos(E):
	return (E[1] / 100.0, E[2] / 100.0, -E[0] / 100.0)


def GodotDir(D):
	return (D[1], D[2], -D[0])


def Fmt(V):
	return f"{V:.6g}"


def GodotTransform(Basis, Origin):
	# tscn Transform3D = 기저 행 우선 9개(열 = 로컬 축) + 원점
	return "Transform3D(" + ", ".join(Fmt(Basis[I][J]) for I in range(3) for J in range(3)) + ", " + ", ".join(Fmt(V) for V in Origin) + ")"


def GodotModelBasis(Yaw, Scale):
	# 엔진 Yaw θ = Godot Y축 -θ. 엔진 스케일 (X, Y, Z) → glTF 축 (x = 엔진 Y, y = 엔진 Z, z = 엔진 X)
	R = math.radians(-Yaw)
	C, S = math.cos(R), math.sin(R)
	Rot = [[C, 0, S], [0, 1, 0], [-S, 0, C]]
	Sg = (Scale[1], Scale[2], Scale[0])
	return [[Rot[I][J] * Sg[J] for J in range(3)] for I in range(3)]


def LookBasis(Forward):
	# Godot 카메라/라이트는 로컬 -Z를 본다 (Basis.looking_at, 위 = +Y)
	F = np.array(Forward, dtype=float)
	Z = -F / np.linalg.norm(F)
	X = np.cross([0.0, 1.0, 0.0], Z)
	X /= np.linalg.norm(X)
	Y = np.cross(Z, X)
	return [[X[I], Y[I], Z[I]] for I in range(3)]


def EngineForward(Pitch, Yaw):
	# FQuat::FromEuler(Pitch, Yaw) 로컬 +X (UE 규약: +Pitch 기수 위, +Yaw 오른쪽 = +X에서 +Y로)
	P, Y = math.radians(Pitch), math.radians(Yaw)
	return (math.cos(P) * math.cos(Y), math.cos(P) * math.sin(Y), math.sin(P))


def CopyContent(GodotRoot, Rel, Copied):
	# Content 기준 파일을 res://content/ 아래 같은 상대 경로로 복사 (+ glTF가 참조하는 .bin/이미지)
	Rel = os.path.normpath(Rel).replace("\\", "/")
	if Rel in Copied:
		return
	Copied.add(Rel)
	Src = os.path.join(CONTENT, *Rel.split("/"))
	Dst = os.path.join(GodotRoot, "content", *Rel.split("/"))
	if not os.path.exists(Dst) or os.path.getsize(Dst) != os.path.getsize(Src) or os.path.getmtime(Dst) < os.path.getmtime(Src):
		os.makedirs(os.path.dirname(Dst), exist_ok=True)
		shutil.copy2(Src, Dst)
	if Rel.lower().endswith(".gltf"):
		with open(Src, encoding="utf-8") as File:
			Doc = json.load(File)
		Dir = os.path.dirname(Rel)
		for Item in Doc.get("buffers", []) + Doc.get("images", []):
			if "uri" in Item and not Item["uri"].startswith("data:"):
				CopyContent(GodotRoot, os.path.join(Dir, Item["uri"]), Copied)


def WriteGodotScene(P, Backings, GodotRoot):
	Copied = set()
	Ext = {}

	def ExtId(Path):
		if Path not in Ext:
			Ext[Path] = str(len(Ext) + 1)
		return Ext[Path]

	ScriptId = ExtId("res://scripts/bench_square.gd")
	Nodes = []

	def ModelNode(Name, Asset, Pos, Yaw, Scale, Parent=".", Meta=None):
		CopyContent(GodotRoot, Asset, Copied)
		Lines = [f'[node name="{Name}" parent="{Parent}" instance=ExtResource("{ExtId("res://content/" + Asset)}")]',
			f"transform = {GodotTransform(GodotModelBasis(Yaw, Scale), GodotPos(Pos))}"]
		for Key, Value in (Meta or {}).items():
			Lines.append(f"metadata/{Key} = {Value}")
		Nodes.append("\n".join(Lines))

	# 하늘·환경: 절차적 하늘, 환경광 = 하늘, ACES, SSAO + 블룸(글로우). SSR/SSIL/SDFGI/안개 끔 (EngineCompare.ps1 엔진 쪽 설정과 짝)
	Sub = [
		'[sub_resource type="ProceduralSkyMaterial" id="SkyMaterial"]', "",
		'[sub_resource type="Sky" id="Sky"]', 'sky_material = SubResource("SkyMaterial")', "",
		'[sub_resource type="Environment" id="Environment"]',
		"background_mode = 2", 'sky = SubResource("Sky")',
		"ambient_light_source = 3", f"ambient_light_energy = {Fmt(SKY_LIGHT)}", "reflected_light_source = 2",
		"tonemap_mode = 3",
		"ssao_enabled = true", "ssao_radius = 0.8", "ssao_intensity = 1.0",
		"glow_enabled = true", "glow_hdr_threshold = 1.0", "glow_bloom = 0.0", "glow_intensity = 0.3", "",
	]
	Nodes.append('[node name="WorldEnvironment" type="WorldEnvironment" parent="."]\nenvironment = SubResource("Environment")')
	Splits = CascadeSplitFractions()
	Sun = LookBasis(GodotDir(EngineForward(SUN["Pitch"], SUN["Yaw"])))
	R, G, B = SUN["Color"]
	Nodes.append("\n".join([
		'[node name="Sun" type="DirectionalLight3D" parent="."]',
		f"transform = {GodotTransform(Sun, (0.0, 20.0, 0.0))}",
		f"light_color = Color({Fmt(R)}, {Fmt(G)}, {Fmt(B)}, 1)",
		f"light_energy = {Fmt(SUN['Intensity'] * GODOT_DIRECTIONAL_SCALE)}",
		"shadow_enabled = true",
		"directional_shadow_mode = 2",
		f"directional_shadow_split_1 = {Fmt(Splits[0])}", f"directional_shadow_split_2 = {Fmt(Splits[1])}", f"directional_shadow_split_3 = {Fmt(Splits[2])}",
		f"directional_shadow_max_distance = {Fmt(SHADOW_DISTANCE / 100.0)}"]))

	Nodes.append('[node name="Static" type="Node3D" parent="."]')
	Nodes.append('[node name="Units" type="Node3D" parent="."]')
	Nodes.append('[node name="Lights" type="Node3D" parent="."]')
	for Index, (Pos, Yaw, Width, Height) in enumerate(Backings):
		ModelNode(f"Interior_{Index}", f"{KIT_OUT}/InteriorWall.gltf", Pos, Yaw, (1.0, Width, Height), "Static")
	for M in P.Models:
		if M["Walker"]:
			Meta = {"walker_speed": Fmt(M["Walker"]["Speed"]), "walker_yaw_offset": Fmt(M["Walker"]["YawOffset"]),
					"anim_clip": f'"{M["Anim"]["Clip"]}"', "anim_speed": Fmt(M["Anim"]["Speed"])}
			ModelNode(M["Name"], M["Asset"], M["Pos"], M["Yaw"], M["Scale"], "Units", Meta)
		else:
			ModelNode(M["Name"], M["Asset"], M["Pos"], M["Yaw"], M["Scale"], "Static")
	for L in P.Lights:
		Params = L["Params"]
		R, G, B = Params["Color"]
		Common = [f"light_color = Color({Fmt(R)}, {Fmt(G)}, {Fmt(B)}, 1)", f"light_energy = {Fmt(Params['Intensity'] * GODOT_LOCAL_SCALE)}",
				  f"shadow_enabled = {'true' if Params['CastShadows'] else 'false'}"]
		if L["Type"] == "Point":
			Nodes.append("\n".join([f'[node name="{L["Name"]}" type="OmniLight3D" parent="Lights"]',
				f"transform = {GodotTransform([[1, 0, 0], [0, 1, 0], [0, 0, 1]], GodotPos(L['Pos']))}"] + Common +
				[f"omni_range = {Fmt(Params['Radius'] / 100.0)}"]))
		else:
			Basis = LookBasis(GodotDir(EngineForward(L["Pitch"], L["Yaw"])))
			Nodes.append("\n".join([f'[node name="{L["Name"]}" type="SpotLight3D" parent="Lights"]',
				f"transform = {GodotTransform(Basis, GodotPos(L['Pos']))}"] + Common +
				[f"spot_range = {Fmt(Params['Radius'] / 100.0)}", f"spot_angle = {Fmt(Params['Outer'])}"]))
	Nodes.append("\n".join(['[node name="Camera" type="Camera3D" parent="."]',
		f"transform = {GodotTransform(LookBasis(GodotDir((-1, 0, 0))), GodotPos((1150, 0, 420)))}",
		"current = true", "fov = 60.0", f"near = {Fmt(CAMERA_NEAR / 100.0)}", "far = 1000.0"]))

	Header = ["[gd_scene format=3]", ""]
	for Path, Id in Ext.items():
		Type = "Script" if Path.endswith(".gd") else "PackedScene"
		Header.append(f'[ext_resource type="{Type}" path="{Path}" id="{Id}"]')
	Header.append("")
	Root = '[node name="BenchSquare" type="Node3D"]\n' + f'script = ExtResource("{ScriptId}")'
	Out = os.path.join(GodotRoot, "scenes", "BenchSquare.tscn")
	os.makedirs(os.path.dirname(Out), exist_ok=True)
	with open(Out, "w", encoding="utf-8", newline="\n") as File:
		File.write("\n".join(Header + Sub) + "\n" + Root + "\n\n" + "\n\n".join(Nodes) + "\n")
	ScriptOut = os.path.join(GodotRoot, "scripts", "bench_square.gd")
	os.makedirs(os.path.dirname(ScriptOut), exist_ok=True)
	with open(ScriptOut, "w", encoding="utf-8", newline="\n") as File:
		File.write(GODOT_SCRIPT)
	for Rel in ("Asset/KayKit/LICENSE.txt",):
		if os.path.exists(os.path.join(CONTENT, *Rel.split("/"))):
			CopyContent(GodotRoot, Rel, Copied)
	return len(Copied)


GODOT_SCRIPT = """# 엔진 비교 벤치(BenchSquare) 루트 — ProjectE Tools/DemoMap/BuildBenchSquare.py가 생성 (손으로 고치지 말 것)
#   캐릭터 = ProjectE StressWalkerComponent(SampleGame)와 같은 식, 카메라 = Scripts/Bench/BenchCamera.lua와 같은 궤도.
#   계산은 엔진 좌표(왼손 Z-up, cm)로 하고 Godot(glTF 축, m)로 옮긴다: g = (E.y, E.z, -E.x) / 100, 엔진 Yaw θ = Godot Y축 -θ
#   유닛마다 스크립트를 두지 않고 한 루프에서 처리한다 (엔진의 ECS View 순회와 같은 구조)
extends Node3D

var _units: Array[Node3D] = []
var _radius := PackedFloat32Array()
var _angle := PackedFloat32Array()
var _speed := PackedFloat32Array()      # cm/s (음수 = 시계 방향)
var _yaw_offset := PackedFloat32Array() # 도
var _height := PackedFloat32Array()
var _time := 0.0
@onready var _camera: Camera3D = $Camera


func _ready() -> void:
	# 진단용(비교 측정에는 쓰지 않음): --no-anim = 애니메이션 재생 안 함, --no-local-shadows = 점광원/스포트 그림자 끔
	var args := OS.get_cmdline_user_args()
	var animate := not args.has("--no-anim")
	if args.has("--no-local-shadows"):
		for light in $Lights.get_children():
			(light as Light3D).shadow_enabled = false
	var units := $Units.get_children()
	for child in units:
		var unit := child as Node3D
		var ex := -unit.position.z * 100.0
		var ey := unit.position.x * 100.0
		_units.append(unit)
		_radius.append(sqrt(ex * ex + ey * ey))
		_angle.append(atan2(ey, ex))
		_speed.append(unit.get_meta("walker_speed"))
		_yaw_offset.append(unit.get_meta("walker_yaw_offset"))
		_height.append(unit.position.y)
		if animate:
			_start_animation(unit)
	print("[BenchSquare] 유닛 %d개" % _units.size())


func _start_animation(unit: Node) -> void:
	var players := unit.find_children("*", "AnimationPlayer", true, false)
	if players.is_empty():
		push_warning("[BenchSquare] AnimationPlayer 없음: %s" % unit.name)
		return
	var player := players[0] as AnimationPlayer
	var clip: String = unit.get_meta("anim_clip")
	if not player.has_animation(clip):
		push_warning("[BenchSquare] 클립 없음: %s (%s)" % [clip, unit.name])
		return
	player.get_animation(clip).loop_mode = Animation.LOOP_LINEAR
	player.play(clip, 0.0, unit.get_meta("anim_speed", 1.0))


func _process(delta: float) -> void:
	_time += delta
	for i in _units.size():
		var radius := _radius[i]
		var speed := _speed[i]
		var angle := _angle[i] + speed / radius * delta
		_angle[i] = angle
		var heading := rad_to_deg(angle) + (90.0 if speed >= 0.0 else -90.0) + _yaw_offset[i]
		_units[i].transform = Transform3D(Basis(Vector3.UP, deg_to_rad(-heading)),
			Vector3(radius * sin(angle) / 100.0, _height[i], -radius * cos(angle) / 100.0))
	var a := TAU * _time / 20.0
	var from := _to_godot(Vector3(1150.0 * cos(a), 1150.0 * sin(a), 420.0 + 70.0 * sin(2.0 * a)))
	var at := _to_godot(Vector3(-1500.0 * cos(a + 0.5), -1500.0 * sin(a + 0.5), 380.0))
	_camera.look_at_from_position(from, at, Vector3.UP)


func _to_godot(e: Vector3) -> Vector3:
	return Vector3(e.y, e.z, -e.x) / 100.0
"""


def Main():
	Parser = argparse.ArgumentParser()
	Parser.add_argument("--godot", default=DEFAULT_GODOT, help="Godot 프로젝트 폴더 (project.godot 위치)")
	Parser.add_argument("--no-godot", action="store_true")
	Args = Parser.parse_args()
	WriteGeneratedModels()
	Parts, Backings = BuildArchitecture()
	P = BuildPlacement()
	Entities = WriteEngineScene(P, Backings)
	Units = sum(1 for M in P.Models if M["Walker"])
	Shadowed = sum(1 for L in P.Lights if L["Params"]["CastShadows"])
	print(f"BenchSquare 생성: 엔티티 {Entities}, 모델 배치 {len(P.Models)} (유닛 {Units}), 라이트 {len(P.Lights)} (그림자 {Shadowed}), 건물 조각 {Parts}")
	if not Args.no_godot:
		if not os.path.exists(os.path.join(Args.godot, "project.godot")):
			raise SystemExit(f"Godot 프로젝트 없음: {Args.godot}")
		Files = WriteGodotScene(P, Backings, Args.godot)
		print(f"Godot 씬: {os.path.join(Args.godot, 'scenes', 'BenchSquare.tscn')} (복사한 콘텐츠 파일 {Files})")


if __name__ == "__main__":
	Main()
