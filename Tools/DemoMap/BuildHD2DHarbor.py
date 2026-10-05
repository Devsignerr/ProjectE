# 데모 서브맵 "HD2D 항구"(갈매기 항구 — 바닷가 항구 마을 + 해안 절벽 길. 메인 맵 남쪽 시냇가 길에서 언덕을 넘어 온다) 생성:
#   지형(.eterrain: 풀·모래·자갈 바닥·바위) + 바다(물 상자 하나) + 석축 부두·나무 잔교·방파제 등대(절차 메시 HD2DMeshKit) + 반목조 집(HD2DEnvironment)
#   + 어시장 노점(도트 생선 더미 스프라이트) + 창고·짐 더미 + 나룻배·상선 + 해안 절벽(Poly Haven 바위) + 해적 야영지 + 동쪽 후미(보스) + 파도 물보라·갈매기
#   + 가로등·창 불빛(낮밤 — HD2DWorld.lua가 켜고 끈다, 이름 Night_<n>/NightWin_<n>) + 플레이어/게임 관리자(HD2DGameplay)
#   실행: python Tools/DemoMap/BuildHD2DHarbor.py [--views]   (머티리얼·파티클 일부는 BuildHD2D.py가 먼저 써 둔 HD2D 공용 것을 쓴다)
#   --views: 확인용 변형(커밋하지 않음) — Scenes/Demo/_HD2DHarbor_<시점>.escene(시작 자리만 다름), _HD2DHarbor_Over<이름>.escene(흐림 끈 자유 시점),
#            _HD2DHarborAutoPlay.escene(자동 검증), _HD2DHarborShot_<이름>.escene(스크린샷·측정)
#   내비메시: 씬 그대로 굽는다 (엔진 굽기가 지형 + 정적 콜라이더를 넣는다 — 잔교·방파제 바닥 상자 포함)
#     .\Scripts\Verify.ps1 -Target Editor -Config Release -Frames 30 -ExtraArgs "--scene Scenes/Demo/HD2DHarbor.escene --bake-navmesh" → 옆에 HD2DHarbor.enav
#   게임: 자리·적 종류·상자·보스는 HD2DHarborLayout.py → HD2DGameplay.FMapLayout("Harbor") → AddGame (타이틀 없음 — 바로 플레이)
#   규약: 메인 맵과 같은 고정 원근 디오라마 카메라(+Y 위에서 -Y를 봄, 피치 -28·시야각 24). 바다·잔교는 카메라 쪽(+Y, 낮음), 집은 안쪽(-Y).
#         카메라 쪽 물체 높이는 앞 거리 × 0.5 아래로 (그보다 높으면 캐릭터를 가린다 — 등대는 520cm, 걷는 길과 X를 비킨다)
#   배치를 바꿀 때는 씬 파일이 아니라 이 스크립트를 고치고 다시 실행한다 (결정적 — 고정 시드)
import json
import math
import os
import random
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402
from BuildCampfire import Const, Curve, Emitter, Fbm, FlickerScript, Init, Mod, Rand, Shape, Smoothstep, Sprite as PSprite, WriteFoliage, WriteJson  # noqa: E402
from BuildCampfire import SHAPE_BOX, SHAPE_SPHERE  # noqa: E402
from AssetFixes import AlphaModel  # noqa: E402
import BuildHD2D as Main  # noqa: E402 — 카메라 규약·공용 함수 (임포트만)
import HD2DEnvironment as Env  # noqa: E402
import HD2DGameplay  # noqa: E402
import HD2DArt  # noqa: E402
import HD2DWorldArt  # noqa: E402
import HD2DMeshKit as MK  # noqa: E402
import HD2DHarborLayout as Layout  # noqa: E402
import HD2DMapArt  # noqa: E402

CONTENT = Main.CONTENT
PH      = "Asset/PolyHaven"
H_MAT   = "Materials/Demo/HD2D/Harbor"
H_FX    = "Particles/Demo/HD2D/Harbor"
H_KIT   = "Asset/DemoKits/HD2D/Harbor"
SCENE   = "Scenes/Demo/HD2DHarbor.escene"
NAV_ASSET = "Scenes/Demo/HD2DHarbor.enav"

# ---- 지형 ----------------------------------------------------------------------------------------------------------
TERRAIN_SIZE   = 16000.0
TERRAIN_RES    = 321       # 칸 50cm
TERRAIN_CENTER = (500.0, -300.0)
HEIGHT_RANGE   = 8000.0
SEA = Layout.SEA_LEVEL
SEA_FLOOR = -380.0
# 앞 해안선 (X → 물가 Y): 서쪽 모래톱 · 석축 부두(직선) · 절벽 아래 바위 물가 · 동쪽 후미 모래 해변
SHORE_PTS = [(-9000.0, 1700.0), (-4600.0, 860.0), (-3500.0, 600.0), (-2750.0, 380.0), (-2600.0, Layout.QUAY_Y), (1250.0, Layout.QUAY_Y),
			 (1450.0, 470.0), (2100.0, 520.0), (3000.0, 540.0), (3550.0, 590.0), (3900.0, 700.0), (4500.0, 760.0), (5000.0, 700.0), (5300.0, 520.0),
			 (5500.0, 200.0)]
EAST_SHORE = [(-3000.0, 6400.0), (-1300.0, 5900.0), (-700.0, 5600.0), (0.0, 5450.0), (520.0, 5300.0)]  # Y → 동쪽 물가 X (후미 동쪽 끝)
# 해안 절벽 길 높이 (X → 윗면) — 동쪽 문에서 오르막, 절벽 위 평지, 후미로 내리막
PATH_Z = [(1450.0, 0.0), (2150.0, Layout.CLIFF_TOP), (3350.0, Layout.CLIFF_TOP), (3950.0, 30.0), (4300.0, 12.0)]
# 해안 절벽 길 흙길 (지형 무늬 + 자동 조종 길)
COAST_TRACK = [(1300.0, -60.0), (1800.0, -120.0), (2200.0, -160.0), (2800.0, -180.0), (3350.0, -140.0), (3800.0, 40.0), (4300.0, 60.0), (4900.0, 120.0)]
HARBOR_PATH = [(-3450.0, -1250.0), (-3050.0, -620.0), (-1700.0, -560.0), (-300.0, -420.0), (1300.0, -60.0), (2200.0, -160.0), (3350.0, -140.0), (4300.0, 60.0)]


def ShoreY(X):
	return np.interp(X, [P[0] for P in SHORE_PTS], [P[1] for P in SHORE_PTS])


def EastShoreX(Y):
	return np.interp(Y, [P[0] for P in EAST_SHORE], [P[1] for P in EAST_SHORE], left=6400.0, right=5300.0)


def RoadZ(Y):
	# 언덕길 높이 (Y → Z): 마을 거리에서 안쪽으로 오른다
	return np.interp(Y, [-2600.0, -1880.0, -1450.0, -950.0], [230.0, 140.0, 55.0, 0.0])


def PathZ(X):
	return np.interp(X, [P[0] for P in PATH_Z], [P[1] for P in PATH_Z])


def ShoreDistance(X, Y):
	# 물가에서 육지 쪽으로 양수 (앞 해안선과 동쪽 물가 중 가까운 쪽 — 근사)
	Front = ShoreY(X) - Y
	East = EastShoreX(Y) - X
	return np.minimum(Front, East)


def BuildHeights():
	Coords = (np.arange(TERRAIN_RES) / (TERRAIN_RES - 1) - 0.5) * TERRAIN_SIZE
	X, Y = np.meshgrid(Coords + TERRAIN_CENTER[0], Coords + TERRAIN_CENTER[1])
	Noise = Fbm(X, Y, 900.0, 501, 3)
	Rough = Fbm(X, Y, 260.0, 502, 2)
	East = Smoothstep(1300.0, 1600.0, X)   # 동쪽 문 너머 (해안 절벽 길) 비중
	Town = 1.0 - East
	# 마을: 평지 0 + 뒤 언덕 (언덕길은 깎아 비탈을 둔다)
	Hill = Smoothstep(-1650.0, -2700.0, Y + Noise * 120.0) * (430.0 + Noise * 160.0 + Rough * 30.0)
	Hill += Smoothstep(-2700.0, -4200.0, Y) * 500.0
	Hill += Smoothstep(-4400.0, -6200.0, X) * 380.0 * Smoothstep(800.0, -600.0, Y)  # 서쪽 끝 언덕 (화면 밖)
	RoadD = Main.SegmentDistance(X, Y, Layout.ROAD)
	Road = RoadZ(Y)
	Cut = Smoothstep(320.0, 170.0, RoadD) * Smoothstep(-900.0, -1300.0, Y)
	HTown = Hill * (1.0 - Cut) + np.minimum(Hill, Road) * Cut
	HTown = np.maximum(HTown, Road * Smoothstep(260.0, 120.0, RoadD) * Smoothstep(-900.0, -1100.0, Y))
	# 해안 절벽 길: 길 높이 + 뒤(-Y) 바위 언덕 / 후미 뒤 벼랑
	PZ = PathZ(X)
	Back = Smoothstep(-720.0, -1500.0, Y + Noise * 140.0) * (320.0 + Noise * 140.0 + Rough * 40.0)
	Back += Smoothstep(-1500.0, -3000.0, Y) * 450.0
	CoveWall = Smoothstep(3700.0, 4100.0, X) * Smoothstep(-640.0, -1050.0, Y + Rough * 60.0) * 520.0
	HEast = PZ + np.maximum(Back, CoveWall) + Rough * 6.0
	H = HTown * Town + HEast * East
	# 해안선: 물가 거리 D (육지 +). 모래톱·후미는 완만한 해변, 부두는 수직(석축이 가린다), 절벽은 가파른 벼랑(바위 모델이 가린다)
	D = ShoreDistance(X, Y)
	Beach = (X < -2600.0) | (X > 3750.0)
	Cliff = (X > 1250.0) & (X <= 3750.0)
	BeachZ = SEA + 6.0 + D * 0.14 + D * D * 0.000165   # 물가 -64 → 330cm 안쪽에서 0 (물가는 완만 — 거품 띠가 계단지지 않게)
	H = np.where(Beach & (D < 330.0), np.minimum(H, BeachZ), H)
	CliffDrop = Smoothstep(-20.0, 140.0, D)            # 벼랑 위 가장자리 140cm 안쪽부터 바다 쪽으로 떨어짐
	H = np.where(Cliff, H * CliffDrop + (SEA - 60.0 + Rough * 25.0) * (1.0 - CliffDrop), H)
	Sea = Smoothstep(0.0, -1.0, D)
	Floor = np.where(Beach, np.maximum(SEA_FLOOR, SEA + 6.0 + D * 0.2), SEA_FLOOR + Rough * 20.0)
	H = H * (1.0 - Sea) + np.minimum(Floor, SEA - 10.0) * Sea
	return X, Y, H, D


def BuildWeights(X, Y, H, D):
	# 레이어: 0 풀, 1 모래(해변·흙길·바다 밑), 2 자갈 바닥(마을 거리·광장·부두), 3 바위(벼랑·비탈)
	GY, GX = np.gradient(H, TERRAIN_SIZE / (TERRAIN_RES - 1))
	Slope = np.sqrt(GX * GX + GY * GY)
	Noise = Fbm(X, Y, 360.0, 511, 3)
	N2 = Noise * 40.0
	# 자갈: 마을 직사각 영역(거리 띠 + 광장·부두) — 가장자리는 노이즈로 흐트러뜨림
	Street = Smoothstep(-820.0 + N2, -760.0 + N2, Y) * Smoothstep(320.0, 280.0, Y) * Smoothstep(-2720.0 + N2, -2640.0 + N2, X) * Smoothstep(1500.0 + N2, 1420.0 + N2, X)
	WestStreet = Smoothstep(-800.0 + N2, -740.0 + N2, Y) * Smoothstep(-420.0 - N2, -480.0 - N2, Y) * Smoothstep(-3800.0, -3700.0, X) * Smoothstep(-2600.0, -2700.0, X)
	Front = Smoothstep(-1700.0 + N2, -1620.0 + N2, Y) * Smoothstep(-760.0, -820.0, Y) * Smoothstep(-2700.0, -2600.0, X) * Smoothstep(1400.0, 1300.0, X) * 0.85
	Cobble = np.maximum.reduce([Street, WestStreet, Front])
	RoadD = Main.SegmentDistance(X, Y, Layout.ROAD)
	TrackD = Main.SegmentDistance(X, Y, COAST_TRACK)
	Dirt = np.maximum(Smoothstep(190.0 + N2, 110.0 + N2, RoadD), Smoothstep(170.0 + N2, 90.0 + N2, TrackD) * Smoothstep(1350.0, 1500.0, X))
	Beach = Smoothstep(420.0 + N2 * 2.0, 220.0 + N2 * 2.0, D) * ((X < -2550.0) | (X > 3700.0))
	Cove = Smoothstep(1.25, 1.0, Main.EllipseValue(X, Y, Layout.COVE))
	Sand = np.maximum.reduce([Dirt, Beach, Cove, Smoothstep(20.0, -40.0, D)])
	Sand = Sand * (1.0 - Cobble)
	Rock = np.maximum(Smoothstep(0.55, 0.9, Slope + Noise * 0.15), Smoothstep(180.0, 60.0, D) * ((X > 1250.0) & (X < 3750.0)))
	Rock = Rock * (1.0 - Cobble) * (1.0 - Sand * 0.7)
	W0 = np.clip(1.0 - Cobble - Sand - Rock, 0.0, 1.0)
	Stack = np.stack([W0, Sand, Cobble, Rock], axis=-1)
	Stack = Stack / np.maximum(Stack.sum(axis=-1, keepdims=True), 1e-6)
	Bytes = np.floor(Stack * 255.0 + 0.5).astype(np.int32)
	Bytes[..., 0] += 255 - Bytes.sum(axis=-1)
	Bytes = np.clip(Bytes, 0, 255).astype(np.uint32)
	return Bytes[..., 0] | (Bytes[..., 1] << 8) | (Bytes[..., 2] << 16) | (Bytes[..., 3] << 24), Stack


def WriteTerrain(Path, H, Weights):
	import base64
	H16 = np.clip(np.round((H / HEIGHT_RANGE + 0.5) * 65535.0), 0, 65535).astype("<u2")
	Doc = {"Heights": base64.b64encode(H16.tobytes()).decode("ascii"), "Resolution": TERRAIN_RES, "Version": 1,
		   "Weights": base64.b64encode(Weights.astype("<u4").tobytes()).decode("ascii")}
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent="\t")
		File.write("\n")


