# 데모 서브맵 "Forest"(개울이 흐르는 전나무 숲 골짜기) 생성: 지형(.eterrain) + 지형/풀 머티리얼 + 임포트 설정 + 폴리지(.efoliage)
#   + 개울 소리(.wav, 절차 생성) + 씬(Scenes/Demo/Forest.escene)
#   실행: python Tools/DemoMap/BuildForest.py [--overview[=<시점 이름>,... | =x,y,지면 위 높이,pitch,yaw]]  (먼저 Scripts/FetchDemoAssets.ps1로 Poly Haven 에셋을 받는다)
#   --overview: 플레이어 대신 고정 카메라를 둔 확인용 변형(Scenes/Demo/_ForestOverview*.escene)도 쓴다 — 커밋하지 않는다
#   보여 주는 기능: 지형(높이 + 4레이어), 엔진 폴리지(.efoliage, GPU 인스턴싱 — 풀 + 먼 비탈 침엽수, 거리 페이드), 스캔 나무/바위 모델
#                   (쿠킹 삼각형 상한 + 잎 솎아내기 + 화면 크기 LOD, 걷는 길에서 멀수록 성긴 잎 단계), 캐시된 그림자(정적 숲),
#                   아침 햇빛 + 볼류메트릭 안개(나무 사이 빛줄기), 개울·웅덩이(물 상자 하나 + 흐름), 공간 음향(절차 생성 개울 소리)
#   좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 개울은 -X(상류, 서쪽) → +X(하류, 동쪽)로 흐르고 아침 해는 하류 쪽(+X) 낮은 고도
#   배치를 바꿀 때는 씬 파일이 아니라 이 스크립트를 고치고 다시 실행한다 (결정적 — 고정 시드)
import base64
import json
import math
import os
import random
import struct
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402
from AssetFixes import SwapAlphaImages  # noqa: E402

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTENT = os.path.join(ROOT, "Projects", "Sample", "Content")
PH      = "Asset/PolyHaven"
MAT_DIR = "Materials/Demo/Forest"

# ---- 지형 ----------------------------------------------------------------------------------------------------------
TERRAIN_SIZE = 25600.0  # cm (가로·세로)
TERRAIN_RES  = 513      # 칸 50cm
HEIGHT_RANGE = 12000.0  # cm (16비트 전체 범위, 가운데 = 위치 Z = 0)
FLOOR_SLOPE  = 0.003    # 골짜기 바닥 경사 (하류 +X로 내려감 — 수면 하나가 상류까지 둑 아래에 있도록 완만하게)
CHANNEL_HALF = 170.0    # cm: 개울 바닥 반폭 (가장 깊은 곳)
BANK_HALF    = 430.0    # cm: 개울 둑 끝 (여기서 골짜기 바닥 높이)
CHANNEL_DEPTH = 75.0    # cm: 수면 아래 개울 바닥 깊이 (가운데)
SPRING_X     = -10200.0  # 개울 시작 (상류 끝 바위틈 샘 — 여기서 서쪽은 비탈로 막힌다)
POND_X, POND_RX, POND_RY = 8600.0, 1800.0, 1300.0  # 하류 끝 웅덩이(타원) — 그 너머는 비탈로 막힌다
WATER_BELOW_BANK = 30.0  # cm: 가장 낮은 둑(웅덩이 하류 기슭)보다 수면이 낮은 정도


def Smoothstep(E0, E1, X):
	T = np.clip((X - E0) / (E1 - E0), 0.0, 1.0)
	return T * T * (3.0 - 2.0 * T)


def ValueNoise(X, Y, Scale, Seed):
	# 부드러운 값 노이즈 (격자 난수 + 3차 보간) — numpy 배열 입력 (BuildHub.py와 같은 식)
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


def StreamY(X):
	# 개울 가운데 선 (굽이침)
	return 1100.0 * np.sin(X / 4800.0 + 0.4) + 380.0 * np.sin(X / 1700.0 + 2.0)


def PathY(X):
	# 남쪽 둑 오솔길 (개울을 따라가며 조금씩 멀어졌다 가까워짐)
	return StreamY(X) - 780.0 - 220.0 * np.sin(X / 2600.0 + 1.0)


def ChannelDist(X, Y):
	# 개울 가운데 선까지 거리를 폭 변화로 늘이고 줄인 값 (둑이 자연스럽게 들쭉날쭉 — 높이/레이어 공용)
	return np.abs(Y - StreamY(X)) / (1.0 + 0.3 * Fbm(X, Y, 1100.0, 191, 3))


def FloorZ(X):
	return -X * FLOOR_SLOPE


def PondY():
	return float(StreamY(POND_X))


def WaterLevel():
	# 개울·웅덩이 공용 수면 (물 상자 하나 — 여러 상자는 굽은 곳에서 겹치거나 틈이 생긴다) = 가장 낮은 기슭 바닥 - WATER_BELOW_BANK.
	#   골짜기 바닥은 이보다 항상 높고(완만한 경사 + 양수 노이즈) 개울 바닥·웅덩이만 그 아래로 파낸다
	return float(FloorZ(POND_X + POND_RX)) - WATER_BELOW_BANK


def PondE(X, Y):
	# 웅덩이 타원 정규 거리 (1 = 기슭), 기슭선이 들쭉날쭉하게 노이즈
	E = np.sqrt(((X - POND_X) / POND_RX) ** 2 + ((Y - PondY()) / POND_RY) ** 2)
	return E * (1.0 + 0.1 * Fbm(X, Y, 900.0, 197, 3))


