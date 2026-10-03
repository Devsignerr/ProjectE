# 데모 Hub 맵(해안 항구) 생성: 지형(.eterrain) + 지형 머티리얼 + 임포트 설정 + 플레이어 프리팹 + 씬(Scenes/Demo/Hub.escene)
#   실행: python Tools/DemoMap/BuildHub.py  (먼저 Scripts/FetchDemoAssets.ps1로 Poly Haven 에셋을 받는다)
#   배치를 바꿀 때는 씬 파일이 아니라 이 스크립트를 고치고 다시 실행한다 (결정적 — 고정 시드)
#   좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 바다는 +X(북쪽), 요새 곶은 +Y(동쪽), 언덕은 -X(남쪽)
import base64
import json
import math
import os
import random
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402

ROOT     = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTENT  = os.path.join(ROOT, "Projects", "Sample", "Content")
PH       = "Asset/PolyHaven"

# ---- 지형 ----------------------------------------------------------------------------------------------------------
TERRAIN_SIZE   = 40000.0  # cm (가로·세로)
TERRAIN_RES    = 513
HEIGHT_RANGE   = 10000.0  # cm (16비트 전체 범위, 가운데 = 위치 Z = 0)
PLAZA_HEIGHT   = 250.0
SEA_LEVEL      = 0.0
FORT_CENTER    = (4200.0, 9200.0)
FORT_HEIGHT    = 1050.0


def Smoothstep(E0, E1, X):
	T = np.clip((X - E0) / (E1 - E0), 0.0, 1.0)
	return T * T * (3.0 - 2.0 * T)


def ValueNoise(X, Y, Scale, Seed):
	# 부드러운 값 노이즈 (격자 난수 + 3차 보간) — numpy 배열 입력
	Rng = np.random.default_rng(Seed)
	Grid = Rng.random((64, 64))
	FX, FY = X / Scale, Y / Scale
	IX, IY = np.floor(FX).astype(int), np.floor(FY).astype(int)
	TX, TY = FX - IX, FY - IY
	TX, TY = TX * TX * (3 - 2 * TX), TY * TY * (3 - 2 * TY)

	def G(A, B):
		return Grid[A % 64, B % 64]
	Top = G(IX, IY) * (1 - TX) + G(IX + 1, IY) * TX
	Bot = G(IX, IY + 1) * (1 - TX) + G(IX + 1, IY + 1) * TX
	return Top * (1 - TY) + Bot * TY


def Fbm(X, Y, Scale, Seed, Octaves=4):
	Sum, Amp, Norm = 0.0, 1.0, 0.0
	for Octave in range(Octaves):
		Sum = Sum + ValueNoise(X, Y, Scale / (2 ** Octave), Seed + Octave) * Amp
		Norm += Amp
		Amp *= 0.5
	return Sum / Norm * 2.0 - 1.0


def ShorelineX(Y):
	# 바닷가 선(이 X보다 바다 쪽이면 해변 경사) — 곶에서는 바다 쪽으로 튀어나온다
	Base = 5200.0 + 900.0 * np.sin(Y / 4200.0) + 500.0 * np.sin(Y / 1700.0 + 1.3)
	Cape = 3600.0 * Smoothstep(5200.0, 8200.0, Y) * (1.0 - Smoothstep(13500.0, 16500.0, Y))
	return Base + Cape