class FHarborHeight:
	# 지형 높이(이중 선형) + 풀 비율 + 법선 — BuildHD2DCave.FCaveHeight와 같은 방식
	def __init__(self, H, Stack):
		self.H, self.Stack = H, Stack
		self.Cell = TERRAIN_SIZE / (TERRAIN_RES - 1)

	def _F(self, PX, PY):
		return (PX - TERRAIN_CENTER[0] + TERRAIN_SIZE * 0.5) / self.Cell, (PY - TERRAIN_CENTER[1] + TERRAIN_SIZE * 0.5) / self.Cell

	def __call__(self, PX, PY):
		FX, FY = self._F(PX, PY)
		IX = int(np.clip(math.floor(FX), 0, TERRAIN_RES - 2))
		IY = int(np.clip(math.floor(FY), 0, TERRAIN_RES - 2))
		TX, TY = FX - IX, FY - IY
		H = self.H
		Top = H[IY, IX] * (1 - TX) + H[IY, IX + 1] * TX
		Bot = H[IY + 1, IX] * (1 - TX) + H[IY + 1, IX + 1] * TX
		return float(Top * (1 - TY) + Bot * TY)

	def Layer(self, PX, PY, Index):
		FX, FY = self._F(np.asarray(PX), np.asarray(PY))
		IX = np.clip(np.round(FX).astype(int), 0, TERRAIN_RES - 1)
		IY = np.clip(np.round(FY).astype(int), 0, TERRAIN_RES - 1)
		return self.Stack[IY, IX, Index]

	def Normal(self, PX, PY):
		E = self.Cell
		DX = (self(PX + E, PY) - self(PX - E, PY)) / (2 * E)
		DY = (self(PX, PY + E) - self(PX, PY - E)) / (2 * E)
		N = np.array([-DX, -DY, 1.0])
		return N / np.linalg.norm(N)


# ---- 머티리얼 ---------------------------------------------------------------------------------------------------------
def HMat(Name):
	return f"{H_MAT}/{Name}.emat"


def WriteMaterials():
	Folder = os.path.join(CONTENT, *H_MAT.split("/"))
	Terrain = {  # 레이어 머티리얼 (Content 기준 PolyHaven 묶음, 색 배율)
		"TerrainGrass":  ("leafy_grass", [0.8, 1.0, 0.66]),
		"TerrainSand":   ("coast_sand_01", [1.02, 0.96, 0.86]),
		"TerrainCobble": ("cobblestone_floor_04", [0.92, 0.9, 0.86]),
		"TerrainRock":   ("mossy_rock", [0.62, 0.66, 0.6]),
	}
	for Name, (Id, Tint) in Terrain.items():
		Rel = f"../../../../{PH}/{Id}/{Id}"
		WriteJson(os.path.join(Folder, f"{Name}.emat"), {
			"Name": f"HD2DHarbor{Name}", "BaseColorFactor": Tint + [1.0], "EmissiveFactor": [0.0, 0.0, 0.0],
			"Metallic": 1.0, "Roughness": 1.0, "NormalScale": 1.0, "OcclusionStrength": 1.0,
			"BaseColorTexture": f"{Rel}_diff_2k.jpg", "MetallicRoughnessTexture": f"{Rel}_arm_2k.jpg",
			"NormalTexture": f"{Rel}_nor_gl_2k.jpg", "OcclusionTexture": f"{Rel}_arm_2k.jpg", "EmissiveTexture": "",
		})
	# 건축면 (HD2D 공용 박스 투영 그래프 인스턴스 — 텍스처 밀도가 크기와 무관)
	Fort = "modular_fort_01/textures/modular_fort_01"
	Instances = {
		"PlasterWhite": ("plastered_wall_04/plastered_wall_04", 260.0, (1.08, 1.06, 1.02)),
		"PlasterBlue":  ("plastered_wall_04/plastered_wall_04", 260.0, (0.7, 0.86, 1.0)),
		"PlasterSand":  ("plastered_wall_04/plastered_wall_04", 260.0, (1.0, 0.88, 0.7)),
		"RoofTeal":     ("rough_wood/rough_wood", 160.0, (0.36, 0.62, 0.66)),
		"RoofBlue":     ("rough_wood/rough_wood", 160.0, (0.34, 0.46, 0.72)),
		"RoofOchre":    ("red_brick/red_brick", 150.0, (1.0, 0.72, 0.42)),
		"PlanksGrey":   ("weathered_planks/weathered_planks", 200.0, (0.78, 0.76, 0.72)),
		"PlanksBlue":   ("weathered_planks/weathered_planks", 200.0, (0.55, 0.7, 0.85)),
		"QuayStone":    (f"{Fort}_wall", 280.0, (0.86, 0.84, 0.8)),
		"QuayStoneWet": (f"{Fort}_wall", 280.0, (0.46, 0.5, 0.5)),
		"Granite":      ("granite_tile/granite_tile", 220.0, (0.86, 0.85, 0.82)),
		"SeaRock":      ("cliff_side/cliff_side", 380.0, (0.72, 0.7, 0.66)),
	}
	for Name, (Stem, Size, Tint) in Instances.items():
		Doc = Env.TriplanarInstance(f"Harbor{Name}", Stem, Size, Tint, 1.0, "../HD2DTriplanar.emat")
		for P in Doc["Parameters"]:  # 폴더가 한 단계 깊다
			if P["Type"] == "Texture":
				P["Value"] = "../" + P["Value"]
		WriteJson(os.path.join(Folder, f"{Name}.emat"), Doc)
	Plains = {
		"LampOn":     Env.Plain("HarborLampOn", (0.25, 0.18, 0.08, 1.0), 0.3, (5.5, 3.4, 1.3)),
		"LampOff":    Env.Plain("HarborLampOff", (0.16, 0.2, 0.22, 1.0), 0.12),
		"Rope":       Env.Plain("HarborRope", (0.42, 0.33, 0.2, 1.0), 0.9),
		"Iron":       Env.Plain("HarborIron", (0.05, 0.05, 0.055, 1.0), 0.5, Metallic=1.0),
		"FlagBlack":  Env.Plain("HarborFlagBlack", (0.04, 0.035, 0.04, 1.0), 0.95),
		"SailCloth":  Env.Plain("HarborSailCloth", (0.8, 0.76, 0.66, 1.0), 0.95),
		"Ember":      Env.Plain("HarborEmber", (0.05, 0.02, 0.01, 1.0), 0.9, (6.0, 1.8, 0.4)),
		"CaveDark":   Env.Plain("HarborCaveDark", (0.004, 0.005, 0.006, 1.0), 1.0),
		"Grass":      Env.Plain("HarborGrass", (0.6, 0.95, 0.42, 1.0), 0.85),
		"Dune":       Env.Plain("HarborDune", (1.0, 0.92, 0.58, 1.0), 0.9),
	}
	for Name, Doc in Plains.items():
		WriteJson(os.path.join(Folder, f"{Name}.emat"), Doc)


HARBOR_GRASS = Env.FoliageType("HarborGrass", "foliage:grass", f"{H_MAT}/Grass.emat", 0.45, 0.85, ZOffset=-2.0, Cull=6000.0)
HARBOR_DUNE = Env.FoliageType("HarborDune", "foliage:grass", f"{H_MAT}/Dune.emat", 0.7, 1.2, ZOffset=-2.0, Cull=5500.0)


# ---- 절차 메시 (HD2DMeshKit) --------------------------------------------------------------------------------------------
def KitPath(Name):
	return f"{H_KIT}/{Name}.gltf"