def BuildHeights():
	Coords = (np.arange(TERRAIN_RES) / (TERRAIN_RES - 1) - 0.5) * TERRAIN_SIZE
	# 지형 격자: 배열 [행(Y), 열(X)]
	X, Y = np.meshgrid(Coords, Coords)
	Side = Y - StreamY(X)            # + = 북쪽 둑
	D = np.abs(Side)
	# 골짜기 바닥: 하류로 내려가는 경사 + 낮은 기복 (둑 근처는 개울보다 아래로 내려가지 않게 양수 쪽 노이즈)
	Floor = FloorZ(X) + (Fbm(X, Y, 2600.0, 101) * 0.5 + 0.5) * 55.0 + Fbm(X, Y, 500.0, 105, 2) * 6.0
	# 산비탈: 바닥 반폭(굽이마다 다름)부터 오르막, 북쪽이 더 높고 가파르다 (바위 노두)
	HalfWidth = 1500.0 + Fbm(X, Y, 5200.0, 111) * 500.0
	North = np.where(Side > 0, 1.2, 1.0)
	Rise = Smoothstep(HalfWidth, HalfWidth + 6500.0, D)
	Hill = (Rise ** 1.3) * (2100.0 + Fbm(X, Y, 4200.0, 121) * 700.0) * North
	Hill += Smoothstep(8500.0, 12500.0, D) * (1200.0 + Fbm(X, Y, 3000.0, 131) * 400.0)
	# 비탈의 주름 (작은 골·능선)
	Hill += Rise * Fbm(X, Y, 1300.0, 141, 3) * 260.0
	# 골짜기 양 끝을 막는 비탈 (상류 샘 너머, 하류 웅덩이 너머 — 지형 끝이 보이지 않게)
	Ends = Smoothstep(SPRING_X + 300.0, -TERRAIN_SIZE * 0.5, X) + Smoothstep(POND_X + POND_RX - 600.0, TERRAIN_SIZE * 0.5, X)
	Hill += Ends * (1700.0 + Fbm(X, Y, 2500.0, 135) * 400.0)
	H = Floor + Hill
	# 개울 바닥: 둑(BANK_HALF)에서 바닥 깊이(CHANNEL_DEPTH)까지 파낸다 (샘에서 시작)
	DC = ChannelDist(X, Y)
	Depth = CHANNEL_DEPTH * Smoothstep(SPRING_X, SPRING_X + 700.0, X)
	Bank = Smoothstep(CHANNEL_HALF, BANK_HALF, DC)
	Bed = (WaterLevel() - Depth) * (1.0 - Bank) + FloorZ(X) * Bank + Fbm(X, Y, 300.0, 151, 2) * 10.0
	Channel = Smoothstep(BANK_HALF + 120.0, BANK_HALF, DC) * Smoothstep(SPRING_X - 300.0, SPRING_X + 100.0, X) * Smoothstep(POND_X + 300.0, POND_X - 300.0, X)
	H = H * (1 - Channel) + np.minimum(H, Bed) * Channel
	# 웅덩이: 기슭(타원 1)에서 수면 바로 아래, 가운데로 갈수록 깊게
	E = PondE(X, Y)
	PondBed = WaterLevel() - 20.0 - 170.0 * (1.0 - Smoothstep(0.3, 0.97, E))
	Pond = Smoothstep(1.15, 0.95, E)
	H = H * (1 - Pond) + np.minimum(H, PondBed) * Pond
	# 오솔길: 기복을 조금 펴 준다
	Path = Smoothstep(160.0, 60.0, np.abs(Y - PathY(X))) * Smoothstep(PORTAL_X - 600.0, PORTAL_X - 200.0, X) * Smoothstep(POND_X - POND_RX + 200.0, POND_X - POND_RX - 400.0, X)
	H = H * (1 - Path * 0.5) + (FloorZ(X) + 20.0) * Path * 0.5
	return X, Y, H


def BuildWeights(X, Y, H):
	# 레이어: 0 침엽수 낙엽(나무 아래 바탕), 1 흙·자갈(오솔길·개울 바닥·웅덩이 기슭), 2 풀·이끼(빈터·비탈 얼룩), 3 이끼 바위(가파른 곳)
	GY, GX = np.gradient(H, TERRAIN_SIZE / (TERRAIN_RES - 1))
	Slope = np.sqrt(GX * GX + GY * GY)
	Noise = Fbm(X, Y, 700.0, 161, 3)
	Water = Smoothstep(BANK_HALF + 140.0 + Noise * 110.0, BANK_HALF - 40.0, ChannelDist(X, Y)) * Smoothstep(SPRING_X - 400.0, SPRING_X, X)
	Water = np.maximum(Water, Smoothstep(1.22 + Noise * 0.05, 1.0, PondE(X, Y)))
	Path = Smoothstep(130.0 + Noise * 30.0, 55.0, np.abs(Y - PathY(X))) * Smoothstep(PORTAL_X - 600.0, PORTAL_X - 200.0, X) * Smoothstep(POND_X - POND_RX + 200.0, POND_X - POND_RX - 400.0, X)
	Dirt = np.maximum(Water, Path)
	Rock = np.clip(Smoothstep(0.6, 1.0, Slope + Noise * 0.15), 0, 1)
	Grass = np.clip(Smoothstep(0.1, 0.5, Fbm(X, Y, 1800.0, 171, 3) + Noise * 0.2) * 0.85, 0, 1)
	W3 = Rock
	W2 = Grass * (1 - W3) * (1 - Dirt)
	W1 = Dirt * (1 - W3)
	W0 = np.clip(1 - W1 - W2 - W3, 0, 1)
	Stack = np.stack([W0, W1, W2, W3], axis=-1)
	Stack = Stack / np.maximum(Stack.sum(axis=-1, keepdims=True), 1e-6)
	Bytes = np.floor(Stack * 255.0 + 0.5).astype(np.int32)
	Bytes[..., 0] += 255 - Bytes.sum(axis=-1)  # 합 255 맞춤
	Bytes = np.clip(Bytes, 0, 255).astype(np.uint32)
	return Bytes[..., 0] | (Bytes[..., 1] << 8) | (Bytes[..., 2] << 16) | (Bytes[..., 3] << 24), Slope


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
	# 지형 높이/경사 표본 (격자 쌍선형 — 셀 대각선 차이는 무시해도 될 만큼 작다)
	def __init__(self, H, Slope):
		self.H = H
		self.Slope = Slope
		self.Cell = TERRAIN_SIZE / (TERRAIN_RES - 1)

	def _Index(self, PX, PY):
		FX = (PX + TERRAIN_SIZE * 0.5) / self.Cell
		FY = (PY + TERRAIN_SIZE * 0.5) / self.Cell
		IX = int(np.clip(math.floor(FX), 0, TERRAIN_RES - 2))
		IY = int(np.clip(math.floor(FY), 0, TERRAIN_RES - 2))
		return IX, IY, FX - IX, FY - IY

	def __call__(self, PX, PY):
		IX, IY, TX, TY = self._Index(PX, PY)
		H = self.H
		Top = H[IY, IX] * (1 - TX) + H[IY, IX + 1] * TX
		Bot = H[IY + 1, IX] * (1 - TX) + H[IY + 1, IX + 1] * TX
		return float(Top * (1 - TY) + Bot * TY)

	def SlopeAt(self, PX, PY):
		IX, IY, _, _ = self._Index(PX, PY)
		return float(self.Slope[IY, IX])

	def Normal(self, PX, PY):
		E = self.Cell
		DX = (self(PX + E, PY) - self(PX - E, PY)) / (2 * E)
		DY = (self(PX, PY + E) - self(PX, PY - E)) / (2 * E)
		N = np.array([-DX, -DY, 1.0])
		return N / np.linalg.norm(N)


def InBounds(X, Y, Margin=300.0):
	Half = TERRAIN_SIZE * 0.5 - Margin
	return -Half < X < Half and -Half < Y < Half


# ---- 머티리얼 / 임포트 설정 ----------------------------------------------------------------------------------------
TERRAIN_TEXTURES = {
	"Needles": ("forest_leaves_04", [0.56, 0.5, 0.45]),
	"Ground":  ("forest_ground_04", [0.85, 0.85, 0.82]),
	"Moss":    ("leafy_grass", [0.5, 0.64, 0.4]),  # Hub와 같은 원본 (받아 둔 것)
	"Rock":    ("mossy_rock", [0.8, 0.85, 0.75]),
}


def WriteJson(Path, Doc):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")


