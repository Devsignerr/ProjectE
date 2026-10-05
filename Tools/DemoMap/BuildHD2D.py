# 데모 서브맵 "HD2D"(HD-2D 느낌 — 3D 디오라마 + 도트 스프라이트 캐릭터) 생성:
#   도트 아트(HD2DArt.py → Sprites/HD2D/) + 지형(.eterrain)·지형/풀 머티리얼 + 폴리지 + 프리팹(플레이어·슬라임·효과 조각) + 씬(Scenes/Demo/HD2D.escene)
#   실행: python Tools/DemoMap/BuildHD2D.py [--views]   (Poly Haven 에셋은 Scripts/FetchDemoAssets.ps1로 먼저 받는다)
#   --views: 확인용 변형도 쓴다(커밋하지 않음) — Scenes/Demo/_HD2D_<시점>.escene(플레이어 시작 자리만 다름), _HD2DAutoPlay.escene(자동 플레이 검증)
#   보여 주는 것: 원근 고정 시점 카메라(좁은 시야각 — 디오라마) + 조명 받는 Masked 도트 스프라이트(그림자 드리움, TAA 떨림 없음),
#                 해 질 녘 하늘·볼류메트릭 안개, 등불·창문·대장간·모닥불 점광원(깜빡임), 파티클(불꽃·연기·반딧불), 물(연못), 풀 폴리지
#   게임: 게임플레이 콘텐츠(무기·인벤토리·적·보스·보물상자·마을 사람·상점·퀘스트·UI·데이터 표)는 HD2DGameplay.py — Scripts/Demo/HD2D/*.lua
#   좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 카메라는 +Y 쪽 위에서 -Y를 내려다본다 → 화면 오른쪽 = +X, 화면 안쪽 = -Y.
#         마을 = 서쪽(-X), 들판 = 동쪽(+X), 큰 건물·산은 안쪽(-Y)에 두고 카메라 쪽(+Y)에는 낮은 물체만 둔다 (캐릭터를 가리지 않게)
#   배치를 바꿀 때는 씬 파일이 아니라 이 스크립트를 고치고 다시 실행한다 (결정적 — 고정 시드)
import json
import math
import os
import random
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402
from AssetFixes import AlphaModel  # noqa: E402
from BuildCampfire import Fbm, FlickerScript, PlainMaterial, Smoothstep, WriteFoliage, WriteJson  # noqa: E402
import HD2DArt  # noqa: E402
import HD2DEnvironment as Env  # noqa: E402
import HD2DGameplay  # noqa: E402
import HD2DMapArt  # noqa: E402
import ModelBounds  # noqa: E402

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTENT = os.path.join(ROOT, "Projects", "Sample", "Content")
PH      = "Asset/PolyHaven"
KK      = "Asset/KayKit/Village"
MAT_DIR = "Materials/Demo/HD2D"
PREFABS = "Prefabs/Demo/HD2D"
KS      = 4.5  # KayKit 마을 키트 배율 (1 유닛 = 1m 말판 → 도트 캐릭터 170cm에 맞춘 크기)

# ---- 지형 ----------------------------------------------------------------------------------------------------------
TERRAIN_SIZE = 16000.0
TERRAIN_RES  = 321      # 칸 50cm
HEIGHT_RANGE = 8000.0
POND         = (2500.0, -1050.0, 700.0, 430.0)  # 가운데 X, Y, 반지름 X, Y
POND_LEVEL   = -30.0
PLAZA        = (-2400.0, -250.0, 1050.0, 680.0)
PATH = [(-2400.0, -250.0), (-1300.0, -100.0), (-500.0, 60.0), (500.0, 180.0), (1500.0, 120.0), (2600.0, 300.0), (3700.0, 250.0), (4800.0, 380.0), (6500.0, 420.0)]
PATH_MILL = [(3700.0, 250.0), (3900.0, -500.0), (3950.0, -1200.0)]
# 놀이 영역 (보이지 않는 벽) / 카메라 초점 범위
PLAY_MIN = (-4400.0, -1950.0)
PLAY_MAX = (5400.0, 1350.0)
# 마을 뒤 단(옹벽 위 높은 터): 옹벽 앞면 Y, 동쪽 끝 X(개울 쪽 옆벽), 높이 / 광장에서 오르는 돌계단(가운데 X, 폭)
TERRACE_Y, TERRACE_EAST, TERRACE_H = -1000.0, -620.0, 150.0
STAIRS_X, STAIRS_W = -2400.0, 340.0
# 개울: 언덕 밑 폭포 웅덩이에서 마을과 들판 사이를 지나 카메라 쪽으로 (길과 만나는 곳에 나무 다리)
CREEK_BASE = [(-430.0, -2290.0), (-300.0, -1700.0), (-200.0, -950.0), (-160.0, -200.0), (-120.0, 500.0), (-20.0, 1300.0), (120.0, 2200.0), (260.0, 3200.0)]