def WriteMeshes():
	# 나무 잔교 (로컬: 원점 = 뿌리 가운데, +Y = 바다 쪽, 판 윗면 Z 0): 가로 판자(사이 틈) + 세로 들보 + 둥근 말뚝 + 가장자리 낮은 말뚝·밧줄
	Length, Width = Layout.PIER[1] - Layout.QUAY_Y, Layout.PIER[2]
	M = MK.FMeshBuilder()
	Plank = M.Material(MK.FMaterialDef("HarborPierPlank", "weathered_planks", (0.92, 0.84, 0.74), 1.0))
	Post = M.Material(MK.FMaterialDef("HarborPierPost", "dark_wooden_planks", (0.62, 0.52, 0.42), 1.0))
	Rope = M.Material(MK.FMaterialDef("HarborRope", None, (0.42, 0.33, 0.2), 0.9))
	Rng = random.Random(901)
	Y = 6.0
	while Y < Length - 10.0:
		W = Rng.uniform(26.0, 32.0)
		M.Box(Plank, (Rng.uniform(-3.0, 3.0), Y + W * 0.5, -4.0 + Rng.uniform(-0.6, 0.6)), (Width + Rng.uniform(-10.0, 10.0), W - 3.0, 8.0),
			  Yaw=Rng.uniform(-1.2, 1.2), TexSize=180.0)
		Y += W
	for SX in (-1, 0, 1):
		M.Box(Post, (SX * (Width * 0.5 - 30.0), Length * 0.5, -20.0), (18.0, Length, 24.0), TexSize=180.0, Faces="xXyYz")
	Posts = []
	PostY = 40.0
	while PostY < Length:
		Posts.append(PostY)
		PostY += 235.0
	for K, PY in enumerate(Posts):
		for SX in (-1, 1):
			M.Frustum(Post, (SX * (Width * 0.5 + 4.0), PY, 0.0), 15.0, 14.0, 420.0 + 58.0, 10, 120.0, Caps=(False, True), Z0=-420.0, Phase=Rng.uniform(0, 1))
	# 가장자리 밧줄 (말뚝 머리를 잇는다 — 처짐)
	for SX in (-1, 1):
		PX = SX * (Width * 0.5 + 4.0)
		for A, B in zip(Posts[:-1], Posts[1:]):
			for S in range(4):
				T0, T1 = S / 4.0, (S + 1) / 4.0
				Z0, Z1 = 50.0 - 14.0 * math.sin(T0 * math.pi), 50.0 - 14.0 * math.sin(T1 * math.pi)
				M.Cylinder(Rope, (PX, A + (B - A) * T0, Z0), (PX, A + (B - A) * T1, Z1), 1.8, 6, 20.0, Caps=False)
	M.Save(H_KIT, "HarborPier", CONTENT)

	# 등대 (원점 = 바닥 가운데, 지붕 끝 ≈ 545cm): 돌 받침 + 흰·붉은 띠 탑(가늘어짐) + 문(서쪽) + 회랑(난간) + 등실 쇠살 + 지붕·꼭지
	M = MK.FMeshBuilder()
	Stone = M.Material(MK.FMaterialDef("HarborLighthouseStone", "modular_fort_01/textures/modular_fort_01_wall", (0.9, 0.88, 0.84), 1.0))
	White = M.Material(MK.FMaterialDef("HarborLighthouseWhite", "plastered_wall_04", (1.05, 1.03, 0.98), 1.0))
	Red = M.Material(MK.FMaterialDef("HarborLighthouseRed", "plastered_wall_04", (0.78, 0.16, 0.12), 1.0))
	Iron = M.Material(MK.FMaterialDef("HarborLighthouseIron", None, (0.06, 0.065, 0.07), 0.45, 1.0))
	Roof = M.Material(MK.FMaterialDef("HarborLighthouseRoof", None, (0.55, 0.12, 0.1), 0.5, 0.6))
	Door = M.Material(MK.FMaterialDef("HarborLighthouseDoor", "dark_wooden_planks", (0.55, 0.4, 0.3), 1.0))
	M.Frustum(Stone, (0, 0, 0), 175.0, 160.0, 70.0, 24, 160.0, Z0=-20.0)
	R0, R1, Top = 140.0, 108.0, 350.0

	def RAt(Z):
		return R0 + (R1 - R0) * (Z - 50.0) / (Top - 50.0)
	for Z0, Z1, Mat in ((50.0, 120.0, White), (120.0, 165.0, Red), (165.0, 245.0, White), (245.0, 290.0, Red), (290.0, Top, White)):
		M.Frustum(Mat, (0, 0, 0), RAt(Z0), RAt(Z1), Z1 - Z0, 28, 180.0, Caps=(False, False), Z0=Z0)
	M.Box(Door, (-RAt(120.0) + 4.0, 0.0, 122.0), (14.0, 76.0, 144.0), TexSize=120.0)
	M.Box(Stone, (-RAt(200.0) + 2.0, 0.0, 200.0), (16.0, 96.0, 14.0), TexSize=120.0)
	for Z in (238.0, 310.0):
		M.Box(Iron, (-(RAt(Z) - 3.0), 0.0, Z), (8.0, 22.0, 34.0))
	M.Frustum(Iron, (0, 0, 0), 150.0, 150.0, 12.0, 28, 100.0, Z0=Top)
	for K in range(16):
		A = K / 16.0 * 2.0 * math.pi
		M.Cylinder(Iron, (math.cos(A) * 144.0, math.sin(A) * 144.0, Top + 12.0), (math.cos(A) * 144.0, math.sin(A) * 144.0, Top + 62.0), 2.2, 6)
	for K in range(32):
		A0, A1 = K / 32.0 * 2.0 * math.pi, (K + 1) / 32.0 * 2.0 * math.pi
		M.Cylinder(Iron, (math.cos(A0) * 144.0, math.sin(A0) * 144.0, Top + 62.0), (math.cos(A1) * 144.0, math.sin(A1) * 144.0, Top + 62.0), 2.6, 6, Caps=False)
	LZ0, LZ1 = Top + 12.0, Top + 112.0
	M.Frustum(Iron, (0, 0, 0), 92.0, 92.0, 14.0, 8, 100.0, Z0=LZ0, Smooth=False, Phase=math.pi / 8.0)
	for K in range(8):
		A = K / 8.0 * 2.0 * math.pi + math.pi / 8.0
		M.Cylinder(Iron, (math.cos(A) * 88.0, math.sin(A) * 88.0, LZ0 + 14.0), (math.cos(A) * 88.0, math.sin(A) * 88.0, LZ1), 3.0, 6)
	M.Frustum(Iron, (0, 0, 0), 94.0, 94.0, 10.0, 8, 100.0, Z0=LZ1, Smooth=False, Phase=math.pi / 8.0)
	M.Lathe(Roof, (0, 0, 0), [(112.0, LZ1 + 10.0), (100.0, LZ1 + 22.0), (70.0, LZ1 + 50.0), (30.0, LZ1 + 66.0), (10.0, LZ1 + 72.0), (0.0, LZ1 + 74.0)], 24, 80.0)
	M.Cylinder(Iron, (0, 0, LZ1 + 70.0), (0, 0, LZ1 + 100.0), 2.5, 6)
	M.Lathe(Iron, (0, 0, LZ1 + 98.0), [(0.0, 0.0), (7.0, 4.0), (7.0, 10.0), (0.0, 14.0)], 10, 40.0)
	M.Save(H_KIT, "HarborLighthouse", CONTENT)

	# 나룻배 세 색 (원점 = 용골 가운데 바닥, 로컬 +X = 이물): 판자 선체(양면) + 뱃전 띠 + 가로 걸상 둘 + 노
	for Variant, Tint in (("Blue", (0.52, 0.68, 0.86)), ("Red", (0.86, 0.46, 0.38)), ("Wood", (0.9, 0.8, 0.68))):
		M = MK.FMeshBuilder()
		Hull = M.Material(MK.FMaterialDef(f"HarborBoat{Variant}", "weathered_planks", Tint, 1.0, DoubleSided=True))
		Trim = M.Material(MK.FMaterialDef("HarborBoatTrim", "dark_wooden_planks", (0.7, 0.55, 0.42), 1.0))
		L, Beam, Depth = 380.0, 140.0, 58.0
		Stations, Ring = 14, 9
		Grid = []
		for I in range(Stations + 1):
			T = I / Stations * 2.0 - 1.0          # -1 고물 ~ +1 이물
			Half = Beam * 0.5 * max(0.0, 1.0 - abs(T) ** (2.6 if T > 0 else 4.0)) ** 0.5
			Half = max(Half, 3.0)
			Sheer = Depth + 16.0 * T * T + (8.0 * T if T > 0 else 0.0)
			Keel = 4.0 * T * T
			Row = []
			for J in range(Ring + 1):
				A = J / Ring * math.pi                 # 0 = 왼 뱃전, π/2 = 용골, π = 오른 뱃전
				Row.append((T * L * 0.5, -math.cos(A) * Half, Keel + (Sheer - Keel) * (1.0 - math.sin(A) ** 0.75)))
			Grid.append(Row)
		for I in range(Stations):
			for J in range(Ring):
				A, B, C, D = Grid[I][J], Grid[I + 1][J], Grid[I + 1][J + 1], Grid[I][J + 1]
				Nn = np.cross(np.subtract(B, A), np.subtract(D, A))
				Mid = np.mean([A, B, C, D], axis=0)
				if np.dot(Nn, np.array((0.0, Mid[1], Mid[2] - Depth * 0.5))) < 0:
					Nn = -Nn
				Nn = tuple(Nn / max(np.linalg.norm(Nn), 1e-9))

				def UV(P):
					return (P[0] / 160.0, (P[2] + abs(P[1])) / 160.0)
				M.Quad(Hull, A, B, C, D, Nn, Nn, Nn, Nn, UV(A), UV(B), UV(C), UV(D))
		for Side in (0, Ring):
			for I in range(Stations):
				P0, P1 = Grid[I][Side], Grid[I + 1][Side]
				M.Cylinder(Trim, (P0[0], P0[1], P0[2] + 2.0), (P1[0], P1[1], P1[2] + 2.0), 4.0, 6, 60.0, Caps=False)
		for SX in (-75.0, 70.0):
			Half = Beam * 0.5 * max(0.0, 1.0 - abs(SX / (L * 0.5)) ** (2.6 if SX > 0 else 4.0)) ** 0.5
			M.Box(Trim, (SX, 0.0, Depth - 14.0), (30.0, Half * 2.0 - 6.0, 6.0), TexSize=80.0)
		for Side in (-1, 1):
			M.Cylinder(Trim, (-150.0, Side * 30.0, 20.0), (150.0, Side * 60.0, Depth + 2.0), 2.6, 6, 60.0)
			M.Box(Trim, (164.0, Side * 61.0, Depth + 5.0), (40.0, 3.0, 13.0), Yaw=Side * 6.0, TexSize=60.0)
		M.Save(H_KIT, f"HarborRowboat{Variant}", CONTENT)

	# 계류 기둥 (쇠, 버섯 모양 머리)
	M = MK.FMeshBuilder()
	Iron = M.Material(MK.FMaterialDef("HarborBollard", None, (0.08, 0.085, 0.09), 0.55, 1.0))
	M.Lathe(Iron, (0, 0, 0), [(16.0, -5.0), (15.0, 0.0), (11.0, 8.0), (10.0, 40.0), (14.0, 46.0), (17.0, 52.0), (15.0, 58.0), (0.0, 60.0)], 14, 60.0)
	M.Save(H_KIT, "HarborBollard", CONTENT)

	# 그물 말리는 틀 (기둥 둘 + 가로대 + 늘어뜨린 그물 — 마스크 텍스처)
	HD2DWorldArt.WriteNetTexture(os.path.join(CONTENT, *H_KIT.split("/"), "HarborNet.png"))
	M = MK.FMeshBuilder()
	Wood = M.Material(MK.FMaterialDef("HarborNetPost", "dark_wooden_planks", (0.62, 0.52, 0.42), 1.0))
	Net = M.Material(MK.FMaterialDef("HarborNet", None, (0.85, 0.82, 0.72), 0.95, Texture="HarborNet.png", Mask=True, DoubleSided=True))
	for SX in (-120.0, 120.0):
		M.Cylinder(Wood, (SX, 0.0, -20.0), (SX, 0.0, 190.0), 6.0, 8, 80.0)
	M.Cylinder(Wood, (-135.0, 0.0, 180.0), (135.0, 0.0, 180.0), 4.5, 8, 80.0)
	Cols, Rows = 12, 7
	Pts = []
	for J in range(Rows + 1):
		Row = []
		for I in range(Cols + 1):
			T = I / Cols
			Drop = J / Rows * (150.0 + 25.0 * math.sin(T * math.pi * 2.0 + 0.6))
			Row.append((-118.0 + 236.0 * T, 4.0 * math.sin(T * 9.0 + J) + J * 1.5, 178.0 - Drop - (8.0 * math.sin(T * math.pi) if J > 0 else 0.0)))
		Pts.append(Row)
	for J in range(Rows):
		for I in range(Cols):
			A, B, C, D = Pts[J][I], Pts[J][I + 1], Pts[J + 1][I + 1], Pts[J + 1][I]
			M.Quad(Net, A, B, C, D, *((0.0, 1.0, 0.0),) * 4, (I / Cols * 3.0, J / Rows * 2.0), ((I + 1) / Cols * 3.0, J / Rows * 2.0),
				   ((I + 1) / Cols * 3.0, (J + 1) / Rows * 2.0), (I / Cols * 3.0, (J + 1) / Rows * 2.0))
	M.Save(H_KIT, "HarborNetRack", CONTENT)

	# 둥근 장대 (해적 깃발대·돛대 끝)
	M = MK.FMeshBuilder()
	Wood = M.Material(MK.FMaterialDef("HarborPole", "dark_wooden_planks", (0.6, 0.5, 0.4), 1.0))
	M.Frustum(Wood, (0, 0, 0), 9.0, 6.0, 470.0, 10, 120.0, Z0=-20.0)
	M.Cylinder(Wood, (0.0, -50.0, 405.0), (0.0, 50.0, 405.0), 3.5, 6, 60.0)
	M.Save(H_KIT, "HarborFlagPole", CONTENT)

	# 둥근 돌 단 (방파제 끝 — 등대 받침): 젖은 아랫단 + 윗단
	M = MK.FMeshBuilder()
	Stone = M.Material(MK.FMaterialDef("HarborJettyStone", "modular_fort_01/textures/modular_fort_01_wall", (0.86, 0.84, 0.8), 1.0))
	Wet = M.Material(MK.FMaterialDef("HarborJettyWet", "modular_fort_01/textures/modular_fort_01_wall", (0.46, 0.5, 0.5), 1.0))
	M.Frustum(Wet, (0, 0, 0), 300.0, 286.0, 300.0, 36, 200.0, Caps=(False, False), Z0=-330.0)
	M.Frustum(Stone, (0, 0, 0), 286.0, 280.0, 40.0, 36, 200.0, Caps=(False, True), Z0=-30.0)
	M.Save(H_KIT, "HarborJettyRound", CONTENT)

	# 닻 기념비 (녹슨 쇠 닻 — 원점 = 아래 둥근 머리): 축 + 나무 가로대 + 위 고리 + 아래 굽은 팔 + 삽 모양 갈고리 끝
	M = MK.FMeshBuilder()
	Rust = M.Material(MK.FMaterialDef("HarborAnchorIron", "rusty_metal_02", (0.26, 0.25, 0.26), 0.9, Metal=0.55))
	Wood = M.Material(MK.FMaterialDef("HarborAnchorStock", "dark_wooden_planks", (0.66, 0.5, 0.38), 1.0))
	M.Cylinder(Rust, (0.0, 0.0, 10.0), (0.0, 0.0, 232.0), 8.5, 12, 70.0)
	M.Box(Wood, (0.0, 0.0, 196.0), (22.0, 150.0, 20.0), TexSize=90.0)
	for K in range(16):
		A0, A1 = K / 16.0 * 2.0 * math.pi, (K + 1) / 16.0 * 2.0 * math.pi
		M.Cylinder(Rust, (math.sin(A0) * 20.0, 0.0, 252.0 + math.cos(A0) * 20.0), (math.sin(A1) * 20.0, 0.0, 252.0 + math.cos(A1) * 20.0), 4.0, 8, 40.0, Caps=False)
	Arc = [math.radians(-78.0 + K * 156.0 / 12.0) for K in range(13)]
	for A0, A1 in zip(Arc[:-1], Arc[1:]):
		M.Cylinder(Rust, (math.sin(A0) * 85.0, 0.0, 88.0 - math.cos(A0) * 85.0), (math.sin(A1) * 85.0, 0.0, 88.0 - math.cos(A1) * 85.0), 8.0, 10, 60.0)
	M.Lathe(Rust, (0.0, 0.0, -2.0), [(0.0, 0.0), (11.0, 3.0), (12.0, 12.0), (8.5, 18.0)], 12, 40.0)
	for Sign in (-1, 1):
		A = math.radians(78.0) * Sign
		TX, TZ = math.sin(A) * 85.0, 88.0 - math.cos(A) * 85.0
		M.Box(Rust, (TX - Sign * 6.0, 0.0, TZ - 6.0), (34.0, 9.0, 42.0), Pitch=-Sign * 30.0, TexSize=60.0)
	M.Save(H_KIT, "HarborAnchor", CONTENT)


# ---- 파티클 --------------------------------------------------------------------------------------------------------------
SMOKE_TEX = "../../Textures/CampfireSmoke.png"
FLAME_TEX = "../../Textures/CampfireFlame.png"


def SpraySystem():
	# 바위에 부서지는 파도: 주기 6초 (0초 큰 파도, 3초 작은 파도) — 로컬 -Y = 육지 쪽, 수면 아래로 떨어지면 사라진다
	Splash = Emitter("Splash", 951, 360,
		[Mod("SpawnBurstInstantaneous", SpawnCount=Rand(150.0, 190.0), SpawnTime=Const(0.0)), Mod("SpawnBurstInstantaneous", SpawnCount=Rand(70.0, 100.0), SpawnTime=Const(3.0))],
		[Init(Rand(0.8, 1.5), Rand((0.7, 0.76, 0.8, 0.55), (0.85, 0.9, 0.95, 0.8)), Rand((4.0, 4.0), (9.0, 9.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_BOX, Box=(180.0, 40.0, 10.0)),
		 Mod("AddVelocityInCone", ConeAxis=Const(0.0, -0.4, 1.0), ConeAngle=Const(30.0), Speed=Rand(260.0, 600.0))],
		[Mod("GravityForce", Gravity=Const(0.0, 0.0, -980.0)),
		 Mod("Drag", Drag=Const(1.2)),
		 Mod("ScaleSpriteSize", Scale=Curve((0.0, (0.5, 0.5)), (1.0, (3.0, 3.0)))),
		 Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 1.0)), (0.6, (1.0, 1.0, 1.0, 0.7)), (1.0, (1.0, 1.0, 1.0, 0.0)))),
		 Mod("Collision", PlaneHeight=Const(SEA - 6.0), Restitution=Const(0.0), Friction=Const(0.0), KillOnCollide=Const(1.0)),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0))],
		PSprite(0, SMOKE_TEX, 4, 4), Sim="GPU", Duration=6.0, Bounds=((-400.0, -400.0, -200.0), (400.0, 300.0, 500.0)))
	Mist = Emitter("Mist", 952, 40,
		[Mod("SpawnBurstInstantaneous", SpawnCount=Const(10.0), SpawnTime=Const(0.15)), Mod("SpawnBurstInstantaneous", SpawnCount=Const(6.0), SpawnTime=Const(3.15))],
		[Init(Rand(2.0, 3.4), Rand((0.6, 0.66, 0.7, 0.12), (0.7, 0.76, 0.8, 0.2)), Rand((90.0, 90.0), (160.0, 160.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_BOX, Box=(180.0, 40.0, 30.0), Offset=(0.0, 0.0, 80.0)),
		 Mod("AddVelocity", Velocity=Rand((-30.0, -90.0, 20.0), (30.0, -30.0, 60.0)))],
		[Mod("Drag", Drag=Const(0.5)),
		 Mod("SpriteRotationRate", RotationRate=Rand(-10.0, 10.0)),
		 Mod("ScaleSpriteSize", Scale=Curve((0.0, (0.6, 0.6)), (1.0, (2.2, 2.2)))),
		 Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 0.0)), (0.2, (1.0, 1.0, 1.0, 1.0)), (1.0, (1.0, 1.0, 1.0, 0.0)))),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0))],
		PSprite(0, SMOKE_TEX, 4, 4), Duration=6.0)
	return {"Name": "HD2DHarborSpray", "Version": 2, "Emitters": [Mist, Splash]}


def MotesSystem():
	# 바닷바람에 날리는 반짝이는 소금 먼지
	Twinkle = Curve((0.0, (1, 1, 1, 0)), (0.25, (1, 1, 1, 0.8)), (0.45, (1, 1, 1, 0.25)), (0.65, (1, 1, 1, 1.0)), (1.0, (1, 1, 1, 0)))
	Motes = Emitter("Motes", 961, 900,
		[Mod("SpawnRate", SpawnRate=Const(70.0))],
		[Init(Rand(6.0, 10.0), Rand((1.6, 1.6, 1.5, 0.5), (2.4, 2.4, 2.2, 0.85)), Rand((4.0, 4.0), (7.0, 7.0))),
		 Shape(SHAPE_BOX, Box=(11000.0, 3600.0, 360.0), Offset=(0.0, 0.0, 220.0)),
		 Mod("AddVelocity", Velocity=Rand((10.0, -12.0, -4.0), (30.0, 6.0, 8.0)))],
		[Mod("CurlNoiseForce", Strength=Const(16.0), Frequency=Const(0.006), PanSpeed=Const(12.0, 0.0, 2.0)),
		 Mod("Drag", Drag=Const(0.4)),
		 Mod("ScaleColor", Scale=Twinkle)],
		PSprite(1), Sim="GPU", Duration=10.0, Bounds=((-6000.0, -2400.0, -100.0), (6000.0, 2400.0, 900.0)))
	return {"Name": "HD2DHarborMotes", "Version": 2, "Emitters": [Motes]}


def CampfireSystem():
	# 해적 야영지 모닥불
	Flames = Emitter("Flames", 971, 90,
		[Mod("SpawnRate", SpawnRate=Const(40.0))],
		[Init(Rand(0.4, 0.75), Rand((0.8, 0.3, 0.07, 0.9), (1.1, 0.45, 0.1, 1.0)), Rand((20.0, 30.0), (30.0, 48.0)), Rand(-12.0, 12.0)),
		 Shape(SHAPE_SPHERE, Radius=18.0),
		 Mod("AddVelocity", Velocity=Rand((-8.0, -8.0, 50.0), (8.0, 8.0, 95.0)))],
		[Mod("ScaleColor", Scale=Curve((0.0, (0.5, 0.5, 0.5, 0.0)), (0.12, (1, 1, 1, 1)), (1.0, (0.4, 0.12, 0.04, 0.0)))),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0))],
		PSprite(1, FLAME_TEX, 4, 4))
	Embers = Emitter("Embers", 972, 80,
		[Mod("SpawnRate", SpawnRate=Const(10.0))],
		[Init(Rand(1.0, 2.2), Rand((6.0, 2.0, 0.4, 1.0), (9.0, 3.0, 0.8, 1.0)), Rand((1.2, 1.2), (2.0, 2.0))),
		 Shape(SHAPE_SPHERE, Radius=16.0),
		 Mod("AddVelocity", Velocity=Rand((-15.0, -15.0, 70.0), (15.0, 15.0, 150.0)))],
		[Mod("CurlNoiseForce", Strength=Const(260.0), Frequency=Const(0.02), PanSpeed=Const(0.0, 0.0, 40.0)),
		 Mod("Drag", Drag=Const(0.8)),
		 Mod("ScaleColor", Scale=Curve((0.0, (1, 1, 1, 1)), (1.0, (0.2, 0.05, 0.02, 0.0))))],
		PSprite(1, Velocity=True, Stretch=0.015))
	return {"Name": "HD2DHarborCampfire", "Version": 2, "Emitters": [Flames, Embers]}