def WriteMaterials():
	for Name, (Id, Tint) in TERRAIN_TEXTURES.items():
		Rel = f"../../../{PH}/{Id}/{Id}"
		WriteJson(os.path.join(CONTENT, MAT_DIR, f"Terrain{Name}.emat"), {
			"Name": f"ForestTerrain{Name}",
			"BaseColorFactor": Tint + [1.0],
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
		})
	# 엔진 내장 침엽수(foliage:pine)용: 정점 색(어두운 바늘잎) 그대로 + 거친 표면
	WriteJson(os.path.join(CONTENT, MAT_DIR, "ForestFarPine.emat"), {
		"Name": "ForestFarPine", "BaseColorFactor": [0.38, 0.45, 0.38, 1.0], "EmissiveFactor": [0.0, 0.0, 0.0],
		"Metallic": 0.0, "Roughness": 0.95, "NormalScale": 1.0, "OcclusionStrength": 1.0,
		"BaseColorTexture": "", "MetallicRoughnessTexture": "", "NormalTexture": "", "OcclusionTexture": "", "EmissiveTexture": "",
	})
	# 엔진 내장 풀 메시(foliage:grass)용: 정점 색 그대로 + 거친 표면 (그늘 숲 바닥이라 조금 누렇게)
	WriteJson(os.path.join(CONTENT, MAT_DIR, "ForestGrass.emat"), {
		"Name": "ForestGrass", "BaseColorFactor": [1.15, 1.1, 0.75, 1.0], "EmissiveFactor": [0.0, 0.0, 0.0],
		"Metallic": 0.0, "Roughness": 0.92, "NormalScale": 1.0, "OcclusionStrength": 1.0,
		"BaseColorTexture": "", "MetallicRoughnessTexture": "", "NormalTexture": "", "OcclusionTexture": "", "EmissiveTexture": "",
	})


# 임포트 설정: 스캔 에셋은 쿠킹 때 LOD0 삼각형 상한으로 줄인다 (실제 glTF 삼각형 수 기준 — API polycount는 틀릴 수 있다).
#   잎이 BLEND로 저장된 에셋은 마스크로 (그림자·깊이 정렬, 잎 솎아내기 LOD 대상)
#   PARTS 에셋은 한 파일에 변형 여러 개가 나란히(격자) 놓여 있어 변형마다 나눈 glTF(<Id>_<변형>.part.gltf)를 만들고 그것을 배치한다
#   — 상한은 변형 하나 기준
IMPORT_SETTINGS = {
	"fir_sapling_medium": {"BlendAsMasked": True, "MaxTriangles": 90000},  # 변형 3개 × 잎 카드 약 42~67만 (가지 4~9천은 그대로) — 숲 주력 나무
	"pine_sapling_small": {"MaxTriangles": 5000},                          # 변형 3개 × 잎 약 12~14만 (줄기 5백은 그대로)
	"pine_roots":         {"MaxTriangles": 8000},                          # 변형 2개, 원본 16만
	"dead_tree_trunk":    {"MaxTriangles": 12000},                         # 원본 10만
	"dead_tree_trunk_02": {"MaxTriangles": 14000},                         # 원본 8만
	"tree_stump_02":      {"MaxTriangles": 10000},
	"rock_moss_set_01":   {"MaxTriangles": 4000},                          # 바위 6개, 원본 6만
	"rock_moss_set_02":   {"MaxTriangles": 4000},                          # 바위 7개, 원본 6만
	"boulder_01":         {"MaxTriangles": 9000},
	"root_cluster_01":    {"MaxTriangles": 20000},                         # 원본 22만
	"grass_medium_02":    {"BlendAsMasked": True},                         # 변형 5개
	"fern_02":            {},                                              # 변형 4개 (Hub도 원본 파일을 쓴다 — 원본 설정 없음)
	"mountainside":       {"MaxTriangles": 40000},
	"shrub_04":           {"MaxTriangles": 3000},                          # 작은 덤불, 원본 2.7만
	"rock_face_01":       {"MaxTriangles": 12000},
	"dry_branches_medium_01": {"MaxTriangles": 3000},                      # 변형 3개
}
PARTS = ["fir_sapling_medium", "pine_sapling_small", "pine_roots", "rock_moss_set_01", "rock_moss_set_02", "grass_medium_02", "fern_02",
		 "dry_branches_medium_01"]
PART_NAMES = {}  # Id → [변형 접미사, ...]
# 잎 카드 알파가 별도 맵인 에셋: 나눈 glTF는 FetchDemoAssets.ps1이 합친 RGBA PNG(<Id>_<접두사>_diffalpha_2k.png)를 색 텍스처로 쓴다
#   (원본 JPG 색에는 알파가 없어 카드가 검은 사각형으로 보인다 — 잠금 파일 AlphaMaps와 맞춘다)
ALPHA_MAPS = {"fir_sapling_medium": "twigs", "pine_sapling_small": "twig"}
# 잎 카드뿐인 에셋(줄기 없음 — 나누지 않음): 나눈 glTF 전체의 색 텍스처를 알파 합친 PNG로 (Id → 잠금 파일 AlphaMaps 접두사, Hub ALPHA_FIX와 같음)
#   grass_medium_02·shrub_04는 잎이 실제 기하(UV가 잎 모양에 딱 맞음)라 대상이 아니다 (2026-10-04 확인)
WHOLE_ALPHA_MAPS = {"fern_02": ["*"]}
LEAF_TINT = [0.6, 0.72, 0.62, 1.0]
# 잎 상한 단계: 솎아내기는 남은 잎 카드를 키워 덮는 면적을 유지하므로 많이 줄이면 잎이 넓적해진다 — 걷는 길 바로 옆 나무만 촘촘하게.
#   (접미사, 상한) — 접미사 "" = IMPORT_SETTINGS 값(가장 촘촘). 모두 화면 크기 LOD가 더 줄인다
TRUNK_SETTINGS = {"fir_sapling_medium": {"MaxTriangles": 5000}}  # 줄기·가지 (변형 a는 1만 — QEM으로 절반까지만, 판이 되지 않게)
LEAF_TIERS = {"fir_sapling_medium": [("mid", {"BlendAsMasked": True, "MaxTriangles": 35000}), ("lo", {"BlendAsMasked": True, "MaxTriangles": 15000})]}