def BuildHeights():
	Coords = (np.arange(TERRAIN_RES) / (TERRAIN_RES - 1) - 0.5) * TERRAIN_SIZE
	# 지형 격자: X = 열(+X 앞), Y = 행(+Y 오른쪽) → 배열 [행(Y), 열(X)]
	X, Y = np.meshgrid(Coords, Coords)
	H = np.full(X.shape, PLAZA_HEIGHT)

	# 마을 땅의 완만한 기복
	H += Fbm(X, Y, 6000.0, 11) * 70.0

	# 남쪽(-X) 언덕과 서쪽(-Y) 능선
	Hill = Smoothstep(-5500.0, -15000.0, X) * (1900.0 + Fbm(X, Y, 5000.0, 21) * 700.0)
	Ridge = Smoothstep(-8000.0, -16000.0, Y) * (1300.0 + Fbm(X, Y, 4000.0, 31) * 500.0)
	H += np.maximum(Hill, Ridge)

	# 요새 곶 (동쪽 고지대)
	DX, DY = X - FORT_CENTER[0], Y - FORT_CENTER[1]
	CapeMask = Smoothstep(6200.0, 3600.0, np.sqrt(DX * DX * 0.55 + DY * DY))
	H = H * (1 - CapeMask) + (FORT_HEIGHT + Fbm(X, Y, 3000.0, 41) * 40.0) * CapeMask
	# 곶으로 오르는 비탈길 (광장 동쪽 → 요새 문)
	Ramp = Smoothstep(900.0, 0.0, np.abs(Y - (1400.0 + X * 0.95))) * Smoothstep(-500.0, 1500.0, X) * (1 - Smoothstep(4200.0, 5200.0, X))
	H = H * (1 - Ramp * 0.35) + (PLAZA_HEIGHT + (X - 500.0) * 0.22) * Ramp * 0.35

	# 광장 평탄화
	Plaza = Smoothstep(2200.0, 1500.0, np.sqrt(X * X + Y * Y))
	H = H * (1 - Plaza) + PLAZA_HEIGHT * Plaza
	# 부두로 가는 길 평탄화
	Road = Smoothstep(500.0, 250.0, np.abs(Y + 1500.0)) * Smoothstep(0.0, 800.0, X)
	H = H * (1 - Road * 0.7) + np.minimum(H, PLAZA_HEIGHT) * Road * 0.7

	# 해변: 바닷가 선 바깥은 바다 바닥으로 내려간다 (곶은 절벽처럼 가파르게)
	Shore = X - ShorelineX(Y)
	Steep = np.where(CapeMask > 0.3, 0.55, 0.11)
	Beach = H - np.maximum(Shore, 0.0) * Steep
	H = np.maximum(Beach, -650.0 + Fbm(X, Y, 3000.0, 51) * 80.0)
	return X, Y, H


def BuildWeights(X, Y, H):
	# 레이어: 0 풀밭, 1 모래, 2 자갈 포장(광장/길), 3 바위(경사/고지)
	GY, GX = np.gradient(H, TERRAIN_SIZE / (TERRAIN_RES - 1))
	Slope = np.sqrt(GX * GX + GY * GY)
	Noise = Fbm(X, Y, 900.0, 61, 3)
	Sand = Smoothstep(PLAZA_HEIGHT + 60.0, PLAZA_HEIGHT - 60.0, H + Noise * 60.0) * Smoothstep(ShorelineX(Y) - 2400.0, ShorelineX(Y) - 900.0, X)
	Sand = np.maximum(Sand, Smoothstep(80.0, -20.0, H))
	Rock = np.clip(Smoothstep(0.45, 0.8, Slope + Noise * 0.1) + Smoothstep(1500.0, 2200.0, H) * 0.7, 0, 1)
	Dist = np.sqrt(X * X + Y * Y)
	Cobble = Smoothstep(1550.0 + Noise * 120.0, 1300.0, Dist)
	Cobble = np.maximum(Cobble, Smoothstep(330.0, 200.0, np.abs(Y + 1500.0 + Noise * 40.0)) * Smoothstep(1500.0, 1800.0, X) * Smoothstep(ShorelineX(Y) - 500.0, ShorelineX(Y) - 1300.0, X))
	W3 = Rock
	W2 = Cobble * (1 - W3)
	W1 = Sand * (1 - W2) * (1 - W3)
	W0 = np.clip(1 - W1 - W2 - W3, 0, 1)
	Stack = np.stack([W0, W1, W2, W3], axis=-1)
	Stack = Stack / np.maximum(Stack.sum(axis=-1, keepdims=True), 1e-6)
	Bytes = np.floor(Stack * 255.0 + 0.5).astype(np.int32)
	Bytes[..., 0] += 255 - Bytes.sum(axis=-1)  # 합 255 맞춤
	Bytes = np.clip(Bytes, 0, 255).astype(np.uint32)
	return Bytes[..., 0] | (Bytes[..., 1] << 8) | (Bytes[..., 2] << 16) | (Bytes[..., 3] << 24)