def WriteParticles():
	for Name, Doc in {"HD2DHarborSpray": SpraySystem(), "HD2DHarborMotes": MotesSystem(), "HD2DHarborCampfire": CampfireSystem()}.items():
		WriteJson(os.path.join(CONTENT, *H_FX.split("/"), f"{Name}.eparticle"), Doc)


def Fx(Name):
	return f"{H_FX}/{Name}.eparticle"


# ---- 씬 ---------------------------------------------------------------------------------------------------------------
def StandZ(Height, X, Y):
	# 서 있는 바닥 높이: 나무 잔교·방파제 위는 판 높이(지형은 바다 밑), 그 밖은 지형
	PX, PEnd, PW = Layout.PIER
	JX, _, JW = Layout.JETTY
	LX, LY = Layout.LIGHTHOUSE
	if abs(X - PX) <= PW * 0.5 + 5.0 and Layout.QUAY_Y - 5.0 <= Y <= PEnd + 5.0:
		return 0.0
	if (abs(X - JX) <= JW * 0.5 + 5.0 and Layout.QUAY_Y - 5.0 <= Y <= LY) or math.hypot(X - LX, Y - LY) <= 285.0:
		return 8.0
	return Height(X, Y)


NIGHT_LIGHT_PREFIX = "Night_"       # 밤에 켜는 점광원 (HD2DWorld.lua — Regions.etable NightLights)
NIGHT_WINDOW_PREFIX = "NightWin_"   # 밤에 불 켜는 창 유리 (머티리얼을 바꾼다)


def BuildScene(Height, Start=Layout.PLAYER_START, Overview=None, AutoPlay="", bGame=True):
	Rng = random.Random(311)
	S = FScene()
	Bounds = Main.FBoundsCache()
	Occupied = []
	NightIndex = [0]

	def Reserve(X, Y, Radius):
		Occupied.append((X, Y, Radius))

	def Free(X, Y, Radius):
		return all((X - OX) ** 2 + (Y - OY) ** 2 >= (Radius + OR) ** 2 for OX, OY, OR in Occupied)

	def BoxCollider(Name, Center, Half, Yaw=0.0):
		return S.Add(Name, {"BoxColliderComponent": {"HalfExtents": [float(V) for V in Half]}}, Center, QuatFromEuler(Yaw=Yaw))

	def Point(Name, Pos, Color, Intensity, Radius, Shadows=False, Flicker=None, Specular=1.0):
		Comps = {"PointLightComponent": {"Color": list(Color), "Intensity": Intensity, "Radius": Radius, "CastShadows": Shadows}}
		if Specular != 1.0:
			Comps["PointLightComponent"]["SpecularScale"] = Specular
		if Flicker:
			Comps["ScriptComponent"] = FlickerScript(Intensity, **Flicker)
		return S.Add(Name, Comps, Pos)

	def NightLight(Pos, Color, Intensity, Radius, Shadows=False, Flicker=None):
		# 밤에만 켜는 등불 (씬에는 켠 값으로 두고 HD2DWorld.lua가 시간에 따라 배율을 곱한다)
		I = NightIndex[0]
		NightIndex[0] += 1
		return Point(f"{NIGHT_LIGHT_PREFIX}{I}", Pos, Color, Intensity, Radius, Shadows, Flicker or {"Style": "Fire", "Seed": 600 + I, "Amount": 0.12, "Speed": 0.6})

	def Particles(Name, Asset, Pos, Speed=1.0, Rotation=None):
		return S.Add(Name, {"ParticleSystemComponent": {"Asset": Asset, "Playing": True, "Speed": Speed}}, Pos, Rotation)

	def Place(Name, Asset, X, Y, Yaw=0.0, Scale=1.0, Sink=0.0, Z=None, Collide=False, Shrink=0.85, Pitch=0.0, Roll=0.0, Parent=-1, ReserveR=None):
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
			Reserve(WX, WY, ReserveR if ReserveR else max(HX, HY) * 0.9)
		elif ReserveR:
			Reserve(X, Y, ReserveR)
		return Index

	def PH_(Name, Id, X, Y, Yaw=0.0, Scale=1.0, **Kw):
		Asset = AlphaModel(Id) if Id in ("wild_rooibos_bush", "fern_02") else f"{PH}/{Id}/{Id}.gltf"
		return Place(Name, Asset, X, Y, Yaw, Scale, **Kw)

	def Kit(Name, Mesh, X, Y, Yaw=0.0, Scale=1.0, Z=None, **Kw):
		return Place(Name, KitPath(Mesh), X, Y, Yaw, Scale, Z=Z, **Kw)

	def SpriteProp(Name, Slice, Pos, Flat=False, Scale=1.0, Lit=True, Billboard=2, Flipbook="", Yaw=0.0, Parent=-1, Shadows=True):
		# 도트 소품 스프라이트 (HD2DWorldArt — Sprites/HD2D/HarborProps.esprite): 세운 것은 세로축 빌보드, 눕힌 것(생선 더미 등)은 바닥에
		Comps = {"SpriteComponent": HD2DGameplay.Sprite("Sprites/HD2D/HarborProps.esprite", Slice, Lit=Lit, Shadows=Shadows and not Flat, Billboard=0 if Flat else Billboard)}
		if Flipbook:
			Comps["FlipbookComponent"] = HD2DGameplay.Flipbook(Flipbook)
		Rot = QuatFromEuler(Roll=-90.0, Yaw=Yaw) if Flat else (QuatFromEuler(Yaw=Yaw) if Yaw else None)
		Sc = Scale if isinstance(Scale, (list, tuple)) else (Scale, Scale, Scale)
		return S.Add(Name, Comps, Pos, Rot, Sc, Parent)

	E = Env.FDressing(S, Height, Rng, Reserve, BoxCollider, Point)

	# ---- 환경: 하늘·대기·구름 + 시간대 (HD2DWorld.lua가 게임 시각으로 돌린다) + 바닷바람 안개
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.92, 0.8], "Intensity": 6.5}}, (0, 0, 3000), QuatFromEuler(Pitch=-40, Yaw=Main.SUN_AZIMUTH + 180.0))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {"MoonIntensity": 0.35, "MoonColor": [0.62, 0.74, 1.0], "NightSkyColor": [0.012, 0.02, 0.042], "StarIntensity": 0.6},
		"VolumetricCloudComponent": {"Coverage": 0.36, "CloudType": 0.45, "WindSpeed": 7.0, "WindDirection": 20.0},
		"TimeOfDayComponent": {"TimeOfDay": 11.0, "DayLengthMinutes": 0.0, "MaxSunElevation": 50.0, "NorthAzimuth": Main.NORTH_AZIMUTH, "AnimateInEditor": False},
		"SkyLightComponent": {"Intensity": 4.0},
		"HeightFogComponent": {
			"Color": [0.5, 0.56, 0.62], "Density": 0.0007, "HeightFalloff": 0.07, "StartDistance": 1800.0, "MaxOpacity": 0.5,
			"DirectionalInscatteringColor": [0.9, 0.78, 0.6], "Volumetric": True, "VolumetricDistance": 6000.0,
			"VolumetricAlbedo": [0.95, 0.96, 1.0], "VolumetricExtinctionScale": 0.5, "VolumetricAnisotropy": 0.5,
			"VolumetricDirectionalScale": 0.45, "VolumetricLocalLightScale": 0.45},
	})
	S.Add("Terrain", {"TerrainComponent": {
		"Asset": "Terrain/Demo/HD2DHarbor.eterrain", "Size": [TERRAIN_SIZE, TERRAIN_SIZE], "HeightRange": HEIGHT_RANGE,
		"Layer0Material": HMat("TerrainGrass"), "Layer1Material": HMat("TerrainSand"), "Layer2Material": HMat("TerrainCobble"), "Layer3Material": HMat("TerrainRock"),
		"Layer0Tiling": 450.0, "Layer1Tiling": 420.0, "Layer2Tiling": 300.0, "Layer3Tiling": 520.0,
		"CastShadows": True, "Collision": True}}, (TERRAIN_CENTER[0], TERRAIN_CENTER[1], 0.0))
	S.Add("Foliage", {"FoliageComponent": {"Asset": "Foliage/Demo/HD2DHarbor.efoliage", "Visible": True}})
	# 바다: 물 상자 하나 (육지는 수면보다 높아 가려진다) — 맑은 청록, 잔잔한 물결, 물가 거품
	S.Add("Sea", {"WaterBodyComponent": {
		"Size": [17000.0, 12000.0, 500.0], "ScatterColor": [0.01, 0.07, 0.085], "Absorption": [0.42, 0.09, 0.07],
		"NormalStrength": 0.42, "WaveScale": 260.0, "WaveSpeed": 9.0, "FlowDirection": 20.0, "FlowSpeed": 6.0,
		"FoamIntensity": 0.75, "FoamDistance": 22.0, "RefractionStrength": 0.035, "ReflectionIntensity": 1.0, "Roughness": 0.06}},
		(1000.0, 2000.0, SEA - 250.0))
	S.Add("Sea_Sound", {"AudioSourceComponent": {"ClipAsset": "Audio/Demo/CampfireWaves.wav", "Volume": 0.55, "Pitch": 1.0, "Loop": True,
		"PlayOnStart": True, "Spatial": True, "MinDistance": 800.0, "MaxDistance": 5000.0}}, (-500.0, 1200.0, 0.0))
	S.Add("Sea_Sound2", {"AudioSourceComponent": {"ClipAsset": "Audio/Demo/CampfireWaves.wav", "Volume": 0.6, "Pitch": 0.9, "Loop": True,
		"PlayOnStart": True, "Spatial": True, "MinDistance": 800.0, "MaxDistance": 5000.0}}, (3800.0, 900.0, 0.0))

	# 보이지 않는 벽 (놀이 영역) + 물가 (바다로 걸어 들어가지 않게)
	(MinX, MinY), (MaxX, MaxY) = Layout.PLAY_MIN, Layout.PLAY_MAX
	MidX, MidY = (MinX + MaxX) * 0.5, (MinY + MaxY) * 0.5
	BoxCollider("Bound_Back", (MidX, MinY - 100.0, 300.0), ((MaxX - MinX) * 0.5 + 200.0, 100.0, 900.0))
	BoxCollider("Bound_Front", (MidX, MaxY + 100.0, 300.0), ((MaxX - MinX) * 0.5 + 200.0, 100.0, 900.0))
	BoxCollider("Bound_West", (MinX - 100.0, MidY, 300.0), (100.0, (MaxY - MinY) * 0.5 + 200.0, 900.0))
	BoxCollider("Bound_East", (MaxX + 100.0, MidY, 300.0), (100.0, (MaxY - MinY) * 0.5 + 200.0, 900.0))
	Openings = [(Layout.PIER[0] - Layout.PIER[2] * 0.5 + 10.0, Layout.PIER[0] + Layout.PIER[2] * 0.5 - 10.0),
				(Layout.JETTY[0] - Layout.JETTY[2] * 0.5 + 10.0, Layout.JETTY[0] + Layout.JETTY[2] * 0.5 - 10.0)]

	def ShoreWall(X0, X1, Inset, Step=200.0, Prefix="Shore"):
		# 물가를 따라 막는 상자 (해안선에서 Inset만큼 바다 쪽, 조각마다 기울기 따라 돌림). 잔교·방파제 입구 구간은 잘라 비운다
		#   (조각끼리는 30cm 겹치되 입구 쪽으로는 넘치지 않게 — 넘친 상자가 방파제 뿌리를 막았다)
		Pieces = []
		X = X0
		while X < X1:
			XB = min(X + Step, X1)
			Cuts = [(X, XB)]
			for A, B in Openings:
				Next = []
				for P0, P1 in Cuts:
					if P1 <= A or P0 >= B:
						Next.append((P0, P1))
					else:
						if P0 < A:
							Next.append((P0, A))
						if P1 > B:
							Next.append((B, P1))
				Cuts = Next
			Pieces.extend(P for P in Cuts if P[1] - P[0] > 20.0)
			X = XB
		for K, (PA, PB) in enumerate(Pieces):
			Over = 0.0 if any(abs(PA - B) < 1.0 or abs(PB - A) < 1.0 for A, B in Openings) else 30.0
			YA, YB = float(ShoreY(PA)) + Inset, float(ShoreY(PB)) + Inset
			Yaw = math.degrees(math.atan2(YB - YA, PB - PA))
			BoxCollider(f"{Prefix}_Wall_{K}", ((PA + PB) * 0.5, (YA + YB) * 0.5 + 60.0, 150.0), (math.hypot(PB - PA, YB - YA) * 0.5 + Over, 60.0, 400.0), Yaw)
	ShoreWall(MinX - 100.0, -2600.0, -40.0, Prefix="BeachW")
	ShoreWall(-2600.0, 1250.0, 0.0, Step=150.0, Prefix="Quay")
	ShoreWall(1250.0, 3750.0, -190.0, Prefix="Cliff")
	ShoreWall(3750.0, MaxX + 100.0, -40.0, Prefix="Cove")

	# 게임 자리 비움 (배경 무작위 배치가 피하게)
	for _, X, Y in Layout.ENEMIES + Layout.NIGHT_ENEMIES:
		Reserve(X, Y, 140.0)
	for _, X, Y in Layout.NPCS:
		Reserve(X, Y, 120.0)
	for X, Y, _ in Layout.CHESTS:
		Reserve(X, Y, 130.0)
	for _, X, Y in Layout.PROPS:
		Reserve(X, Y, 110.0)
	Reserve(*Layout.BOSS, Layout.BOSS_ARENA_RADIUS)
	for Name, (X, Y, Yaw) in Layout.SPAWNS.items():
		S.Add(f"Spawn_{Name}", {}, (X, Y, Height(X, Y)), QuatFromEuler(Yaw=Yaw))
		Reserve(X, Y, 150.0)

	C = FCtx(S=S, E=E, Height=Height, Rng=Rng, Reserve=Reserve, Free=Free, BoxCollider=BoxCollider, Point=Point, NightLight=NightLight,
			 Particles=Particles, Place=Place, PH_=PH_, Kit=Kit, SpriteProp=SpriteProp)
	BuildTown(C)
	BuildHarborFront(C)
	BuildCoast(C)

	# 창 유리: 불 켤 수 있는 창(HD2DEnvironment 집의 Lit/Dim 유리)을 NightWin_<n>으로 이름을 바꾸고 낮 값(어두운 유리)으로 둔다
	WinIndex = 0
	for Entity in S.Entities:
		Mesh = Entity["Components"].get("StaticMeshComponent")
		if Mesh and Mesh["MaterialAsset"] in (Env.Mat("EnvWindowLit"), Env.Mat("EnvWindowDim")):
			Entity["Name"] = f"{NIGHT_WINDOW_PREFIX}{WinIndex}"
			WinIndex += 1

	# 환경 파티클
	Particles("Env_Motes", Fx("HD2DHarborMotes"), (500.0, -200.0, 0.0))

	# ---- 풀 폴리지 (풀 레이어 위, 물체·길 피함) + 해안 사구 풀 + 언덕 나무
	GrassRng = np.random.default_rng(31)
	Grass, Dune, Trees = [], [], {"Tree": [], "Pine": []}
	P = GrassRng.uniform((-6500.0, -4200.0), (7500.0, 1500.0), size=(160000, 2))
	Keep = (Height.Layer(P[:, 0], P[:, 1], 0) >= 0.72) & (GrassRng.random(len(P)) < 0.55)
	Keep &= np.hypot(P[:, 0] - Start[0], P[:, 1] - Start[1]) >= 120.0
	for OX, OY, OR in Occupied:
		Keep &= (P[:, 0] - OX) ** 2 + (P[:, 1] - OY) ** 2 >= (OR * 0.8 + 20.0) ** 2
	for X, Y in P[Keep]:
		X, Y = float(X), float(Y)
		Z = Height(X, Y)
		if Z < SEA + 20.0:
			continue
		N = Height.Normal(X, Y)
		Grass.append((X, Y, Z, float(GrassRng.uniform(0, 360)), float(GrassRng.uniform(0.45, 0.85)), float(N[0]), float(N[1]), float(N[2])))
	DP = GrassRng.uniform((-5200.0, -900.0), (6200.0, 1200.0), size=(30000, 2))
	DKeep = (Height.Layer(DP[:, 0], DP[:, 1], 1) >= 0.5) & (Height.Layer(DP[:, 0], DP[:, 1], 0) >= 0.15)
	for OX, OY, OR in Occupied:
		DKeep &= (DP[:, 0] - OX) ** 2 + (DP[:, 1] - OY) ** 2 >= (OR * 0.8) ** 2
	for X, Y in DP[DKeep]:
		X, Y = float(X), float(Y)
		Z = Height(X, Y)
		if Z < SEA + 30.0:
			continue
		N = Height.Normal(X, Y)
		Dune.append((X, Y, Z, float(GrassRng.uniform(0, 360)), float(GrassRng.uniform(0.7, 1.2)), float(N[0]), float(N[1]), float(N[2])))
	Taken = []
	for X, Y in GrassRng.uniform((-6500.0, -6000.0), (7000.0, -1750.0), size=(7000, 2)):
		X, Y = float(X), float(Y)
		if Y > -2100.0 and X < 1300.0 and GrassRng.random() < 0.7:
			continue  # 집 뒤는 성기게 (지붕을 가리지 않게)
		if abs(X - Layout.ROAD[0][0]) < 380.0 and Y > -2700.0:
			continue  # 언덕길
		if any((X - TX) ** 2 + (Y - TY) ** 2 < 300.0 ** 2 for TX, TY in Taken[-300:]) or not Free(X, Y, 160.0):
			continue
		Z = Height(X, Y)
		if Z < 60.0:
			continue
		Taken.append((X, Y))
		Kind = "Pine" if GrassRng.random() < 0.7 else "Tree"
		Trees[Kind].append((X, Y, Z, float(GrassRng.uniform(0, 360)), float(GrassRng.uniform(1.1, 1.6)), 0.0, 0.0, 1.0))
	Foliage = [(HARBOR_GRASS, Grass), (HARBOR_DUNE, Dune), (Env.TREE_TYPE, Trees["Tree"]), (Env.PINE_TYPE, Trees["Pine"])]

	# ---- 게임: 관리자·HUD + 카메라 + 플레이어
	if bGame:
		Harbor = HD2DGameplay.FMapLayout(
			"Harbor", Npcs=Layout.NPCS, Chests=Layout.CHESTS, Props=Layout.PROPS, Enemies=Layout.ENEMIES, Boss=Layout.BOSS, BossKind="PirateCaptain",
			BossReward=Layout.BOSS_REWARD, Respawn=True,
			Extra={"BossChestAtSpawn": True,  # 보상 상자 = 보스 처음 자리 (물가에서 쓰러져도 닿는 곳)
				   "Gulls": "-2100,-1900,760,520;700,-2050,820,600;-3600,420,430,320;2900,150,620,520",  # 지붕 너머·모래톱·절벽 위 (카메라 앞은 비운다 — 흐린 덩어리로 가렸다)
				   "NightEnemies": ";".join(f"{K},{X:.0f},{Y:.0f},{Height(X, Y) + HD2DGameplay.ENEMY_CAPSULE[K][0] + HD2DGameplay.ENEMY_CAPSULE[K][1] + 4.0:.0f}"
											for K, X, Y in Layout.NIGHT_ENEMIES)})
		HD2DGameplay.AddGame(S, FStandHeight(Height), HARBOR_PATH, AutoPlay, Layout=Harbor, Title=False, NavMesh=NAV_ASSET, Minimap=MINIMAP)
	else:
		HD2DGameplay.AddGame(S, Height, HARBOR_PATH, AutoPlay, Layout=HD2DGameplay.FMapLayout("Harbor", Props=[("SavePoint", -2700.0, -520.0)]), Title=False, NavMesh=None,
							 Minimap=MINIMAP)
	Stand = FStandHeight(Height)
	(TX, TY), Half = Layout.TRAVEL_HOME
	HD2DGameplay.AddTravel(S, Height, "HartRoad_Travel", TX, TY, "Scenes/Demo/HD2D.escene", "Harbor", Half)
	StartZ = Stand(*Start) + HD2DGameplay.PLAYER_RADIUS + HD2DGameplay.PLAYER_HALF + 4.0
	Forward = (0.0, -math.cos(math.radians(-Main.CAMERA_PITCH)), -math.sin(math.radians(-Main.CAMERA_PITCH)))
	Focus = (Start[0], Start[1], StartZ - 85.0 + 70.0)
	# 항구 카메라: 메인과 같은 깊이 + 틸트시프트·육각 보케. 색 보정·비네트는 HD2DWorld.lua가 시각에 따라 덮어쓴다 (씬 값 = 한낮)
	S.Add("Camera", {"CameraComponent": {"FovYDegrees": Main.CAMERA_FOV, "NearZ": 50.0, "FarZ": 60000.0, "Primary": True, "Priority": 10},
					 "DepthOfFieldComponent": {"Enabled": Overview is None, "FocusDistance": Main.CAMERA_DISTANCE, "FocalRegion": Main.DOF_FOCAL_REGION,
											   "NearTransition": 700.0, "FarTransition": 1800.0, "NearBlurSize": 1.4, "FarBlurSize": 1.7,
											   "PreviewInEditor": False, "Mode": 2, "TiltShiftCenter": 0.53, "TiltShiftBand": 0.11,
											   "TiltShiftTransition": 0.38, "TiltShiftAngle": 0.0, "BokehBladeCount": 6, "BokehRotation": 15.0,
											   "BokehHighlightBoost": 3.0, "BokehHighlightThreshold": 1.0},
					 "ColorGradingComponent": {"Enabled": True, "Temperature": 0.05, "Tint": 0.0, "Saturation": 1.12, "Contrast": 1.08,
											   "Lift": [0.0, 0.006, 0.02], "Gamma": [1.0, 1.0, 1.0], "Gain": [1.02, 1.01, 1.0],
											   "LookupTable": "", "LookupTableIntensity": 1.0},
					 "VignetteComponent": {"Enabled": True, "Intensity": 0.45, "Size": 0.44, "Smoothness": 0.62, "Roundness": 1.0,
										   "Color": [0.02, 0.03, 0.05]}},
		  tuple(Focus[I] - Forward[I] * Main.CAMERA_DISTANCE for I in range(3)), QuatFromEuler(Pitch=Main.CAMERA_PITCH, Yaw=-90.0))
	if Overview:
		X, Y, Z, Pitch, Yaw, Fov = Overview
		S.Add("OverviewCamera", {"CameraComponent": {"FovYDegrees": Fov, "NearZ": 50.0, "FarZ": 80000.0, "Primary": True, "Priority": 100}},
			  (X, Y, Z), QuatFromEuler(Pitch=Pitch, Yaw=Yaw))
	HD2DGameplay.AddPlayer(S, Start, Stand)
	return S, Foliage