def MeanderLine(Points, Step=140.0, Amplitude=60.0, Wavelength=1500.0):
	# 꺾은선을 촘촘히 나누고 진행 방향의 옆으로 사인 굽이를 더한다 (양 끝은 굽이 없음 — 폭포 웅덩이·화면 밖)
	Out, Travel = [], 0.0
	Total = sum(math.dist(A, B) for A, B in zip(Points[:-1], Points[1:]))
	for (AX, AY), (BX, BY) in zip(Points[:-1], Points[1:]):
		Length = math.dist((AX, AY), (BX, BY))
		NX, NY = -(BY - AY) / Length, (BX - AX) / Length
		Count = max(1, int(Length // Step))
		for K in range(Count):
			T = K / Count
			D = Travel + Length * T
			Fade = min(1.0, D / 600.0, (Total - D) / 600.0)
			Offset = math.sin(D / Wavelength * math.tau) * Amplitude * Fade
			Out.append((AX + (BX - AX) * T + NX * Offset, AY + (BY - AY) * T + NY * Offset))
		Travel += Length
	Out.append(Points[-1])
	return Out


CREEK = MeanderLine(CREEK_BASE)
CREEK_BED = -95.0
FALLS = (-430.0, -2300.0)        # 폭포 웅덩이 가운데 (폭포 면은 그 뒤 벼랑)
FALLS_BASIN = (-430.0, -2640.0)  # 벼랑 위 작은 못 (폭포가 넘쳐 흐르는 곳)
WHEAT = (4250.0, -1300.0, 5250.0, -300.0)  # 밀밭 (X0, Y0, X1, Y1 — Y1이 카메라 쪽 울타리)
# 들판 뒤 언덕 밑 성채 (modular_fort_01 조립 — Asset/DemoKits/HD2D/HD2DCastle.gltf): 바깥면(카메라 쪽) Y, 서쪽 끝 X, 배율.
#   카메라(피치 28·시야각 24)가 놀이 영역 뒤 15~20m까지만 보므로 더 멀면 화면 위로 잘려 보이지 않는다 → 놀이 영역 뒤 4m에 성벽 아랫부분이 보이게
CASTLE = (850.0, -2380.0, 0.6)
CASTLE_LENGTH = 4500.0  # 키트 단위 cm (탑 + 성벽 + 성문 + 성벽 + 탑)
CASTLE_KIT = "Asset/DemoKits/HD2D/HD2DCastle.gltf"
# 폭포 옆 벼랑 동굴 입구 (HD2DCave.escene으로 가는 이동 트리거) / 돌아왔을 때 나타나는 자리
CAVE_GATE = (330.0, -1985.0)
CAVE_EXIT_SPAWN = (330.0, -1650.0)
# 여관(선술집 겸) 문 앞 빈자리 — 저장 지점 등 게임 배치용
INN_DOOR_SPOT = (-3665.0, -1150.0)
NOTICE_BOARD = (-2780.0, -800.0)
EXTRA_FOLIAGE = []  # BuildScene이 채우는 추가 폴리지 (꽃·밀·긴 풀) — Main이 풀과 함께 쓴다
# 지도 화면·미니맵 (HD2DMapArt — Main이 장면을 만든 뒤 그림을 쓴다): 놀이 영역 + 지명 (이름, X, Y, Place|Exit)
MINIMAP = HD2DMapArt.Info("Village", (PLAY_MIN[0] - 100.0, PLAY_MIN[1] - 100.0, PLAY_MAX[0] + 100.0, PLAY_MAX[1] + 100.0), "하르트 마을과 황혼의 들판", [
	("하르트 마을", -2450.0, -420.0, "Place"), ("연못", 2500.0, -1050.0, "Place"), ("야영지", 1300.0, 1020.0, "Place"),
	("방앗간", 3950.0, -1180.0, "Place"), ("밀밭", 4760.0, -760.0, "Place"), ("수호자의 언덕", 4950.0, 760.0, "Place"),
	("동굴 유적", 330.0, -1800.0, "Exit")])


def InRect(X, Y, Rect, Margin):
	return (X > Rect[0] - Margin) & (X < Rect[2] + Margin) & (Y > Rect[1] - Margin) & (Y < Rect[3] + Margin)


def SegmentDistance(X, Y, Points):
	D = np.full(np.shape(X), 1.0e9)
	for (AX, AY), (BX, BY) in zip(Points[:-1], Points[1:]):
		VX, VY = BX - AX, BY - AY
		T = np.clip(((X - AX) * VX + (Y - AY) * VY) / (VX * VX + VY * VY), 0.0, 1.0)
		D = np.minimum(D, np.hypot(X - (AX + VX * T), Y - (AY + VY * T)))
	return D


def EllipseValue(X, Y, E):
	return np.sqrt(((X - E[0]) / E[2]) ** 2 + ((Y - E[1]) / E[3]) ** 2)


def BuildHeights():
	Coords = (np.arange(TERRAIN_RES) / (TERRAIN_RES - 1) - 0.5) * TERRAIN_SIZE
	X, Y = np.meshgrid(Coords, Coords)  # 배열 [행(Y), 열(X)]
	H = Fbm(X, Y, 1800.0, 3, 3) * 35.0 * Smoothstep(-800.0, 400.0, X)  # 들판 잔물결
	H = np.maximum(H, -12.0)  # 물 상자(수면 -30) 밖 웅덩이가 생기지 않게
	# 안쪽(-Y) 언덕: 놀이 영역 뒤로 솟아 산자락이 된다 / 서·동쪽 끝과 카메라 쪽 끝도 조금 올려 경계를 감춘다
	H += Smoothstep(-2100.0, -4200.0, Y) * (700.0 + Fbm(X, Y, 2600.0, 13) * 380.0)
	H += Smoothstep(-4600.0, -6500.0, X) * 500.0 + Smoothstep(5700.0, 7500.0, X) * 600.0
	H += Smoothstep(1700.0, 3500.0, Y) * 250.0
	# 방앗간 언덕
	H += np.exp(-(((X - 4000.0) / 900.0) ** 2 + ((Y + 1650.0) / 650.0) ** 2)) * 160.0
	# 폭포 벼랑: 개울 머리 뒤로 바위턱이 솟는다 (가운데 = 폭포, 양옆으로 낮아짐)
	H += Smoothstep(-2380.0, -2520.0, Y) * 380.0 * np.exp(-((X - FALLS[0]) / 1300.0) ** 2)
	# 성채 터: 성벽 띠를 언덕 높이 평균으로 고른다
	CX0, CY0, CS = CASTLE
	Strip = Smoothstep(250.0, 0.0, np.maximum(np.maximum(CX0 - 200.0 - X, X - (CX0 + CASTLE_LENGTH * CS + 200.0)), np.maximum(CY0 - 700.0 - Y, Y - (CY0 + 120.0))))
	Target = float(np.mean(H[Strip > 0.99])) if np.any(Strip > 0.99) else 0.0
	FALLS_INFO["CastleZ"] = Target
	H = H * (1.0 - Strip) + Target * Strip
	# 광장·길은 평평하게
	Flat = np.maximum(Smoothstep(1.25, 0.95, EllipseValue(X, Y, PLAZA)), Smoothstep(320.0, 160.0, SegmentDistance(X, Y, PATH)))
	H = H * (1.0 - Flat)
	# 마을 뒤 단 (옹벽이 가린다 — 경계는 한 칸으로 가파르게)
	H += TerraceMask(X, Y) * TERRACE_H
	# 벼랑 위 못: 둥글게 파 평평한 바닥
	BasinTop = float(np.max(H[np.hypot(X - FALLS_BASIN[0], Y - FALLS_BASIN[1]) < 60.0]))
	FALLS_INFO["Top"] = BasinTop
	Basin = Smoothstep(260.0, 150.0, np.hypot(X - FALLS_BASIN[0], Y - FALLS_BASIN[1]))
	H = H * (1.0 - Basin) + (BasinTop - 70.0) * Basin
	# 개울·폭포 웅덩이: 물길을 파 바닥 CREEK_BED (물 상자 수면 -30)
	Creek = np.maximum(Smoothstep(220.0, 105.0, SegmentDistance(X, Y, CREEK)), Smoothstep(320.0, 190.0, np.hypot(X - FALLS[0], Y - FALLS[1])))
	H = H * (1.0 - Creek) + CREEK_BED * Creek
	# 연못: 가장자리에서 완만히 파여 -110
	PondT = EllipseValue(X, Y, POND)
	H = H - Smoothstep(1.15, 0.55, PondT) * (110.0 + H)
	return X, Y, H


def TerraceMask(X, Y):
	# 동쪽 옆벽(개울 쪽)은 놀이 영역 뒤 끝까지만 — 그 너머 언덕에서는 단이 전체로 퍼져 경계선(턱)이 언덕을 타고 오르지 않게
	East = np.maximum(Smoothstep(TERRACE_EAST + 20.0, TERRACE_EAST - 20.0, X), Smoothstep(-2150.0, -2500.0, Y))
	return Smoothstep(TERRACE_Y + 20.0, TERRACE_Y - 20.0, Y) * East


FALLS_INFO = {}


def BuildWeights(X, Y, H):
	# 레이어: 0 풀, 1 흙길, 2 자갈 광장(+ 단 위 집 앞 길), 3 이끼 바위(경사·연못가·개울 바닥)
	GY, GX = np.gradient(H, TERRAIN_SIZE / (TERRAIN_RES - 1))
	Slope = np.sqrt(GX * GX + GY * GY)
	Noise = Fbm(X, Y, 500.0, 71, 3)
	Plaza = Smoothstep(1.02 + Noise * 0.06, 0.9 + Noise * 0.06, EllipseValue(X, Y, PLAZA))
	# 단 위 집 앞 자갈길 (옹벽 뒤 띠)
	Walk = TerraceMask(X, Y) * Smoothstep(TERRACE_Y - 420.0 + Noise * 60.0, TERRACE_Y - 330.0 + Noise * 60.0, Y) * Smoothstep(-4700.0, -4500.0, X)
	Plaza = np.maximum(Plaza, Walk)
	CreekD = SegmentDistance(X, Y, CREEK)
	PathD = np.minimum(SegmentDistance(X, Y, PATH), SegmentDistance(X, Y, PATH_MILL) + 40.0)
	Path = Smoothstep(150.0 + Noise * 40.0, 95.0 + Noise * 40.0, PathD) * (1.0 - Plaza) * Smoothstep(200.0, 280.0, CreekD)
	Wet = np.maximum(Smoothstep(1.35, 1.0, EllipseValue(X, Y, POND)) * 0.85, Smoothstep(300.0, 200.0, CreekD))
	Wet = np.maximum(Wet, Smoothstep(380.0, 250.0, np.hypot(X - FALLS[0], Y - FALLS[1])))
	Moss = np.maximum(Smoothstep(0.38, 0.65, Slope + Noise * 0.08), Wet)  # 완만한 언덕은 풀 (이끼 바위 텍스처가 노을빛에 주황으로 떴다)
	Moss = Moss * (1.0 - Plaza) * (1.0 - Path)
	W1, W2, W3 = Path, Plaza, Moss
	W0 = np.clip(1.0 - W1 - W2 - W3, 0.0, 1.0)
	Stack = np.stack([W0, W1, W2, W3], axis=-1)
	Stack = Stack / np.maximum(Stack.sum(axis=-1, keepdims=True), 1e-6)
	Bytes = np.floor(Stack * 255.0 + 0.5).astype(np.int32)
	Bytes[..., 0] += 255 - Bytes.sum(axis=-1)
	Bytes = np.clip(Bytes, 0, 255).astype(np.uint32)
	return Bytes[..., 0] | (Bytes[..., 1] << 8) | (Bytes[..., 2] << 16) | (Bytes[..., 3] << 24), Slope, Stack


def WriteTerrain(Path, H, Weights):
	import base64
	H16 = np.clip(np.round((H / HEIGHT_RANGE + 0.5) * 65535.0), 0, 65535).astype("<u2")
	Doc = {"Heights": base64.b64encode(H16.tobytes()).decode("ascii"), "Resolution": TERRAIN_RES, "Version": 1,
		   "Weights": base64.b64encode(Weights.astype("<u4").tobytes()).decode("ascii")}
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent="\t")
		File.write("\n")


class FHeightSampler:
	def __init__(self, H, Stack):
		self.H, self.Stack = H, Stack
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

	def GrassWeight(self, PX, PY):
		# 가장 가까운 격자점의 풀 레이어 비율 (배열)
		IX = np.clip(np.round((PX + TERRAIN_SIZE * 0.5) / self.Cell).astype(int), 0, TERRAIN_RES - 1)
		IY = np.clip(np.round((PY + TERRAIN_SIZE * 0.5) / self.Cell).astype(int), 0, TERRAIN_RES - 1)
		return self.Stack[IY, IX, 0]

	def Normal(self, PX, PY):
		E = self.Cell
		DX = (self(PX + E, PY) - self(PX - E, PY)) / (2 * E)
		DY = (self(PX, PY + E) - self(PX, PY - E)) / (2 * E)
		N = np.array([-DX, -DY, 1.0])
		return N / np.linalg.norm(N)


# ---- 머티리얼 -------------------------------------------------------------------------------------------------------
TERRAIN_TEXTURES = {
	"Grass":  ("leafy_grass", [0.82, 1.0, 0.62]),        # 노을빛에 너무 누렇지 않게 초록 쪽으로
	"Path":   ("forest_ground_04", [1.0, 0.92, 0.8]),
	"Cobble": ("cobblestone_floor_04", [0.95, 0.9, 0.85]),
	"Moss":   ("aerial_grass_rock", [0.85, 0.9, 0.7]),
}


def WriteMaterials():
	for Name, (Id, Tint) in TERRAIN_TEXTURES.items():
		Rel = f"../../../{PH}/{Id}/{Id}"
		WriteJson(os.path.join(CONTENT, MAT_DIR, f"Terrain{Name}.emat"), {
			"Name": f"HD2DTerrain{Name}", "BaseColorFactor": Tint + [1.0], "EmissiveFactor": [0.0, 0.0, 0.0],
			"Metallic": 1.0, "Roughness": 1.0, "NormalScale": 1.0, "OcclusionStrength": 1.0,
			"BaseColorTexture": f"{Rel}_diff_2k.jpg",
			"MetallicRoughnessTexture": f"{Rel}_arm_2k.jpg",  # Poly Haven ARM: R=AO, G=거칠기, B=금속
			"NormalTexture": f"{Rel}_nor_gl_2k.jpg", "OcclusionTexture": f"{Rel}_arm_2k.jpg", "EmissiveTexture": "",
		})
	# 풀(엔진 내장 foliage:grass): 정점 색 × 싱그러운 초록
	WriteJson(os.path.join(CONTENT, MAT_DIR, "WoodPost.emat"), PlainMaterial("HD2DWoodPost", (0.3, 0.19, 0.11, 1.0), 0.8))
	WriteJson(os.path.join(CONTENT, MAT_DIR, "Grass.emat"), PlainMaterial("HD2DGrass", (0.62, 1.0, 0.42, 1.0), 0.85))
	WriteJson(os.path.join(CONTENT, MAT_DIR, "GrassDry.emat"), PlainMaterial("HD2DGrassDry", (1.05, 0.95, 0.5, 1.0), 0.9))


GRASS_TYPE = {
	"Name": "Grass", "Mesh": "foliage:grass", "Material": f"{MAT_DIR}/Grass.emat", "Density": 20.0,
	"MinScale": 0.45, "MaxScale": 0.85, "MaxSlope": 40.0, "MinHeight": -1.0e7, "MaxHeight": 1.0e7, "AlignToNormal": True, "RandomYaw": True,
	"ZOffset": -2.0, "CullDistance": 6000.0, "ShadowDistance": 0.0, "Collision": False, "CollisionRadius": 25.0, "CollisionHeight": 400.0,
}
GRASS_DRY_TYPE = dict(GRASS_TYPE, Name="GrassDry", Material=f"{MAT_DIR}/GrassDry.emat", MinScale=0.35, MaxScale=0.6)


# ---- 프리팹 ---------------------------------------------------------------------------------------------------------
def Link(Id):
	return {"Id": str(Id), "Root": -1}


def Transform(Position=(0.0, 0.0, 0.0), Rotation=None, Scale=(1.0, 1.0, 1.0)):
	return {"Position": [float(V) for V in Position], "Rotation": Rotation or [0.0, 0.0, 0.0, 1.0], "Scale": [float(V) for V in Scale]}


def Sprite(Asset, Slice="", Lit=True, Shadows=True, Blend=3, Visible=True, Cutoff=0.5):
	# Blend: 0 알파, 1 프리멀티플라이드, 2 가산, 3 마스크 (ESpriteBlendMode 번호 — 씬 JSON은 번호)
	return {"Sprite": Asset, "Slice": Slice, "Color": [1, 1, 1, 1], "FlipX": False, "FlipY": False, "SortingLayer": "", "OrderInLayer": 0,
			"Lit": Lit, "CastShadows": Shadows, "Size": [0.0, 0.0], "Visible": Visible, "Blend": Blend, "AlphaCutoff": Cutoff, "SliceMode": 0}


def Flipbook(Path):
	return {"Flipbook": Path, "Speed": 1.0, "Playing": True, "StartTime": 0.0}


FLAT = QuatFromEuler(Roll=-90.0)  # 스프라이트 평면(XZ, 앞 +Y)을 바닥(XY, 앞 +Z)에 눕힌다 (+Roll = +Y가 아래로)


# ---- 씬 배치 --------------------------------------------------------------------------------------------------------
TIME_OF_DAY = 16.9          # 해 질 녘 (간이 모델 6~18시) — 고도 약 11도, 긴 그림자 + 따뜻한 빛
MAX_SUN_ELEVATION = 40.0
SUN_AZIMUTH = 118.0         # 해가 있는 쪽 = 카메라 쪽 왼편(-X, +Y) → 스프라이트 앞면이 빛을 받고 그림자는 안쪽 오른편으로
NORTH_AZIMUTH = SUN_AZIMUTH - 90.0 - (TIME_OF_DAY - 6.0) / 12.0 * 180.0

CAMERA_PITCH = -28.0
CAMERA_FOV = 24.0
CAMERA_DISTANCE = 2900.0
DOF_FOCAL_REGION = 300.0  # 초점 앞뒤 선명한 깊이 (cm) — 플레이어 주변 놀이 공간
PLAYER_START = (-2050.0, 250.0)
VIEW_STARTS = {
	"Village": (-2050.0, 250.0),
	"Market":  (-3100.0, -600.0),
	"Field":   (1700.0, 0.0),
	"Pond":    (2500.0, -300.0),
	"Mill":    (3800.0, -700.0),
	"Terrace": (-2400.0, -1150.0),
	"Falls":   (-850.0, -1350.0),
	"Bridge":  (350.0, 250.0),
	"Ruins":   (1500.0, -1100.0),
	"CaveGate": (330.0, -1600.0),
	"Castle":  (2300.0, -1450.0),
	"Inn":     (-3500.0, -1150.0),
}
OVERVIEW_VIEWS = {
	"Village": (-2300.0, 3600.0, 3200.0, -38.0, -90.0, 40.0),
	"Field":   (2400.0, 3800.0, 3400.0, -38.0, -90.0, 42.0),
	"Falls":   (-300.0, -900.0, 700.0, -12.0, -95.0, 45.0),
	"West":    (-6200.0, 600.0, 1600.0, -25.0, -20.0, 45.0),
}


class FBoundsCache:
	def __init__(self):
		self.Cache = {}
		ModelBounds._CONVERT = lambda P: (-P[2] * 100.0, P[0] * 100.0, P[1] * 100.0)

	def __call__(self, Asset):
		if Asset not in self.Cache:
			self.Cache[Asset] = ModelBounds.ComputeBounds(os.path.join(CONTENT, *Asset.split("/")))
		return self.Cache[Asset]


def BuildScene(Height, Start=PLAYER_START, AutoPlay=False):
	Rng = random.Random(41)
	S = FScene()
	Bounds = FBoundsCache()
	Occupied = []
	Grass = []

	def Free(X, Y, Radius):
		return all((X - OX) ** 2 + (Y - OY) ** 2 >= (Radius + OR) ** 2 for OX, OY, OR in Occupied)

	def Reserve(X, Y, Radius):
		Occupied.append((X, Y, Radius))

	def BoxCollider(Name, Center, Half, Yaw=0.0):
		return S.Add(Name, {"BoxColliderComponent": {"HalfExtents": [float(V) for V in Half]}}, Center, QuatFromEuler(Yaw=Yaw))

	def Place(Name, Asset, X, Y, Yaw=0.0, Scale=1.0, Sink=0.0, Z=None, Collide=False, Shrink=0.85, Pitch=0.0, Roll=0.0, Parent=-1):
		# 모델 + (선택) 경계 상자 콜라이더 (모델 로컬 경계를 배율·Yaw로 돌려 놓음, 가로는 Shrink만큼 줄여 걸리지 않게)
		Base = Height(X, Y) if Z is None else Z
		Sc = Scale if isinstance(Scale, (list, tuple)) else (Scale, Scale, Scale)
		Index = S.Add(Name, {"ModelComponent": {"AssetPath": Asset}}, (X, Y, Base - Sink), QuatFromEuler(Pitch, Yaw, Roll), Sc, Parent)
		if Collide:
			Lo, Hi = Bounds(Asset)
			CX, CY, CZ = [(Lo[I] + Hi[I]) * 0.5 * Sc[I] for I in range(3)]
			HX, HY, HZ = [(Hi[I] - Lo[I]) * 0.5 * Sc[I] for I in range(3)]
			R = math.radians(Yaw)
			WX, WY = X + CX * math.cos(R) - CY * math.sin(R), Y + CX * math.sin(R) + CY * math.cos(R)
			BoxCollider(f"{Name}_Collision", (WX, WY, Base - Sink + CZ), (HX * Shrink, HY * Shrink, max(HZ, 60.0)), Yaw)
			Reserve(WX, WY, max(HX, HY) * 0.9)
		return Index

	def KK_(Name, Id, X, Y, Yaw=0.0, Scale=KS, **Kw):
		return Place(Name, f"{KK}/{Id}.gltf", X, Y, Yaw, Scale, **Kw)

	def PH_(Name, Id, X, Y, Yaw=0.0, Scale=1.0, **Kw):
		Asset = AlphaModel(Id) if Id in ("wild_rooibos_bush", "fern_02", "jacaranda_tree") else f"{PH}/{Id}/{Id}.gltf"
		return Place(Name, Asset, X, Y, Yaw, Scale, **Kw)

	def Particles(Name, Asset, Pos, Speed=1.0):
		return S.Add(Name, {"ParticleSystemComponent": {"Asset": Asset, "Playing": True, "Speed": Speed}}, Pos)

	def Point(Name, Pos, Color, Intensity, Radius, Shadows=False, Flicker=None):
		Comps = {"PointLightComponent": {"Color": list(Color), "Intensity": Intensity, "Radius": Radius, "CastShadows": Shadows}}
		if Flicker:
			Comps["ScriptComponent"] = FlickerScript(Intensity, **Flicker)
		return S.Add(Name, Comps, Pos)

	LanternIndex = [0]

	def LanternPost(X, Y, Shadows=False):
		# 나무 기둥(내장 큐브 = 100cm) + 꼭대기 나무 등불 + 심지 불꽃 + 깜빡이는 따뜻한 점광원
		I = LanternIndex[0]
		LanternIndex[0] += 1
		Ground = Height(X, Y)
		S.Add(f"LanternPost_{I}", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MAT_DIR}/WoodPost.emat"}},
			  (X, Y, Ground + 110.0), QuatFromEuler(Yaw=Rng.uniform(0, 90)), (0.13, 0.13, 2.4))
		S.Add(f"LanternPost_{I}_Cap", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MAT_DIR}/WoodPost.emat"}},
			  (X, Y, Ground + 232.0), None, (0.3, 0.3, 0.06))
		Base = Ground + 235.0
		PH_(f"LanternPost_{I}_Lantern", "wooden_lantern_01", X, Y, Rng.uniform(0, 360), 1.6, Z=Base)
		Particles(f"LanternPost_{I}_Flame", "Particles/Demo/CampfireLanternFlame.eparticle", (X, Y, Base + 27.0))
		Point(f"LanternPost_{I}_Light", (X, Y, Base + 40.0), (1.0, 0.62, 0.3), 8.0, 800.0, Shadows,
			  Flicker={"Style": "Fire", "Seed": 60 + I, "Amount": 0.18})
		BoxCollider(f"LanternPost_{I}_Collision", (X, Y, Ground + 100.0), (14.0, 14.0, 100.0))
		Reserve(X, Y, 60.0)

	# ---- 환경: 해 질 녘 하늘 + 구름 + 하늘빛 + 옅은 볼류메트릭 안개(빛줄기·등불 무리)
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.8, 0.58], "Intensity": 6.0}}, (0, 0, 3000), QuatFromEuler(Pitch=-11, Yaw=SUN_AZIMUTH + 180.0))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {"MoonIntensity": 0.2, "MoonColor": [0.6, 0.72, 1.0], "NightSkyColor": [0.008, 0.012, 0.026], "StarIntensity": 0.3},
		"VolumetricCloudComponent": {"Coverage": 0.32, "CloudType": 0.4, "WindSpeed": 5.0, "WindDirection": 30.0},
		"TimeOfDayComponent": {"TimeOfDay": TIME_OF_DAY, "DayLengthMinutes": 0.0, "MaxSunElevation": MAX_SUN_ELEVATION,
							   "NorthAzimuth": NORTH_AZIMUTH, "AnimateInEditor": False},
		"SkyLightComponent": {"Intensity": 4.5},
		"HeightFogComponent": {
			"Color": [0.45, 0.42, 0.4], "Density": 0.0009, "HeightFalloff": 0.08, "StartDistance": 1500.0, "MaxOpacity": 0.6,
			"DirectionalInscatteringColor": [0.9, 0.6, 0.35], "Volumetric": True, "VolumetricDistance": 6000.0,
			"VolumetricAlbedo": [0.95, 0.92, 0.88], "VolumetricExtinctionScale": 0.6, "VolumetricAnisotropy": 0.5,
			"VolumetricDirectionalScale": 0.5, "VolumetricLocalLightScale": 0.35},
	})
	E = Env.FDressing(S, Height, Rng, Reserve, BoxCollider, Point)

	# ---- 지형 + 폴리지 + 물 (연못·개울·폭포 웅덩이를 덮는 물 상자 하나 — 바깥 지형은 수면보다 높다)
	S.Add("Terrain", {"TerrainComponent": {
		"Asset": "Terrain/Demo/HD2D.eterrain", "Size": [TERRAIN_SIZE, TERRAIN_SIZE], "HeightRange": HEIGHT_RANGE,
		"Layer0Material": f"{MAT_DIR}/TerrainGrass.emat", "Layer1Material": f"{MAT_DIR}/TerrainPath.emat",
		"Layer2Material": f"{MAT_DIR}/TerrainCobble.emat", "Layer3Material": f"{MAT_DIR}/TerrainMoss.emat",
		"Layer0Tiling": 450.0, "Layer1Tiling": 380.0, "Layer2Tiling": 300.0, "Layer3Tiling": 600.0,
		"CastShadows": True, "Collision": True}})
	S.Add("Foliage", {"FoliageComponent": {"Asset": "Foliage/Demo/HD2D.efoliage", "Visible": True}})
	PX, PY, PRX, PRY = POND
	WaterMin, WaterMax = (-1100.0, -2750.0), (3450.0, 3400.0)
	WaterProps = {"ScatterColor": [0.02, 0.07, 0.07], "Absorption": [0.35, 0.12, 0.1],
				  "NormalStrength": 0.35, "WaveScale": 160.0, "WaveSpeed": 6.0, "FlowDirection": 0.0, "FlowSpeed": 0.0,
				  "FoamIntensity": 0.4, "FoamDistance": 12.0, "RefractionStrength": 0.03, "ReflectionIntensity": 1.0, "Roughness": 0.05}
	S.Add("Pond", {"WaterBodyComponent": dict(WaterProps, Size=[WaterMax[0] - WaterMin[0], WaterMax[1] - WaterMin[1], 200.0])},
		  ((WaterMin[0] + WaterMax[0]) * 0.5, (WaterMin[1] + WaterMax[1]) * 0.5, POND_LEVEL - 100.0))
	BasinZ = FALLS_INFO["Top"] - 20.0
	S.Add("Falls_Basin", {"WaterBodyComponent": dict(WaterProps, Size=[620.0, 620.0, 120.0], FoamIntensity=0.8)},
		  (FALLS_BASIN[0], FALLS_BASIN[1], BasinZ - 60.0))
	# 연못 안으로 걸어 들어가지 않게 (타원 안쪽을 상자 셋으로 덮음)
	for Index, (FX, FY) in enumerate(((0.92, 0.55), (0.7, 0.85), (0.4, 1.0))):
		BoxCollider(f"Pond_Wall_{Index}", (PX, PY, POND_LEVEL + 40.0), (PRX * FX, PRY * FY, 90.0))
	Reserve(PX, PY, PRX + 120.0)
	Particles("Pond_Fireflies", "Particles/Demo/CampfireFireflies.eparticle", (PX, PY, Height(PX, PY + PRY) + 20.0))

	# 보이지 않는 벽 (놀이 영역)
	(MinX, MinY), (MaxX, MaxY) = PLAY_MIN, PLAY_MAX
	MidX, MidY = (MinX + MaxX) * 0.5, (MinY + MaxY) * 0.5
	BoxCollider("Bound_Back", (MidX, MinY - 100.0, 300.0), ((MaxX - MinX) * 0.5 + 200.0, 100.0, 800.0))
	BoxCollider("Bound_Front", (MidX, MaxY + 100.0, 300.0), ((MaxX - MinX) * 0.5 + 200.0, 100.0, 800.0))
	BoxCollider("Bound_West", (MinX - 100.0, MidY, 300.0), (100.0, (MaxY - MinY) * 0.5 + 200.0, 800.0))
	BoxCollider("Bound_East", (MaxX + 100.0, MidY, 300.0), (100.0, (MaxY - MinY) * 0.5 + 200.0, 800.0))

	TreeFoliage = {"Tree": [], "TreeAutumn": [], "Pine": []}

	def Fir(Name, X, Y, Scale, Tier="lo", Collide=False, Sink=20.0, Kind=None):
		# 나무 = 엔진 폴리지(foliage:tree 활엽수 / foliage:pine 침엽수, 약 6m — 잎은 풀잎 텍스처 × 정점 색). 놀이 영역 안 줄기는 캡슐 충돌
		#   (스캔 전나무는 이 높은 시점에서 잎 카드가 성겨 앙상해 보였다)
		del Tier, Sink
		Kind = Kind or ("Pine" if Rng.random() < 0.45 else ("TreeAutumn" if Rng.random() < 0.15 else "Tree"))
		TreeFoliage[Kind].append((float(X), float(Y), Height(X, Y), Rng.uniform(0, 360), Scale * 1.25 * Rng.uniform(0.9, 1.1), 0.0, 0.0, 1.0))
		if Collide:
			S.Add(f"{Name}_Trunk", {"CapsuleColliderComponent": {"Radius": 24.0 * Scale, "HalfHeight": 120.0, "Offset": [0.0, 0.0, 0.0]}}, (X, Y, Height(X, Y) + 150.0))
		Reserve(X, Y, 110.0 * Scale)

	def Reeds(Name, X, Y, Scale):
		return Place(Name, f"{PH}/grass_medium_02/grass_medium_02_{'abcde'[Rng.randrange(5)]}.part.gltf", X, Y, Rng.uniform(0, 360), Scale, Sink=3.0)

	# ---- 마을 (서쪽) ----------------------------------------------------------------------------------------------
	#   뒤 단(옹벽 위, +150): 서쪽 집 · 선술집 · 종탑 + 예배당 · 장미색 오두막 · 동쪽 집 / 아래 광장: 우물 · 노점 · 상점 · 대장간 · 서남쪽 오두막
	TZ = TERRACE_H
	CX, CY = PLAZA[0], PLAZA[1]
	E.House("House_West", -4520.0, -1580.0, 560.0, 520.0, H1=280.0, H2=240.0, Ridge="Y", Pitch=52.0, Roof="EnvRoofBrown", Upper="EnvPlaster", Lit=0.5,
			Shutter="EnvShutterGreen", Chimney=0.5, Z=TZ)
	E.House("Tavern", -3560.0, -1600.0, 840.0, 560.0, H1=290.0, H2=270.0, Ridge="X", Pitch=46.0, Roof="EnvRoofRed", Upper="EnvPlasterWarm", Lit=0.85,
			Shutter="EnvShutterRed", Chimney=-0.55, DoorX=-140.0, Lantern=True, Z=TZ)
	E.Tower("Chapel_Tower", -2865.0, -1560.0, 230.0, 1000.0, Z=TZ)
	E.House("Chapel", -2380.0, -1790.0, 520.0, 760.0, H1=560.0, Ridge="Y", Pitch=56.0, Roof="EnvRoofSlate", Ground="EnvStone", Lit=0.9, Shutter=None,
			Chimney=None, Z=TZ, GroundWin=(70.0, 190.0), FlowerBoxes=False)
	E.House("Cottage_Rose", -1840.0, -1620.0, 440.0, 480.0, H1=300.0, Ridge="X", Pitch=50.0, Roof="EnvRoofRed", Ground="EnvPlasterRose", Lit=0.7,
			Shutter="EnvShutterBlue", Chimney=0.45, Z=TZ, GroundTimber=True)
	E.House("HomeB", -1180.0, -1560.0, 640.0, 560.0, H1=280.0, H2=250.0, Ridge="Y", Pitch=50.0, Roof="EnvRoofSlate", Upper="EnvPlaster", Lit=0.6,
			Shutter="EnvShutterBlue", Chimney=0.5, Z=TZ)
	Point("Tavern_DoorLight", (-3560.0 - 140.0 + 95.0, -1600.0 + 280.0 + 60.0, TZ + 230.0), (1.0, 0.62, 0.3), 5.0, 650.0, Flicker={"Style": "Fire", "Seed": 21, "Amount": 0.15})
	Point("Chapel_Glow", (-2380.0, -1390.0, TZ + 300.0), (1.0, 0.7, 0.4), 3.0, 500.0)
	# 창 불빛이 단 위 자갈길에 고이게 (그림자 없는 약한 점광원)
	for Index, (X, Y) in enumerate(((-4520.0, -1250.0), (-1840.0, -1310.0), (-1180.0, -1210.0))):
		Point(f"WindowPool_{Index}", (X, Y, TZ + 170.0), (1.0, 0.66, 0.36), 2.5, 450.0, Flicker={"Style": "Fire", "Seed": 30 + Index, "Amount": 0.06, "Speed": 0.4})
	# 옹벽 (단 앞 + 개울 쪽 옆) + 광장에서 오르는 돌계단(볼 받침 + 양옆 난간 돌)
	WallFront, WallThick, WallTop = TERRACE_Y + 25.0, 60.0, TZ + 58.0
	Gap = (STAIRS_X - STAIRS_W * 0.5, STAIRS_X + STAIRS_W * 0.5)
	for Index, (X0, X1) in enumerate(((-4950.0, Gap[0]), (Gap[1], TERRACE_EAST))):
		Count = int(math.ceil((X1 - X0) / 420.0))
		for K in range(Count):
			A, B = X0 + (X1 - X0) * K / Count, X0 + (X1 - X0) * (K + 1) / Count
			E.Box(f"TerraceWall_{Index}_{K}", ((A + B) * 0.5, WallFront, (WallTop - 50.0) * 0.5), (B - A + 2.0, WallThick, WallTop + 50.0), "EnvStone")
			E.Box(f"TerraceWall_{Index}_{K}_Cap", ((A + B) * 0.5, WallFront, WallTop + 6.0), (B - A + 4.0, WallThick + 14.0, 12.0), "EnvStoneDark")
		BoxCollider(f"TerraceWall_{Index}_Collision", ((X0 + X1) * 0.5, WallFront, WallTop * 0.5 + 40.0), ((X1 - X0) * 0.5, WallThick * 0.5, WallTop * 0.5 + 60.0))
	for K in range(3):
		A, B = TERRACE_Y + 30.0 - K * 400.0, TERRACE_Y + 30.0 - (K + 1) * 400.0
		E.Box(f"TerraceSide_{K}", (TERRACE_EAST + 5.0, (A + B) * 0.5, (WallTop - 50.0) * 0.5), (WallThick, abs(B - A) + 2.0, WallTop + 50.0), "EnvStone")
		E.Box(f"TerraceSide_{K}_Cap", (TERRACE_EAST + 5.0, (A + B) * 0.5, WallTop + 6.0), (WallThick + 14.0, abs(B - A) + 4.0, 12.0), "EnvStoneDark")
	BoxCollider("TerraceSide_Collision", (TERRACE_EAST + 5.0, TERRACE_Y - 600.0, WallTop * 0.5 + 40.0), (WallThick * 0.5, 630.0, WallTop * 0.5 + 60.0))
	Steps, Rise_, Tread = 6, TZ / 6.0, 44.0
	StairFront = WallFront + WallThick * 0.5
	for K in range(Steps):
		Top = Rise_ * (K + 1)
		Y0 = StairFront + (Steps - 1 - K) * Tread
		Y1 = Y0 + Tread if K > 0 else Y0 + Tread
		Back = TERRACE_Y - 60.0 if K == Steps - 1 else Y0
		E.Box(f"Stairs_{K}", (STAIRS_X, (Back + Y1) * 0.5, (Top - 40.0) * 0.5), (STAIRS_W, Y1 - Back, Top + 40.0), "EnvStone")
		BoxCollider(f"Stairs_{K}_Collision", (STAIRS_X, (Back + Y1) * 0.5, (Top - 40.0) * 0.5), (STAIRS_W * 0.5, (Y1 - Back) * 0.5, (Top + 40.0) * 0.5))
		for Side in (-1, 1):
			E.Box(f"Stairs_{K}_Cheek{Side}", (STAIRS_X + Side * (STAIRS_W * 0.5 + 22.0), (Y0 + Y1) * 0.5, (Top + 30.0) * 0.5), (44.0, Tread + 1.0, Top + 70.0), "EnvStoneDark")
	for Side in (-1, 1):
		BoxCollider(f"Stairs_Cheek{Side}_Collision", (STAIRS_X + Side * (STAIRS_W * 0.5 + 22.0), StairFront + Steps * Tread * 0.5, 90.0), (22.0, Steps * Tread * 0.5, 90.0))
	Reserve(STAIRS_X, StairFront + Steps * Tread * 0.5, 200.0)
	# 계단 위 깃발 기둥 둘 + 단 위 화분
	for Side, Cloth in ((-1, "EnvClothRed"), (1, "EnvClothBlue")):
		BX, BY = STAIRS_X + Side * (STAIRS_W * 0.5 + 70.0), TERRACE_Y - 80.0
		E.Box(f"Banner{Side}_Pole", (BX, BY, TZ + 210.0), (12.0, 12.0, 420.0), "EnvTimber")
		E.Box(f"Banner{Side}_Bar", (BX, BY + 6.0, TZ + 400.0), (80.0, 6.0, 6.0), "EnvTimber")
		E.Box(f"Banner{Side}_Cloth", (BX, BY + 8.0, TZ + 320.0), (70.0, 2.0, 150.0), Cloth)
		E.Box(f"Banner{Side}_Trim", (BX, BY + 9.5, TZ + 248.0), (70.0, 2.0, 8.0), "EnvClothYellow")
		BoxCollider(f"Banner{Side}_Collision", (BX, BY, TZ + 100.0), (10.0, 10.0, 100.0))
	for Index, X in enumerate((-4150.0, -3050.0, -1650.0, -900.0)):
		PH_(f"Terrace_Planter_{Index}", "planter_box_01", X, TERRACE_Y - 70.0, 0.0, 1.0, Z=TZ, Collide=True)
		for F in range(3):
			PH_(f"Terrace_Planter_{Index}_Flower_{F}", "flower_gazania", X + (F - 1) * 35.0, TERRACE_Y - 70.0, Rng.uniform(0, 360), 1.6, Z=TZ + 32.0)
	E.Laundry("Laundry", (-2000.0, -1290.0, TZ + 250.0), (-1530.0, -1180.0, TZ + 250.0))
	for Index, (X, Y) in enumerate(((-2000.0, -1290.0), (-1530.0, -1180.0))):
		E.Box(f"Laundry_Pole{Index}", (X, Y, TZ + 130.0), (10.0, 10.0, 260.0), "EnvTimber")
	PH_("Terrace_Barrel_0", "wine_barrel_01", -3080.0, -1260.0, 20.0, 1.0, Z=TZ, Collide=True)
	PH_("Terrace_Barrel_1", "wine_barrel_01", -3010.0, -1300.0, 70.0, 1.0, Z=TZ, Collide=True)
	PH_("Terrace_Bench", "painted_wooden_bench", -4200.0, -1200.0, -90.0, 1.0, Z=TZ, Collide=True)

	# 아래 광장: 대장간 (광장을 봄 — 앞 = +X) + 화로 지붕
	E.House("Smithy", -4080.0, -250.0, 520.0, 460.0, H1=300.0, Ridge="X", Pitch=45.0, Roof="EnvRoofBrown", Ground="EnvStone", Yaw=-90.0, Lit=0.6,
			Shutter=None, Chimney=-0.4, FlowerBoxes=False)
	for Index, (PX_, PY_) in enumerate(((-3560.0, -470.0), (-3560.0, 140.0))):
		E.Box(f"Forge_Post{Index}", (PX_, PY_, 120.0), (14.0, 14.0, 240.0), "EnvTimber")
		BoxCollider(f"Forge_Post{Index}_Collision", (PX_, PY_, 100.0), (10.0, 10.0, 100.0))
	E.Box("Forge_Roof", (-3690.0, -165.0, 262.0), (300.0, 660.0, 12.0), "EnvRoofBrown", QuatFromEuler(Pitch=-12.0))
	E.Box("Forge_Beam", (-3560.0, -165.0, 236.0), (16.0, 640.0, 16.0), "EnvTimber")
	FX_, FY_ = -3650.0, -60.0
	FZ = Height(FX_, FY_)
	PH_("Forge", "barrel_stove", FX_, FY_, 0.0, 1.1, Sink=2.0, Collide=True)
	Particles("Forge_Fire", "Particles/Demo/AlleyStoveFire.eparticle", (FX_, FY_, FZ + 88.0))
	Particles("Forge_Smoke", "Particles/Demo/AlleyStoveSmoke.eparticle", (FX_, FY_, FZ + 125.0))
	Point("Forge_Light", (FX_ + 30.0, FY_ + 40.0, FZ + 140.0), (1.0, 0.46, 0.16), 16.0, 900.0, True, Flicker={"Style": "Fire", "Seed": 11})
	S.Add("Forge_Sound", {"AudioSourceComponent": {"ClipAsset": "Audio/Demo/CampfireCrackle.wav", "Volume": 0.5, "Pitch": 1.2, "Loop": True,
		"PlayOnStart": True, "Spatial": True, "MinDistance": 200.0, "MaxDistance": 2200.0}}, (FX_, FY_, FZ + 80.0))
	for Index, (Id, DX, DY, Yaw) in enumerate((("weaponrack", 230.0, 120.0, 90.0), ("resource_lumber", 180.0, -260.0, 20.0))):
		KK_(f"Smithy_{Id}_{Index}", Id, -3950.0 + DX, -250.0 + DY, Yaw, KS * 0.9, Collide=True)
	for Index, (Id, DX, DY, Yaw) in enumerate((("wine_barrel_01", 260.0, 330.0, 0.0), ("wooden_crate_02", 310.0, -150.0, 25.0), ("wooden_crate_01", 300.0, -70.0, 50.0))):
		PH_(f"Smithy_{Id}_{Index}", Id, -3950.0 + DX, -250.0 + DY, Yaw, 1.0, Sink=2.0, Collide=True)
	PH_("Smithy_Anvil_Stump", "tree_stump_02", -3560.0, -330.0, 30.0, 0.5, Sink=4.0, Collide=True)
	PH_("Smithy_Hammer", "sledgehammer_01", -3520.0, -330.0, 70.0, 1.0, Z=Height(-3560, -330) + 38.0, Roll=90.0)
	# 상점 (광장 동쪽, 2층 장미색 회벽 + 줄무늬 차양) / 서남쪽 오두막 (카메라 쪽 — 낮은 단층)
	#   상점은 단층·낮은 지붕: 광장 가운데 높이라 높으면 뒤 단(옹벽 위)을 걷는 캐릭터를 가린다 (카메라 피치 28도)
	ShopX, ShopY, ShopD = -1150.0, -420.0, 400.0
	E.House("Shop", ShopX, ShopY, 600.0, ShopD, H1=300.0, Ridge="X", Pitch=34.0, Roof="EnvRoofSlate", Ground="EnvPlasterRose", Lit=0.8,
			Shutter="EnvShutterGreen", Chimney=0.5, DoorX=-110.0, Lantern=True, GroundTimber=True)
	E.Box("Shop_Awning", (ShopX + 60.0, ShopY + ShopD * 0.5 + 55.0, 262.0), (400.0, 120.0, 4.0), "EnvAwningGreen", QuatFromEuler(Roll=18.0))
	E.Box("Shop_Valance", (ShopX + 60.0, ShopY + ShopD * 0.5 + 113.0, 235.0), (400.0, 3.0, 22.0), "EnvAwningGreen")
	Point("Shop_DoorLight", (ShopX - 110.0 + 95.0, ShopY + ShopD * 0.5 + 60.0, 230.0), (1.0, 0.62, 0.3), 5.0, 650.0, Flicker={"Style": "Fire", "Seed": 22, "Amount": 0.15})
	E.House("Cottage_SW", -4560.0, 980.0, 460.0, 380.0, H1=270.0, Ridge="X", Pitch=50.0, Roof="EnvRoofRed", Ground="EnvPlaster", Lit=0.6,
			Shutter="EnvShutterRed", Chimney=0.4, GroundTimber=True)
	# 우물 + 노점 + 시장 탁자
	E.Well("Well", CX, CY)
	Point("Well_Lantern", (CX + 120.0, CY + 90.0, Height(CX, CY) + 240.0), (1.0, 0.65, 0.35), 3.0, 500.0, Flicker={"Style": "Fire", "Seed": 3, "Amount": 0.15})
	E.Stall("Stall_0", -1720.0, 300.0, 0.0, "EnvAwningRed", ("EnvApple", "EnvOrange", "EnvApple"))
	E.Stall("Stall_1", -1110.0, 440.0, -8.0, "EnvAwningBlue", ("EnvCabbage", "EnvBread", "EnvOrange"))
	E.Stall("Stall_2", -3020.0, 520.0, 6.0, "EnvAwningGreen", ("EnvBread", "EnvApple", "EnvCabbage"))
	Table = PH_("Market_Table", "wooden_picnic_table", -1400.0, 110.0, 10.0, 1.0, Collide=True)
	for G, (Id, DX, DY) in enumerate((("wicker_basket_01", -20.0, -70.0), ("jug_01", 15.0, -15.0), ("wicker_basket_02", -10.0, 45.0), ("jug_01", 20.0, 90.0))):
		S.Model(f"Market_Goods_{G}", f"{PH}/{Id}/{Id}.gltf", (DX, DY, 75.0), Rng.uniform(0, 360), 1.0, Parent=Table)
	for Index, (Id, X, Y, Scale) in enumerate((("wine_barrel_01", -780.0, -300.0, 1.0), ("wine_barrel_01", -760.0, -230.0, 1.0), ("wooden_crate_02", -800.0, -140.0, 1.0),
											   ("wooden_barrels_01", -2950.0, -830.0, 0.85), ("wooden_crate_01", -1830.0, -850.0, 1.0), ("wooden_crate_02", -1760.0, -830.0, 0.9))):
		PH_(f"Village_{Id}_{Index}", Id, X, Y, Rng.uniform(0, 360), Scale, Sink=2.0, Collide=True)
	for Index, (Id, X, Y) in enumerate((("sack", -1480.0, 380.0), ("sack", -1440.0, 410.0), ("sack", -2840.0, 640.0))):
		KK_(f"Village_{Id}_{Index}", Id, X, Y, Rng.uniform(0, 360), KS)
	PH_("Village_Bucket", "wooden_bucket_01", -2050.0, -560.0, 30.0, 1.0)
	PH_("Village_Crates", "wooden_crate_02", -2950.0, 120.0, 15.0, 1.0, Sink=2.0, Collide=True)
	PH_("Village_CratesTop", "wooden_crate_01", -2960.0, 110.0, 50.0, 0.9, Z=Height(-2950, 120) + 44.0)
	# 광장 앞(카메라 쪽) 텃밭 + 장작 더미
	E.Garden("Garden", -2250.0, 880.0, 620.0, 320.0)
	E.Fence("Garden_Fence_W", (-2600.0, 700.0), (-2600.0, 1060.0), Spacing=120.0, Height=70.0, Collide=False)
	E.Fence("Garden_Fence_E", (-1900.0, 700.0), (-1900.0, 1060.0), Spacing=120.0, Height=70.0, Collide=False)
	E.Fence("Garden_Fence_S", (-2600.0, 1060.0), (-1900.0, 1060.0), Spacing=140.0, Height=70.0, Collide=False)
	KK_("Woodpile", "resource_lumber", -3420.0, 880.0, 15.0, KS * 0.9, Collide=True)
	PH_("Woodpile_Stump", "tree_stump_02", -3200.0, 960.0, 40.0, 0.5, Sink=4.0, Collide=True)
	PH_("Woodpile_Axe", "hatchet", -3190.0, 955.0, 20.0, 1.0, Z=Height(-3200, 960) + 40.0, Pitch=-60.0)
	# 광장 벤치 (우물을 봄) + 옹벽 아래 화분
	for Index, (DX, DY) in enumerate(((-520.0, 200.0), (520.0, 220.0))):
		PH_(f"Plaza_Bench_{Index}", "painted_wooden_bench", CX + DX, CY + DY, -90.0, 1.0, Collide=True)  # 앉는 쪽이 카메라(+Y)
	for Index, (X, Y) in enumerate(((-3300.0, -905.0), (-2750.0, -880.0), (-2020.0, -880.0), (-1450.0, -905.0), (-3720.0, 520.0))):
		PH_(f"Planter_{Index}", "planter_box_01", X, Y, 0.0, 1.0, Collide=True)
		for F in range(3):
			PH_(f"Planter_{Index}_Flower_{F}", "flower_gazania", X + (F - 1) * 35.0, Y, Rng.uniform(0, 360), 1.6, Z=Height(X, Y) + 32.0)
	# 깃발 줄: 광장 위를 가로지르는 줄 둘 + 대장간·상점으로 내려오는 줄
	for Index, (X, Y) in enumerate(((-3420.0, -840.0), (-1380.0, -840.0))):
		E.Box(f"BuntingPole_{Index}", (X, Y, 260.0), (14.0, 14.0, 520.0), "EnvTimber")
		BoxCollider(f"BuntingPole_{Index}_Collision", (X, Y, 100.0), (10.0, 10.0, 100.0))
		Reserve(X, Y, 50.0)
	E.Bunting("Bunting_0", (-3420.0, -840.0, 500.0), (-1380.0, -840.0, 500.0), Sag=80.0)
	E.Bunting("Bunting_1", (-3420.0, -840.0, 470.0), (-3830.0, -60.0, 300.0), Sag=40.0)
	E.Bunting("Bunting_2", (-1380.0, -840.0, 470.0), (-1450.0, -220.0, 300.0), Sag=30.0)
	# 등불 기둥: 광장 둘레 + 마을 출구
	for X, Y in ((-3150.0, 350.0), (-1600.0, -760.0), (-3150.0, -760.0), (-1750.0, 620.0), (-650.0, 300.0), (-650.0, -250.0)):
		LanternPost(X, Y, Shadows=(X, Y) == (-1750.0, 620.0))
	# 나무 울타리: 마을 동쪽 경계(가운데 = 길 문) + 카메라 쪽 앞줄 일부 + 서남쪽 오두막 뜰
	E.Fence("Fence_East_N", (-705.0, -940.0), (-705.0, -120.0))
	E.Fence("Fence_East_S", (-705.0, 170.0), (-705.0, 1150.0))
	for Index, Y in enumerate((-120.0, 170.0)):
		E.Box(f"Gate_Post{Index}", (-705.0, Y, Height(-705, Y) + 90.0), (20.0, 20.0, 180.0), "EnvTimber")
	E.Box("Gate_Lintel", (-705.0, 25.0, 182.0), (16.0, 330.0, 16.0), "EnvTimber")
	E.Box("Gate_Sign", (-705.0 + 10.0, 25.0, 155.0), (4.0, 120.0, 34.0), "EnvPlanks")
	E.Fence("Fence_Front_0", (-3600.0, 1240.0), (-2700.0, 1240.0), Collide=False)
	E.Fence("Fence_Front_1", (-1700.0, 1240.0), (-950.0, 1240.0), Collide=False)
	E.Fence("Fence_Garden", (-4300.0, 1250.0), (-4300.0, 700.0), Collide=False)
	# 마을 둘레 전나무 (안쪽·서쪽 — 카메라 쪽은 작은 덤불만)
	for Index, (X, Y, Scale) in enumerate(((-4900.0, -2050.0, 1.2), (-4050.0, -2150.0, 1.1), (-3150.0, -2120.0, 1.3), (-1950.0, -2200.0, 1.2),
										   (-1600.0, -2050.0, 1.0), (-780.0, -2050.0, 1.15), (-3050.0, -1380.0, 0.75), (-4800.0, -600.0, 1.0),
										   (-4950.0, 400.0, 0.9))):
		Fir(f"VillageFir_{Index}", X, Y, Scale, Tier="mid" if Y > -1500 else "lo", Collide=Y > -1500)
	for Index in range(10):
		for _ in range(30):
			X, Y = Rng.uniform(-4300.0, -800.0), Rng.uniform(800.0, 1300.0)
			if Free(X, Y, 90.0):
				break
		PH_(f"VillageBush_{Index}", Rng.choice(["shrub_02", "shrub_03", "wild_rooibos_bush"]), X, Y, Rng.uniform(0, 360), Rng.uniform(0.5, 0.8), Sink=4.0)
		Reserve(X, Y, 80.0)

	# ---- 돌아가는 포털 (마을 서쪽 끝, 광장을 봄) ---------------------------------------------------------------------
	PortX, PortY = -4250.0, 250.0
	Portal = S.Add("Portal_Hub", {"ScriptComponent": {"ScriptAsset": "Scripts/Demo/DemoPortal.lua", "ExecutionLocation": 0,
		"PropertyOverrides": json.dumps({"TargetScene": {"Asset": "Scenes/Demo/Hub.escene"}, "Label": "Hub로", "Ready": True,
			"RememberReturn": False}, ensure_ascii=False)}},
		(PortX, PortY, Height(PortX, PortY)), QuatFromEuler(Yaw=0.0))
	S.Model("Portal_Hub_Door", f"{PH}/large_castle_door/large_castle_door.gltf", (0, 0, -3), 0.0, 1.0, Parent=Portal)
	for Side in (-1, 1):
		S.Model("Portal_Hub_Post", f"{PH}/tree_stump_01/tree_stump_01.gltf", (40, Side * 150, -25), 40.0 * Side, 0.45, Parent=Portal)
		S.Model("Portal_Hub_Lantern", f"{PH}/wooden_lantern_01/wooden_lantern_01.gltf", (40, Side * 150, 8), 15.0 * Side, 1.3, Parent=Portal)
	S.Add("Portal_Hub_Glow", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Demo/PortalGlow.emat"}},
		(18, 0, 148), None, (0.02, 1.85, 2.85), Parent=Portal)
	S.Add("Portal_Hub_Light", {"PointLightComponent": {"Color": [0.5, 0.75, 1.0], "Intensity": 2.0, "Radius": 400.0, "CastShadows": False}},
		(80, 0, 220), Parent=Portal)
	Reserve(PortX, PortY, 260.0)

	# ---- 개울 + 나무 다리 + 폭포 ---------------------------------------------------------------------------------------
	# 다리 = 길과 개울이 만나는 곳 (길 방향으로)
	Cross = None
	for (AX, AY), (BX, BY) in zip(PATH[:-1], PATH[1:]):
		for (CX0, CY0), (CX1, CY1) in zip(CREEK[:-1], CREEK[1:]):
			Den = (BX - AX) * (CY1 - CY0) - (BY - AY) * (CX1 - CX0)
			if abs(Den) < 1e-6:
				continue
			T = ((CX0 - AX) * (CY1 - CY0) - (CY0 - AY) * (CX1 - CX0)) / Den
			U = ((CX0 - AX) * (BY - AY) - (CY0 - AY) * (BX - AX)) / Den
			if 0.0 <= T <= 1.0 and 0.0 <= U <= 1.0:
				Cross = (AX + (BX - AX) * T, AY + (BY - AY) * T, math.degrees(math.atan2(BY - AY, BX - AX)))
	BridgeX, BridgeY, BridgeYaw = Cross
	E.Bridge("Bridge", BridgeX, BridgeY, BridgeYaw, 600.0, 250.0, 2.0)
	S.Add("Creek_Sound", {"AudioSourceComponent": {"ClipAsset": "Audio/Demo/ForestStream.wav", "Volume": 0.55, "Pitch": 1.0, "Loop": True,
		"PlayOnStart": True, "Spatial": True, "MinDistance": 250.0, "MaxDistance": 2400.0}}, (BridgeX, BridgeY, 0.0))
	# 개울에 걸어 들어가지 않게: 물길을 따라 상자 (다리 근처는 비움)
	Samples = []
	for (AX, AY), (BX, BY) in zip(CREEK[:-1], CREEK[1:]):
		Count = max(1, int(math.dist((AX, AY), (BX, BY)) // 180.0))
		for K in range(Count):
			T = (K + 0.5) / Count
			Samples.append((AX + (BX - AX) * T, AY + (BY - AY) * T, math.degrees(math.atan2(BY - AY, BX - AX))))
	for Index, (X, Y, Yaw) in enumerate(Samples):
		if Y > PLAY_MAX[1] + 200.0 or math.hypot(X - BridgeX, Y - BridgeY) < 260.0:
			continue
		BoxCollider(f"Creek_Wall_{Index}", (X, Y, 0.0), (130.0, 150.0, 220.0), Yaw)
		Reserve(X, Y, 200.0)
	# 개울가 바위·갈대
	for Index, (X, Y, Yaw) in enumerate(Samples):
		if math.hypot(X - BridgeX, Y - BridgeY) < 380.0 or Y > PLAY_MAX[1] + 900.0:
			continue
		for Side in (-1, 1):
			if Rng.random() < 0.35:
				continue
			R = math.radians(Yaw + 90.0 * Side)
			Dist = Rng.uniform(150.0, 240.0)
			RX, RY = X + math.cos(R) * Dist, Y + math.sin(R) * Dist
			if TerraceMask(np.array(RX), np.array(RY)) > 0.1:
				continue
			if Rng.random() < 0.4:
				PH_(f"Creek_Rock_{Index}_{Side}", "rock_07", RX, RY, Rng.uniform(0, 360), Rng.uniform(0.45, 0.9), Sink=12.0)
			else:
				Reeds(f"Creek_Reeds_{Index}_{Side}", RX, RY, Rng.uniform(2.0, 3.0))
	# 폭포: 벼랑 위 못에서 웅덩이로 (바위 벽 + 물 면 + 물보라 + 소리)
	FallTop = BasinZ
	FallFace = FALLS_BASIN[1] + 160.0
	E.Waterfall("Falls", FALLS[0], FallFace, FallTop, POND_LEVEL - 10.0, 200.0)
	for Index, (DX, DY, SX, SY, SZ, Yaw) in enumerate(((-230.0, -40.0, 220.0, 260.0, 520.0, 12.0), (230.0, -30.0, 240.0, 250.0, 500.0, -15.0),
													   (-420.0, -110.0, 300.0, 260.0, 470.0, 30.0), (430.0, -100.0, 280.0, 260.0, 450.0, -25.0),
													   (0.0, -150.0, 260.0, 200.0, 600.0, 5.0))):
		E.Box(f"Falls_Cliff_{Index}", (FALLS[0] + DX, FallFace + DY, FallTop - SZ * 0.5 + 40.0 + Rng.uniform(-20, 20)), (SX, SY, SZ), "EnvCliff",
			  QuatFromEuler(Pitch=Rng.uniform(-8, 8), Yaw=Yaw, Roll=Rng.uniform(-6, 6)))
	for Index, (DX, DY, Scale) in enumerate(((-260.0, 120.0, 1.3), (250.0, 140.0, 1.1), (-120.0, 260.0, 0.8), (380.0, 40.0, 1.5), (-430.0, 60.0, 1.4))):
		PH_(f"Falls_Rock_{Index}", "rock_07", FALLS[0] + DX, FallFace + DY, Rng.uniform(0, 360), Scale, Sink=25.0)
	for Index, (DX, DY) in enumerate(((-300.0, -330.0), (310.0, -300.0), (-520.0, -200.0), (520.0, -230.0))):
		PH_(f"Falls_TopRock_{Index}", "rock_07", FALLS_BASIN[0] + DX, FALLS_BASIN[1] + DY, Rng.uniform(0, 360), Rng.uniform(1.0, 1.6), Sink=20.0)
	S.Add("Falls_Sound", {"AudioSourceComponent": {"ClipAsset": "Audio/Demo/ForestStream.wav", "Volume": 0.9, "Pitch": 0.8, "Loop": True,
		"PlayOnStart": True, "Spatial": True, "MinDistance": 300.0, "MaxDistance": 3200.0}}, (FALLS[0], FALLS[1], 100.0))

	# ---- 들판 (동쪽) ----------------------------------------------------------------------------------------------
	# 방앗간 언덕 + 밀밭(울타리, 가운데 틈) + 수레
	#   풍차는 놀이 영역 뒤 끝(길 끝)에 — 더 멀면 날개가 화면 위로 잘린다
	MillX, MillY = 3950.0, -1430.0
	E.Windmill("Windmill", MillX, MillY, -6.0, Size=340.0, Height=560.0)
	KK_("Mill_Sacks", "sack", MillX - 280.0, MillY + 330.0, 30.0, KS)
	KK_("Mill_Cart", "wheelbarrow", MillX - 420.0, MillY + 200.0, 60.0, KS, Collide=True)
	Point("Mill_Lantern", (MillX + 150.0, MillY + 330.0, Height(MillX, MillY + 330) + 220.0), (1.0, 0.62, 0.3), 4.0, 600.0, Flicker={"Style": "Fire", "Seed": 90})
	E.Fence("Wheat_Fence_0", (WHEAT[0], WHEAT[3]), (4620.0, WHEAT[3]))
	E.Fence("Wheat_Fence_1", (4900.0, WHEAT[3]), (WHEAT[2], WHEAT[3]))
	E.Fence("Wheat_Fence_2", (WHEAT[0], WHEAT[1]), (WHEAT[0], -800.0))
	E.Scarecrow("Wheat_Scarecrow", 4980.0, -900.0, 8.0)
	E.Signpost("Signpost_Mill", 3560.0, 420.0, [(-75.0, 175.0), (180.0, 150.0), (5.0, 125.0)])
	# 낮은 돌담 (길가 두 곳)
	E.StoneWall("FieldWall_0", (780.0, -130.0), (1380.0, -160.0))
	E.StoneWall("FieldWall_1", (2760.0, 590.0), (3280.0, 560.0))
	# 옛 성소 폐허 (들판 뒤쪽): 이끼 낀 돌기둥 셋 + 쓰러진 들보 + 무너진 담 + 덤불
	RuinX, RuinY = 2150.0, -1480.0
	RZ = Height(RuinX, RuinY)
	for Index, (DX, DY, H, Tilt) in enumerate(((-230.0, 0.0, 300.0, 2.0), (0.0, -30.0, 190.0, -4.0), (240.0, 10.0, 120.0, 6.0))):
		E.Box(f"Ruin_Pillar{Index}", (RuinX + DX, RuinY + DY, RZ + H * 0.5 - 20.0), (62.0, 62.0, H + 40.0), "EnvStoneDark", QuatFromEuler(Pitch=Tilt, Roll=Tilt * 0.5))
		E.Box(f"Ruin_Pillar{Index}_Base", (RuinX + DX, RuinY + DY, RZ + 10.0), (88.0, 88.0, 30.0), "EnvCliff")
		E.Box(f"Ruin_Pillar{Index}_Cap", (RuinX + DX, RuinY + DY, RZ + H - 12.0), (78.0, 78.0, 22.0), "EnvStone", QuatFromEuler(Pitch=Tilt, Yaw=7.0 * Index))
		BoxCollider(f"Ruin_Pillar{Index}_Collision", (RuinX + DX, RuinY + DY, RZ + 100.0), (40.0, 40.0, 100.0))
	E.Box("Ruin_Lintel", (RuinX - 60.0, RuinY + 150.0, RZ + 22.0), (300.0, 50.0, 44.0), "EnvStone", QuatFromEuler(Pitch=4.0, Yaw=14.0, Roll=-6.0))
	E.StoneWall("Ruin_Wall", (RuinX - 420.0, RuinY - 120.0), (RuinX - 300.0, RuinY - 120.0), Height=55.0, Material="EnvCliff")
	E.StoneWall("Ruin_Wall2", (RuinX + 350.0, RuinY - 100.0), (RuinX + 520.0, RuinY - 60.0), Height=40.0, Material="EnvCliff")
	Reserve(RuinX, RuinY, 420.0)
	for Index, (DX, DY) in enumerate(((-380.0, 120.0), (330.0, 160.0), (60.0, -170.0))):
		PH_(f"Ruin_Bush_{Index}", Rng.choice(["shrub_02", "wild_rooibos_bush"]), RuinX + DX, RuinY + DY, Rng.uniform(0, 360), Rng.uniform(0.6, 0.8), Sink=4.0)
	# 모험가 야영지: A자 천막 둘 + 모닥불(불꽃·불빛 그림자) + 통나무 의자
	CampX, CampY = 1300.0, 820.0
	CZ = Height(CampX, CampY)
	PH_("Camp_FirePit", "stone_fire_pit", CampX, CampY, 15.0, 1.0, Sink=10.0)
	Particles("Camp_Fire", "Particles/Demo/CampfireFire.eparticle", (CampX, CampY, CZ + 2.0))
	Point("Camp_Light", (CampX, CampY, CZ + 80.0), (1.0, 0.52, 0.2), 18.0, 1300.0, True, Flicker={"Style": "Fire", "Seed": 4})
	S.Add("Camp_Sound", {"AudioSourceComponent": {"ClipAsset": "Audio/Demo/CampfireCrackle.wav", "Volume": 0.8, "Pitch": 1.0, "Loop": True,
		"PlayOnStart": True, "Spatial": True, "MinDistance": 200.0, "MaxDistance": 2500.0}}, (CampX, CampY, CZ + 40.0))
	BoxCollider("Camp_FirePit_Collision", (CampX, CampY, CZ + 20.0), (70.0, 70.0, 40.0))
	Reserve(CampX, CampY, 160.0)
	E.Tent("Camp_Tent_A", CampX - 430.0, CampY - 90.0, 25.0, 320.0, 270.0, 200.0, "EnvCanvas")
	E.Tent("Camp_Tent_B", CampX + 450.0, CampY - 150.0, -30.0, 290.0, 250.0, 185.0, "EnvTentGreen")
	PH_("Camp_Log", "dead_tree_trunk", CampX + 30.0, CampY - 230.0, 92.0, 0.9, Sink=3.0, Collide=True)
	PH_("Camp_Crate", "wooden_crate_02", CampX - 260.0, CampY + 200.0, 30.0, 1.0, Collide=True)
	PH_("Camp_Bucket", "wooden_bucket_01", CampX + 230.0, CampY + 170.0, 0.0, 1.0)
	# 연못가: 자카란다(보라 꽃나무) + 바위 + 덤불 + 꽃 + 수련·갈대
	PH_("Pond_Tree", "jacaranda_tree", PX - 900.0, PY - 450.0, 30.0, 0.55, Sink=10.0)
	BoxCollider("Pond_Tree_Collision", (PX - 900.0, PY - 450.0, Height(PX - 900, PY - 450) + 150.0), (40.0, 40.0, 150.0))
	Reserve(PX - 900.0, PY - 450.0, 160.0)
	Particles("Pond_Tree_Petals", Env.Fx("HD2DPondPetals"), (PX - 900.0, PY - 450.0, Height(PX - 900, PY - 450)))
	for Index, (Id, A, Scale) in enumerate((("rock_moss_set_01", 20.0, 1.0), ("boulder_01", 160.0, 0.6), ("rock_07", 220.0, 1.0),
											 ("rock_moss_set_02", 300.0, 0.9), ("rock_07", 95.0, 1.3))):
		R = math.radians(A)
		X, Y = PX + math.cos(R) * (PRX + 60.0), PY + math.sin(R) * (PRY + 50.0)
		PH_(f"Pond_Rock_{Index}", Id, X, Y, Rng.uniform(0, 360), Scale, Sink=12.0, Collide=Id in ("boulder_01",), Shrink=0.6)
	for Index in range(14):
		A = Rng.uniform(0, math.tau)
		X, Y = PX + math.cos(A) * (PRX + Rng.uniform(120.0, 300.0)), PY + math.sin(A) * (PRY + Rng.uniform(100.0, 260.0))
		if Free(X, Y, 50.0):
			PH_(f"Pond_Flower_{Index}", "flower_gazania", X, Y, Rng.uniform(0, 360), Rng.uniform(1.6, 2.2), Sink=1.0)
	PH_("Pond_Fern_0", "fern_02", PX + 650.0, PY - 380.0, 40.0, 1.0, Sink=4.0)
	PH_("Pond_Fern_1", "fern_02", PX - 700.0, PY + 300.0, 110.0, 0.9, Sink=4.0)
	for Index in range(26):
		A = Rng.uniform(0, math.tau)
		T = math.sqrt(Rng.uniform(0.35, 0.85))
		X, Y = PX + math.cos(A) * PRX * T * 0.85, PY + math.sin(A) * PRY * T * 0.85
		E.Ball(f"Pond_Lily_{Index}", (X, Y, POND_LEVEL + 0.6), Rng.uniform(35.0, 70.0), "EnvLily", Squash=0.03)
		if Index % 4 == 0:
			E.Ball(f"Pond_LilyFlower_{Index}", (X + 6.0, Y + 4.0, POND_LEVEL + 4.0), 12.0, Rng.choice(["EnvFlowerPink", "EnvFlowerWhite"]), Squash=0.6)
	for Index in range(12):
		A = Rng.uniform(0, math.tau)
		X, Y = PX + math.cos(A) * (PRX + 20.0), PY + math.sin(A) * (PRY + 15.0)
		Reeds(f"Pond_Reeds_{Index}", X, Y, Rng.uniform(1.8, 2.6))
	# 길가 등불
	for X, Y in ((700.0, -60.0), (2100.0, 450.0), (3300.0, 0.0), (4400.0, 600.0), (3550.0, -950.0)):
		LanternPost(X, Y)
	# 들판 나무(전나무)·바위·덤불 (길·연못·야영지·소환 지점은 비움)
	HD2DGameplay.ReserveSpots(Reserve)
	#   카메라 쪽(+Y)에는 큰 나무를 두지 않는다 (피치 28도 — 나무 뒤 10m 넘게 캐릭터를 가림) → 덤불 무리
	#   동굴 입구(CAVE_GATE) 앞과 성채 앞은 비운다 (나무가 입구·성벽을 가렸다)
	Trees = [(1900.0, -1750.0, 1.1), (5100.0, -1450.0, 1.2), (5600.0, -300.0, 1.2), (4800.0, -2250.0, 1.4), (3200.0, -1850.0, 1.1)]
	for Index, (X, Y) in enumerate(((5050.0, 1050.0), (2750.0, 1250.0), (3900.0, 1250.0), (4600.0, 1150.0))):
		for K in range(3):
			BX, BY = X + Rng.uniform(-120, 120), Y + Rng.uniform(-60, 60)
			PH_(f"FieldHedge_{Index}_{K}", Rng.choice(["shrub_02", "shrub_03", "wild_rooibos_bush"]), BX, BY, Rng.uniform(0, 360), Rng.uniform(0.6, 0.9), Sink=4.0)
		Reserve(X, Y, 180.0)
	for Index, (X, Y, Scale) in enumerate(Trees):
		Fir(f"FieldFir_{Index}", X, Y, Scale, Tier="lo" if Y < -1900 else "mid", Collide=Y > -1950)
	for Index in range(10):
		for _ in range(40):
			X, Y = Rng.uniform(300.0, 5200.0), Rng.uniform(-1800.0, 1250.0)
			if Free(X, Y, 120.0) and SegmentDistance(np.array(X), np.array(Y), PATH) > 220.0 and not InRect(X, Y, WHEAT, 80.0):
				break
		PH_(f"FieldRock_{Index}", "rock_07", X, Y, Rng.uniform(0, 360), Rng.uniform(0.6, 1.2), Sink=10.0, Collide=Index % 2 == 0, Shrink=0.6)
		Reserve(X, Y, 100.0)
	for Index in range(22):
		for _ in range(40):
			X, Y = Rng.uniform(300.0, 5300.0), Rng.uniform(-1900.0, 1300.0)
			if Free(X, Y, 90.0) and SegmentDistance(np.array(X), np.array(Y), PATH) > 200.0 and not InRect(X, Y, WHEAT, 60.0):
				break
		Id = Rng.choice(["shrub_02", "shrub_03", "shrub_04", "wild_rooibos_bush", "flower_gazania"])
		PH_(f"FieldBush_{Index}", Id, X, Y, Rng.uniform(0, 360), Rng.uniform(1.8, 2.6) if Id == "flower_gazania" else Rng.uniform(0.5, 0.9), Sink=4.0)
		Reserve(X, Y, 70.0)

	# ---- 들판 뒤 성채 (흐린 원경 — 성벽 아랫부분과 성문이 화면 위쪽에 걸린다)
	CX0, CY0, CS = CASTLE
	CastleZ = FALLS_INFO["CastleZ"]
	S.Model("Castle", CASTLE_KIT, (CX0, CY0, CastleZ), 0.0, CS)
	GateX = CX0 + (600.0 + 1482.0 + 741.0 * 0.5) * CS
	for Side in (-1, 1):
		BX = GateX + Side * 300.0 * CS * 1.4
		S.Model(f"Castle_Banner{Side}", "Asset/KayKit/Dungeon/banner_patternA_red.glb", (BX, CY0 + 30.0, CastleZ + 180.0), 90.0, 0.8)
		Point(f"Castle_GateLight{Side}", (GateX + Side * 200.0, CY0 + 120.0, CastleZ + 260.0), (1.0, 0.6, 0.28), 6.0, 700.0,
			  Flicker={"Style": "Fire", "Seed": 70 + Side, "Amount": 0.2})
		S.Add(f"Castle_Torch{Side}", {"ModelComponent": {"AssetPath": "Asset/KayKit/Dungeon/torch_mounted.glb"}}, (GateX + Side * 200.0, CY0 + 40.0, CastleZ + 230.0),
			  QuatFromEuler(Yaw=-90.0), (0.8, 0.8, 0.8))
		Particles(f"Castle_TorchFlame{Side}", "Particles/Demo/CampfireLanternFlame.eparticle", (GateX + Side * 200.0, CY0 + 58.0, CastleZ + 287.0))
	# ---- 폭포 옆 벼랑 동굴 입구 + 이동 트리거 (HD2DTravel.lua — lane 계약) + 돌아오는 자리
	GX, GY = CAVE_GATE
	E.CaveMouth("CaveMouth", GX, GY - 40.0)
	S.Add("CaveGate_Travel", {
		"BoxColliderComponent": {"HalfExtents": [150.0, 70.0, 130.0], "IsTrigger": True},
		"ScriptComponent": {"ScriptAsset": "Scripts/Demo/HD2D/HD2DTravel.lua", "ExecutionLocation": 0,
							"PropertyOverrides": json.dumps({"TargetScene": "Scenes/Demo/HD2DCave.escene", "SpawnName": "CaveEntry"}, ensure_ascii=False)}},
		(GX, GY + 95.0, Height(GX, GY + 95.0) + 120.0))
	S.Add("Spawn_CaveExit", {}, (CAVE_EXIT_SPAWN[0], CAVE_EXIT_SPAWN[1], Height(*CAVE_EXIT_SPAWN)), QuatFromEuler(Yaw=90.0))
	Reserve(GX, GY + 100.0, 280.0)
	Reserve(*CAVE_EXIT_SPAWN, 150.0)
	# ---- 게시판 (광장, 계단 서쪽) + 여관 간판 (선술집 겸 여관 — 문 앞 INN_DOOR_SPOT은 비워 둔다)
	E.NoticeBoard("NoticeBoard", NOTICE_BOARD[0], NOTICE_BOARD[1])
	E.HangingSign("Inn_Sign", INN_DOOR_SPOT[0] + 150.0, -1600.0 + 280.0, TERRACE_H + 255.0, "Bed")
	E.HangingSign("Inn_SignMug", -3560.0 + 330.0, -1600.0 + 280.0, TERRACE_H + 255.0, "Mug")
	Reserve(INN_DOOR_SPOT[0], INN_DOOR_SPOT[1], 110.0)

	# ---- 먼 배경: 언덕 위 전나무 숲 (화면 위쪽 = 놀이 영역 뒤 15~25m) + 먼 산
	HillRng = np.random.default_rng(57)
	Taken = []
	for X, Y in HillRng.uniform((-6500.0, -6000.0), (7500.0, -2200.0), size=(9000, 2)):
		X, Y = float(X), float(Y)
		Spacing = 260.0 if Y > -3600.0 else 340.0
		if math.hypot(X - FALLS[0], Y - FALLS[1]) < 600.0 or math.hypot(X - FALLS_BASIN[0], Y - FALLS_BASIN[1]) < 420.0:
			continue
		if CASTLE[0] - 500.0 < X < CASTLE[0] + CASTLE_LENGTH * CASTLE[2] + 500.0 and Y > CASTLE[1] - 1300.0:
			continue  # 성채 앞·성벽 위
		if abs(X - CAVE_GATE[0]) < 900.0 and Y > CAVE_GATE[1] - 600.0:
			continue  # 동굴 입구 바위
		if Y > -2350.0 and HillRng.random() < 0.6:
			continue  # 놀이 영역 바로 뒤는 조금 성기게
		if any((X - TX) ** 2 + (Y - TY) ** 2 < Spacing ** 2 for TX, TY in Taken[-400:]) or not Free(X, Y, 150.0):
			continue
		Taken.append((X, Y))
		Kind = "Pine" if HillRng.random() < (0.35 if Y > -3400.0 else 0.65) else ("TreeAutumn" if HillRng.random() < 0.12 else "Tree")
		TreeFoliage[Kind].append((X, Y, Height(X, Y), float(HillRng.uniform(0, 360)), float(HillRng.uniform(1.1, 1.7)), 0.0, 0.0, 1.0))
	for Index, (Id, X, Y, Scale, Yaw) in enumerate((("mountain_B_grass_trees", -3000.0, -6800.0, 22.0, 20.0), ("mountain_A_grass_trees", 1500.0, -7500.0, 26.0, 70.0),
													 ("mountain_B_grass_trees", 6000.0, -6800.0, 21.0, 140.0))):
		KK_(f"Backdrop_{Index}", Id, X, Y, Yaw, Scale, Sink=40.0)

	# ---- 환경 파티클: 마을 위 꽃잎, 들판 위 낙엽, 놀이 영역 전체 빛 먼지
	Particles("Env_Petals", Env.Fx("HD2DPetals"), (-2300.0, -300.0, 0.0))
	Particles("Env_Leaves", Env.Fx("HD2DLeaves"), (2600.0, -300.0, 0.0))
	Particles("Env_Motes", Env.Fx("HD2DMotes"), (500.0, -300.0, Height(500, -300)))

	# ---- 풀 폴리지 (풀 레이어 위만, 물체·길·광장·연못 피함) + 꽃(무리별 색) + 밀밭 + 개울가 긴 풀
	GrassRng = np.random.default_rng(17)
	Dry = []
	P = GrassRng.uniform((-6000.0, -3600.0), (7000.0, 2600.0), size=(150000, 2))
	Keep = (Height.GrassWeight(P[:, 0], P[:, 1]) >= 0.75) & (GrassRng.random(len(P)) < 0.55)
	Keep &= np.hypot(P[:, 0] - Start[0], P[:, 1] - Start[1]) >= 120.0
	Keep &= ~InRect(P[:, 0], P[:, 1], WHEAT, 0.0)
	for OX, OY, OR in Occupied:
		Keep &= (P[:, 0] - OX) ** 2 + (P[:, 1] - OY) ** 2 >= (OR + 25.0) ** 2
	for X, Y in P[Keep]:
		Z = Height(X, Y)
		N = Height.Normal(X, Y)
		Item = (float(X), float(Y), Z, float(GrassRng.uniform(0, 360)), float(GrassRng.uniform(0.45, 0.85)), float(N[0]), float(N[1]), float(N[2]))
		(Dry if GrassRng.random() < 0.18 else Grass).append(Item)
	EXTRA_FOLIAGE.clear()
	Flowers = {Color: [] for Color in Env.FLOWER_COLORS}
	FP = GrassRng.uniform((-5200.0, -2600.0), (6200.0, 1700.0), size=(160000, 2))
	Patch = Fbm(FP[:, 0], FP[:, 1], 650.0, 91, 3)
	Hue = Fbm(FP[:, 0], FP[:, 1], 1400.0, 92, 2)
	Threshold = np.where(FP[:, 0] > -600.0, 0.08, 0.22)  # 들판은 꽃무리를 더 넓게
	Keep = (Height.GrassWeight(FP[:, 0], FP[:, 1]) >= 0.7) & (Patch > Threshold) & (GrassRng.random(len(FP)) < np.clip((Patch - Threshold) * 4.0, 0.0, 0.7))
	Keep &= SegmentDistance(FP[:, 0], FP[:, 1], PATH) > 160.0
	Keep &= ~InRect(FP[:, 0], FP[:, 1], WHEAT, 40.0)
	for OX, OY, OR in Occupied:
		Keep &= (FP[:, 0] - OX) ** 2 + (FP[:, 1] - OY) ** 2 >= (OR + 10.0) ** 2
	# 옹벽 앞 꽃밭 띠 (계단 앞은 비움)
	Bed = (FP[:, 1] > TERRACE_Y + 60.0) & (FP[:, 1] < TERRACE_Y + 115.0) & (FP[:, 0] < TERRACE_EAST - 60.0) & (np.abs(FP[:, 0] - STAIRS_X) > STAIRS_W * 0.5 + 80.0)
	Bed &= GrassRng.random(len(FP)) < 0.9
	for Index in np.nonzero(Keep | Bed)[0]:
		X, Y = float(FP[Index, 0]), float(FP[Index, 1])
		if Bed[Index]:
			Color = ("Red", "Yellow", "White", "Pink")[int((X + 6000.0) // 140.0) % 4]
		else:
			H_ = (Hue[Index] + 1.0) * 0.5 + GrassRng.uniform(-0.12, 0.12)
			Color = Env.FLOWER_COLORS[int(np.clip(H_, 0.0, 0.999) * len(Env.FLOWER_COLORS))]
		N = Height.Normal(X, Y)
		Scale = float(GrassRng.uniform(0.11, 0.16) if Bed[Index] else GrassRng.uniform(0.08, 0.13))  # 지름 1m 구 → 8~16cm 꽃송이
		Flowers[Color].append((X, Y, Height(X, Y), float(GrassRng.uniform(0, 360)), Scale, float(N[0]), float(N[1]), float(N[2])))
	for Color in Env.FLOWER_COLORS:
		EXTRA_FOLIAGE.append((Env.FLOWER_TYPES[Color], Flowers[Color]))
	Wheat = []
	WX0, WY0, WX1, WY1 = WHEAT
	for GX in np.arange(WX0 + 30.0, WX1 - 20.0, 26.0):
		for GY in np.arange(WY0 + 20.0, WY1 - 30.0, 26.0):
			X, Y = float(GX + GrassRng.uniform(-9, 9)), float(GY + GrassRng.uniform(-9, 9))
			N = Height.Normal(X, Y)
			Wheat.append((X, Y, Height(X, Y), float(GrassRng.uniform(0, 360)), float(GrassRng.uniform(1.25, 1.75)), float(N[0]), float(N[1]), float(N[2])))
	EXTRA_FOLIAGE.append((Env.WHEAT_TYPE, Wheat))
	Meadow = []
	MP = GrassRng.uniform((-1200.0, -2600.0), (6000.0, 1700.0), size=(40000, 2))
	MKeep = (SegmentDistance(MP[:, 0], MP[:, 1], CREEK) > 160.0) & (SegmentDistance(MP[:, 0], MP[:, 1], CREEK) < 420.0)
	MKeep |= (EllipseValue(MP[:, 0], MP[:, 1], POND) > 1.18) & (EllipseValue(MP[:, 0], MP[:, 1], POND) < 1.6) & (GrassRng.random(len(MP)) < 0.6)
	MKeep &= SegmentDistance(MP[:, 0], MP[:, 1], PATH) > 200.0
	for X, Y in MP[MKeep]:
		X, Y = float(X), float(Y)
		N = Height.Normal(X, Y)
		Meadow.append((X, Y, Height(X, Y), float(GrassRng.uniform(0, 360)), float(GrassRng.uniform(0.8, 1.2)), float(N[0]), float(N[1]), float(N[2])))
	EXTRA_FOLIAGE.append((Env.MEADOW_TYPE, Meadow))
	EXTRA_FOLIAGE.extend(((Env.TREE_TYPE, TreeFoliage["Tree"]), (Env.TREE_AUTUMN_TYPE, TreeFoliage["TreeAutumn"]), (Env.PINE_TYPE, TreeFoliage["Pine"])))

	# ---- 게임: 관리자·HUD(HD2DGameplay) + 카메라 + 플레이어
	HD2DGameplay.AddGame(S, Height, PATH, AutoPlay, Title=None if AutoPlay else Start == PLAYER_START,  # 타이틀은 기본 씬에만 (시점 변형 제외)
						 Minimap=MINIMAP)
	StartZ = Height(*Start) + HD2DGameplay.PLAYER_RADIUS + HD2DGameplay.PLAYER_HALF + 4.0
	Forward = (0.0, -math.cos(math.radians(-CAMERA_PITCH)), -math.sin(math.radians(-CAMERA_PITCH)))
	Focus = (Start[0], Start[1], StartZ - 85.0 + 70.0)
	# 피사계 심도 (HD-2D 미니어처 느낌): 초점 = 카메라에서 플레이어 가슴까지(카메라 거리 고정 — 따라가도 그대로), 앞 땅·뒤 산과 하늘은 흐림.
	#   깊이 + 틸트시프트(화면 위아래로 갈수록 흐림 — 둘 중 큰 쪽), 육각 보케 + 밝은 점 강조(등불·창 불빛·불티가 빛망울로)
	# 색 보정: 해 질 녘 금빛 하이라이트 + 살짝 푸른 그림자, 채도·대비 조금 위 (옥토패스풍), 비네트: 가장자리를 짙은 보랏빛으로
	S.Add("Camera", {"CameraComponent": {"FovYDegrees": CAMERA_FOV, "NearZ": 50.0, "FarZ": 60000.0, "Primary": True, "Priority": 10},
					 "DepthOfFieldComponent": {"Enabled": True, "FocusDistance": CAMERA_DISTANCE, "FocalRegion": DOF_FOCAL_REGION,
											   "NearTransition": 700.0, "FarTransition": 1800.0, "NearBlurSize": 1.4, "FarBlurSize": 1.7,
											   "PreviewInEditor": False, "Mode": 2, "TiltShiftCenter": 0.53, "TiltShiftBand": 0.11,
											   "TiltShiftTransition": 0.38, "TiltShiftAngle": 0.0, "BokehBladeCount": 6, "BokehRotation": 15.0,
											   "BokehHighlightBoost": 3.0, "BokehHighlightThreshold": 1.0},
					 "ColorGradingComponent": {"Enabled": True, "Temperature": 0.2, "Tint": 0.04, "Saturation": 1.12, "Contrast": 1.1,
											   "Lift": [0.0, 0.008, 0.035], "Gamma": [1.0, 1.0, 1.02], "Gain": [1.04, 1.0, 0.96],
											   "LookupTable": "", "LookupTableIntensity": 1.0},
					 "VignetteComponent": {"Enabled": True, "Intensity": 0.5, "Size": 0.42, "Smoothness": 0.62, "Roundness": 1.0,
										   "Color": [0.03, 0.015, 0.04]}},
		  tuple(Focus[I] - Forward[I] * CAMERA_DISTANCE for I in range(3)), QuatFromEuler(Pitch=CAMERA_PITCH, Yaw=-90.0))
	HD2DGameplay.AddPlayer(S, Start, Height)
	return S, Grass, Dry


def WriteMinimap(Sampler, Scene):
	# 지도 그림: 지형 레이어(풀/흙길/자갈/이끼·바위) + 물(수면 아래) + 밀밭 + 나무(폴리지) + 건물(콜라이더) — HD2DMapArt
	Cell = Sampler.Cell

	def Classify(X, Y):
		IX = np.clip(np.round((X + TERRAIN_SIZE * 0.5) / Cell).astype(int), 0, TERRAIN_RES - 1)
		IY = np.clip(np.round((Y + TERRAIN_SIZE * 0.5) / Cell).astype(int), 0, TERRAIN_RES - 1)
		H, W = Sampler.H[IY, IX], Sampler.Stack[IY, IX]
		Layer = np.argmax(W, axis=-1)
		Kind = np.choose(Layer, [HD2DMapArt.K_GRASS, HD2DMapArt.K_PATH, HD2DMapArt.K_PLAZA, HD2DMapArt.K_MOSS])
		Kind = np.where((Kind == HD2DMapArt.K_GRASS) & (X > WHEAT[0]) & (X < WHEAT[2]) & (Y > WHEAT[1]) & (Y < WHEAT[3]), HD2DMapArt.K_FIELD, Kind)
		return np.where(H < POND_LEVEL + 5.0, HD2DMapArt.K_WATER, Kind)

	Trees = [(T[0], T[1], 95.0 * T[4]) for Type, Items in EXTRA_FOLIAGE if Type in (Env.TREE_TYPE, Env.TREE_AUTUMN_TYPE, Env.PINE_TYPE) for T in Items]
	HD2DMapArt.WriteMinimap(CONTENT, MINIMAP, Classify, Scene, Sampler, Trees)


def Main():
	bViews = "--views" in sys.argv
	HD2DArt.WriteAll(os.path.join(CONTENT, "Sprites", "HD2D"))
	X, Y, H = BuildHeights()
	Weights, _, Stack = BuildWeights(X, Y, H)
	WriteTerrain(os.path.join(CONTENT, "Terrain", "Demo", "HD2D.eterrain"), H, Weights)
	Sampler = FHeightSampler(H, Stack)
	WriteMaterials()
	Env.WriteMaterials(CONTENT)
	Env.WriteCastleKit(CONTENT, CASTLE_KIT)
	Env.WriteParticles(CONTENT, (PLAY_MAX[0] - PLAY_MIN[0] + 1600.0, PLAY_MAX[1] - PLAY_MIN[1] + 1400.0))
	HD2DGameplay.WriteAll(CONTENT, CAMERA_DISTANCE, PLAY_MIN, PLAY_MAX)
	Scene, Grass, Dry = BuildScene(Sampler)
	WriteMinimap(Sampler, Scene)
	WriteFoliage(os.path.join(CONTENT, "Foliage", "Demo", "HD2D.efoliage"), [(GRASS_TYPE, Grass), (GRASS_DRY_TYPE, Dry)] + EXTRA_FOLIAGE)
	Scene.Save(os.path.join(CONTENT, "Scenes", "Demo", "HD2D.escene"))
	print(f"HD2D 생성: 엔티티 {len(Scene.Entities)}개, 풀 {len(Grass) + len(Dry)}개, 지형 {TERRAIN_RES}² 높이 {H.min():.0f}~{H.max():.0f}cm")
	if bViews:
		for Name, Start in VIEW_STARTS.items():
			Variant, _, _ = BuildScene(Sampler, Start)
			Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_HD2D_{Name}.escene"))
		HD2DGameplay.WriteNavBake(CONTENT, Scene, Sampler, PLAY_MIN, PLAY_MAX, PATH)
		Variant, _, _ = BuildScene(Sampler, HD2DGameplay.AUTOPLAY_START, AutoPlay=True)
		Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", "_HD2DAutoPlay.escene"))
		# 배경 확인용 자유 시점(우선순위 높은 카메라를 더함 — 흐림 없음): 이름 → (X, Y, Z, Pitch, Yaw, 시야각)
		for Name, (X, Y, Z, Pitch, Yaw, Fov) in OVERVIEW_VIEWS.items():
			Variant, _, _ = BuildScene(Sampler, VIEW_STARTS["Village"])
			for Entity in Variant.Entities:
				if Entity["Name"] == "Camera":
					Entity["Components"]["DepthOfFieldComponent"]["Enabled"] = False  # 확인용: 흐림 끔 (게임 카메라 설정은 그대로)
			Variant.Add("OverviewCamera", {"CameraComponent": {"FovYDegrees": Fov, "NearZ": 50.0, "FarZ": 80000.0, "Primary": True, "Priority": 100}},
						(X, Y, Z), QuatFromEuler(Pitch=Pitch, Yaw=Yaw))
			Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_HD2D_Over{Name}.escene"))
		for Name, (ShotStart, Scenario) in HD2DGameplay.SHOT_SCENES.items():
			Variant, _, _ = BuildScene(Sampler, ShotStart, AutoPlay=Scenario)
			Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_HD2D{Name}.escene"))
		print("확인용 변형: Scenes/Demo/_HD2D_*.escene, _HD2DAutoPlay.escene (커밋하지 않음)")


if __name__ == "__main__":
	Main()