def WriteTerrain(Path, H, Weights):
	H16 = np.clip(np.round((H / HEIGHT_RANGE + 0.5) * 65535.0), 0, 65535).astype("<u2")
	Doc = {
		"Heights": base64.b64encode(H16.tobytes()).decode("ascii"),
		"Resolution": TERRAIN_RES,
		"Version": 1,
		"Weights": base64.b64encode(Weights.astype("<u4").tobytes()).decode("ascii"),
	}
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent="\t")
		File.write("\n")


class FHeightSampler:
	# 지형 높이 표본 (렌더/충돌과 같은 격자 쌍선형 — 셀 대각선 차이는 무시해도 될 만큼 작다)
	def __init__(self, H):
		self.H = H

	def __call__(self, PX, PY):
		Cell = TERRAIN_SIZE / (TERRAIN_RES - 1)
		FX = (PX + TERRAIN_SIZE * 0.5) / Cell
		FY = (PY + TERRAIN_SIZE * 0.5) / Cell
		IX = int(np.clip(math.floor(FX), 0, TERRAIN_RES - 2))
		IY = int(np.clip(math.floor(FY), 0, TERRAIN_RES - 2))
		TX, TY = FX - IX, FY - IY
		H = self.H
		Top = H[IY, IX] * (1 - TX) + H[IY, IX + 1] * TX
		Bot = H[IY + 1, IX] * (1 - TX) + H[IY + 1, IX + 1] * TX
		return float(Top * (1 - TY) + Bot * TY)


# ---- 머티리얼 / 임포트 설정 / 프리팹 --------------------------------------------------------------------------------
TERRAIN_TEXTURES = {
	"Grass": ("leafy_grass", 0.95, [0.78, 0.95, 0.62]),
	"Sand": ("coast_sand_01", 0.9, [1.0, 1.0, 1.0]),
	"Cobble": ("cobblestone_floor_04", 0.85, [1.0, 1.0, 1.0]),
	"Rock": ("cliff_side", 0.9, [1.0, 1.0, 1.0]),
}


def WriteTerrainMaterials():
	Folder = os.path.join(CONTENT, "Materials", "Demo")
	os.makedirs(Folder, exist_ok=True)
	for Name, (Id, Rough, Tint) in TERRAIN_TEXTURES.items():
		Rel = f"../../{PH}/{Id}/{Id}"
		Mat = {
			"Name": f"DemoTerrain{Name}",
			"BaseColorFactor": Tint + [1.0],  # 풀밭은 초록 쪽으로 (원본은 마른 잔디색)
			"EmissiveFactor": [0.0, 0.0, 0.0],
			"Metallic": 1.0,
			"Roughness": 1.0,
			"NormalScale": 1.0,
			"OcclusionStrength": 1.0,
			"BaseColorTexture": f"{Rel}_diff_2k.jpg",
			"MetallicRoughnessTexture": f"{Rel}_arm_2k.jpg",  # Poly Haven ARM: R=AO, G=거칠기, B=금속 (엔진 규약과 같음)
			"NormalTexture": f"{Rel}_nor_gl_2k.jpg",
			"OcclusionTexture": f"{Rel}_arm_2k.jpg",
			"EmissiveTexture": "",
		}
		with open(os.path.join(Folder, f"Terrain{Name}.emat"), "w", encoding="utf-8", newline="\n") as File:
			json.dump(Mat, File, indent=2)
			File.write("\n")