class FStandHeight:
	# 높이 함수 래퍼 (게임 배치용 — 잔교·방파제 위는 판 높이). 지형 높이 함수와 같은 호출 모양 + Normal/Layer 그대로
	def __init__(self, Height):
		self.Height = Height

	def __call__(self, X, Y):
		return StandZ(self.Height, X, Y)

	def __getattr__(self, Name):
		return getattr(self.Height, Name)


class FCtx:
	# 구역 빌더가 함께 쓰는 씬·도우미 묶음 (BuildScene이 만든다)
	def __init__(self, **Kw):
		self.__dict__.update(Kw)


def StreetLamp(C, Name, X, Y, Yaw=0.0):
	# Poly Haven 쇠 가로등 (387cm) + 밤 등불 (등 머리 높이)
	Z = C.Height(X, Y)
	C.PH_(Name, "street_lamp_01", X, Y, Yaw, 0.92, Sink=2.0)
	C.BoxCollider(f"{Name}_Collision", (X, Y, Z + 90.0), (14.0, 14.0, 90.0))
	C.NightLight((X, Y + 10.0, Z + 330.0), (1.0, 0.7, 0.42), 9.0, 950.0)
	C.Reserve(X, Y, 70.0)


def Signboard(C, Name, X, Y, Slices, Yaw=0.0):
	# 그림 표지판: 나무 기둥 + 도트 그림 판(HarborProps — 글자 없는 그림: 집·닻·해골 + 화살표)을 높이별로
	Z = C.Height(X, Y)
	C.E.Box(f"{Name}_Pole", (X, Y, Z + 95.0), (12.0, 12.0, 210.0), "EnvTimber")
	C.E.Box(f"{Name}_Cap", (X, Y, Z + 202.0), (20.0, 20.0, 8.0), "EnvTimber")
	for Index, (Slice, H, DX) in enumerate(Slices):
		C.SpriteProp(f"{Name}_Board{Index}", Slice, (X + DX, Y + 9.0, Z + H), Billboard=0, Yaw=Yaw)
	C.BoxCollider(f"{Name}_Collision", (X, Y, Z + 90.0), (12.0, 12.0, 90.0))
	C.Reserve(X, Y, 70.0)


def BuildTown(C):
	S, E, H, Rng = C.S, C.E, C.Height, C.Rng
	# ---- 뒤 집 줄 (앞면 = 카메라 쪽, 앞면 Y ≈ -1030) — 회벽·하늘색 덧창·청록 지붕의 항구 마을
	E.House("Cottage_W", -4250.0, -1230.0, 470.0, 440.0, H1=280.0, Ridge="X", Pitch=48.0, Roof="Harbor/RoofTeal", Ground="Harbor/PlasterBlue", Lit=0.6,
			Shutter="EnvShutterBlue", Chimney=0.4, GroundTimber=True)
	E.House("House_A", -2960.0, -1390.0, 560.0, 520.0, H1=280.0, H2=230.0, Ridge="Y", Pitch=50.0, Roof="Harbor/RoofOchre", Upper="Harbor/PlasterWhite",
			Ground="EnvStone", Lit=0.7, Shutter="EnvShutterBlue", Chimney=0.5)
	InnX, InnY, InnD = -2050.0, -1330.0, 560.0
	E.House("Inn", InnX, InnY, 880.0, InnD, H1=290.0, H2=260.0, Ridge="X", Pitch=44.0, Roof="Harbor/RoofBlue", Upper="Harbor/PlasterSand", Ground="EnvStone",
			Lit=0.9, Shutter="EnvShutterGreen", Chimney=-0.55, DoorX=0.0, Lantern=True)
	E.HangingSign("Inn_Sign", InnX + 200.0, InnY + InnD * 0.5, 255.0, "Bed")
	E.HangingSign("Inn_SignMug", InnX - 260.0, InnY + InnD * 0.5, 255.0, "Mug")
	C.NightLight((InnX + 95.0, InnY + InnD * 0.5 + 70.0, 235.0), (1.0, 0.62, 0.3), 6.0, 700.0)
	OffX, OffY, OffD = -820.0, -1290.0, 480.0
	E.House("Office", OffX, OffY, 620.0, OffD, H1=310.0, Ridge="X", Pitch=40.0, Roof="Harbor/RoofTeal", Ground="Harbor/PlasterWhite", Lit=0.6,
			Shutter="EnvShutterBlue", Chimney=0.45, GroundTimber=True, Lantern=True, DoorX=-120.0)
	C.Kit("Office_FlagPole", "HarborFlagPole", OffX + 380.0, OffY + OffD * 0.5 + 40.0, 0.0, 1.0, Collide=True)
	C.SpriteProp("Office_Flag", "FlagHarbor0", (OffX + 380.0 + 6.0, OffY + OffD * 0.5 + 40.0, H(OffX + 380.0, OffY + OffD * 0.5 + 40.0) + 330.0),
				 Billboard=0, Flipbook="Sprites/HD2D/HarborFlag_Harbor.eflipbook")
	E.House("House_B", 160.0, -1430.0, 500.0, 480.0, H1=270.0, H2=220.0, Ridge="Y", Pitch=52.0, Roof="Harbor/RoofTeal", Upper="Harbor/PlasterBlue",
			Ground="EnvStone", Lit=0.6, Shutter="EnvShutterRed", Chimney=0.4)
	WhX, WhY, WhD = 880.0, -1300.0, 600.0
	E.House("Warehouse", WhX, WhY, 880.0, WhD, H1=380.0, Ridge="X", Pitch=28.0, Roof="EnvRoofSlate", Ground="Harbor/PlanksGrey", Lit=0.0, Shutter=None,
			Chimney=None, FlowerBoxes=False, GroundWin=(60.0, 50.0), DoorX=200.0)
	E.Box("Warehouse_BigDoor", (WhX - 160.0, WhY + WhD * 0.5 + 3.0, 150.0), (260.0, 6.0, 300.0), "Harbor/PlanksBlue")
	for Side in (-1, 1):
		E.Box(f"Warehouse_DoorFrame{Side}", (WhX - 160.0 + Side * 138.0, WhY + WhD * 0.5 + 6.0, 155.0), (16.0, 10.0, 310.0), "EnvTimber")
	E.Box("Warehouse_DoorLintel", (WhX - 160.0, WhY + WhD * 0.5 + 6.0, 310.0), (300.0, 10.0, 16.0), "EnvTimber")
	E.Box("Warehouse_Pulley", (WhX - 160.0, WhY + WhD * 0.5 + 40.0, 420.0), (14.0, 80.0, 14.0), "EnvTimber")
	# 집 앞 꾸밈: 화분·통·벤치·빨랫줄
	for Index, (Id, X, Y, Yaw, Sc) in enumerate((("potted_plant_02", -4020.0, -980.0, 0.0, 1.0), ("wine_barrel_01", -3220.0, -1100.0, 30.0, 1.0),
												  ("potted_plant_04", -2560.0, -1010.0, 0.0, 1.0), ("painted_wooden_bench", -1650.0, -960.0, 0.0, 1.0),
												  ("potted_plant_02", -1110.0, -1020.0, 0.0, 1.0), ("wooden_barrels_01", 460.0, -1060.0, 70.0, 0.75),
												  ("wooden_crate_02", 1330.0, -1000.0, 15.0, 1.0), ("wooden_crate_01", 1300.0, -920.0, 50.0, 0.9))):
		C.PH_(f"Town_{Id}_{Index}", Id, X, Y, Yaw, Sc, Sink=2.0, Collide=True)
	E.Laundry("Town_Laundry", (-2620.0, -1090.0, 250.0), (-2470.0, -1080.0, 250.0))
	E.Bunting("Town_Bunting", (-2620.0, -1020.0, 360.0), (-1150.0, -1030.0, 340.0), Sag=70.0)
	# 거리 가로등
	for Index, (X, Y) in enumerate(((-3250.0, -800.0), (-1520.0, -820.0), (-250.0, -820.0), (1150.0, -800.0))):
		StreetLamp(C, f"Lamp_Street{Index}", X, Y)
	# ---- 하르트 마을로 가는 언덕길: 나무 아치 문 + 매단 등 + 길가 울타리 + 표지판
	AX, AY = -3650.0, -1560.0
	AZ = H(AX, AY)
	for Side in (-1, 1):
		E.Box(f"RoadGate_Post{Side}", (AX + Side * 190.0, AY, AZ + 150.0), (22.0, 22.0, 330.0), "EnvTimber")
		C.BoxCollider(f"RoadGate_Post{Side}_Collision", (AX + Side * 190.0, AY, AZ + 100.0), (14.0, 14.0, 100.0))
		C.PH_(f"RoadGate_Lantern{Side}", "wooden_lantern_01", AX + Side * 150.0, AY + 14.0, 0.0, 1.3, Z=AZ + 232.0)
		C.NightLight((AX + Side * 150.0, AY + 30.0, AZ + 250.0), (1.0, 0.62, 0.32), 5.0, 600.0)
	E.Box("RoadGate_Beam", (AX, AY, AZ + 312.0), (430.0, 26.0, 22.0), "EnvTimber")
	E.Box("RoadGate_Beam2", (AX, AY + 2.0, AZ + 286.0), (380.0, 18.0, 14.0), "EnvTimber")
	E.Box("RoadGate_Board", (AX, AY + 14.0, AZ + 296.0), (190.0, 5.0, 44.0), "EnvPlanks")
	C.SpriteProp("RoadGate_Emblem", "SignAnchor", (AX, AY + 18.0, AZ + 274.0), Billboard=0)
	for Side, (Y0, Y1) in ((-1, (-1880.0, -1620.0)), (1, (-1880.0, -1620.0))):
		E.Fence(f"Road_Fence{Side}", (AX + Side * 260.0, Y0), (AX + Side * 250.0, Y1), Spacing=130.0, Height=90.0)
	Signboard(C, "Road_Sign", -3330.0, -1150.0, [("SignVillage", 175.0, 0.0), ("SignMarket", 135.0, 0.0), ("SignDanger", 95.0, 0.0)])
	for Index, (X, Y) in enumerate(((-3980.0, -1700.0), (-3300.0, -1780.0), (-4100.0, -1500.0))):
		C.PH_(f"Road_Bush{Index}", Rng.choice(["shrub_02", "shrub_03", "wild_rooibos_bush"]), X, Y, Rng.uniform(0, 360), Rng.uniform(0.6, 0.85), Sink=4.0)
		C.Reserve(X, Y, 80.0)
	for Index, (X, Y, Sc) in enumerate(((-3950.0, -1900.0, 0.9), (-3350.0, -1950.0, 1.1), (-4300.0, -1750.0, 0.8))):
		C.PH_(f"Road_Rock{Index}", "rock_moss_set_01" if Index % 2 else "boulder_01", X, Y, Rng.uniform(0, 360), Sc, Sink=10.0)
		C.Reserve(X, Y, 100.0)


