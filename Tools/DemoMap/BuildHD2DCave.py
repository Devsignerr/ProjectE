# 데모 서브맵 "HD2D 동굴"(고대 유적 던전 — HD2D 메인 맵 폭포 옆 벼랑 입구에서 들어온다) 생성:
#   지형(.eterrain: 동굴 바닥·바위 벽·유적 바닥돌·이끼) + KayKit 던전 키트(벽·기둥·아치·횃불·통·상자·깃발) + 바위·종유석 + 푸른 수정
#   + 물(지하 호수·구덩이) + 횃불 점광원(깜빡임)·수정 발광·볼류메트릭 안개·물방울·먼지 파티클 + 플레이어/게임 관리자(HD2DGameplay)
#   실행: python Tools/DemoMap/BuildHD2DCave.py [--views]   (머티리얼·파티클 일부는 BuildHD2D.py가 먼저 써 둔 HD2D 공용 것을 쓴다)
#   --views: 확인용 변형(커밋하지 않음) — Scenes/Demo/_HD2DCave_<방>.escene(시작 자리만 다름), _HD2DCave_Over<이름>.escene(흐림 끈 자유 시점),
#            _HD2DCaveAutoPlay.escene(자동 검증 — 동굴 전 구간 + 메인 맵 귀환·엔딩), _HD2DCaveShot_<이름>.escene(스크린샷·측정), _HD2DCaveNavBake.escene(내비메시 굽기)
#   --install-nav: 구운 _HD2DCaveNavBake.enav → Scenes/Demo/HD2DCave.enav (순서는 Docs/Rules/DataAndDemos.md HD2D 동굴 항목)
#   게임: 적·보물·보스·문·함정 자리는 HD2DCaveLayout.py → HD2DGameplay.FMapLayout("Cave") → AddGame (타이틀 없음 — 바로 플레이),
#         던전 장치는 Scripts/Demo/HD2D/HD2DDungeon.lua, 보스는 HD2DSpiderQueen.lua. 데이터 표·UI·프리팹은 BuildHD2D.py와 같은 HD2DGameplay.WriteAll
#   규약: 메인 맵과 같은 고정 원근 디오라마 카메라(+Y 위에서 -Y를 봄, 피치 -28·시야각 24). 동굴 벽은 안쪽(-Y)으로 높이 솟고
#         카메라 쪽(+Y)은 낮은 바위 턱만 둔다(캐릭터를 가리지 않게). 자리 계약(적·보물·보스·도착 자리)은 HD2DCaveLayout.py
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
import BuildHD2D as Main  # noqa: E402 — 카메라 규약·공용 함수 (임포트만 — Main()은 돌지 않는다)
import HD2DEnvironment as Env  # noqa: E402
import HD2DGameplay  # noqa: E402
import HD2DArt  # noqa: E402
import HD2DCaveLayout as Layout  # noqa: E402
import HD2DMapArt  # noqa: E402

CONTENT  = Main.CONTENT
PH       = "Asset/PolyHaven"
KIT      = "Asset/KayKit/Dungeon"
CAVE_MAT = "Materials/Demo/HD2D/Cave"
CAVE_FX  = "Particles/Demo/HD2D/Cave"
SCENE    = "Scenes/Demo/HD2DCave.escene"

# ---- 지형 ----------------------------------------------------------------------------------------------------------
TERRAIN_SIZE   = 12000.0
TERRAIN_RES    = 241       # 칸 50cm
TERRAIN_CENTER = (500.0, -300.0)
HEIGHT_RANGE   = 6000.0
WALL_HEIGHT    = 760.0     # 안쪽 바위 벽
RIM_HEIGHT     = 70.0      # 카메라 쪽 바위 턱 (그 앞은 어둠으로 꺼지는 낭떠러지)
LEDGE_H        = 160.0     # 갈림길 뒤 보물 단
LAKE           = (2650.0, -700.0, 520.0, 300.0)   # 지하 호수 (가운데 X, Y, 반지름 X, Y)
LAKE_LEVEL     = -60.0
PIT            = (650.0, 1150.0)                  # 함정 복도 구덩이 X 범위 (복도 폭 전체)
BRIDGE_HALF_W  = 115.0
CORRIDOR_HALF  = 330.0
# ---- 게임 ------------------------------------------------------------------------------------------------------------
CAVE_PATH = [(-3150.0, -100.0), (-900.0, -100.0), (900.0, 0.0), (2600.0, 150.0), (3880.0, 0.0)]  # 내비메시가 없을 때 자동 조종 길
NAV_ASSET = "Scenes/Demo/HD2DCave.enav"
GATE_LIFT = 200.0          # 보스 방 문 프리팹 루트(콜라이더 가운데) = 지면 + 이만큼
GATE_BAR_H = 270.0         # 창살 높이
GATE_DOWN = GATE_BAR_H + 40.0  # 열린 문: 창살 묶음을 이만큼 땅속으로 (HD2DDungeon.lua와 같은 값)

# 걷는 방 모양: (이름, 종류, 값) — 종류 "E" 타원 (X, Y, RX, RY), "R" 사각 (X0, Y0, X1, Y1), "C" 캡슐 (AX, AY, BX, BY, R)
ROOMS = [
	("Hall", "E", (-2700.0, -150.0, 980.0, 680.0)),
	("HallMouth", "C", (-3800.0, -120.0, -3300.0, -120.0, 330.0)),
	("Pass", "C", (-1900.0, -150.0, -1300.0, -120.0, 340.0)),
	("Fork", "E", (-850.0, -150.0, 620.0, 560.0)),
	("Ledge", "R", (-1500.0, -1480.0, -350.0, -980.0)),
	("LedgeStairs", "R", (-1010.0, -1000.0, -790.0, -640.0)),  # 갈림길 → 보물 단 돌계단 자리 (바위 벽이 계단을 덮지 않게 판다)
	("Corridor", "R", (-350.0, -CORRIDOR_HALF, 2000.0, CORRIDOR_HALF)),
	("Lake", "E", (2600.0, -250.0, 880.0, 680.0)),
	("Boss", "E", (3880.0, -350.0, 820.0, 720.0)),
]


def RoomDistance(X, Y, Kind, V):
	# 바깥쪽 양수 거리(cm, 근사) — 안쪽은 음수
	if Kind == "E":
		CX, CY, RX, RY = V
		return (np.sqrt(((X - CX) / RX) ** 2 + ((Y - CY) / RY) ** 2) - 1.0) * min(RX, RY)
	if Kind == "R":
		X0, Y0, X1, Y1 = V
		DX = np.maximum(X0 - X, X - X1)
		DY = np.maximum(Y0 - Y, Y - Y1)
		Outside = np.hypot(np.maximum(DX, 0.0), np.maximum(DY, 0.0))
		return np.where((DX <= 0) & (DY <= 0), np.maximum(DX, DY), Outside)
	AX, AY, BX, BY, R = V
	return Main.SegmentDistance(X, Y, [(AX, AY), (BX, BY)]) - R


def WalkDistance(X, Y):
	D = np.full(np.shape(X), 1.0e9)
	for _, Kind, V in ROOMS:
		D = np.minimum(D, RoomDistance(X, Y, Kind, V))
	return D


def LedgeMask(X, Y):
	return Smoothstep(25.0, -25.0, RoomDistance(X, Y, "R", (-1500.0, -1480.0, -350.0, -1000.0)))


def InPit(X, Y):
	return (X > PIT[0]) & (X < PIT[1]) & (np.abs(Y) < CORRIDOR_HALF + 400.0)


def BuildHeights():
	Coords = (np.arange(TERRAIN_RES) / (TERRAIN_RES - 1) - 0.5) * TERRAIN_SIZE
	X, Y = np.meshgrid(Coords + TERRAIN_CENTER[0], Coords + TERRAIN_CENTER[1])
	D = WalkDistance(X, Y)
	# 카메라 쪽 판정: 열(X)마다 걷는 영역의 가장 앞(+Y) 끝 — 그보다 앞이면 낮은 턱, 뒤(나머지)는 높은 벽
	Walk = D < 0.0
	FrontY = np.where(Walk.any(axis=0), np.where(Walk, Y, -1.0e9).max(axis=0), -1.0e9)[None, :]
	Front = Y > FrontY - 1.0
	Noise = Fbm(X, Y, 420.0, 301, 3)
	Rough = Fbm(X, Y, 160.0, 302, 2)
	Floor = Fbm(X, Y, 900.0, 303, 2) * 12.0 + LedgeMask(X, Y) * LEDGE_H
	Back = Smoothstep(10.0, 170.0, D + Noise * 50.0) * (WALL_HEIGHT + Noise * 260.0 + Rough * 60.0)
	Back += Smoothstep(600.0, 2200.0, D) * 500.0
	Rim = Smoothstep(10.0, 90.0, D + Noise * 30.0) * (RIM_HEIGHT + Rough * 40.0) * Smoothstep(300.0, 160.0, D + Noise * 60.0) - Smoothstep(180.0, 420.0, D + Noise * 60.0) * 900.0
	H = Floor + np.where(Front, Rim, Back)
	# 구덩이 (복도 가운데 — 다리로 건넌다) / 지하 호수
	H = np.where(InPit(X, Y), np.minimum(H, -650.0 + Rough * 40.0), H)
	Lake = Smoothstep(1.1, 0.75, Main.EllipseValue(X, Y, LAKE))
	H = H * (1.0 - Lake) + (-170.0 + Rough * 15.0) * Lake
	return X, Y, H, D