# 임포트 설정 (.eimport): 스캔 에셋은 쿠킹 때 LOD0 삼각형 상한(MaxTriangles)으로 줄인다 — 원본은 수십만~수백만 삼각형.
#   잎·풀 컷아웃이 BLEND로 저장된 에셋은 마스크로 (그림자·깊이 정렬, 잎 솎아내기 LOD 대상)
IMPORT_SETTINGS = {
	"jacaranda_tree":      {"BlendAsMasked": True, "MaxTriangles": 600000},
	"wild_rooibos_bush":   {"BlendAsMasked": True},
	"coast_land_rocks_03": {"MaxTriangles": 60000},
	"coast_rocks_05":      {"MaxTriangles": 50000},
	"sand_rocks_small_01": {"MaxTriangles": 40000},
	"coastal_cliff_01":    {"MaxTriangles": 150000},
	"coastal_cliff_02":    {"MaxTriangles": 150000},
}


def WritePortalMaterial():
	Mat = {
		"Name": "DemoPortalGlow", "BlendMode": "Additive", "TwoSided": True,
		"BaseColorFactor": [0.0, 0.0, 0.0, 1.0], "EmissiveFactor": [1.2, 3.0, 6.0],
		"Metallic": 0.0, "Roughness": 1.0, "NormalScale": 1.0, "OcclusionStrength": 1.0,
		"BaseColorTexture": "", "MetallicRoughnessTexture": "", "NormalTexture": "", "OcclusionTexture": "", "EmissiveTexture": "",
	}
	with open(os.path.join(CONTENT, "Materials", "Demo", "PortalGlow.emat"), "w", encoding="utf-8", newline="\n") as File:
		json.dump(Mat, File, indent=2)
		File.write("\n")


def WriteImportSettings():
	for Id, Settings in IMPORT_SETTINGS.items():
		Path = os.path.join(CONTENT, "Asset", "PolyHaven", Id, f"{Id}.gltf.eimport")
		with open(Path, "w", encoding="utf-8", newline="\n") as File:
			json.dump(Settings, File, indent=2)
			File.write("\n")


def Link(Id):
	return {"Id": str(Id), "Root": -1}


def WritePlayerPrefab():
	# 루트 = 캡슐 캐릭터 + 스크립트, Body(1인칭에서 숨김) > Mesh(마네킹, 발 = 캡슐 바닥, 앞 = +X)
	Entities = [
		{"Name": "Player", "Parent": -1, "Components": {
			"CharacterMovementComponent": {
				"AirControl": 0.35, "CapsuleHalfHeight": 55.0, "CapsuleRadius": 35.0, "FaceControlYaw": True,
				"GravityScale": 1.0, "JumpZVelocity": 520.0, "Mass": 80.0, "MaxSlopeAngle": 50.0,
				"MaxStepHeight": 40.0, "MaxWalkSpeed": 450.0, "PushForce": 4000.0},
			"ScriptComponent": {"ExecutionLocation": 2, "PropertyOverrides": "", "ScriptAsset": "Scripts/Demo/DemoPlayer.lua"},
			"PrefabLinkComponent": Link(1),
			"TransformComponent": {"Position": [0.0, 0.0, 100.0], "Rotation": [0.0, 0.0, 0.0, 1.0], "Scale": [1.0, 1.0, 1.0]}}},
		{"Name": "Body", "Parent": 0, "Components": {
			"PrefabLinkComponent": Link(2),
			"TransformComponent": {"Position": [0.0, 0.0, 0.0], "Rotation": [0.0, 0.0, 0.0, 1.0], "Scale": [1.0, 1.0, 1.0]}}},
		{"Name": "Mesh", "Parent": 1, "Components": {
			"ModelComponent": {"AssetPath": "Asset/Quaternius/UAL2/UAL2_Standard.glb"},
			"AnimationComponent": {"BlendTime": 0.2, "Clip": "", "Loop": True, "Playing": True, "Speed": 1.0},
			"AnimGraphComponent": {"Graph": "Animations/Demo/DemoMannequin.eanimgraph", "UseCharacterMovement": True},
			"PrefabLinkComponent": Link(3),
			"TransformComponent": {"Position": [0.0, 0.0, -90.0], "Rotation": QuatFromEuler(Yaw=MANNEQUIN_YAW), "Scale": [1.0, 1.0, 1.0]}}},
	]
	Folder = os.path.join(CONTENT, "Prefabs", "Demo")
	os.makedirs(Folder, exist_ok=True)
	with open(os.path.join(Folder, "DemoPlayer.eprefab"), "w", encoding="utf-8", newline="\n") as File:
		json.dump({"Entities": Entities, "NextId": 4, "Version": 1}, File, indent=2, ensure_ascii=False)
		File.write("\n")