def LanternPost(C, Name, X, Y, Z=None):
	# 나무 기둥 등불 (메인 맵과 같은 모양) — 밤 등불
	G = C.Height(X, Y) if Z is None else Z
	C.E.Box(f"{Name}_Post", (X, Y, G + 110.0), (13.0, 13.0, 240.0), "EnvTimber")
	C.E.Box(f"{Name}_Cap", (X, Y, G + 232.0), (30.0, 30.0, 6.0), "EnvTimber")
	C.PH_(f"{Name}_Lantern", "wooden_lantern_01", X, Y, C.Rng.uniform(0, 360), 1.6, Z=G + 235.0)
	C.NightLight((X, Y, G + 275.0), (1.0, 0.62, 0.3), 7.0, 800.0)
	C.BoxCollider(f"{Name}_Collision", (X, Y, G + 100.0), (14.0, 14.0, 100.0))
	C.Reserve(X, Y, 60.0)


def FishStall(C, Name, X, Y, Awning, Fish=("FishPileA", "FishPileB", "FishPileC")):
	# 어시장 노점 (HD2DEnvironment.Stall 골격 + 생선 상자 위 도트 생선 더미(눕힌 스프라이트) + 처마 밑 매단 생선)
	E, Rng = C.E, C.Rng
	W, D = 280.0, 150.0
	Z = C.Height(X, Y)
	Root = E.Group(Name, (X, Y, Z))
	for SX in (-1, 1):
		for SY, H in ((1, 215.0), (-1, 265.0)):
			E.Box(f"{Name}_Post{SX}{SY}", (SX * (W * 0.5 - 6.0), SY * (D * 0.5 - 6.0), H * 0.5), (11.0, 11.0, H), "EnvTimber", None, Root)
	E.Box(f"{Name}_Counter", (0.0, D * 0.5 - 30.0, 40.0), (W - 10.0, 56.0, 80.0), "Harbor/PlanksBlue", None, Root)
	E.Box(f"{Name}_CounterTop", (0.0, D * 0.5 - 28.0, 83.0), (W + 10.0, 66.0, 6.0), "EnvTimber", None, Root)
	E.Box(f"{Name}_Back", (0.0, -D * 0.5 + 15.0, 70.0), (W - 20.0, 26.0, 140.0), "EnvPlanks", None, Root)
	Tilt = math.degrees(math.atan2(50.0, D + 50.0))
	E.Box(f"{Name}_Awning", (0.0, 8.0, 247.0), (W + 50.0, D + 70.0, 4.0), Awning, QuatFromEuler(Roll=Tilt), Root)
	E.Box(f"{Name}_Valance", (0.0, D * 0.5 + 43.0, 207.0), (W + 50.0, 3.0, 26.0), Awning, None, Root)
	for Index, Slice in enumerate(Fish):
		GX = (Index - (len(Fish) - 1) * 0.5) * 86.0
		E.Box(f"{Name}_Crate{Index}", (GX, D * 0.5 - 28.0, 95.0), (74.0, 50.0, 18.0), "EnvPlanks", None, Root)
		C.SpriteProp(f"{Name}_Fish{Index}", Slice, (X + GX, Y + D * 0.5 - 28.0, Z + 105.0), Flat=True, Yaw=Rng.uniform(-6, 6))
	C.SpriteProp(f"{Name}_Hanging", "FishHanging", (X, Y + D * 0.5 + 30.0, Z + 196.0), Billboard=0)
	C.BoxCollider(f"{Name}_Collision", (X, Y, Z + 60.0), (W * 0.5, D * 0.5, 60.0))
	C.Reserve(X, Y, 180.0)