def BuildWeights(X, Y, H, D):
	# 레이어: 0 동굴 흙, 1 바위 벽, 2 유적 바닥돌(복도·보스 방·단), 3 이끼(호수가)
	Noise = Fbm(X, Y, 300.0, 311, 3)
	Rock = Smoothstep(30.0, 140.0, D + Noise * 60.0)
	Ruin = np.zeros_like(X)
	for Name, Kind, V in ROOMS:
		if Name in ("Corridor", "Boss", "Ledge"):
			Ruin = np.maximum(Ruin, Smoothstep(10.0 + Noise * 50.0, -60.0 + Noise * 50.0, RoomDistance(X, Y, Kind, V)))
	Ruin *= (1.0 - Rock)
	Moss = Smoothstep(1.7, 1.1, Main.EllipseValue(X, Y, LAKE)) * (1.0 - Rock) * (1.0 - Ruin) * 0.9
	W0 = np.clip(1.0 - Rock - Ruin - Moss, 0.0, 1.0)
	Stack = np.stack([W0, Rock, Ruin, Moss], axis=-1)
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


class FCaveHeight:
	# 지형 높이(이중 선형) — 단(Ledge)도 지형에 들어 있다
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

	def Floor(self, PX, PY):
		# 바닥 높이 (단 위면 LEDGE_H) — 경계의 지형 경사와 무관하게 소품을 놓을 때
		return LEDGE_H if float(LedgeMask(np.array(PX), np.array(PY))) > 0.5 else 0.0

	def Normal(self, PX, PY):
		E = self.Cell
		DX = (self(PX + E, PY) - self(PX - E, PY)) / (2 * E)
		DY = (self(PX, PY + E) - self(PX, PY - E)) / (2 * E)
		N = np.array([-DX, -DY, 1.0])
		return N / np.linalg.norm(N)


# ---- 머티리얼 / 파티클 -----------------------------------------------------------------------------------------------
def WriteMaterials():
	Folder = os.path.join(CONTENT, *CAVE_MAT.split("/"))
	Terrain = {
		"TerrainDirt":  ("forest_ground_04", [0.42, 0.42, 0.46]),
		"TerrainRock":  ("cliff_side", [0.5, 0.52, 0.58]),
		"TerrainTiles": ("cobblestone_floor_04", [0.62, 0.64, 0.7]),
		"TerrainMoss":  ("mossy_rock", [0.55, 0.65, 0.6]),
	}
	for Name, (Id, Tint) in Terrain.items():
		Rel = f"../../../../{PH}/{Id}/{Id}"
		WriteJson(os.path.join(Folder, f"{Name}.emat"), {
			"Name": f"HD2DCave{Name}", "BaseColorFactor": Tint + [1.0], "EmissiveFactor": [0.0, 0.0, 0.0],
			"Metallic": 1.0, "Roughness": 1.0, "NormalScale": 1.0, "OcclusionStrength": 1.0,
			"BaseColorTexture": f"{Rel}_diff_2k.jpg", "MetallicRoughnessTexture": f"{Rel}_arm_2k.jpg",
			"NormalTexture": f"{Rel}_nor_gl_2k.jpg", "OcclusionTexture": f"{Rel}_arm_2k.jpg", "EmissiveTexture": "",
		})
	Plains = {
		"CrystalBlue":   Env.Plain("CaveCrystalBlue", (0.1, 0.35, 0.6, 1.0), 0.15, (0.22, 1.0, 2.2)),
		"CrystalViolet": Env.Plain("CaveCrystalViolet", (0.3, 0.15, 0.6, 1.0), 0.15, (0.9, 0.35, 2.0)),
		"Bone":          Env.Plain("CaveBone", (0.62, 0.58, 0.5, 1.0), 0.7),
		"Ember":         Env.Plain("CaveEmber", (0.05, 0.02, 0.01, 1.0), 0.9, (6.0, 1.8, 0.4)),
		"Rune":          Env.Plain("CaveRune", (0.02, 0.05, 0.08, 1.0), 0.6, (0.3, 1.6, 3.0)),
		"Spike":         Env.Plain("CaveSpike", (0.12, 0.12, 0.13, 1.0), 0.35, Metallic=1.0),
		"Grass":         Env.Plain("CaveGrass", (0.32, 0.5, 0.42, 1.0), 0.9),
	}
	for Name, Doc in Plains.items():
		WriteJson(os.path.join(Folder, f"{Name}.emat"), Doc)


def CMat(Name):
	return f"{CAVE_MAT}/{Name}.emat"


def DripSystem():
	# 천장에서 떨어지는 물방울: 높은 곳에서 생겨 곧장 떨어지다 바닥(월드 Z 0 근처)에 닿으면 사라진다 + 작은 물보라
	Drops = Emitter("Drops", 901, 300,
		[Mod("SpawnRate", SpawnRate=Const(14.0))],
		[Init(Rand(1.2, 1.6), Rand((0.55, 0.75, 0.95, 0.7), (0.7, 0.9, 1.0, 0.9)), Rand((1.2, 4.0), (1.6, 6.0))),
		 Shape(SHAPE_BOX, Box=(7600.0, 1500.0, 40.0), Offset=(0.0, 0.0, 650.0)),
		 Mod("AddVelocity", Velocity=Const(0.0, 0.0, -150.0))],
		[Mod("GravityForce", Gravity=Const(0.0, 0.0, -900.0)),
		 Mod("Collision", PlaneHeight=Const(0.0), Restitution=Const(0.0), Friction=Const(0.0), KillOnCollide=Const(1.0))],
		PSprite(1, Velocity=True, Stretch=0.02), Sim="GPU", Duration=10.0, Bounds=((-4500.0, -1500.0, -200.0), (4500.0, 1500.0, 900.0)))
	return {"Name": "HD2DCaveDrips", "Version": 2, "Emitters": [Drops]}


def DustSystem():
	# 푸른 빛 먼지 (수정빛을 받은 먼지가 천천히 떠다니며 반짝임)
	Twinkle = Curve((0.0, (1, 1, 1, 0)), (0.25, (1, 1, 1, 0.9)), (0.45, (1, 1, 1, 0.3)), (0.65, (1, 1, 1, 1.0)), (1.0, (1, 1, 1, 0)))
	Dust = Emitter("Dust", 902, 1200,
		[Mod("SpawnRate", SpawnRate=Const(90.0))],
		[Init(Rand(7.0, 12.0), Rand((0.4, 1.2, 2.2, 0.6), (0.8, 1.8, 3.0, 0.9)), Rand((4.0, 4.0), (7.0, 7.0))),
		 Shape(SHAPE_BOX, Box=(8400.0, 2200.0, 400.0), Offset=(0.0, 0.0, 220.0)),
		 Mod("AddVelocity", Velocity=Rand((-6.0, -6.0, -3.0), (6.0, 6.0, 6.0)))],
		[Mod("CurlNoiseForce", Strength=Const(14.0), Frequency=Const(0.006), PanSpeed=Const(3.0, 0.0, 2.0)),
		 Mod("Drag", Drag=Const(0.5)),
		 Mod("ScaleColor", Scale=Twinkle)],
		PSprite(1), Sim="GPU", Duration=10.0, Bounds=((-4800.0, -1700.0, -200.0), (4800.0, 1700.0, 900.0)))
	return {"Name": "HD2DCaveDust", "Version": 2, "Emitters": [Dust]}