def WriteParts():
	# 변형마다: 그 노드 하나(수평 이동 제거, 높이 이동·회전·크기 유지) + 그 메시 하나만 남긴 glTF. 버퍼·텍스처는 원본 파일을 그대로 가리킨다
	for Id in PARTS:
		Folder = os.path.join(CONTENT, "Asset", "PolyHaven", Id)
		Source = json.load(open(os.path.join(Folder, f"{Id}.gltf"), encoding="utf-8"))
		if Id in WHOLE_ALPHA_MAPS:
			SwapAlphaImages(Source, Id, WHOLE_ALPHA_MAPS[Id])
		Roots = Source["scenes"][Source.get("scene", 0)]["nodes"]
		Names = []
		for Root in Roots:
			Node = dict(Source["nodes"][Root])
			assert "children" not in Node and "mesh" in Node, f"{Id}: 자식 노드 있는 변형은 지원 안 함"
			Suffix = Node.get("name", str(Root)).removeprefix(f"{Id}_").removesuffix("_LOD0") or str(Root)
			Translation = Node.get("translation", [0.0, 0.0, 0.0])
			Node["translation"] = [0.0, Translation[1], 0.0]
			Mesh = Source["meshes"][Node["mesh"]]
			Node["mesh"] = 0
			Doc = dict(Source)
			Doc["nodes"] = [Node]
			Doc["meshes"] = [Mesh]
			Doc["scenes"] = [{"name": Source["scenes"][0].get("name", "Scene"), "nodes": [0]}]
			Doc["scene"] = 0
			Files = {f"{Id}_{Suffix}.part.gltf": Doc}
			if Id in ALPHA_MAPS:
				# 잎 카드(알파 맵 머티리얼)는 따로 나눈다: 줄기·가지(삼각형 적음)는 원본 그대로, 잎만 상한으로 솎아낸다
				#   (한 파일이면 상한이 메시 비율로 나뉘어 가지가 납작한 판이 된다). 배치는 줄기 모델 + 자식 잎 모델
				Prefix = ALPHA_MAPS[Id]
				Leaf = [Prim for Prim in Mesh["primitives"] if Source["materials"][Prim["material"]]["name"].endswith(f"_{Prefix}")]
				Rest = [Prim for Prim in Mesh["primitives"] if Prim not in Leaf]
				assert Leaf and Rest, f"{Id}: 잎/줄기 프리미티브 나누기 실패"
				Doc["meshes"] = [dict(Mesh, primitives=Rest)]
				Leaves = dict(Doc)
				Leaves["meshes"] = [dict(Mesh, primitives=Leaf)]
				Leaves["images"] = [dict(Image, uri=Image["uri"].replace(f"{Prefix}_diff_2k.jpg", f"{Prefix}_diffalpha_2k.png")) for Image in Source["images"]]
				assert any("diffalpha" in Image["uri"] for Image in Leaves["images"]), f"{Id}: 색 텍스처 {Prefix}_diff 없음"
				# 잎 색: 원본은 밝은 올리브색이라 침엽수답게 조금 어둡고 푸르게
				Leaves["materials"] = [dict(Material, pbrMetallicRoughness=dict(Material.get("pbrMetallicRoughness", {}), baseColorFactor=LEAF_TINT))
					if Material["name"].endswith(f"_{Prefix}") else Material for Material in Source["materials"]]
				Files[f"{Id}_{Suffix}_leaves.part.gltf"] = Leaves
				for Tier, _ in LEAF_TIERS.get(Id, []):
					Files[f"{Id}_{Suffix}_leaves{Tier}.part.gltf"] = Leaves  # 같은 내용, 임포트 상한만 다름
			for FileName, Content in Files.items():
				with open(os.path.join(Folder, FileName), "w", encoding="utf-8", newline="\n") as File:
					json.dump(Content, File, separators=(",", ":"))
					File.write("\n")
			Names.append(Suffix)
		PART_NAMES[Id] = sorted(Names)


def WriteImportSettings():
	for Id, Settings in IMPORT_SETTINGS.items():
		if Id in PARTS:
			for Suffix in PART_NAMES[Id]:
				Name = f"{Id}_{Suffix}_leaves" if Id in ALPHA_MAPS else f"{Id}_{Suffix}"  # 잎을 나눈 에셋은 잎에만 상한 (줄기는 원본 + LOD)
				if Settings:
					WriteJson(os.path.join(CONTENT, "Asset", "PolyHaven", Id, f"{Name}.part.gltf.eimport"), Settings)
				if Id in TRUNK_SETTINGS:
					WriteJson(os.path.join(CONTENT, "Asset", "PolyHaven", Id, f"{Id}_{Suffix}.part.gltf.eimport"), TRUNK_SETTINGS[Id])
				for Tier, TierSettings in LEAF_TIERS.get(Id, []):
					WriteJson(os.path.join(CONTENT, "Asset", "PolyHaven", Id, f"{Id}_{Suffix}_leaves{Tier}.part.gltf.eimport"), TierSettings)
		elif Settings:
			WriteJson(os.path.join(CONTENT, "Asset", "PolyHaven", Id, f"{Id}.gltf.eimport"), Settings)


# ---- 개울 소리 (절차 생성 — 흐르는 물: 걸러낸 잡음 + 무작위 물방울 소리, 끊김 없는 반복) --------------------------------
def WriteStreamSound(Path, Seconds=12.0, Rate=32000):
	Rng = np.random.default_rng(5)
	N = int(Seconds * Rate)
	# 쉬익 소리: 백색 잡음을 원형(주기) FFT 대역 통과 → 반복 이음매 없음
	Spectrum = np.fft.rfft(Rng.standard_normal(N))
	Freq = np.fft.rfftfreq(N, 1.0 / Rate)
	Band = np.exp(-((np.log(np.maximum(Freq, 1.0)) - math.log(700.0)) ** 2) / (2 * 0.9 ** 2))
	Rush = np.fft.irfft(Spectrum * Band, N)
	Rush /= np.max(np.abs(Rush))
	# 느린 세기 변화 (주기적)
	T = np.arange(N) / Rate
	Rush *= 0.75 + 0.15 * np.sin(2 * math.pi * T / Seconds * 3) + 0.1 * np.sin(2 * math.pi * T / Seconds * 7 + 1.0)
	# 물방울: 짧게 감쇠하는 사인 + 위로 미끄러지는 음높이 (원형으로 겹쳐 씀)
	Drops = np.zeros(N)
	for _ in range(int(Seconds * 9)):
		Start = int(Rng.uniform(0, N))
		Length = int(Rate * Rng.uniform(0.03, 0.09))
		F0 = Rng.uniform(500.0, 1600.0)
		Tau = np.arange(Length) / Rate
		Pitch = F0 * (1.0 + Tau * Rng.uniform(4.0, 10.0))
		Phase = 2 * math.pi * np.cumsum(Pitch) / Rate
		Env = np.exp(-Tau / (Length / Rate * 0.3)) * (1 - np.exp(-Tau * 900.0))
		Index = (Start + np.arange(Length)) % N
		Drops[Index] += np.sin(Phase) * Env * Rng.uniform(0.15, 0.45)
	Mix = Rush * 0.55 + Drops * 0.5
	Mix = Mix / np.max(np.abs(Mix)) * 0.7
	Pcm = (Mix * 32767.0).astype("<i2")
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with wave.open(Path, "wb") as File:
		File.setnchannels(1)
		File.setsampwidth(2)
		File.setframerate(Rate)
		File.writeframes(Pcm.tobytes())


# ---- 폴리지 (.efoliage — 엔진 폴리지 시스템: 내장 풀 메시를 GPU 인스턴싱으로) -------------------------------------
def WriteFoliage(Path, Types):
	# Types: [(타입 dict, [(x, y, z, yaw, scale, nx, ny, nz), ...]), ...] — FFoliageInstance 32바이트 = 실수 8개
	Doc = {"Revision": 0, "Types": [], "Version": 1}
	for Type, Instances in Types:
		Entry = dict(Type)
		Entry["InstanceCount"] = len(Instances)
		Data = b"".join(struct.pack("<8f", *Instance) for Instance in Instances)
		Entry["Instances"] = base64.b64encode(Data).decode("ascii")
		Doc["Types"].append(Entry)
	WriteJson(Path, Doc)


def Model(Id, Variant=0):
	# 나눈 에셋은 변형 번호로 고른다 (번호는 변형 수로 나머지)
	if Id in PART_NAMES:
		Names = PART_NAMES[Id]
		return f"{PH}/{Id}/{Id}_{Names[Variant % len(Names)]}.part.gltf"
	return f"{PH}/{Id}/{Id}.gltf"