def BuildHarborFront(C):
	S, E, H, Rng = C.S, C.E, C.Height, C.Rng
	QY = Layout.QUAY_Y
	# ---- 석축 부두 (앞면 Y = QUAY_Y): 젖은 아랫단 + 마른 윗단 + 화강암 갓돌 — 잔교 자리는 갓돌만 비우고 방파제 자리는 방파제가 잇는다
	X0, X1 = Layout.QUAY_X[0], Layout.JETTY[0] - Layout.JETTY[2] * 0.5
	Count = int(math.ceil((X1 - X0) / 320.0))
	for K in range(Count):
		A, B = X0 + (X1 - X0) * K / Count, X0 + (X1 - X0) * (K + 1) / Count
		CX = (A + B) * 0.5
		E.Box(f"Quay_Wet_{K}", (CX, QY + 36.0, -240.0), (B - A + 2.0, 80.0, 380.0), "Harbor/QuayStoneWet")
		E.Box(f"Quay_Dry_{K}", (CX, QY + 36.0, -28.0), (B - A + 2.0, 82.0, 48.0), "Harbor/QuayStone")
		if not (Layout.PIER[0] - Layout.PIER[2] * 0.5 - 20.0 < CX < Layout.PIER[0] + Layout.PIER[2] * 0.5 + 20.0):
			E.Box(f"Quay_Cap_{K}", (CX, QY + 30.0, -1.0), (B - A - 4.0, 94.0, 14.0), "Harbor/QuayStone", QuatFromEuler(Yaw=Rng.uniform(-0.4, 0.4)))
	# 서쪽 끝: 모래톱으로 내려가는 옆벽 + 기둥돌
	E.Box("Quay_WestEnd", (X0 - 18.0, QY - 120.0, -60.0), (40.0, 340.0, 130.0), "Harbor/QuayStone")
	E.Box("Quay_WestPillar", (X0 - 10.0, QY + 40.0, 30.0), (70.0, 90.0, 90.0), "Harbor/Granite")
	C.BoxCollider("Quay_WestPillar_Collision", (X0 - 10.0, QY + 40.0, 40.0), (35.0, 45.0, 60.0))
	for Index, X in enumerate((-2300.0, -1250.0, -420.0, 420.0)):
		C.Kit(f"Quay_Bollard{Index}", "HarborBollard", X, QY - 12.0, 0.0, 1.0, Z=5.0)
		C.BoxCollider(f"Quay_Bollard{Index}_Collision", (X, QY - 12.0, 30.0), (16.0, 16.0, 30.0))
		C.Reserve(X, QY - 12.0, 60.0)
	# 앉아 쉬는 갈매기 (계류 기둥·잔교 말뚝 위)
	for Index, (X, Y, Z) in enumerate(((-1250.0, QY - 12.0, 62.0), (420.0, QY - 12.0, 62.0), (Layout.PIER[0] - Layout.PIER[2] * 0.5 - 4.0, QY + 275.0, 60.0))):
		C.SpriteProp(f"Gull_Perch{Index}", "GullStand0", (X, Y, Z), Flipbook="Sprites/HD2D/Harbor_GullStand.eflipbook", Scale=1.1)
	C.PH_("Quay_Ladder", "wooden_ladder", -820.0, QY + 70.0, 90.0, (1.0, 1.0, 1.7), Z=-150.0, Pitch=-12.0)
	for Index, X in enumerate((-2350.0, -950.0, 420.0)):
		StreetLamp(C, f"Lamp_Quay{Index}", X, QY - 70.0)

	# ---- 나무 잔교 + 끝 등불 + 짐 + 나룻배
	PX, PEnd, PW = Layout.PIER
	C.Kit("Pier", "HarborPier", PX, QY, 0.0, 1.0, Z=0.0)
	Mid = (QY + PEnd) * 0.5
	C.BoxCollider("Pier_Deck", (PX, Mid, -16.0), (PW * 0.5, (PEnd - QY) * 0.5 + 20.0, 16.0))
	for Side in (-1, 1):
		C.BoxCollider(f"Pier_Rail{Side}", (PX + Side * (PW * 0.5 + 12.0), Mid + 30.0, 60.0), (12.0, (PEnd - QY) * 0.5 - 20.0, 90.0))
	C.BoxCollider("Pier_End", (PX, PEnd + 16.0, 60.0), (PW * 0.5 + 24.0, 12.0, 90.0))
	LanternPost(C, "Pier_Lantern", PX + PW * 0.5 - 30.0, PEnd - 70.0, Z=0.0)
	for Index, (Id, DX, DY, Yaw, Sc) in enumerate((("wooden_crate_02", -95.0, 520.0, 12.0, 1.0), ("wine_barrel_01", 100.0, 420.0, 0.0, 0.95),
												   ("wicker_basket_01", 70.0, 850.0, 20.0, 1.0), ("wooden_bucket_01", -90.0, 900.0, 0.0, 1.0))):
		C.PH_(f"Pier_{Id}_{Index}", Id, PX + DX, QY + DY, Yaw, Sc, Z=0.0, Collide=Id != "wooden_bucket_01")
	C.SpriteProp("Pier_FishCrate", "FishPileA", (PX - 95.0, QY + 520.0, 57.0), Flat=True, Scale=0.85)
	C.Kit("Pier_Boat0", "HarborRowboatBlue", PX + PW * 0.5 + 120.0, QY + 520.0, 92.0, 1.0, Z=SEA - 30.0)
	C.Kit("Pier_Boat1", "HarborRowboatRed", PX - PW * 0.5 - 120.0, QY + 760.0, 86.0, 1.0, Z=SEA - 30.0)
	C.Kit("Pier_Boat2", "HarborRowboatWood", PX + PW * 0.5 + 140.0, QY + 860.0, 97.0, 0.95, Z=SEA - 30.0)
	C.PH_("Harbor_Ship", "dutch_ship_medium", -380.0, 1760.0, 90.0, 0.3, Z=SEA + 4.0)
	C.PH_("Harbor_Buoy0", "ocean_buoy", -2500.0, 1150.0, 0.0, 0.55, Z=SEA - 10.0)
	C.PH_("Harbor_Buoy1", "ocean_buoy", 520.0, 1250.0, 30.0, 0.5, Z=SEA - 10.0)

	# ---- 방파제 + 등대 (끝 둥근 돌 단 동쪽) — 서쪽 절반이 걷는 길
	JX, JEnd, JW = Layout.JETTY
	LX, LY = Layout.LIGHTHOUSE
	JY1 = LY - 40.0
	E.Box("Jetty_Wet", (JX, (QY + JY1) * 0.5, -230.0), (JW, JY1 - QY, 400.0), "Harbor/QuayStoneWet")
	E.Box("Jetty_Dry", (JX, (QY + JY1) * 0.5, -28.0), (JW + 2.0, JY1 - QY, 48.0), "Harbor/QuayStone")
	E.Box("Jetty_Top", (JX, (QY + JY1) * 0.5 - 20.0, -2.0), (JW + 14.0, JY1 - QY + 40.0, 14.0), "Harbor/QuayStone")
	C.Kit("Jetty_Round", "HarborJettyRound", LX, LY, 0.0, 1.0, Z=10.0)
	C.Kit("Lighthouse", "HarborLighthouse", LX, LY, 0.0, 1.0, Z=10.0)
	C.BoxCollider("Lighthouse_Collision", (LX, LY, 200.0), (150.0, 150.0, 200.0))
	C.Reserve(LX, LY, 200.0)
	# 등실 유리 8장 (HD2DWorld.lua: 퀘스트 전 꺼짐 / 뒤에는 밤에 켜짐) + 불 심지 + 빛 + 회전 빛줄기
	GZ = 10.0 + 350.0 + 12.0 + 14.0 + 43.0
	for K in range(8):
		A = K / 8.0 * 2.0 * math.pi
		S.Add(f"Lighthouse_Glass_{K}", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": HMat("LampOff")}},
			  (LX + math.cos(A) * 84.0, LY + math.sin(A) * 84.0, GZ), QuatFromEuler(Yaw=math.degrees(A)), (0.03, 0.66, 0.84))
	S.Add("Lighthouse_Core", {"StaticMeshComponent": {"MeshAsset": "primitive:sphere", "MaterialAsset": HMat("LampOff")}}, (LX, LY, GZ), None, (0.5, 0.5, 0.6))
	C.Point("Lighthouse_Lamp", (LX, LY, GZ), (1.0, 0.78, 0.45), 0.0, 1600.0)
	S.Add("Lighthouse_Beam", {"SpotLightComponent": {"Color": [1.0, 0.88, 0.62], "Intensity": 0.0, "Radius": 5200.0, "InnerConeAngle": 4.0,
													  "OuterConeAngle": 9.0, "CastShadows": False, "SpecularScale": 0.3}}, (LX, LY, GZ), QuatFromEuler(Pitch=-4.0, Yaw=0.0))
	# 걷는 바닥 (방파제 판 + 둥근 단 — 지형은 바다 밑이라 상자로 받친다)
	C.BoxCollider("Jetty_Deck", (JX, (QY + JY1) * 0.5, -12.0), (JW * 0.5, (JY1 - QY) * 0.5 + 10.0, 16.0))
	for K in range(3):
		Yaw = K * 60.0
		C.BoxCollider(f"Jetty_RoundDeck{K}", (LX, LY, -6.0), (282.0, 160.0, 16.0), Yaw)
	# 방파제 옆 막이 + 둥근 단 둘레 막이
	C.BoxCollider("Jetty_SideW", (JX - JW * 0.5 - 12.0, (QY + JY1) * 0.5 + 20.0, 60.0), (12.0, (JY1 - QY) * 0.5, 90.0))
	C.BoxCollider("Jetty_SideE", (JX + JW * 0.5 + 12.0, (QY + LY - 250.0) * 0.5 + 20.0, 60.0), (12.0, (LY - 250.0 - QY) * 0.5, 90.0))
	for K in range(14):
		A = math.radians(-60.0 + K * 240.0 / 13.0)
		C.BoxCollider(f"Jetty_Ring{K}", (LX + math.cos(A) * 300.0, LY + math.sin(A) * 300.0, 60.0), (16.0, 75.0, 90.0), math.degrees(A))
	for Index, (X, Y) in enumerate(((JX + JW * 0.5 - 24.0, 820.0), (JX - JW * 0.5 + 12.0, 1000.0))):  # 서쪽 걷는 길은 비운다
		C.Kit(f"Jetty_Bollard{Index}", "HarborBollard", X, Y, 0.0, 0.9, Z=5.0)
		C.BoxCollider(f"Jetty_Bollard{Index}_Collision", (X, Y, 30.0), (15.0, 15.0, 30.0))
	E.Box("Jetty_BuoyPost", (JX + JW * 0.5 - 30.0, 430.0, 70.0), (12.0, 12.0, 150.0), "EnvTimber")
	C.PH_("Jetty_Lifebuoy", "lifebuoy", JX + JW * 0.5 - 22.0, 440.0, 90.0, 1.3, Z=95.0)
	C.PH_("Jetty_Cannon", "cannon_01", LX - 300.0, LY + 120.0, 70.0, 0.85, Z=10.0, Collide=True)
	LanternPost(C, "Jetty_Lantern", JX + JW * 0.5 - 30.0, 680.0, Z=0.0)  # 동쪽 가장자리 (서쪽 걷는 길을 막지 않게)
	C.PH_("Jetty_Rope", "wooden_barrels_01", JX + 150.0, 520.0, 100.0, 0.5, Z=0.0, Collide=True)

	# ---- 어시장 광장: 생선 노점 셋 + 바닥 생선 상자 + 닻 기념비 + 벤치
	FishStall(C, "Market_Stall0", -2420.0, -390.0, "EnvAwningBlue", ("FishPileA", "FishPileC", "FishPileB"))
	FishStall(C, "Market_Stall1", -2030.0, -420.0, "EnvAwningRed", ("FishPileB", "FishPileA", "CrabPile"))
	FishStall(C, "Market_Stall2", -1570.0, -390.0, "EnvAwningGreen", ("FishPileC", "ShellPile", "FishPileA"))
	for Index, (X, Y, Slice) in enumerate(((-2300.0, 60.0, "FishPileB"), (-2210.0, 110.0, "FishPileA"), (-1450.0, 70.0, "CrabPile"))):
		C.PH_(f"Market_Crate{Index}", "wooden_crate_02", X, Y, Rng.uniform(-10, 10), 0.9, Sink=1.0, Collide=True)
		C.SpriteProp(f"Market_CrateFish{Index}", Slice, (X, Y, H(X, Y) + 52.0), Flat=True, Scale=0.9)
	for Index, (Id, X, Y) in enumerate((("wicker_basket_02", -2150.0, -200.0), ("wicker_basket_01", -1720.0, -230.0), ("wooden_bucket_01", -2560.0, -230.0),
										("jug_01", -1640.0, -215.0), ("wine_barrel_01", -2700.0, -300.0))):
		C.PH_(f"Market_{Id}_{Index}", Id, X, Y, Rng.uniform(0, 360), 1.0, Sink=1.0, Collide=Id in ("wine_barrel_01",))
	MX, MY = -1180.0, -70.0
	E.Box("Monument_Base", (MX, MY, 22.0), (190.0, 190.0, 64.0), "Harbor/Granite")
	E.Box("Monument_Step", (MX, MY, 4.0), (240.0, 240.0, 20.0), "Harbor/QuayStone")
	C.Kit("Monument_Anchor", "HarborAnchor", MX, MY, 15.0, 1.0, Z=54.0)
	C.BoxCollider("Monument_Collision", (MX, MY, 60.0), (100.0, 100.0, 60.0))
	C.Reserve(MX, MY, 180.0)
	for Index, (X, Y, Yaw) in enumerate(((-1180.0, 170.0, -90.0), (-2500.0, 150.0, -90.0))):
		C.PH_(f"Market_Bench{Index}", "painted_wooden_bench", X, Y, Yaw, 1.0, Collide=True)

	# ---- 창고 부두: 짐 상자·통·자루·수레
	for Index, (Id, X, Y, Yaw, Sc, Z) in enumerate((("wooden_crate_02", -480.0, -280.0, 5.0, 1.0, None), ("wooden_crate_02", -400.0, -300.0, 18.0, 1.0, None),
													 ("wooden_crate_01", -440.0, -290.0, 40.0, 0.9, 44.0), ("wooden_barrels_01", 120.0, -260.0, 20.0, 0.8, None),
													 ("wooden_crate_01", 520.0, -320.0, 0.0, 1.0, None), ("wooden_crate_02", 600.0, -330.0, 30.0, 1.0, None),
													 ("wooden_crate_02", 560.0, -320.0, 60.0, 0.9, 44.0), ("wine_barrel_01", 760.0, -200.0, 0.0, 1.0, None),
													 ("wine_barrel_01", 820.0, -150.0, 0.0, 1.0, None), ("wooden_crate_01", 260.0, 120.0, 10.0, 1.0, None))):
		C.PH_(f"Cargo_{Id}_{Index}", Id, X, Y, Yaw, Sc, Sink=1.0, Z=(H(X, Y) + Z) if Z else None, Collide=Z is None)
	for Index, (X, Y, Z, Yaw) in enumerate(((560.0, -830.0, 0.0, 0.0), (650.0, -835.0, 0.0, 4.0), (605.0, -830.0, 44.0, -6.0), (740.0, -820.0, 0.0, 8.0))):
		C.PH_(f"Cargo_Stack{Index}", "wooden_crate_02", X, Y, Yaw, 1.0, Z=H(X, Y) + Z, Collide=Z == 0.0)
	C.PH_("Cargo_Barrels", "wooden_barrels_01", -120.0, -330.0, 15.0, 0.75, Sink=1.0, Collide=True)

	# ---- 서쪽 모래톱: 끌어올린 나룻배 · 그물 틀 · 유목 · 바위 · 조개
	C.Kit("Beach_Boat0", "HarborRowboatWood", -3350.0, 430.0, 158.0, 1.0, Z=H(-3350.0, 430.0) - 6.0, Collide=True)
	C.Kit("Beach_Boat1", "HarborRowboatRed", -4080.0, 690.0, 118.0, 1.0, Z=H(-4080.0, 690.0) - 10.0)
	C.Kit("Beach_Net0", "HarborNetRack", -3020.0, 60.0, 4.0, 1.0, Collide=True)
	C.Kit("Beach_Net1", "HarborNetRack", -3720.0, 170.0, -8.0, 1.0, Collide=True)
	C.PH_("Beach_Driftwood", "dead_tree_trunk", -2880.0, 560.0, 70.0, 0.8, Sink=6.0)
	for Index, (Id, X, Y, Sc) in enumerate((("sand_rocks_small_01", -4300.0, 480.0, 0.7), ("coast_rocks_05", -3700.0, 860.0, 0.8),
											("boulder_01", -4380.0, 200.0, 0.9), ("coast_rocks_05", -2860.0, 780.0, 0.6))):
		C.PH_(f"Beach_Rock{Index}", Id, X, Y, Rng.uniform(0, 360), Sc, Sink=12.0)
	for Index in range(9):
		X, Y = Rng.uniform(-4300.0, -2800.0), Rng.uniform(250.0, 620.0)
		if H(X, Y) > SEA + 8.0 and C.Free(X, Y, 60.0):
			C.SpriteProp(f"Beach_Shell{Index}", Rng.choice(["Shell0", "Shell1", "Starfish"]), (X, Y, H(X, Y) + 1.5), Flat=True, Yaw=Rng.uniform(0, 360))
	LanternPost(C, "Beach_Lantern", -3450.0, 140.0)
	for Index, (X, Y) in enumerate(((-3150.0, 330.0), (-3900.0, 330.0))):
		C.PH_(f"Beach_Trap{Index}", "wooden_crate_01", X, Y, Rng.uniform(0, 90), 0.8, Sink=6.0, Collide=True)