def BrazierSystem():
	# 화로 불꽃 (모닥불 불꽃 텍스처, 작게)
	Flames = Emitter("Flames", 911, 60,
		[Mod("SpawnRate", SpawnRate=Const(30.0))],
		[Init(Rand(0.35, 0.6), Rand((0.8, 0.3, 0.07, 0.9), (1.1, 0.45, 0.1, 1.0)), Rand((14.0, 24.0), (20.0, 36.0)), Rand(-12.0, 12.0)),
		 Shape(SHAPE_SPHERE, Radius=10.0),
		 Mod("AddVelocity", Velocity=Rand((-6.0, -6.0, 40.0), (6.0, 6.0, 80.0)))],
		[Mod("ScaleColor", Scale=Curve((0.0, (0.5, 0.5, 0.5, 0.0)), (0.12, (1, 1, 1, 1)), (1.0, (0.4, 0.12, 0.04, 0.0)))),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0))],
		PSprite(1, "../../Textures/CampfireFlame.png", 4, 4))
	Embers = Emitter("Embers", 912, 80,
		[Mod("SpawnRate", SpawnRate=Const(8.0))],
		[Init(Rand(1.0, 2.2), Rand((6.0, 2.0, 0.4, 1.0), (9.0, 3.0, 0.8, 1.0)), Rand((1.0, 1.0), (1.8, 1.8))),
		 Shape(SHAPE_SPHERE, Radius=12.0),
		 Mod("AddVelocity", Velocity=Rand((-15.0, -15.0, 60.0), (15.0, 15.0, 140.0)))],
		[Mod("CurlNoiseForce", Strength=Const(260.0), Frequency=Const(0.02), PanSpeed=Const(0.0, 0.0, 40.0)),
		 Mod("Drag", Drag=Const(0.8)),
		 Mod("ScaleColor", Scale=Curve((0.0, (1, 1, 1, 1)), (1.0, (0.2, 0.05, 0.02, 0.0))))],
		PSprite(1, Velocity=True, Stretch=0.015))
	return {"Name": "HD2DCaveBrazier", "Version": 2, "Emitters": [Flames, Embers]}


def WriteParticles():
	for Name, Doc in {"HD2DCaveDrips": DripSystem(), "HD2DCaveDust": DustSystem(), "HD2DCaveBrazier": BrazierSystem()}.items():
		WriteJson(os.path.join(CONTENT, *CAVE_FX.split("/"), f"{Name}.eparticle"), Doc)


def Fx(Name):
	return f"{CAVE_FX}/{Name}.eparticle"