MANNEQUIN_YAW = 180.0  # glTF +Z 앞 → 엔진 -X 앞이므로 180도 돌려 +X를 보게 한다


# ---- 씬 배치 --------------------------------------------------------------------------------------------------------
PORTALS = [
	# (이름, 표시 이름, 서브맵, 각도: 광장 중심에서 방위 — 0 = +X 바다 쪽)
	("Portal_Lighting", "빛과 GI (선술집 홀)", "Scenes/Demo/Lighting.escene", 115.0),
	("Portal_Alley", "밤의 골목", "Scenes/Demo/Alley.escene", 145.0),
	("Portal_Gallery", "머티리얼 갤러리", "Scenes/Demo/Gallery.escene", 170.0),
	("Portal_Forest", "숲 (지형·식생)", "Scenes/Demo/Forest.escene", 195.0),
	("Portal_Workshop", "창고 (물리)", "Scenes/Demo/Workshop.escene", 220.0),
	("Portal_Training", "훈련장 (애니메이션·AI)", "Scenes/Demo/Training.escene", 245.0),
	("Portal_Campfire", "해변 캠프 (파티클)", "Scenes/Demo/Campfire.escene", 270.0),
]

READY_PORTALS = {"Portal_Lighting", "Portal_Alley", "Portal_Gallery", "Portal_Forest"}  # 서브맵이 만들어진 포털 (Build<이름>.py)


def Model(Id):
	return f"{PH}/{Id}/{Id}.gltf"