def BuildCoast(C):
	S, E, H, Rng = C.S, C.E, C.Height, C.Rng
	# ---- 마을 동쪽 문 (여기부터 마물 구역): 울타리 + 문기둥 + 경고 표지판
	GX = Layout.EAST_GATE_X
	E.Fence("EastGate_FenceN", (GX, -960.0), (GX, -360.0), Spacing=150.0, Height=100.0)
	E.Fence("EastGate_FenceS", (GX, 260.0), (GX + 60.0, 430.0), Spacing=120.0, Height=100.0)
	for Index, Y in enumerate((-330.0, 230.0)):
		G = H(GX, Y)
		E.Box(f"EastGate_Post{Index}", (GX, Y, G + 120.0), (24.0, 24.0, 270.0), "EnvTimber")
		E.Box(f"EastGate_PostCap{Index}", (GX, Y, G + 258.0), (34.0, 34.0, 12.0), "EnvTimber")
		C.BoxCollider(f"EastGate_Post{Index}_Collision", (GX, Y, G + 100.0), (16.0, 16.0, 100.0))
	E.Box("EastGate_Beam", (GX, -50.0, H(GX, -50.0) + 236.0), (20.0, 600.0, 20.0), "EnvTimber")
	C.SpriteProp("EastGate_Skull", "SignDanger", (GX + 12.0, -50.0, H(GX, -50.0) + 206.0), Billboard=0)
	Signboard(C, "EastGate_Sign", GX - 150.0, -420.0, [("SignDanger", 170.0, 0.0), ("SignLighthouse", 130.0, 0.0)])
	StreetLamp(C, "Lamp_EastGate", GX - 140.0, 330.0)

	# ---- 해안 절벽 길: 벼랑 가장자리 나무 난간(갈 수 없는 곳 표시) + 벼랑 바위 + 파도 물보라
	Xs = list(np.arange(1700.0, 3650.0, 210.0))
	for A, B in zip(Xs[:-1], Xs[1:]):
		YA, YB = float(ShoreY(A)) - 175.0, float(ShoreY(B)) - 175.0
		E.Fence(f"Cliff_Rail_{int(A)}", (A, YA), (B, YB), Spacing=210.0, Height=85.0, Collide=False)
	# 벼랑 면: Poly Haven 해안 절벽 조각을 가장자리를 따라 이어 붙인다 (윗면 = 길 높이 + 20 — 지형의 가파른 면이 보이지 않게)
	for Index, (X, Id, Sc, Yaw) in enumerate(((1560.0, "coastal_cliff_02", 0.26, 96.0), (2350.0, "coastal_cliff_02", 0.32, 88.0),
											   (3180.0, "coastal_cliff_02", 0.32, 93.0), (3800.0, "coastal_cliff_02", 0.26, 84.0))):
		Top = float(PathZ(X)) + 25.0
		Z = Top - 983.0 * Sc
		C.PH_(f"Cliff_Face{Index}", Id, X, float(ShoreY(X)) - 20.0, Yaw, Sc, Z=Z)
	for Index, (X, Sc) in enumerate(((1520.0, 0.55), (1980.0, 0.7), (2400.0, 0.6), (2850.0, 0.75), (3250.0, 0.6), (3620.0, 0.7))):
		Y = float(ShoreY(X)) + 150.0
		C.PH_(f"Cliff_SeaRock{Index}", "coast_rocks_05" if Index % 2 else "coast_land_rocks_03", X, Y, Rng.uniform(0, 360), Sc, Z=SEA - 50.0)
	for Index, X in enumerate((1900.0, 2700.0, 3450.0)):
		C.Particles(f"Cliff_Spray{Index}", Fx("HD2DHarborSpray"), (X, float(ShoreY(X)) + 130.0, SEA))
	# 길 뒤(-Y) 바위·덤불 (언덕 비탈 시작)
	for Index in range(14):
		for _ in range(40):
			X, Y = Rng.uniform(1600.0, 3700.0), Rng.uniform(-1100.0, -780.0)
			if C.Free(X, Y, 130.0):
				break
		Id = Rng.choice(["boulder_01", "rock_moss_set_01", "rock_moss_set_02", "shrub_02", "wild_rooibos_bush", "shrub_03"])
		Sc = Rng.uniform(0.7, 1.1) if "rock" in Id or "boulder" in Id else Rng.uniform(0.6, 0.9)
		C.PH_(f"Coast_Back{Index}", Id, X, Y, Rng.uniform(0, 360), Sc, Sink=8.0, Collide=Id == "boulder_01", Shrink=0.6)
		C.Reserve(X, Y, 110.0)

	# ---- 해적 야영지 (절벽 위 평지): 천막 둘 + 모닥불 + 해적 깃발 + 짐 + 대포
	TopZ = Layout.CLIFF_TOP
	E.Tent("Pirate_TentA", 2620.0, -800.0, 8.0, 320.0, 270.0, 200.0, "Harbor/SailCloth")
	E.Tent("Pirate_TentB", 3180.0, -720.0, -14.0, 290.0, 250.0, 185.0, "EnvTentGreen")
	FX_, FY_ = 2880.0, -420.0
	FZ = H(FX_, FY_)
	C.PH_("Pirate_FirePit", "stone_fire_pit", FX_, FY_, 15.0, 1.0, Sink=10.0)
	C.Particles("Pirate_Fire", Fx("HD2DHarborCampfire"), (FX_, FY_, FZ + 12.0))
	C.Point("Pirate_FireLight", (FX_, FY_, FZ + 90.0), (1.0, 0.52, 0.2), 14.0, 1200.0, True, Flicker={"Style": "Fire", "Seed": 41})
	C.BoxCollider("Pirate_FirePit_Collision", (FX_, FY_, FZ + 20.0), (65.0, 65.0, 40.0))
	C.Reserve(FX_, FY_, 170.0)
	S.Add("Pirate_FireSound", {"AudioSourceComponent": {"ClipAsset": "Audio/Demo/CampfireCrackle.wav", "Volume": 0.7, "Pitch": 1.0, "Loop": True,
		"PlayOnStart": True, "Spatial": True, "MinDistance": 200.0, "MaxDistance": 2200.0}}, (FX_, FY_, FZ + 40.0))
	for Index, (X, Y, Yaw) in enumerate(((2990.0, -300.0, -10.0), (3070.0, -400.0, 70.0))):  # 모닥불 동쪽 (서쪽은 렌즈로 가는 길)
		C.PH_(f"Pirate_Log{Index}", "dead_tree_trunk", X, Y, Yaw, 0.55, Sink=4.0, Collide=True, Shrink=0.7)
	PoleX, PoleY = 2420.0, -560.0
	C.Kit("Pirate_FlagPole", "HarborFlagPole", PoleX, PoleY, 0.0, 1.0, Collide=True)
	C.SpriteProp("Pirate_Flag", "FlagPirate0", (PoleX + 6.0, PoleY, H(PoleX, PoleY) + 330.0), Billboard=0, Flipbook="Sprites/HD2D/HarborFlag_Pirate.eflipbook")
	for Index, (Id, X, Y, Yaw, Sc) in enumerate((("wine_barrel_01", 2400.0, -820.0, 0.0, 1.0), ("wine_barrel_01", 2470.0, -870.0, 0.0, 1.0),
												  ("wooden_crate_02", 3330.0, -560.0, 20.0, 1.0), ("wooden_crate_01", 3390.0, -620.0, 60.0, 0.9),
												  ("treasure_chest", 3050.0, -590.0, -20.0, 1.3), ("wooden_barrels_01", 2300.0, -700.0, 30.0, 0.7))):
		C.PH_(f"Pirate_{Id}_{Index}", Id, X, Y, Yaw, Sc, Sink=2.0, Collide=True)
	C.PH_("Pirate_Cannon", "cannon_01", 3320.0, 260.0, 80.0, 0.9, Collide=True)
	C.SpriteProp("Pirate_LensCrate", "LensCrate", (2790.0, -610.0, H(2790.0, -610.0)), Billboard=2)
	# 보물상자 바위 틈 (절벽 길 뒤)
	for Index, (DX, DY, Sc) in enumerate(((-170.0, -60.0, 1.0), (170.0, -40.0, 0.9), (0.0, -170.0, 1.2))):
		CX, CY = Layout.CHESTS[1][0] + DX, Layout.CHESTS[1][1] + DY
		C.PH_(f"ChestNook_Rock{Index}", "boulder_01", CX, CY, Rng.uniform(0, 360), Sc, Sink=10.0, Collide=True, Shrink=0.6)

	# ---- 동쪽 후미 (보스): 모래 해변 + 뒤 벼랑 + 해적 굴 입구 + 끌어올린 해적 보트 + 깃발 + 앞바다 해적선
	C.PH_("Cove_CliffBack0", "coastal_cliff_02", 4300.0, -900.0, 92.0, 0.42, Z=-60.0)
	C.PH_("Cove_CliffBack1", "coastal_cliff_02", 5300.0, -760.0, 70.0, 0.4, Z=-60.0)
	C.PH_("Cove_CliffEast", "coastal_cliff_02", 5700.0, -250.0, 8.0, 0.36, Z=SEA - 40.0)
	C.PH_("Cove_Wreck", "ship_pinnace", 5150.0, 760.0, 128.0, 0.2, Z=SEA - 70.0, Roll=16.0, Pitch=-6.0)
	C.Point("Cove_StashLight", (4150.0, -400.0, H(4150.0, -400.0) + 200.0), (1.0, 0.55, 0.25), 5.0, 800.0, Flicker={"Style": "Fire", "Seed": 77, "Amount": 0.2})
	C.Kit("Cove_Boat", "HarborRowboatRed", 5000.0, 470.0, 200.0, 1.0, Z=H(5000.0, 470.0) - 8.0, Collide=True)
	C.Kit("Cove_FlagPole", "HarborFlagPole", 5150.0, -420.0, 0.0, 1.0, Collide=True)
	C.SpriteProp("Cove_Flag", "FlagPirate0", (5156.0, -420.0, H(5150.0, -420.0) + 330.0), Billboard=0, Flipbook="Sprites/HD2D/HarborFlag_Pirate.eflipbook")
	for Index, (Id, X, Y, Yaw, Sc) in enumerate((("wooden_crate_02", 4080.0, -480.0, 10.0, 1.0), ("wine_barrel_01", 4170.0, -520.0, 0.0, 1.0),
												  ("wooden_barrels_01", 5180.0, -100.0, 60.0, 0.6), ("wooden_crate_01", 4000.0, -420.0, 40.0, 0.9))):
		C.PH_(f"Cove_{Id}_{Index}", Id, X, Y, Yaw, Sc, Sink=4.0, Collide=True)
	for Index, (Id, X, Y, Sc) in enumerate((("coast_rocks_05", 5250.0, 900.0, 0.9), ("coast_land_rocks_03", 3950.0, 980.0, 0.5), ("sand_rocks_small_01", 5250.0, 250.0, 0.8))):
		C.PH_(f"Cove_Rock{Index}", Id, X, Y, Rng.uniform(0, 360), Sc, Z=SEA - 40.0 if Index < 2 else None, Sink=10.0)
	C.Particles("Cove_Spray", Fx("HD2DHarborSpray"), (5250.0, 860.0, SEA))
	C.PH_("Pirate_Ship", "dutch_ship_medium", 4500.0, 1850.0, 72.0, 0.32, Z=SEA + 4.0)
	# 해적 은신처 (보스 모래톱 뒤 가장자리 — 싸움터 반지름 밖): 천막 + 모닥불 자국 + 유목·바위
	E.Tent("Cove_Tent", 3980.0, -560.0, 24.0, 300.0, 250.0, 190.0, "Harbor/SailCloth")
	C.PH_("Cove_FirePit", "stone_fire_pit", 4330.0, -640.0, 40.0, 0.9, Sink=10.0)
	C.Particles("Cove_FireEmbers", Fx("HD2DHarborCampfire"), (4330.0, -640.0, H(4330.0, -640.0) + 10.0), 0.6)
	C.BoxCollider("Cove_FirePit_Collision", (4330.0, -640.0, H(4330.0, -640.0) + 20.0), (60.0, 60.0, 40.0))
	for Index, (Id, X, Y, Yaw, Sc) in enumerate((("dead_tree_trunk_02", 3930.0, 520.0, 30.0, 0.8), ("dead_tree_trunk", 5230.0, 120.0, 110.0, 0.7),
												  ("boulder_01", 3880.0, 300.0, 0.0, 1.1), ("rock_moss_set_02", 5250.0, -420.0, 0.0, 1.0),
												  ("boulder_01", 5150.0, -560.0, 40.0, 1.3))):
		C.PH_(f"Cove_Edge{Index}", Id, X, Y, Yaw, Sc, Sink=6.0, Collide=True, Shrink=0.6)
	for Index in range(8):
		X, Y = Rng.uniform(3950.0, 5150.0), Rng.uniform(250.0, 600.0)
		if H(X, Y) > SEA + 8.0 and C.Free(X, Y, 60.0):
			C.SpriteProp(f"Cove_Shell{Index}", Rng.choice(["Shell0", "Shell1", "Starfish"]), (X, Y, H(X, Y) + 1.5), Flat=True, Yaw=Rng.uniform(0, 360))


VIEW_STARTS = {"Road": (-3450.0, -1250.0), "Market": (-2000.0, 50.0), "Pier": (-1700.0, 850.0), "Cargo": (200.0, -100.0), "Jetty": (930.0, 820.0),
			   "Beach": (-3500.0, 150.0), "Gate": (1250.0, -50.0), "Cliff": (2300.0, -150.0), "Camp": (2950.0, -250.0), "Cove": (4350.0, 80.0)}
OVERVIEW_VIEWS = {"West": (-2100.0, 3900.0, 3300.0, -38.0, -90.0, 46.0), "Mid": (300.0, 3900.0, 3300.0, -38.0, -90.0, 46.0),
				  "East": (3200.0, 3900.0, 3300.0, -38.0, -90.0, 46.0)}
AUTO_SCENES = {
	"AutoPlay":    (Layout.PLAYER_START, "Harbor"),        # 도착 → 퀘스트·적·보스·상점·낮밤·여관 → 메인 맵 이동
	"Shot_Day":    ((-1900.0, 0.0), "HarborDay"),          # 어시장 한낮
	"Shot_Night":  ((-1900.0, 0.0), "HarborNight"),        # 어시장 밤 (등불·창 불빛)
	"Shot_Lighthouse": ((900.0, 700.0), "HarborLighthouse"),  # 방파제 등대 (밤 — 불 켜짐)
	"Shot_Combat": ((2500.0, -150.0), "HarborCombat"),     # 절벽 길 전투 (성능 측정)
	"Shot_Boss":   ((4100.0, 100.0), "HarborBoss"),        # 해적 선장
}


# 지도 화면·미니맵 (HD2DMapArt): 놀이 영역 + 지명 (이름, X, Y, Place|Exit)
MINIMAP = HD2DMapArt.Info("Harbor", (Layout.PLAY_MIN[0] - 100.0, Layout.PLAY_MIN[1] - 100.0, Layout.PLAY_MAX[0] + 100.0, Layout.PLAY_MAX[1] + 100.0),
						  "갈매기 항구와 해안 절벽 길", [
	("갈매기 여관", -2050.0, -1300.0, "Place"), ("어시장", -2000.0, -60.0, "Place"), ("항만 사무소", -820.0, -1300.0, "Place"),
	("등대", Layout.LIGHTHOUSE[0], Layout.LIGHTHOUSE[1], "Place"), ("해적 야영지", 2850.0, -700.0, "Place"), ("해적 후미", 4600.0, 200.0, "Place"),
	("하르트 마을로", Layout.TRAVEL_HOME[0][0], Layout.TRAVEL_HOME[0][1] + 150.0, "Exit")])


def WriteMinimap(Sampler, Scene, Foliage):
	# 지도 그림: 지형 레이어(풀/모래/자갈/바위) + 바다(수면 아래) + 나무 + 건물(콜라이더) — 잔교·방파제는 자갈색으로 덧칠
	Cell = Sampler.Cell

	def Classify(X, Y):
		IX = np.clip(np.round((X - TERRAIN_CENTER[0] + TERRAIN_SIZE * 0.5) / Cell).astype(int), 0, TERRAIN_RES - 1)
		IY = np.clip(np.round((Y - TERRAIN_CENTER[1] + TERRAIN_SIZE * 0.5) / Cell).astype(int), 0, TERRAIN_RES - 1)
		H, W = Sampler.H[IY, IX], Sampler.Stack[IY, IX]
		Kind = np.choose(np.argmax(W, axis=-1), [HD2DMapArt.K_GRASS, HD2DMapArt.K_SAND, HD2DMapArt.K_PLAZA, HD2DMapArt.K_ROCK])
		Kind = np.where(H < SEA + 2.0, HD2DMapArt.K_WATER, Kind)
		PX, PEnd, PW = Layout.PIER
		JX, _, JW = Layout.JETTY
		LX, LY = Layout.LIGHTHOUSE
		Deck = ((np.abs(X - PX) < PW * 0.5) & (Y > Layout.QUAY_Y) & (Y < PEnd)) | ((np.abs(X - JX) < JW * 0.5) & (Y > Layout.QUAY_Y) & (Y < LY))
		Deck |= np.hypot(X - LX, Y - LY) < 280.0
		return np.where(Deck, HD2DMapArt.K_PATH, Kind)

	Trees = [(T[0], T[1], 95.0 * T[4]) for Type, Items in Foliage if Type in (Env.TREE_TYPE, Env.PINE_TYPE) for T in Items]
	HD2DMapArt.WriteMinimap(CONTENT, MINIMAP, Classify, Scene, Sampler, Trees)


def Main_():
	bGame = "--layout-only" not in sys.argv
	X, Y, H, D = BuildHeights()
	Weights, Stack = BuildWeights(X, Y, H, D)
	WriteTerrain(os.path.join(CONTENT, "Terrain", "Demo", "HD2DHarbor.eterrain"), H, Weights)
	Sampler = FHarborHeight(H, Stack)
	WriteMaterials()
	WriteParticles()
	WriteMeshes()
	# 게임 공용 (데이터 표·UI·프리팹·4차 도트 아트 — BuildHD2D.py와 같은 결과)
	HD2DGameplay.WriteAll(CONTENT, Main.CAMERA_DISTANCE, Main.PLAY_MIN, Main.PLAY_MAX)
	Scene, Foliage = BuildScene(Sampler, bGame=bGame)
	WriteFoliage(os.path.join(CONTENT, "Foliage", "Demo", "HD2DHarbor.efoliage"), Foliage)
	WriteMinimap(Sampler, Scene, Foliage)
	Scene.Save(os.path.join(CONTENT, *SCENE.split("/")))
	print(f"HD2D 항구 생성: 엔티티 {len(Scene.Entities)}개, 풀 {sum(len(I) for _, I in Foliage)}개, 높이 {H.min():.0f}~{H.max():.0f}cm")
	if "--views" in sys.argv:
		for Name, Start in VIEW_STARTS.items():
			Variant, _ = BuildScene(Sampler, Start, bGame=bGame)
			Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_HD2DHarbor_{Name}.escene"))
		for Name, View in OVERVIEW_VIEWS.items():
			Variant, _ = BuildScene(Sampler, Layout.PLAYER_START, View, bGame=bGame)
			Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_HD2DHarbor_Over{Name}.escene"))
		if bGame:
			for Name, (Start, Scenario) in AUTO_SCENES.items():
				Variant, _ = BuildScene(Sampler, Start, AutoPlay=Scenario)
				Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_HD2DHarbor{Name}.escene"))
		print("확인용 변형: Scenes/Demo/_HD2DHarbor_*.escene, _HD2DHarborAutoPlay.escene, _HD2DHarborShot_*.escene (커밋하지 않음)")


if __name__ == "__main__":
	Main_()