def WriteGatePrefab():
	# 보스 방 문 (Prefabs/Demo/HD2D/CaveGate.eprefab — 관리자가 보스 방에 들어서면 만든다): 루트 = 막는 상자 콜라이더(길목 전체),
	#   "Bars" = 돌 창살 + 수정 끝 + 가로대 묶음. 프리팹에는 땅속(열린 자리)으로 두고 관리자가 GATE_DOWN만큼 올려 닫는다
	_, _, Half = Layout.BOSS_GATE
	Link = HD2DGameplay.Link
	Tf = HD2DGameplay.Transform
	Entities = [
		{"Name": "CaveGate", "Parent": -1, "Components": {"BoxColliderComponent": {"HalfExtents": [40.0, Half, 230.0]},
														   "PrefabLinkComponent": Link(1), "TransformComponent": Tf()}},
		{"Name": "Bars", "Parent": 0, "Components": {"PrefabLinkComponent": Link(2), "TransformComponent": Tf((0.0, 0.0, -GATE_LIFT - GATE_DOWN))}},
	]

	def Mesh(Name, Pos, Scale, Material, Yaw=0.0):
		Entities.append({"Name": Name, "Parent": 1, "Components": {
			"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": Material}, "PrefabLinkComponent": Link(len(Entities) + 1),
			"TransformComponent": Tf(Pos, QuatFromEuler(Yaw=Yaw), Scale)}})

	Count = int((Half * 2.0 - 60.0) // 80.0) + 1
	for K in range(Count):
		Y = -Half + 30.0 + K * 80.0
		Mesh(f"Bar{K}", (0.0, Y, GATE_BAR_H * 0.5), (0.4, 0.4, GATE_BAR_H / 100.0), Env.Mat("EnvStoneDark"))
		Mesh(f"Tip{K}", (0.0, Y, GATE_BAR_H + 18.0), (0.26, 0.26, 0.5), CMat("CrystalViolet" if K % 2 else "CrystalBlue"), 45.0)
	for Index, Z in enumerate((70.0, 200.0)):
		Mesh(f"Cross{Index}", (0.0, 0.0, Z), (0.3, Half * 2.0 / 100.0, 0.22), Env.Mat("EnvStone"))
	HD2DGameplay.WritePrefab(CONTENT, "CaveGate", Entities)


CAVE_GRASS_TYPE = Env.FoliageType("CaveGrass", "foliage:grass", CMat("Grass"), 0.4, 0.7, ZOffset=-2.0, Cull=5000.0)


# ---- 씬 ---------------------------------------------------------------------------------------------------------------
def BuildScene(Height, Start=Layout.PLAYER_START, Overview=None, AutoPlay=""):
	Rng = random.Random(77)
	S = FScene()
	Occupied = []
	TrapSpots = []

	def Reserve(X, Y, Radius):
		Occupied.append((X, Y, Radius))

	def Free(X, Y, Radius):
		return all((X - OX) ** 2 + (Y - OY) ** 2 >= (Radius + OR) ** 2 for OX, OY, OR in Occupied)

	def BoxCollider(Name, Center, Half, Yaw=0.0):
		return S.Add(Name, {"BoxColliderComponent": {"HalfExtents": [float(V) for V in Half]}}, Center, QuatFromEuler(Yaw=Yaw))

	def Point(Name, Pos, Color, Intensity, Radius, Shadows=False, Flicker=None):
		Comps = {"PointLightComponent": {"Color": list(Color), "Intensity": Intensity, "Radius": Radius, "CastShadows": Shadows}}
		if Flicker:
			Comps["ScriptComponent"] = FlickerScript(Intensity, **Flicker)
		return S.Add(Name, Comps, Pos)

	def Particles(Name, Asset, Pos, Speed=1.0):
		return S.Add(Name, {"ParticleSystemComponent": {"Asset": Asset, "Playing": True, "Speed": Speed}}, Pos)

	def Kit(Name, Id, X, Y, Yaw=0.0, Scale=1.0, Z=None, Pitch=0.0, Roll=0.0):
		Base = Height.Floor(X, Y) if Z is None else Z
		Sc = Scale if isinstance(Scale, (list, tuple)) else (Scale, Scale, Scale)
		return S.Add(Name, {"ModelComponent": {"AssetPath": f"{KIT}/{Id}.glb"}}, (X, Y, Base), QuatFromEuler(Pitch, Yaw, Roll), Sc)

	def PH_(Name, Id, X, Y, Yaw=0.0, Scale=1.0, Z=None, Sink=0.0):
		Base = Height(X, Y) if Z is None else Z
		Sc = Scale if isinstance(Scale, (list, tuple)) else (Scale, Scale, Scale)
		return S.Add(Name, {"ModelComponent": {"AssetPath": f"{PH}/{Id}/{Id}.gltf"}}, (X, Y, Base - Sink), QuatFromEuler(Yaw=Yaw), Sc)

	E = Env.FDressing(S, Height, Rng, Reserve, BoxCollider, Point)
	TorchIndex = [0]

	def WallTorch(X, Y, Z, Facing=-90.0, Light=True):
		# KayKit 벽 횃불 (로컬 -X로 벽에 붙음 — Yaw -90이면 +Y(카메라 쪽)로 내민다) + 불꽃 + 깜빡이는 점광원
		I = TorchIndex[0]
		TorchIndex[0] += 1
		S.Add(f"Torch_{I}", {"ModelComponent": {"AssetPath": f"{KIT}/torch_mounted.glb"}}, (X, Y, Z), QuatFromEuler(Yaw=Facing), (1.0, 1.0, 1.0))
		R = math.radians(Facing)
		FX, FY = X - math.cos(R) * 32.0, Y - math.sin(R) * 32.0
		Particles(f"Torch_{I}_Flame", "Particles/Demo/CampfireLanternFlame.eparticle", (FX, FY, Z + 62.0))
		if Light:
			Point(f"Torch_{I}_Light", (FX, FY + 30.0, Z + 80.0), (1.0, 0.55, 0.22), 9.0, 900.0, False, Flicker={"Style": "Fire", "Seed": 200 + I, "Amount": 0.22})

	def Crystal(Name, X, Y, Scale=1.0, Material="CrystalBlue", Light=True, Z=None):
		# 수정 무리: 길쭉한 발광 상자 5~7개를 바깥으로 기울여 꽂는다 + 푸른 점광원
		Base = Height(X, Y) if Z is None else Z
		Root = S.Add(Name, {}, (X, Y, Base - 10.0), QuatFromEuler(Yaw=Rng.uniform(0, 360)), (Scale, Scale, Scale))
		for K in range(Rng.randint(5, 7)):
			A = K * 1.1 + Rng.uniform(-0.3, 0.3)
			Tilt = Rng.uniform(8, 32)
			H = Rng.uniform(50.0, 150.0) * (1.4 if K == 0 else 1.0)
			W = H * Rng.uniform(0.18, 0.26)
			Lean = math.radians(Tilt)
			OX, OY = math.cos(A) * (10.0 + H * 0.5 * math.sin(Lean)), math.sin(A) * (10.0 + H * 0.5 * math.sin(Lean))
			S.Add(f"{Name}_Shard{K}", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": CMat(Material)}},
				  (OX, OY, H * 0.5 * math.cos(Lean)), QuatFromEuler(Pitch=Tilt * math.cos(A + 1.57), Yaw=math.degrees(A) + 45.0, Roll=Tilt * math.sin(A + 1.57)),
				  (W / 100.0, W / 100.0, H / 100.0), Root)
		if Light:
			Color = (0.35, 0.7, 1.0) if Material == "CrystalBlue" else (0.7, 0.4, 1.0)
			Point(f"{Name}_Light", (X, Y + 40.0, Base + 120.0 * Scale), Color, 6.0 * Scale, 700.0 * Scale, Flicker={"Style": "Pulse", "Seed": 300 + len(S.Entities), "Amount": 0.1, "Speed": 0.3})
		Reserve(X, Y, 90.0 * Scale)

	def Stalagmite(Name, X, Y, Height_=200.0):
		PH_(Name, "rock_07", X, Y, Rng.uniform(0, 360), (Height_ / 300.0, Height_ / 300.0, Height_ / 110.0), Sink=20.0)

	def Bones(Name, X, Y):
		Z = Height(X, Y)
		Root = S.Add(Name, {}, (X, Y, Z), QuatFromEuler(Yaw=Rng.uniform(0, 360)))
		S.Add(f"{Name}_Skull", {"StaticMeshComponent": {"MeshAsset": "primitive:sphere", "MaterialAsset": CMat("Bone")}}, (0.0, 0.0, 9.0), None, (0.2, 0.18, 0.17), Root)
		for K in range(4):
			S.Add(f"{Name}_Bone{K}", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": CMat("Bone")}},
				  (Rng.uniform(-40, 40), Rng.uniform(-30, 30), 3.0), QuatFromEuler(Yaw=Rng.uniform(0, 180)), (0.45, 0.05, 0.05), Root)

	# ---- 환경: 깊은 밤(달빛이 바위 틈으로 — 차가운 기본광) + 짙은 볼류메트릭 안개(횃불·수정빛 무리)
	# 동굴 위는 뚫린 하늘로 두되 카메라에는 보이지 않는다: 높은 해를 차갑고 아주 약하게(바위 틈으로 새는 빛) + 하늘빛을 약한 푸른 채움으로
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [0.55, 0.7, 1.0], "Intensity": 1.8}}, (0, 0, 3000), QuatFromEuler(Pitch=-70, Yaw=200.0))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {},
		"TimeOfDayComponent": {"TimeOfDay": 11.0, "DayLengthMinutes": 0.0, "MaxSunElevation": 72.0, "NorthAzimuth": 20.0, "AnimateInEditor": False},
		"SkyLightComponent": {"Intensity": 0.9},
		"HeightFogComponent": {
			"Color": [0.05, 0.08, 0.12], "Density": 0.0014, "HeightFalloff": 0.12, "StartDistance": 400.0, "MaxOpacity": 0.7,
			"DirectionalInscatteringColor": [0.06, 0.1, 0.16], "Volumetric": True, "VolumetricDistance": 6000.0,
			"VolumetricAlbedo": [0.8, 0.88, 1.0], "VolumetricExtinctionScale": 0.9, "VolumetricAnisotropy": 0.45,
			"VolumetricDirectionalScale": 0.15, "VolumetricLocalLightScale": 1.2},
	})
	S.Add("Terrain", {"TerrainComponent": {
		"Asset": "Terrain/Demo/HD2DCave.eterrain", "Size": [TERRAIN_SIZE, TERRAIN_SIZE], "HeightRange": HEIGHT_RANGE,
		"Layer0Material": CMat("TerrainDirt"), "Layer1Material": CMat("TerrainRock"),
		"Layer2Material": CMat("TerrainTiles"), "Layer3Material": CMat("TerrainMoss"),
		"Layer0Tiling": 380.0, "Layer1Tiling": 520.0, "Layer2Tiling": 280.0, "Layer3Tiling": 450.0,
		"CastShadows": True, "Collision": True}}, (TERRAIN_CENTER[0], TERRAIN_CENTER[1], 0.0))
	S.Add("Foliage", {"FoliageComponent": {"Asset": "Foliage/Demo/HD2DCave.efoliage", "Visible": True}})
	# 물: 지하 호수 + 구덩이 바닥의 고인 물 (한 상자)
	LX, LY, LRX, LRY = LAKE
	S.Add("Lake", {"WaterBodyComponent": {
		"Size": [LRX * 2.6, LRY * 2.8, 160.0], "ScatterColor": [0.0, 0.05, 0.09], "Absorption": [0.45, 0.15, 0.08],
		"NormalStrength": 0.25, "WaveScale": 140.0, "WaveSpeed": 3.0, "FlowDirection": 0.0, "FlowSpeed": 0.0,
		"FoamIntensity": 0.2, "FoamDistance": 10.0, "RefractionStrength": 0.03, "ReflectionIntensity": 1.0, "Roughness": 0.04}},
		(LX, LY, LAKE_LEVEL - 80.0))
	for Index, (FX, FY) in enumerate(((0.95, 0.6), (0.7, 0.92), (0.4, 1.05))):
		BoxCollider(f"Lake_Wall_{Index}", (LX, LY, LAKE_LEVEL + 60.0), (LRX * FX, LRY * FY, 120.0))
	Reserve(LX, LY, LRX + 80.0)
	# 보이지 않는 벽 (걷는 영역)
	(MinX, MinY), (MaxX, MaxY) = Layout.PLAY_MIN, Layout.PLAY_MAX
	MidX, MidY = (MinX + MaxX) * 0.5, (MinY + MaxY) * 0.5
	BoxCollider("Bound_Back", (MidX, MinY - 100.0, 300.0), ((MaxX - MinX) * 0.5 + 200.0, 100.0, 800.0))
	BoxCollider("Bound_Front", (MidX, MaxY + 100.0, 300.0), ((MaxX - MinX) * 0.5 + 200.0, 100.0, 800.0))
	BoxCollider("Bound_West", (MinX - 100.0, MidY, 300.0), (100.0, (MaxY - MinY) * 0.5 + 200.0, 800.0))
	BoxCollider("Bound_East", (MaxX + 100.0, MidY, 300.0), (100.0, (MaxY - MinY) * 0.5 + 200.0, 800.0))
	# 게임 자리 비움
	for Spots in Layout.ENEMY_SPOTS.values():
		for X, Y in Spots:
			Reserve(X, Y, 140.0)
	for X, Y in Layout.CHEST_SPOTS:
		Reserve(X, Y, 130.0)
	Reserve(*Layout.BOSS_SPOT, Layout.BOSS_ARENA_RADIUS)
	GateX, GateY, GateHalf = Layout.BOSS_GATE
	for K in range(int(GateHalf * 2.0 // 100.0) + 1):
		Reserve(GateX, GateY - GateHalf + K * 100.0, 90.0)  # 보스 방 문 길목 (문은 관리자가 프리팹으로 세운다)
	for Name, (X, Y, Yaw) in Layout.SPAWNS.items():
		S.Add(f"Spawn_{Name}", {}, (X, Y, Height.Floor(X, Y)), QuatFromEuler(Yaw=Yaw))
		Reserve(X, Y, 150.0)

	# ---- 1. 입구 홀: 서쪽 굴 입구(바깥 달빛 + 돌아가는 이동 트리거), 무너진 유적 벽, 탐험가 야영 흔적
	S.Add("CaveExit_Travel", {
		"BoxColliderComponent": {"HalfExtents": [80.0, 260.0, 140.0], "IsTrigger": True},
		"ScriptComponent": {"ScriptAsset": "Scripts/Demo/HD2D/HD2DTravel.lua", "ExecutionLocation": 0,
							"PropertyOverrides": json.dumps({"TargetScene": "Scenes/Demo/HD2D.escene", "SpawnName": "CaveExit"}, ensure_ascii=False)}},
		(-3560.0, -120.0, 130.0))
	S.Add("Entrance_Shaft", {"SpotLightComponent": {"Color": [0.6, 0.75, 1.0], "Intensity": 60.0, "Radius": 2600.0, "InnerConeAngle": 10.0,
													  "OuterConeAngle": 22.0, "CastShadows": True, "SpecularScale": 0.5}},
		  (-3900.0, -80.0, 1300.0), QuatFromEuler(Pitch=-62.0, Yaw=8.0))
	Point("Entrance_Glow", (-3700.0, -100.0, 160.0), (0.55, 0.7, 1.0), 6.0, 900.0)
	for Index, (Id, X, Y, Yaw) in enumerate((("wall", -3150.0, -830.0, 90.0), ("wall_broken", -2750.0, -840.0, 90.0), ("wall_window_open", -2350.0, -830.0, 90.0),
											  ("wall_half", -1950.0, -800.0, 90.0))):
		Kit(f"Hall_Wall_{Index}", Id, X, Y, Yaw)
	for Index, X in enumerate((-3350.0, -2550.0, -2150.0)):
		Kit(f"Hall_Pillar_{Index}", "pillar" if Index != 1 else "pillar_decorated", X, -720.0, 0.0, 0.9)
		BoxCollider(f"Hall_Pillar_{Index}_Collision", (X, -720.0, 150.0), (60.0, 60.0, 150.0))
	BoxCollider("Hall_Wall_Collision", (-2550.0, -830.0, 200.0), (820.0, 60.0, 200.0))
	WallTorch(-2950.0, -780.0, 200.0)
	WallTorch(-2150.0, -760.0, 200.0)
	Kit("Hall_Rubble", "rubble_half", -3550.0, -640.0, 70.0, 0.7)
	# 야영 흔적: 꺼진 모닥불(불씨) + 통·상자·탁자·의자 + 뼈
	CampX, CampY = -2550.0, 80.0
	PH_("Hall_FirePit", "stone_fire_pit", CampX, CampY, 15.0, 0.9, Sink=8.0)
	Particles("Hall_FireEmbers", Fx("HD2DCaveBrazier"), (CampX, CampY, Height(CampX, CampY) + 15.0), 0.6)
	Point("Hall_FireLight", (CampX, CampY, Height(CampX, CampY) + 70.0), (1.0, 0.45, 0.15), 10.0, 900.0, True, Flicker={"Style": "Fire", "Seed": 7, "Amount": 0.3})
	BoxCollider("Hall_FirePit_Collision", (CampX, CampY, 20.0), (60.0, 60.0, 40.0))
	Reserve(CampX, CampY, 160.0)
	for Index, (Id, X, Y, Yaw, Scale) in enumerate((("barrel_small_stack", -3350.0, -450.0, 20.0, 0.8), ("crates_stacked", -1950.0, -560.0, -10.0, 0.75),
													 ("table_medium_decorated_A", -2850.0, -420.0, 15.0, 0.8), ("stool", -2700.0, -470.0, 40.0, 0.9),
													 ("keg_decorated", -3300.0, 420.0, 80.0, 0.6), ("box_large", -2100.0, 480.0, 30.0, 0.6))):
		Kit(f"Hall_{Id}_{Index}", Id, X, Y, Yaw, Scale)
		BoxCollider(f"Hall_{Id}_{Index}_Collision", (X, Y, 60.0), (60.0 * Scale + 20.0, 60.0 * Scale + 20.0, 60.0), Yaw)
		Reserve(X, Y, 110.0)
	Kit("Hall_Shelf", "shelf_small_candles", -3150.0, -760.0, 90.0, 1.0, Z=150.0)
	Bones("Hall_Bones_0", -1950.0, 300.0)

	# ---- 2. 갈림길: 앞으로는 함정 복도, 뒤로 돌계단을 오르면 보물 단 (단 앞면 옹벽 + 난간 기둥)
	FrontY = -980.0
	for Index, (X0, X1) in enumerate(((-1520.0, -1010.0), (-790.0, -330.0))):
		Seg = X1 - X0
		E.Box(f"Ledge_Wall_{Index}", ((X0 + X1) * 0.5, FrontY + 15.0, (LEDGE_H + 40.0) * 0.5 - 30.0), (Seg, 60.0, LEDGE_H + 100.0), "EnvStoneDark")
		E.Box(f"Ledge_Wall_{Index}_Cap", ((X0 + X1) * 0.5, FrontY + 15.0, LEDGE_H + 30.0), (Seg + 4.0, 72.0, 12.0), "EnvStone")
		BoxCollider(f"Ledge_Wall_{Index}_Collision", ((X0 + X1) * 0.5, FrontY + 15.0, LEDGE_H * 0.5 + 40.0), (Seg * 0.5, 30.0, LEDGE_H * 0.5 + 80.0))
	Steps, Tread = 6, 42.0
	for K in range(Steps):
		Top = LEDGE_H / Steps * (K + 1)
		Y0 = FrontY + 45.0 + (Steps - 1 - K) * Tread
		Back = FrontY - 40.0 if K == Steps - 1 else Y0
		E.Box(f"Ledge_Step_{K}", (-900.0, (Back + Y0 + Tread) * 0.5, (Top - 40.0) * 0.5), (220.0, Y0 + Tread - Back, Top + 40.0), "EnvStone")
		BoxCollider(f"Ledge_Step_{K}_Collision", (-900.0, (Back + Y0 + Tread) * 0.5, (Top - 40.0) * 0.5), (110.0, (Y0 + Tread - Back) * 0.5, (Top + 40.0) * 0.5))
	for Side in (-1, 1):
		BoxCollider(f"Ledge_StepSide{Side}", (-900.0 + Side * 125.0, FrontY + 45.0 + Steps * Tread * 0.5, 90.0), (15.0, Steps * Tread * 0.5, 90.0))
		E.Box(f"Ledge_StepCheek{Side}", (-900.0 + Side * 125.0, FrontY + 45.0 + Steps * Tread * 0.5, 60.0), (30.0, Steps * Tread, 140.0), "EnvStoneDark")
	for Index, (Id, X, Yaw) in enumerate((("wall_arched", -1300.0, 90.0), ("wall", -900.0, 90.0), ("wall_doorway", -500.0, 90.0))):
		Kit(f"Ledge_Back_{Index}", Id, X, -1540.0, Yaw, 1.0, Z=LEDGE_H)
	BoxCollider("Ledge_Back_Collision", (-900.0, -1540.0, LEDGE_H + 200.0), (650.0, 50.0, 200.0))
	Kit("Ledge_Banner", "banner_patternA_red", -900.0, -1490.0, 90.0, 0.8, Z=LEDGE_H + 40.0)
	WallTorch(-1180.0, -1490.0, LEDGE_H + 200.0)
	WallTorch(-620.0, -1490.0, LEDGE_H + 200.0)
	Kit("Ledge_Coins", "coin_stack_small", -1420.0, -1350.0, 20.0, 1.0)
	Kit("Ledge_Bottle", "bottle_A_labeled_green", -1350.0, -1180.0, 0.0, 1.0)
	Crystal("Fork_Crystal_0", -1350.0, 380.0, 0.9)
	Crystal("Fork_Crystal_1", -350.0, -650.0, 1.1, "CrystalViolet")
	Stalagmite("Fork_Stalagmite_0", -1250.0, -500.0, 230.0)
	Stalagmite("Fork_Stalagmite_1", -420.0, 360.0, 160.0)

	# ---- 3. 함정 복도: 양옆(뒤) 유적 벽 + 기둥, 가운데 깊은 구덩이 위 흔들다리, 동쪽 바닥 가시 함정
	for Index in range(6):
		X = -150.0 + Index * 400.0
		if PIT[0] - 200.0 < X < PIT[1] + 200.0:
			continue
		Kit(f"Corridor_Wall_{Index}", ("wall", "wall_broken", "wall_window_open", "wall")[Index % 4], X, -CORRIDOR_HALF - 70.0, 90.0)
	BoxCollider("Corridor_BackWall", (825.0, -CORRIDOR_HALF - 70.0, 200.0), (1175.0, 50.0, 200.0))
	for Index, X in enumerate((-250.0, 450.0, 1350.0, 1950.0)):
		Kit(f"Corridor_Pillar_{Index}", "column", X, -CORRIDOR_HALF + 20.0, 0.0, 1.0)
		BoxCollider(f"Corridor_Pillar_{Index}_Collision", (X, -CORRIDOR_HALF + 20.0, 70.0), (35.0, 35.0, 70.0))
	WallTorch(150.0, -CORRIDOR_HALF - 25.0, 200.0)
	WallTorch(1600.0, -CORRIDOR_HALF - 25.0, 200.0)
	# 구덩이: 다리(판자 + 밧줄 난간) + 다리 밖은 막음 + 바닥 깊은 곳 수정 빛
	Mid = (PIT[0] + PIT[1]) * 0.5
	E.Bridge("Pit_Bridge", Mid, 0.0, 0.0, PIT[1] - PIT[0] + 160.0, BRIDGE_HALF_W * 2.0, 2.0)
	for Side in (-1, 1):
		BoxCollider(f"Pit_Block{Side}", (Mid, Side * (BRIDGE_HALF_W + 20.0 + (CORRIDOR_HALF + 300.0 - BRIDGE_HALF_W) * 0.5), 100.0),
					((PIT[1] - PIT[0]) * 0.5, (CORRIDOR_HALF + 300.0 - BRIDGE_HALF_W) * 0.5, 300.0))
	for Index, (DX, DY) in enumerate(((-150.0, -260.0), (120.0, -160.0), (0.0, -380.0))):
		Crystal(f"Pit_Crystal_{Index}", Mid + DX, DY, 0.9, "CrystalBlue" if Index != 1 else "CrystalViolet", Light=Index != 2, Z=-600.0)
	Point("Pit_Glow", (Mid, 0.0, -350.0), (0.3, 0.6, 1.0), 14.0, 900.0)
	# 가시 함정판 (복도 폭 전체 — 피해 갈 수 없고 때를 맞춰 건넌다): 판(움직이지 않음) + "Trap_<번호>" 가시 묶음(관리자 HD2DDungeon.lua가
	#   주기마다 내렸다 올린다 — 씬에는 솟은 자리로 둔다). 관리자 속성 Traps = "x,y,z,반폭X,반폭Y;..." (z = 판 윗면 근처 가시 묶음 기준)
	TrapHX, TrapHY = Layout.TRAP_HALF
	for Index, X in enumerate(Layout.TRAP_XS):
		Z0 = max(Height(X + DX, DY) for DX in (-TrapHX, 0.0, TrapHX) for DY in (-TrapHY, -TrapHY * 0.5, 0.0, TrapHY * 0.5, TrapHY)) + 1.0
		E.Box(f"Trap_Plate_{Index + 1}", (X, 0.0, Z0 - 4.0), (TrapHX * 2.0 - 6.0, TrapHY * 2.0, 12.0), "EnvStoneDark")
		Root = S.Add(f"Trap_{Index + 1}", {}, (X, 0.0, Z0))
		Rows = int(TrapHY * 2.0 // 62.0)
		for K in range(3):
			for J in range(Rows):
				S.Add(f"Trap_{Index + 1}_Spike{K}_{J}", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": CMat("Spike")}},
					  (-28.0 + K * 28.0 + (J % 2) * 8.0 - 4.0, -TrapHY + 31.0 + J * 62.0, 12.0), QuatFromEuler(Pitch=45.0, Roll=35.3), (0.1, 0.1, 0.1), Root)
		TrapSpots.append((X, 0.0, Z0, TrapHX, TrapHY))
	S.Add("Trap_RuneGlow", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": CMat("Rune")}}, (1300.0, 250.0, 2.0), None, (0.5, 0.5, 0.02))
	Bones("Corridor_Bones_0", 1950.0, 230.0)
	Bones("Corridor_Bones_1", 400.0, -220.0)

	# ---- 4. 수정 호수: 큰 수정 무리 + 물가 바위 + 이끼·풀 + 종유석
	for Index, (X, Y, Scale, Material) in enumerate(((2150.0, -800.0, 1.6, "CrystalBlue"), (3150.0, -900.0, 1.4, "CrystalViolet"), (2650.0, -1150.0, 2.0, "CrystalBlue"),
													 (1950.0, 380.0, 0.8, "CrystalBlue"), (3300.0, 300.0, 0.9, "CrystalViolet"))):
		Crystal(f"Lake_Crystal_{Index}", X, Y, Scale, Material)
	for Index in range(9):
		A = Index / 9.0 * math.tau + 0.3
		X, Y = LX + math.cos(A) * (LRX + 70.0), LY + math.sin(A) * (LRY + 60.0)
		if Y > LY + LRY * 0.6:
			continue
		PH_(f"Lake_Rock_{Index}", "rock_07", X, Y, Rng.uniform(0, 360), Rng.uniform(0.6, 1.1), Sink=15.0)
	for Index, (X, Y, H) in enumerate(((2050.0, -1050.0, 320.0), (3350.0, -800.0, 260.0), (2900.0, 500.0, 140.0), (2350.0, 520.0, 110.0))):
		Stalagmite(f"Lake_Stalagmite_{Index}", X, Y, H)

	# ---- 5. 보스 방: 둥근 유적 홀 — 바닥돌 타일, 기둥 고리, 뒤 제단(계단 단 + 깃발 + 화로), 금화 더미
	BX, BY = Layout.BOSS_SPOT
	for I in range(-1, 2):
		for J in range(-1, 2):
			Kit(f"Boss_Floor_{I}_{J}", "floor_tile_large", BX + I * 400.0, BY + J * 400.0, 0.0, 1.0, Z=-4.0)
	Ring = 8
	for Index in range(Ring):
		A = Index / Ring * math.tau + math.pi / Ring
		PX, PY = BX + math.cos(A) * 720.0, BY + math.sin(A) * 600.0
		if PY > BY + 200.0:
			Kit(f"Boss_PillarStump_{Index}", "column", PX, PY, Rng.uniform(0, 90), 0.7)  # 카메라 쪽은 낮은 기둥 그루터기
			BoxCollider(f"Boss_Pillar_{Index}_Collision", (PX, PY, 50.0), (30.0, 30.0, 50.0))
		else:
			Kit(f"Boss_Pillar_{Index}", "pillar_decorated" if Index % 2 else "pillar", PX, PY, 0.0, 1.0)
			BoxCollider(f"Boss_Pillar_{Index}_Collision", (PX, PY, 200.0), (60.0, 60.0, 200.0))
	DaisY = BY - 820.0
	E.Box("Boss_Dais", (BX, DaisY, 30.0), (900.0, 260.0, 120.0), "EnvStoneDark")
	E.Box("Boss_DaisTop", (BX, DaisY, 92.0), (920.0, 280.0, 8.0), "EnvStone")
	E.Box("Boss_Altar", (BX, DaisY - 40.0, 150.0), (220.0, 90.0, 110.0), "EnvStone")
	S.Add("Boss_AltarRune", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": CMat("Rune")}}, (BX, DaisY + 6.0, 150.0), None, (1.6, 0.02, 0.5))
	BoxCollider("Boss_Dais_Collision", (BX, DaisY, 60.0), (450.0, 130.0, 60.0))
	for Index, (Id, X, Yaw) in enumerate((("wall", BX - 400.0, 90.0), ("wall_arched", BX, 90.0), ("wall", BX + 400.0, 90.0))):
		Kit(f"Boss_BackWall_{Index}", Id, X, DaisY - 170.0, Yaw, 1.0, Z=0.0)
	for Side in (-1, 1):
		Kit(f"Boss_Banner{Side}", "banner_patternA_red", BX + Side * 230.0, DaisY - 120.0, 90.0, 0.9, Z=40.0)
		# 화로 (기둥 위 불꽃) + 불빛 (그림자 — 보스 방 주광)
		HX, HY = BX + Side * 520.0, DaisY + 60.0
		Kit(f"Boss_Brazier{Side}", "column", HX, HY, 0.0, 0.8)
		S.Add(f"Boss_BrazierBowl{Side}", {"StaticMeshComponent": {"MeshAsset": "primitive:sphere", "MaterialAsset": CMat("Ember")}},
			  (HX, HY, 118.0), None, (0.55, 0.55, 0.18))
		Particles(f"Boss_BrazierFire{Side}", Fx("HD2DCaveBrazier"), (HX, HY, 125.0))
		Point(f"Boss_BrazierLight{Side}", (HX, HY + 40.0, 210.0), (1.0, 0.5, 0.2), 22.0, 1500.0, Side == 1, Flicker={"Style": "Fire", "Seed": 400 + Side, "Amount": 0.25})
	for Index, (Id, X, Y) in enumerate((("coin_stack_large", BX - 380.0, DaisY + 170.0), ("coin_stack_small", BX + 340.0, DaisY + 190.0),
										("chest_gold", BX + 250.0, DaisY + 170.0))):
		Kit(f"Boss_Hoard_{Index}", Id, X, Y, Rng.uniform(-20, 20), 0.6)
	Bones("Boss_Bones_0", BX - 450.0, BY + 250.0)
	Bones("Boss_Bones_1", BX + 300.0, BY - 150.0)
	Point("Boss_Rune", (BX, DaisY + 60.0, 160.0), (0.3, 0.7, 1.0), 5.0, 600.0, Flicker={"Style": "Pulse", "Seed": 9, "Amount": 0.35, "Speed": 0.4})

	# ---- 길가에 세운 횃불 (캐릭터가 지나는 앞쪽을 밝힌다 — 낮은 기둥이라 가리지 않음)
	for Index, (X, Y) in enumerate(((-3050.0, 380.0), (-1650.0, 150.0), (-600.0, 300.0), (2050.0, 300.0), (3150.0, 330.0))):
		Z = Height(X, Y)
		E.Box(f"StandTorch_{Index}_Post", (X, Y, Z + 55.0), (14.0, 14.0, 130.0), "EnvTimber")
		S.Add(f"StandTorch_{Index}", {"ModelComponent": {"AssetPath": f"{KIT}/torch_lit.glb"}}, (X, Y, Z + 160.0), None, (1.0, 1.0, 1.0))
		Particles(f"StandTorch_{Index}_Flame", "Particles/Demo/CampfireLanternFlame.eparticle", (X, Y, Z + 222.0))
		Point(f"StandTorch_{Index}_Light", (X, Y + 20.0, Z + 240.0), (1.0, 0.56, 0.24), 8.0, 850.0, False, Flicker={"Style": "Fire", "Seed": 500 + Index, "Amount": 0.2})
		BoxCollider(f"StandTorch_{Index}_Collision", (X, Y, Z + 80.0), (12.0, 12.0, 80.0))
		Reserve(X, Y, 80.0)

	# ---- 카메라 쪽(+Y) 낮은 바위 턱 위 돌무더기 + 안쪽 벽가 종유석 + 공용 파티클
	for Index in range(16):
		for _ in range(40):
			X = Rng.uniform(-3600.0, 4600.0)
			Y = Rng.uniform(250.0, 1100.0)
			D = float(WalkDistance(np.array(X), np.array(Y)))
			if 30.0 < D < 140.0 and Free(X, Y, 120.0):
				break
		PH_(f"Rim_Rock_{Index}", "rock_07", X, Y, Rng.uniform(0, 360), Rng.uniform(0.5, 0.9), Sink=20.0)
		Reserve(X, Y, 100.0)
	for Index in range(14):
		for _ in range(40):
			X = Rng.uniform(-3600.0, 4600.0)
			Y = Rng.uniform(-1700.0, -300.0)
			D = float(WalkDistance(np.array(X), np.array(Y)))
			if -60.0 < D < 120.0 and Free(X, Y, 120.0):
				break
		Stalagmite(f"Wall_Stalagmite_{Index}", X, Y, Rng.uniform(150.0, 320.0))
		Reserve(X, Y, 100.0)
	Particles("Cave_Drips", Fx("HD2DCaveDrips"), (500.0, -400.0, 0.0))
	Particles("Cave_Dust", Fx("HD2DCaveDust"), (500.0, -300.0, 0.0))
	S.Add("Cave_Sound", {"AudioSourceComponent": {"ClipAsset": "Audio/Demo/ForestStream.wav", "Volume": 0.35, "Pitch": 0.6, "Loop": True,
		"PlayOnStart": True, "Spatial": True, "MinDistance": 400.0, "MaxDistance": 3000.0}}, (LX, LY, 0.0))
	S.Add("Boss_Sound", {"AudioSourceComponent": {"ClipAsset": "Audio/Demo/CampfireCrackle.wav", "Volume": 0.5, "Pitch": 0.9, "Loop": True,
		"PlayOnStart": True, "Spatial": True, "MinDistance": 200.0, "MaxDistance": 2000.0}}, (BX, DaisY, 120.0))

	# ---- 풀 (동굴 흙 위 드문드문 — 호수가·입구 쪽 빛 드는 곳)
	GrassRng = np.random.default_rng(71)
	Grass = []
	P = GrassRng.uniform((-3800.0, -1600.0), (4600.0, 900.0), size=(40000, 2))
	Keep = WalkDistance(P[:, 0], P[:, 1]) < -40.0
	Keep &= (np.abs(P[:, 0] + 3500.0) < 900.0) | (Main.EllipseValue(P[:, 0], P[:, 1], LAKE) < 1.9)
	Keep &= ~InPit(P[:, 0], P[:, 1])
	Keep &= GrassRng.random(len(P)) < 0.6
	for OX, OY, OR in Occupied:
		Keep &= (P[:, 0] - OX) ** 2 + (P[:, 1] - OY) ** 2 >= (OR * 0.7) ** 2
	for X, Y in P[Keep]:
		X, Y = float(X), float(Y)
		if Main.EllipseValue(np.array(X), np.array(Y), LAKE) < 1.05:
			continue
		N = Height.Normal(X, Y)
		Grass.append((X, Y, Height(X, Y), float(GrassRng.uniform(0, 360)), float(GrassRng.uniform(0.4, 0.75)), float(N[0]), float(N[1]), float(N[2])))

	# ---- 게임: 관리자·HUD + 카메라 + 플레이어 (HD2DGameplay — 자리는 HD2DCaveLayout). 타이틀 없음: 세션 없이 열면 기본 상태로 바로 플레이
	GX, GY, GHalf = Layout.BOSS_GATE
	GateZ = Height(GX, GY) + GATE_LIFT
	CaveLayout = HD2DGameplay.FMapLayout(
		"Cave", Chests=[(X, Y, C) for (X, Y), C in zip(Layout.CHEST_SPOTS, Layout.CHEST_CONTENTS)],
		Enemies=[(Kind, X, Y) for Room, Spots in Layout.ENEMY_SPOTS.items() for Kind, (X, Y) in zip(Layout.ENEMY_KINDS[Room], Spots)],
		Boss=Layout.BOSS_SPOT, BossKind="SpiderQueen", BossReward=Layout.BOSS_REWARD, Respawn=False,
		Extra={"Traps": ";".join(f"{X:.0f},{Y:.0f},{Z:.1f},{HX:.0f},{HY:.0f}" for X, Y, Z, HX, HY in TrapSpots),
			   "Gate": f"{GX:.0f},{GY:.0f},{GateZ:.0f},{GATE_DOWN:.0f}", "Arena": f"{Layout.BOSS_SPOT[0]:.0f},{Layout.BOSS_SPOT[1]:.0f},{Layout.BOSS_ARENA_RADIUS + 120.0:.0f}"})
	HD2DGameplay.AddGame(S, Height, CAVE_PATH, AutoPlay, Layout=CaveLayout, Title=False, NavMesh=NAV_ASSET, Minimap=MINIMAP)
	StartZ = Height(*Start) + HD2DGameplay.PLAYER_RADIUS + HD2DGameplay.PLAYER_HALF + 4.0
	Forward = (0.0, -math.cos(math.radians(-Main.CAMERA_PITCH)), -math.sin(math.radians(-Main.CAMERA_PITCH)))
	Focus = (Start[0], Start[1], StartZ - 85.0 + 70.0)
	# 동굴 카메라: 메인과 같은 깊이 + 틸트시프트·육각 보케, 색 보정은 차갑고 푸르게(그림자 청록, 하이라이트는 횃불 주황 유지), 비네트 더 짙게
	S.Add("Camera", {"CameraComponent": {"FovYDegrees": Main.CAMERA_FOV, "NearZ": 50.0, "FarZ": 60000.0, "Primary": True, "Priority": 10},
					 "DepthOfFieldComponent": {"Enabled": Overview is None, "FocusDistance": Main.CAMERA_DISTANCE, "FocalRegion": Main.DOF_FOCAL_REGION,
											   "NearTransition": 700.0, "FarTransition": 1800.0, "NearBlurSize": 1.4, "FarBlurSize": 1.7,
											   "PreviewInEditor": False, "Mode": 2, "TiltShiftCenter": 0.53, "TiltShiftBand": 0.11,
											   "TiltShiftTransition": 0.38, "TiltShiftAngle": 0.0, "BokehBladeCount": 6, "BokehRotation": 15.0,
											   "BokehHighlightBoost": 3.5, "BokehHighlightThreshold": 0.8},
					 "ColorGradingComponent": {"Enabled": True, "Temperature": -0.28, "Tint": -0.02, "Saturation": 1.08, "Contrast": 1.14,
											   "Lift": [0.0, 0.012, 0.03], "Gamma": [0.98, 1.0, 1.04], "Gain": [1.02, 1.0, 1.04],
											   "LookupTable": "", "LookupTableIntensity": 1.0},
					 "VignetteComponent": {"Enabled": True, "Intensity": 0.72, "Size": 0.36, "Smoothness": 0.6, "Roundness": 1.0,
										   "Color": [0.0, 0.005, 0.015]}},
		  tuple(Focus[I] - Forward[I] * Main.CAMERA_DISTANCE for I in range(3)), QuatFromEuler(Pitch=Main.CAMERA_PITCH, Yaw=-90.0))
	if Overview:
		X, Y, Z, Pitch, Yaw, Fov = Overview
		S.Add("OverviewCamera", {"CameraComponent": {"FovYDegrees": Fov, "NearZ": 50.0, "FarZ": 80000.0, "Primary": True, "Priority": 100}},
			  (X, Y, Z), QuatFromEuler(Pitch=Pitch, Yaw=Yaw))
	HD2DGameplay.AddPlayer(S, Start, Height)
	return S, Grass


VIEW_STARTS = {"Hall": (-2550.0, 350.0), "Fork": (-900.0, 100.0), "Ledge": (-900.0, -1250.0), "Corridor": (1500.0, 100.0),
			   "Lake": (2500.0, 150.0), "Boss": (3880.0, 150.0)}
OVERVIEW_VIEWS = {"West": (-1500.0, 3200.0, 3600.0, -45.0, -90.0, 50.0), "East": (2600.0, 3200.0, 3600.0, -45.0, -90.0, 50.0)}
# 자동 검증·스크린샷 시나리오 (이름: (시작 자리, HD2DAutoPilot 시나리오)) → _HD2DCave<이름>.escene
AUTO_SCENES = {
	"AutoPlay":    (Layout.PLAYER_START, "Cave"),        # 입구 → 방마다 적 → 상자 3 → 함정 → 보스 → 보상 → 출구로 메인 맵(엔딩까지)
	"Shot_Combat": ((2300.0, 150.0), "CaveCombat"),       # 수정 호수 전투 (성능 측정)
	"Shot_Boss":   ((3500.0, -200.0), "CaveBoss"),        # 보스전
	"Shot_Trap":   ((1250.0, 60.0), "CaveTrap"),          # 가시 함정 경고·솟음
	"Shot_Ending": (Layout.PLAYER_START, "CaveEnding"),   # 엔딩·크레딧 화면 (마지막 단계로 바로)
	"Shot_Map":    ((2300.0, 150.0), "MapScreen"),       # 일시정지 메뉴 지도 화면 (동굴 지도 — HD2DMetaPilot)
}


# 지도 화면·미니맵 (HD2DMapArt): 놀이 영역 + 방 이름 (이름, X, Y, Place|Exit)
MINIMAP = HD2DMapArt.Info("Cave", (Layout.PLAY_MIN[0] - 100.0, Layout.PLAY_MIN[1] - 100.0, Layout.PLAY_MAX[0] + 100.0, Layout.PLAY_MAX[1] + 100.0),
						  "폭포 옆 동굴 유적", [
	("입구 홀", -2650.0, -640.0, "Place"), ("갈림길", -850.0, 330.0, "Place"), ("보물 단", -620.0, -1420.0, "Place"),
	("함정 복도", 820.0, 260.0, "Place"), ("수정 호수", 2650.0, -700.0, "Place"), ("여왕의 둥지", 3900.0, 260.0, "Place"),
	("마을로", -3480.0, -120.0, "Exit")])


def WriteMinimap():
	# 지도 그림: 걷는 방(흙 바닥 / 유적 바닥돌 / 호숫가 이끼) + 지하 호수 + 구덩이(다리만 남김) + 벽 가장자리 바위, 나머지는 어둠
	def Classify(X, Y):
		D = WalkDistance(X, Y)
		Ruin = np.zeros(np.shape(X), dtype=bool)
		for Name, Kind, V in ROOMS:
			if Name in ("Corridor", "Boss", "Ledge"):
				Ruin |= RoomDistance(X, Y, Kind, V) < 0.0
		Lake = Main.EllipseValue(X, Y, LAKE)
		Out = np.where(D < 0.0, np.where(Ruin, HD2DMapArt.K_TILE, HD2DMapArt.K_FLOOR), np.where(D < 160.0, HD2DMapArt.K_ROCK, HD2DMapArt.K_VOID))
		Out = np.where((D < 0.0) & (Lake < 1.45) & ~Ruin, HD2DMapArt.K_MOSS, Out)
		Out = np.where(Lake < 1.0, HD2DMapArt.K_WATER, Out)
		return np.where(InPit(X, Y) & (np.abs(Y) >= BRIDGE_HALF_W) & (D < 160.0), HD2DMapArt.K_VOID, Out)
	HD2DMapArt.WriteMinimap(CONTENT, MINIMAP, Classify)


def NavBakeHeight(Height):
	# 굽기용 바닥 판: 걷는 방 안(벽 경사 시작 전)만 — 바깥 벽 위·낭떠러지는 비운다 (WaterBelow 아래 = 빈칸)
	def Sample(X, Y):
		return Height(X, Y) if float(WalkDistance(np.array(X), np.array(Y))) < 30.0 else -1.0e4
	return Sample


def InstallNav():
	import shutil
	Root = os.path.join(CONTENT, "Scenes", "Demo")
	shutil.copyfile(os.path.join(Root, "_HD2DCaveNavBake.enav"), os.path.join(Root, "HD2DCave.enav"))
	print("Scenes/Demo/HD2DCave.enav 설치")


def Main_():
	if "--install-nav" in sys.argv:
		InstallNav()
		return
	X, Y, H, D = BuildHeights()
	Weights, Stack = BuildWeights(X, Y, H, D)
	WriteTerrain(os.path.join(CONTENT, "Terrain", "Demo", "HD2DCave.eterrain"), H, Weights)
	Sampler = FCaveHeight(H, Stack)
	WriteMaterials()
	WriteParticles()
	# 게임 공용 (데이터 표·UI·프리팹 — BuildHD2D.py와 같은 결과) + 동굴 도트 아트·보스 방 문 프리팹
	HD2DGameplay.WriteAll(CONTENT, Main.CAMERA_DISTANCE, Main.PLAY_MIN, Main.PLAY_MAX)
	HD2DArt.WriteCaveArt(os.path.join(CONTENT, "Sprites", "HD2D"), os.path.join(CONTENT, *HD2DGameplay.UI_DIR.split("/")))
	WriteGatePrefab()
	Scene, Grass = BuildScene(Sampler)
	WriteMinimap()
	WriteFoliage(os.path.join(CONTENT, "Foliage", "Demo", "HD2DCave.efoliage"), [(CAVE_GRASS_TYPE, Grass)])
	Scene.Save(os.path.join(CONTENT, *SCENE.split("/")))
	print(f"HD2D 동굴 생성: 엔티티 {len(Scene.Entities)}개, 풀 {len(Grass)}개, 높이 {H.min():.0f}~{H.max():.0f}cm")
	if "--views" in sys.argv:
		for Name, Start in VIEW_STARTS.items():
			Variant, _ = BuildScene(Sampler, Start)
			Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_HD2DCave_{Name}.escene"))
		for Name, View in OVERVIEW_VIEWS.items():
			Variant, _ = BuildScene(Sampler, Layout.PLAYER_START, View)
			Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_HD2DCave_Over{Name}.escene"))
		for Name, (Start, Scenario) in AUTO_SCENES.items():
			Variant, _ = BuildScene(Sampler, Start, AutoPlay=Scenario)
			Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_HD2DCave{Name}.escene"))
		HD2DGameplay.WriteNavBake(CONTENT, Scene, NavBakeHeight(Sampler), Layout.PLAY_MIN, Layout.PLAY_MAX, CAVE_PATH, "_HD2DCaveNavBake", 50.0)
		print("확인용 변형: Scenes/Demo/_HD2DCave_*.escene, _HD2DCaveAutoPlay.escene, _HD2DCaveShot_*.escene (커밋하지 않음)")


if __name__ == "__main__":
	Main_()