def FaceYaw(DX, DY):
	# Poly Haven 모델 정면 = 엔진 -X(glTF +Z) → 정면이 (DX, DY) 방향을 보게 하는 Yaw
	return math.degrees(math.atan2(-DY, -DX))


# ---- 씬 배치 --------------------------------------------------------------------------------------------------------
MAX_TREES = 1500
SCAN_TREE_DISTANCE = 4500.0  # cm: 개울에서 이보다 먼 비탈은 엔진 폴리지 침엽수만
FAR_PINE_DISTANCE = 2800.0  # cm: 엔진 폴리지 침엽수는 이보다 먼 곳 (스캔 나무 사이 빈틈을 메우는 띠 포함)
LEAF_TIER_DISTANCE = [(1300.0, ""), (3000.0, "mid")]  # cm: 개울·오솔길에서 이 거리 안 나무의 잎 단계 (그 밖은 "lo")
START_X = -8600.0           # 플레이어 시작 (상류 쪽 빈터, 하류 = 해 쪽을 본다)
PORTAL_X = -9700.0


def BuildScene(Height, Overview=None):
	Rng = random.Random(17)
	S = FScene()
	Grass = []   # 엔진 폴리지 인스턴스 (풀)
	FarPines = []  # 엔진 폴리지 인스턴스 (먼 비탈 침엽수)

	PondYValue = PondY()

	def StreamDist(X, Y):
		# 물(개울·웅덩이)까지 대략 거리 — 웅덩이 안은 0, 샘 위쪽은 개울 없음
		if ((X - POND_X) / POND_RX) ** 2 + ((Y - PondYValue) / POND_RY) ** 2 < 1.3:  # 노이즈 없는 타원 (빠름)
			return 0.0
		if X < SPRING_X - 300.0:
			return math.hypot(X - SPRING_X, Y - float(StreamY(SPRING_X)))
		return abs(Y - float(StreamY(X)))

	def PathDist(X, Y):
		return abs(Y - float(PathY(X)))

	StartY = float(PathY(START_X)) - 120.0  # 오솔길에서 개울 반대쪽 (둑 비탈에서 미끄러져 물에 들어가지 않게)
	PortalY = float(PathY(PORTAL_X))
	ClearingCenter = (START_X + 900.0, float(StreamY(START_X + 900.0)) - 300.0)

	def Place(Name, Id, X, Y, Yaw=0.0, Scale=1.0, Sink=0.0, Extra=None, Z=None, Tier=""):
		Ground = Height(X, Y) if Z is None else Z
		Variant = Rng.randrange(64)
		Root = S.Model(Name, Model(Id, Variant), (X, Y, Ground - Sink), Yaw, Scale, -1, Extra)
		if Id in ALPHA_MAPS:
			Leaves = f"_leaves{Tier}.part.gltf"
			S.Model(f"{Name}_Leaves", Model(Id, Variant).replace(".part.gltf", Leaves), (0, 0, 0), 0.0, 1.0, Parent=Root)
		return Root

	# ---- 환경: 이른 아침(7시 반) 해가 하류(+X) 쪽 낮게 + 대기 하늘 + 골짜기에 깔린 아침 안개(볼류메트릭 — 나무 사이 빛줄기)
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.86, 0.66], "Intensity": 5.0}}, (0, 0, 3000), QuatFromEuler(Pitch=-20, Yaw=190))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {},
		"TimeOfDayComponent": {"TimeOfDay": 7.6, "DayLengthMinutes": 0.0, "MaxSunElevation": 50.0, "NorthAzimuth": 256.0, "AnimateInEditor": False},
		"SkyLightComponent": {"Intensity": 0.9},
		"HeightFogComponent": {
			"Color": [0.5, 0.6, 0.72], "Density": 0.0014, "HeightFalloff": 0.035, "StartDistance": 0.0, "MaxOpacity": 0.7,
			"DirectionalInscatteringColor": [0.12, 0.1, 0.06], "DirectionalInscatteringExponent": 6.0, "DirectionalInscatteringStartDistance": 1500.0,
			"Volumetric": True, "VolumetricDistance": 6000.0, "VolumetricAlbedo": [0.95, 0.95, 0.95], "VolumetricExtinctionScale": 1.0,
			"VolumetricAnisotropy": 0.5, "VolumetricDirectionalScale": 0.9, "VolumetricLocalLightScale": 1.0},
	}, (0, 0, 0))

	# ---- 지형 + 폴리지 엔티티
	S.Add("Terrain", {"TerrainComponent": {
		"Asset": "Terrain/Demo/Forest.eterrain", "Size": [TERRAIN_SIZE, TERRAIN_SIZE], "HeightRange": HEIGHT_RANGE,
		"Layer0Material": f"{MAT_DIR}/TerrainNeedles.emat", "Layer1Material": f"{MAT_DIR}/TerrainGround.emat",
		"Layer2Material": f"{MAT_DIR}/TerrainMoss.emat", "Layer3Material": f"{MAT_DIR}/TerrainRock.emat",
		"Layer0Tiling": 380.0, "Layer1Tiling": 300.0, "Layer2Tiling": 420.0, "Layer3Tiling": 600.0,
		"CastShadows": True, "Collision": True}})
	S.Add("Foliage", {"FoliageComponent": {"Asset": "Foliage/Demo/Forest.efoliage", "Visible": True}})

	# ---- 개울 + 웅덩이: 지형 전체를 덮는 물 상자 하나 (수면 위로 솟은 지형이 가린다 — 물은 파낸 개울 바닥·웅덩이에만 보인다)
	S.Add("Stream", {"WaterBodyComponent": {
		"Size": [TERRAIN_SIZE, TERRAIN_SIZE, 600.0], "ScatterColor": [0.008, 0.022, 0.018], "Absorption": [0.35, 0.12, 0.09],
		"NormalStrength": 0.45, "WaveScale": 110.0, "WaveSpeed": 8.0, "FlowDirection": 0.0, "FlowSpeed": 45.0,
		"FoamIntensity": 0.45, "FoamDistance": 10.0, "RefractionStrength": 0.04, "ReflectionIntensity": 0.5, "Roughness": 0.06}},
		(0.0, 0.0, WaterLevel() - 300.0))
	# 개울 속 바위 (물살이 부딪히는 자리 — 굽이마다 몇 개씩)
	for Index, SX in enumerate(np.linspace(SPRING_X + 800.0, POND_X - POND_RX - 200.0, 26)):
		for Rock in range(Rng.randrange(2, 5)):
			RX = float(SX) + Rng.uniform(-250.0, 250.0)
			RY = float(StreamY(RX)) + Rng.uniform(-220.0, 220.0)
			Place(f"StreamRock_{Index}_{Rock}", "boulder_01", RX, RY, Rng.uniform(0, 360), Rng.uniform(0.6, 1.2), Sink=40.0)

	# 개울 소리: 하류 쪽으로 몇 군데 (공간 음향, 반복)
	for Index, SX in enumerate([-10800.0, -7600.0, -3800.0, 600.0, 5200.0, 8800.0]):
		S.Add(f"StreamSound_{Index}", {"AudioSourceComponent": {
			"ClipAsset": "Audio/Demo/ForestStream.wav", "Volume": 0.55, "Pitch": 0.92 + 0.05 * Index, "Loop": True, "PlayOnStart": True,
			"Spatial": True, "MinDistance": 300.0, "MaxDistance": 3500.0}},
			(SX, float(StreamY(SX)), float(FloorZ(SX))))

	# ---- 배치 도우미: 겹침 방지 격자
	Occupied = []

	def Free(X, Y, Radius):
		for OX, OY, OR in Occupied:
			if (X - OX) ** 2 + (Y - OY) ** 2 < (Radius + OR) ** 2:
				return False
		return True

	def Reserve(X, Y, Radius):
		Occupied.append((X, Y, Radius))

	# 포털·시작 자리 비움
	Reserve(PORTAL_X, PortalY, 450.0)
	Reserve(START_X, StartY, 350.0)

	# ---- 큰 바위·절벽 노두 (북쪽 비탈) — 나무보다 먼저 자리 잡는다
	Outcrops = 0
	Tries = 0
	while Outcrops < 22 and Tries < 6000:
		Tries += 1
		X = Rng.uniform(-11500.0, 11500.0)
		Y = float(StreamY(X)) + Rng.uniform(2200.0, 6000.0) * (1 if Rng.random() < 0.8 else -1)
		if not InBounds(X, Y, 1200.0) or Height.SlopeAt(X, Y) < 0.35 or not Free(X, Y, 700.0):
			continue
		N = Height.Normal(X, Y)
		Yaw = math.degrees(math.atan2(-N[1], -N[0]))  # 노두 정면(-X)이 비탈 아래를 보게
		if Outcrops % 2 == 0:
			Place(f"Outcrop_{Outcrops}", "mountainside", X, Y, Yaw + Rng.uniform(-20, 20), Rng.uniform(0.7, 1.0), Sink=180.0)
		else:
			Place(f"Outcrop_{Outcrops}", "rock_face_01", X, Y, Yaw + Rng.uniform(-25, 25), Rng.uniform(1.0, 1.4), Sink=80.0)
		Reserve(X, Y, 600.0)
		Outcrops += 1

	# 이끼 바위 무리 (골짜기 바닥·비탈 아래)
	for Index in range(26):
		for _ in range(200):
			X = Rng.uniform(-12000.0, 12000.0)
			Y = float(StreamY(X)) + Rng.uniform(-3200.0, 3200.0)
			if InBounds(X, Y) and StreamDist(X, Y) > 650.0 and PathDist(X, Y) > 300.0 and Free(X, Y, 300.0):
				Id = "rock_moss_set_01" if Index % 2 == 0 else "rock_moss_set_02"
				Place(f"MossRocks_{Index}", Id, X, Y, Rng.uniform(0, 360), Rng.uniform(0.8, 1.3), Sink=25.0)
				Reserve(X, Y, 250.0)
				break
	# 개울가 둥근 바위
	for Index in range(70):
		for _ in range(100):
			X = Rng.uniform(SPRING_X + 200.0, POND_X - POND_RX)
			Y = float(StreamY(X)) + Rng.choice([-1, 1]) * Rng.uniform(250.0, 650.0)
			if Free(X, Y, 120.0):
				Place(f"BankBoulder_{Index}", "boulder_01", X, Y, Rng.uniform(0, 360), Rng.uniform(0.6, 1.5), Sink=30.0)
				Reserve(X, Y, 110.0)
				break
	# 둑 이끼 돌 (반쯤 물에 잠긴 것 포함 — 둑 선을 깨뜨린다)
	for Index in range(110):
		for _ in range(100):
			if Index % 4 == 3:  # 웅덩이 기슭
				Angle = Rng.uniform(0, 2 * math.pi)
				X, Y = POND_X + math.cos(Angle) * POND_RX * 1.02, PondY() + math.sin(Angle) * POND_RY * 1.02
			else:
				X = Rng.uniform(SPRING_X + 200.0, POND_X - POND_RX)
				Y = float(StreamY(X)) + Rng.choice([-1, 1]) * Rng.uniform(180.0, 560.0)
			if Free(X, Y, 70.0):
				Id = "rock_moss_set_01" if Index % 2 else "rock_moss_set_02"
				Place(f"BankStone_{Index}", Id, X, Y, Rng.uniform(0, 360), Rng.uniform(0.35, 0.8), Sink=15.0)
				Reserve(X, Y, 60.0)
				break

	# ---- 쓰러진 나무·그루터기·뿌리
	# 개울을 가로지르는 통나무 다리 (시작 자리에서 보이는 첫 굽이)
	BridgeX = START_X + 2600.0
	BridgeY = float(StreamY(BridgeX))
	# 모델 긴 축 = 엔진 로컬 Y(glTF X) → Yaw = 개울 방향이면 개울을 가로지른다. 길이 4m × 2.3 = 9m로 둑과 둑에 걸친다 (굵기는 1.3배)
	Tangent = math.degrees(math.atan2(float(StreamY(BridgeX + 50.0) - StreamY(BridgeX - 50.0)), 100.0))
	Place("LogBridge", "dead_tree_trunk_02", BridgeX, BridgeY, Tangent + 8.0, (1.3, 2.3, 1.3), Z=float(FloorZ(BridgeX)) + 30.0)
	Reserve(BridgeX, BridgeY, 500.0)
	Logs = 0
	while Logs < 18:
		X = Rng.uniform(-12000.0, 12000.0)
		Y = float(StreamY(X)) + Rng.uniform(-4500.0, 4500.0)
		if not InBounds(X, Y) or StreamDist(X, Y) < 700.0 or PathDist(X, Y) < 350.0 or not Free(X, Y, 260.0):
			continue
		Id = "dead_tree_trunk" if Logs % 3 else "dead_tree_trunk_02"
		Place(f"Log_{Logs}", Id, X, Y, Rng.uniform(0, 360), Rng.uniform(0.9, 1.3), Sink=8.0)
		Reserve(X, Y, 200.0)
		Logs += 1
	Stumps = 0
	while Stumps < 22:
		X = Rng.uniform(-12000.0, 12000.0)
		Y = float(StreamY(X)) + Rng.uniform(-4000.0, 4000.0)
		if not InBounds(X, Y) or StreamDist(X, Y) < 600.0 or PathDist(X, Y) < 250.0 or not Free(X, Y, 120.0):
			continue
		Id = "tree_stump_02" if Stumps % 2 else "tree_stump_01"
		Place(f"Stump_{Stumps}", Id, X, Y, Rng.uniform(0, 360), Rng.uniform(0.8, 1.2), Sink=6.0)
		Reserve(X, Y, 120.0)
		Stumps += 1
	for Index in range(8):
		for _ in range(200):
			X = Rng.uniform(SPRING_X + 300.0, POND_X - POND_RX)
			Y = float(StreamY(X)) + Rng.choice([-1, 1]) * Rng.uniform(450.0, 800.0)
			if InBounds(X, Y) and PathDist(X, Y) > 250.0 and Free(X, Y, 260.0):
				Place(f"BankRoots_{Index}", "root_cluster_01", X, Y, FaceYaw(0, float(StreamY(X)) - Y) + Rng.uniform(-30, 30), Rng.uniform(0.7, 1.0), Sink=20.0)
				Reserve(X, Y, 220.0)
				break

	# ---- 나무 (전나무: fir_sapling_medium 원본 8.9m → 크기 1.3~2.3배). 골짜기 바닥은 성기게, 비탈은 빽빽하게
	Trees = []
	for _ in range(60000):
		if len(Trees) >= MAX_TREES:
			break
		X = Rng.uniform(-12400.0, 12400.0)
		Y = Rng.uniform(-12400.0, 12400.0)
		D = StreamDist(X, Y)
		if D < 650.0 or PathDist(X, Y) < 300.0 or D > SCAN_TREE_DISTANCE:
			continue
		if math.hypot(X - ClearingCenter[0], Y - ClearingCenter[1]) < 900.0:
			continue  # 시작 빈터 (햇빛이 드는 자리)
		Spacing = 480.0 if D < 4500.0 else 620.0  # 보이는 골짜기 쪽 빽빽하게, 안개 너머 먼 비탈은 성기게
		if D < 1500.0 and Rng.random() < 0.35:
			continue  # 개울가 바닥은 조금 트이게
		if Height.SlopeAt(X, Y) > 0.9 or not Free(X, Y, Spacing * 0.5):
			continue
		Scale = Rng.uniform(1.3, 2.3) if D > 1400.0 else Rng.uniform(1.1, 1.8)
		Trees.append((X, Y, Scale))
		Reserve(X, Y, Spacing * 0.5)
	for Index, (X, Y, Scale) in enumerate(Trees):
		Squash = Rng.uniform(0.88, 1.08)
		Near = min(StreamDist(X, Y), PathDist(X, Y))
		Tier = next((Name for Limit, Name in LEAF_TIER_DISTANCE if Near < Limit), "lo")
		Place(f"Fir_{Index}", "fir_sapling_medium", X, Y, Rng.uniform(0, 360), (Scale * Squash, Scale * Squash, Scale), Sink=25.0, Tier=Tier,
			Extra={"CapsuleColliderComponent": {"Radius": 14.0, "HalfHeight": 220.0, "Offset": [0.0, 0.0, 250.0]}})  # 줄기 충돌 (크기 배율 적용)
		if Index % 9 == 0:
			Place(f"Fir_{Index}_Roots", "pine_roots", X, Y, Rng.uniform(0, 360), Scale * 0.55, Sink=4.0)

	# 먼 비탈: 엔진 폴리지 침엽수 (스캔 나무 띠 바깥 ~ 지형 끝, 겹침 격자는 따로 — 스캔 나무와 섞이는 경계 띠만 확인)
	PineRng = np.random.default_rng(31)
	for X, Y in PineRng.uniform(-12500.0, 12500.0, size=(30000, 2)):
		X, Y = float(X), float(Y)
		D = StreamDist(X, Y)
		EndSlope = X < SPRING_X or X > POND_X + POND_RX  # 골짜기 양 끝 비탈은 더 가까이까지
		if D < (700.0 if EndSlope else FAR_PINE_DISTANCE) or Height.SlopeAt(X, Y) > 1.1 or PineRng.random() > 0.45:
			continue
		if math.hypot(X - PORTAL_X, Y - PortalY) < 1800.0:
			continue  # 포털 뒤 하늘선에 홀로 선 원뿔이 보이지 않게
		if D < SCAN_TREE_DISTANCE + 300.0 and not Free(X, Y, 300.0):  # 스캔 나무와 섞이는 띠: 겹치지 않게
			continue
		N = Height.Normal(X, Y)
		FarPines.append((X, Y, Height(X, Y), float(PineRng.uniform(0, 360)), float(PineRng.uniform(1.5, 2.5)), float(N[0]), float(N[1]), float(N[2])))

	# ---- 하층: 어린 소나무·고사리·풀 덤불·마른 가지
	def Scatter(Prefix, Id, Count, Radius, ScaleRange, MinStream, MaxStream, Sink=2.0, Cluster=None):
		Placed = 0
		for _ in range(Count * 40):
			if Placed >= Count:
				break
			if Cluster and Rng.random() < 0.6:
				CX, CY, CR = Cluster[Rng.randrange(len(Cluster))]
				Angle, Dist = Rng.uniform(0, 2 * math.pi), Rng.uniform(0, CR)
				X, Y = CX + math.cos(Angle) * Dist, CY + math.sin(Angle) * Dist
			else:
				X = Rng.uniform(-12000.0, 12000.0)
				Y = float(StreamY(X)) + Rng.uniform(-MaxStream, MaxStream)
			D = StreamDist(X, Y)
			if not InBounds(X, Y) or D < MinStream or D > MaxStream or PathDist(X, Y) < 150.0 or not Free(X, Y, Radius):
				continue
			if Height.SlopeAt(X, Y) > 0.8:
				continue
			Place(f"{Prefix}_{Placed}", Id, X, Y, Rng.uniform(0, 360), Rng.uniform(*ScaleRange), Sink=Sink)
			Reserve(X, Y, Radius * 0.6)
			Placed += 1

	# 고사리는 개울가와 나무 사이 그늘에 무리지어
	FernClusters = [(X + Rng.uniform(-300, 300), Y + Rng.uniform(-300, 300), 450.0) for X, Y, _ in Trees[::7]]
	FernClusters += [(SX, float(StreamY(SX)) + Rng.choice([-1, 1]) * 700.0, 500.0) for SX in np.linspace(-11500.0, 11500.0, 18)]
	FernClusters += [(SX, float(PathY(SX)) + Side * Rng.uniform(220.0, 400.0), 260.0) for SX in np.linspace(-11000.0, 11000.0, 40) for Side in (-1, 1)]
	Scatter("Fern", "fern_02", 750, 55.0, (0.8, 1.5), 480.0, 6000.0, Cluster=FernClusters)
	Scatter("GrassClump", "grass_medium_02", 260, 40.0, (1.6, 2.6), 420.0, 2500.0, Sink=1.0,
		Cluster=[(SX, float(StreamY(SX)) + Rng.choice([-1, 1]) * 560.0, 380.0) for SX in np.linspace(-11500.0, 11500.0, 30)] + [ClearingCenter + (900.0,)])
	Scatter("Sapling", "pine_sapling_small", 160, 90.0, (0.9, 2.4), 900.0, 7000.0, Sink=3.0)
	Scatter("Shrub", "shrub_04", 90, 60.0, (1.2, 2.0), 600.0, 5000.0)
	Scatter("Branches", "dry_branches_medium_01", 60, 80.0, (0.8, 1.3), 700.0, 5000.0, Sink=1.0)

	# ---- 엔진 폴리지 풀 (골짜기 바닥: 빈터·개울가는 빽빽, 숲 속은 드문드문)
	GrassRng = np.random.default_rng(23)
	Candidates = GrassRng.uniform(-12300.0, 12300.0, size=(260000, 2))
	for X, Y in Candidates:
		X, Y = float(X), float(Y)
		D = StreamDist(X, Y)
		if D < BANK_HALF + 30.0 or D > 4200.0 or PathDist(X, Y) < 70.0:
			continue
		Clear = math.hypot(X - ClearingCenter[0], Y - ClearingCenter[1]) < 1100.0
		Keep = 0.6 if Clear else (0.45 if D < 1200.0 else 0.1)
		if GrassRng.random() > Keep or Height.SlopeAt(X, Y) > 0.6:
			continue
		Z = Height(X, Y)
		N = Height.Normal(X, Y)
		Grass.append((X, Y, Z, float(GrassRng.uniform(0, 360)), float(GrassRng.uniform(0.3, 0.65)), float(N[0]), float(N[1]), float(N[2])))

	# ---- 돌아가는 포털: 오솔길 상류 끝, 하류(+X)를 보는 성문 + 양옆 그루터기 등불 (DemoPortal.lua — Hub로, 돌아가기 자리 기억 안 함)
	Portal = S.Add("Portal_Hub", {"ScriptComponent": {"ScriptAsset": "Scripts/Demo/DemoPortal.lua", "ExecutionLocation": 0,
		"PropertyOverrides": json.dumps({"TargetScene": {"Asset": "Scenes/Demo/Hub.escene"}, "Label": "Hub로", "Ready": True,
			"RememberReturn": False}, ensure_ascii=False)}},
		(PORTAL_X, PortalY, Height(PORTAL_X, PortalY)), QuatFromEuler(Yaw=0.0))
	S.Model("Portal_Hub_Door", Model("large_castle_door"), (0, 0, -3), 0.0, 1.0, Parent=Portal)
	for Side in (-1, 1):
		S.Model("Portal_Hub_Post", Model("tree_stump_01"), (40, Side * 150, -25), 40.0 * Side, 0.45, Parent=Portal)
		S.Model("Portal_Hub_Lantern", Model("wooden_lantern_01"), (40, Side * 150, 8), 15.0 * Side, 1.3, Parent=Portal)
	S.Add("Portal_Hub_Glow", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Demo/PortalGlow.emat"}},
		(18, 0, 148), None, (0.02, 1.85, 2.85), Parent=Portal)
	S.Add("Portal_Hub_Light", {"PointLightComponent": {"Color": [0.5, 0.75, 1.0], "Intensity": 1.5, "Radius": 350.0, "CastShadows": False}}, (80, 0, 220), Parent=Portal)

	if Overview:
		S.Add("OverviewCamera", {"CameraComponent": {"FovYDegrees": 70.0, "NearZ": 10.0, "FarZ": 40000.0, "Primary": True, "Priority": 100}},
			Overview[:3], QuatFromEuler(Pitch=Overview[3], Yaw=Overview[4]))
	else:
		# 플레이어 (상류 빈터 오솔길에서 하류 = 아침 해 쪽을 보며 시작) — 프리팹 인스턴스는 루트만 저장 (PrefabLink.Root = 자기 인덱스)
		PlayerIndex = len(S.Entities)
		S.Add("Player", {
			"PrefabInstanceComponent": {"Asset": "Prefabs/Demo/DemoPlayer.eprefab", "Overrides": ""},
			"PrefabLinkComponent": {"Id": "1", "Root": PlayerIndex}},
			(START_X, StartY, Height(START_X, StartY) + 110.0))
	return S, Grass, FarPines, len(Trees)