def BuildScene(Height):
	Rng = random.Random(7)
	S = FScene()

	def Place(Name, Id, X, Y, Yaw=0.0, Scale=1.0, Z=None, Sink=0.0):
		Ground = Height(X, Y) if Z is None else Z
		return S.Model(Name, Model(Id), (X, Y, Ground - Sink), Yaw, Scale)

	# 환경: 대기 + 시간대(늦은 오후) + 구름 + 옅은 안개, 태양 = 첫 방향광
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.96, 0.9], "Intensity": 3.2}}, (0, 0, 2000), QuatFromEuler(Pitch=-32, Yaw=-35))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {},
		"VolumetricCloudComponent": {"Coverage": 0.38, "WindSpeed": 10.0},
		"TimeOfDayComponent": {"TimeOfDay": 16.3, "DayLengthMinutes": 0.0, "MaxSunElevation": 58.0, "NorthAzimuth": 200.0, "AnimateInEditor": False},
		"SkyLightComponent": {"Intensity": 1.0},
		"HeightFogComponent": {"Color": [0.55, 0.65, 0.78], "Density": 0.0025, "HeightFalloff": 0.06, "StartDistance": 3000.0, "MaxOpacity": 0.8},
	})

	# 지형 + 바다
	S.Add("Terrain", {"TerrainComponent": {
		"Asset": "Terrain/Demo/Hub.eterrain", "Size": [TERRAIN_SIZE, TERRAIN_SIZE], "HeightRange": HEIGHT_RANGE,
		"Layer0Material": "Materials/Demo/TerrainGrass.emat", "Layer1Material": "Materials/Demo/TerrainSand.emat",
		"Layer2Material": "Materials/Demo/TerrainCobble.emat", "Layer3Material": "Materials/Demo/TerrainRock.emat",
		"Layer0Tiling": 450.0, "Layer1Tiling": 450.0, "Layer2Tiling": 300.0, "Layer3Tiling": 800.0,
		"CastShadows": True, "Collision": True}})
	S.Add("Sea", {"WaterBodyComponent": {
		"Size": [60000.0, 90000.0, 1200.0], "ScatterColor": [0.02, 0.10, 0.12], "Absorption": [0.40, 0.08, 0.06],
		"NormalStrength": 0.45, "WaveScale": 420.0, "WaveSpeed": 18.0, "FlowDirection": 0.0, "FlowSpeed": 0.0,
		"FoamIntensity": 0.8, "FoamDistance": 35.0, "RefractionStrength": 0.04, "ReflectionIntensity": 1.0, "Roughness": 0.07}},
		(36000.0, 0.0, SEA_LEVEL - 600.0))

	# 플레이어 (광장 서쪽에서 바다를 보며 시작)
	# 프리팹 인스턴스는 루트만 저장한다 (씬 로드가 원본과 동기화 — PrefabLink.Root = 자기 인덱스)
	PlayerIndex = len(S.Entities)
	S.Add("Player", {
		"PrefabInstanceComponent": {"Asset": "Prefabs/Demo/DemoPlayer.eprefab", "Overrides": ""},
		"PrefabLinkComponent": {"Id": "1", "Root": PlayerIndex}},
		(-900.0, 300.0, Height(-900, 300) + 110.0))

	# 광장: 화로 + 불빛
	Place("Plaza_FirePit", "stone_fire_pit", 0, 0, Sink=-15)
	S.Add("Plaza_FireLight", {"PointLightComponent": {"Color": [1.0, 0.55, 0.25], "Intensity": 25.0, "Radius": 900.0, "CastShadows": False}}, (0, 0, Height(0, 0) + 90))

	for Index in range(4):
		Angle = 45.0 + Index * 90.0
		Rad = math.radians(Angle)
		Place(f"Plaza_Bench_{Index}", "painted_wooden_bench", math.cos(Rad) * 330.0, math.sin(Rad) * 330.0, Angle)
	Stalls = [(-55.0, 950.0), (-25.0, 1050.0), (20.0, 1000.0), (55.0, 900.0)]
	for Index, (Angle, Dist) in enumerate(Stalls):
		Rad = math.radians(Angle)
		SX, SY = math.cos(Rad) * Dist, math.sin(Rad) * Dist
		Yaw = Angle + 90.0
		Ground = Height(SX, SY)
		Table = S.Model(f"Market_{Index}", Model("wooden_picnic_table"), (SX, SY, Ground), Yaw, 1.0)
		Goods = ["wicker_basket_01", "jug_01", "wicker_basket_02", "jug_01"]
		for G, Id in enumerate(Goods):
			S.Model(f"Market_{Index}_Goods", Model(Id), (Rng.uniform(-30, 30), -80 + G * 50 + Rng.uniform(-8, 8), 75.0), Rng.uniform(0, 360), 1.0, Parent=Table)
		Side = Rng.choice([-1, 1])
		S.Model(f"Market_{Index}_Barrel", Model(Rng.choice(["wine_barrel_01", "barrel_03"])), (Rng.uniform(-40, 40), Side * 190.0, 0), Rng.uniform(0, 360), 1.0, Parent=Table)
		S.Model(f"Market_{Index}_Stool", Model("wooden_stool_01"), (Rng.uniform(-170, -140), Rng.uniform(-60, 60), 0), Rng.uniform(0, 360), 1.0, Parent=Table)
	Place("Plaza_Ladder", "wooden_ladder", 700.0, 900.0, 30.0)

	# 포털: 광장 서쪽 반원에 성문 + 양옆 랜턴
	for Name, Label, Target, Angle in PORTALS:
		Rad = math.radians(Angle)
		R = 1350.0
		PX, PY = math.cos(Rad) * R, math.sin(Rad) * R
		FaceYaw = Angle + 180.0  # 광장 중심을 본다
		Ground = Height(PX, PY)
		Root = S.Add(Name, {
			"ScriptComponent": {"ScriptAsset": "Scripts/Demo/DemoPortal.lua", "ExecutionLocation": 0,
				"PropertyOverrides": json.dumps({"TargetScene": {"Asset": Target}, "Label": Label, "Ready": Name in READY_PORTALS}, ensure_ascii=False)}},
			(PX, PY, Ground), QuatFromEuler(Yaw=FaceYaw))
		S.Model(f"{Name}_Door", Model("large_castle_door"), (0, 0, -3), 0.0, 1.0, Parent=Root)
		S.Model(f"{Name}_Planter", Model("planter_box_01"), (60, 0, 0), 90.0, 1.0, Parent=Root)
		for Side in (-1, 1):
			S.Model(f"{Name}_Post", Model("tree_stump_01"), (40, Side * 150, -25), Rng.uniform(0, 360), 0.45, Parent=Root)
			S.Model(f"{Name}_Lantern", Model("wooden_lantern_01"), (40, Side * 150, 8), Rng.uniform(0, 360), 1.3, Parent=Root)
		S.Add(f"{Name}_Glow", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Demo/PortalGlow.emat"}},
			(18, 0, 148), None, (0.02, 1.85, 2.85), Parent=Root)
		S.Add(f"{Name}_Light", {"PointLightComponent": {"Color": [1.0, 0.7, 0.4], "Intensity": 6.0, "Radius": 500.0}}, (80, 0, 220), Parent=Root)

	# 부두 + 배
	PierX = ShorelineX(-1500.0) - 600.0
	S.Model("Pier", Model("modular_wooden_pier"), (PierX + 705.0, -1500.0, SEA_LEVEL + 60.0), 0.0, 1.0)
	S.Model("Pier2", Model("modular_wooden_pier"), (PierX + 705.0 + 1900.0, -1500.0, SEA_LEVEL + 60.0), 0.0, 1.0)
	S.Model("Ship_Pinnace", Model("ship_pinnace"), (PierX + 2600.0, -3100.0, SEA_LEVEL - 40.0), 3.0, 1.0)
	S.Model("Ship_Dutch", Model("dutch_ship_medium"), (PierX + 4800.0, 1400.0, SEA_LEVEL - 60.0), -24.0, 1.0)

	# 부두 앞 짐 더미
	Cargo = [("wooden_barrels_01", 0.0), ("wooden_crate_02", 20.0), ("wooden_crate_01", -15.0), ("wooden_crate_02", 75.0),
			 ("wooden_bucket_01", 0.0), ("treasure_chest", 160.0), ("wooden_crate_01", 40.0)]
	for Index, (Id, Yaw) in enumerate(Cargo):
		CX = PierX - 900.0 + (Index % 4) * 260.0 + Rng.uniform(-60, 60)
		CY = -2300.0 + (Index // 4) * 1500.0 + Rng.uniform(-80, 80)
		Place(f"Cargo_{Index}", Id, CX, CY, Yaw + Rng.uniform(-10, 10))
	Place("Cannon_Beach", "cannon_01", PierX - 300.0, -600.0, 30.0)

	# 요새 (곶 위, 바다를 향함) + 대포
	S.Model("Fort", Model("modular_fort_01"), (FORT_CENTER[0] + 300.0, FORT_CENTER[1] - 3200.0, FORT_HEIGHT - 20.0), 0.0, 1.0)

	# 해안 절벽 (곶 바다 쪽 가장자리와 서쪽 해안)
	CapeEdgeX = ShorelineX(FORT_CENTER[1]) + 200.0
	S.Model("Cliff_Cape_A", Model("coastal_cliff_01"), (CapeEdgeX, FORT_CENTER[1], -250.0), 0.0, 1.35)
	S.Model("Cliff_Cape_B", Model("coastal_cliff_02"), (CapeEdgeX - 2200.0, FORT_CENTER[1] + 6300.0, -150.0), -55.0, 1.3)
	S.Model("Cliff_West_A", Model("coastal_cliff_02"), (ShorelineX(-9000.0) + 300.0, -9000.0, -300.0), 15.0, 1.2)
	S.Model("Cliff_West_B", Model("coastal_cliff_01"), (ShorelineX(-15500.0) + 500.0, -15500.0, -350.0), -20.0, 1.1)

	# 바닷가 바위
	for Index in range(16):
		RY = Rng.uniform(-13000.0, 4500.0)
		RX = ShorelineX(RY) + Rng.uniform(-300.0, 1600.0)
		if abs(RY + 1500.0) < 900.0:
			continue  # 부두 자리 비움
		Id = Rng.choice(["coast_rocks_05", "sand_rocks_small_01", "coast_land_rocks_03"])
		Place(f"Rock_{Index}", Id, RX, RY, Rng.uniform(0, 360), Rng.uniform(0.8, 1.6), Sink=30.0)

	# 나무·덤불·꽃 (광장/길/포털 자리 피함)
	def Free(X, Y, Clear=2400.0):
		if math.hypot(X, Y) < Clear:
			return False
		if abs(Y + 1500.0) < 600.0 and X > 0:
			return False
		Z = Height(X, Y)
		return SEA_LEVEL + 120.0 < Z < 2200.0 and X < ShorelineX(Y) - 800.0

	Trees = 0
	while Trees < 5:
		X, Y = Rng.uniform(-9000.0, 4000.0), Rng.uniform(-11000.0, 7000.0)
		if Free(X, Y, 3200.0):
			Place(f"Tree_Jacaranda_{Trees}", "jacaranda_tree", X, Y, Rng.uniform(0, 360), Rng.uniform(0.75, 1.0), Sink=20.0)
			Trees += 1
	Bushes = 0
	while Bushes < 60:
		X, Y = Rng.uniform(-8000.0, 5000.0), Rng.uniform(-11000.0, 8000.0)
		if Free(X, Y, 2000.0):
			Id = Rng.choice(["shrub_02", "wild_rooibos_bush", "fern_02", "fern_02", "flower_gazania", "flower_gazania"])
			Place(f"Plant_{Bushes}", Id, X, Y, Rng.uniform(0, 360), Rng.uniform(0.8, 1.3), Sink=5.0)
			Bushes += 1
	for Index in range(5):
		X, Y = Rng.uniform(-6000.0, 3000.0), Rng.uniform(-8000.0, 5000.0)
		if Free(X, Y):
			Place(f"Stump_{Index}", "tree_stump_01", X, Y, Rng.uniform(0, 360), 1.0, Sink=10.0)

	# 길가 가로등 (부두 길)
	for Index in range(4):
		LX = 2200.0 + Index * 900.0
		Place(f"StreetLamp_{Index}", "street_lamp_02", LX, -1500.0 + 420.0, -90.0)

	return S


def Main():
	X, Y, H = BuildHeights()
	Weights = BuildWeights(X, Y, H)
	WriteTerrain(os.path.join(CONTENT, "Terrain", "Demo", "Hub.eterrain"), H, Weights)
	WriteTerrainMaterials()
	WritePortalMaterial()
	WriteImportSettings()
	WritePlayerPrefab()
	Scene = BuildScene(FHeightSampler(H))
	os.makedirs(os.path.join(CONTENT, "Scenes", "Demo"), exist_ok=True)
	Scene.Save(os.path.join(CONTENT, "Scenes", "Demo", "Hub.escene"))
	print(f"Hub 생성: 엔티티 {len(Scene.Entities)}개, 지형 {TERRAIN_RES}² 높이 {H.min():.0f}~{H.max():.0f}cm")


if __name__ == "__main__":
	Main()