PINE_TYPE = {
	"Name": "FarPine", "Mesh": "foliage:pine", "Material": f"{MAT_DIR}/ForestFarPine.emat", "Density": 4.0,
	"MinScale": 1.5, "MaxScale": 2.5, "MaxSlope": 50.0, "MinHeight": -1.0e7, "MaxHeight": 1.0e7, "AlignToNormal": False, "RandomYaw": True,
	"ZOffset": -30.0, "CullDistance": 13000.0, "ShadowDistance": 6000.0, "Collision": False, "CollisionRadius": 25.0, "CollisionHeight": 400.0,
}

GRASS_TYPE = {
	"Name": "ForestGrass", "Mesh": "foliage:grass", "Material": f"{MAT_DIR}/ForestGrass.emat", "Density": 20.0,
	"MinScale": 0.3, "MaxScale": 0.65, "MaxSlope": 35.0, "MinHeight": -1.0e7, "MaxHeight": 1.0e7, "AlignToNormal": True, "RandomYaw": True,
	"ZOffset": -2.0, "CullDistance": 4500.0, "ShadowDistance": 0.0, "Collision": False, "CollisionRadius": 25.0, "CollisionHeight": 400.0,
}

# 확인용 시점: 이름 → (X, Y, 지면 위 높이, Pitch, Yaw). --overview=<이름>[,<이름>...] 또는 --overview=x,y,높이,pitch,yaw
OVERVIEW_VIEWS = {
	"start":  (START_X - 300.0, float(PathY(START_X)) - 120.0, 260.0, -6.0, 8.0),    # 시작 자리에서 하류(해) 쪽
	"high":   (-10600.0, -3000.0, 2600.0, -16.0, 18.0),                                 # 남서쪽 비탈 위에서 골짜기 전체
	"west":   (-2500.0, float(PathY(-2500.0)) + 60.0, 220.0, -5.0, 185.0),              # 상류 쪽 (해를 등지고 숲이 밝게)
	"pond":   (POND_X - 3600.0, PondY() - 900.0, 300.0, -8.0, 12.0),                     # 하류 끝 웅덩이
	"bridge": (START_X + 1500.0, float(StreamY(START_X + 1500.0)) - 650.0, 200.0, -6.0, 35.0),  # 통나무 다리 근처 개울
}


def Main():
	Views = []
	for Arg in sys.argv:
		if Arg == "--overview":
			Views.append(("Overview", OVERVIEW_VIEWS["start"]))
		elif Arg.startswith("--overview="):
			Value = Arg.split("=", 1)[1]
			if Value[0].isalpha():
				Views += [(f"Overview_{Name}", OVERVIEW_VIEWS[Name]) for Name in Value.split(",")]
			else:
				Views.append(("Overview", tuple(float(V) for V in Value.split(","))))
	X, Y, H = BuildHeights()
	Weights, Slope = BuildWeights(X, Y, H)
	WriteTerrain(os.path.join(CONTENT, "Terrain", "Demo", "Forest.eterrain"), H, Weights)
	WriteMaterials()
	WriteParts()
	WriteImportSettings()
	WriteStreamSound(os.path.join(CONTENT, "Audio", "Demo", "ForestStream.wav"))
	Sampler = FHeightSampler(H, Slope)
	Scene, Grass, FarPines, TreeCount = BuildScene(Sampler)
	WriteFoliage(os.path.join(CONTENT, "Foliage", "Demo", "Forest.efoliage"), [(GRASS_TYPE, Grass), (PINE_TYPE, FarPines)])
	Scene.Save(os.path.join(CONTENT, "Scenes", "Demo", "Forest.escene"))
	print(f"Forest 생성: 엔티티 {len(Scene.Entities)}개 (나무 {TreeCount}), 풀 {len(Grass)}개, 먼 침엽수 {len(FarPines)}개, 지형 {TERRAIN_RES}² 높이 {H.min():.0f}~{H.max():.0f}cm")
	for Name, View in Views:
		View = (View[0], View[1], Sampler(View[0], View[1]) + View[2], View[3], View[4])
		OverviewScene, _, _, _ = BuildScene(Sampler, View)
		OverviewScene.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_Forest{Name}.escene"))
		print(f"확인용 변형: Scenes/Demo/_Forest{Name}.escene (커밋하지 않음)")


if __name__ == "__main__":
	Main()
