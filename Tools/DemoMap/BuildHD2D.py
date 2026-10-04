# 데모 서브맵 "HD2D"(HD-2D 느낌 — 3D 디오라마 + 도트 스프라이트 캐릭터) 생성:
#   도트 아트(HD2DArt.py → Sprites/HD2D/) + 지형(.eterrain)·지형/풀 머티리얼 + 폴리지 + 프리팹(플레이어·슬라임·효과 조각) + 씬(Scenes/Demo/HD2D.escene)
#   실행: python Tools/DemoMap/BuildHD2D.py [--views]   (Poly Haven 에셋은 Scripts/FetchDemoAssets.ps1로 먼저 받는다)
#   --views: 확인용 변형도 쓴다(커밋하지 않음) — Scenes/Demo/_HD2D_<시점>.escene(플레이어 시작 자리만 다름), _HD2DAutoPlay.escene(자동 플레이 검증)
#   보여 주는 것: 원근 고정 시점 카메라(좁은 시야각 — 디오라마) + 조명 받는 Masked 도트 스프라이트(그림자 드리움, TAA 떨림 없음),
#                 해 질 녘 하늘·볼류메트릭 안개, 등불·창문·대장간·모닥불 점광원(깜빡임), 파티클(불꽃·연기·반딧불), 물(연못), 풀 폴리지
#   게임: 플레이어(이동·4방향·공격·대시) + 슬라임(깡충 이동·추적·접촉 피해·부활) — Scripts/Demo/HD2D/*.lua
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
	# 안쪽(-Y) 언덕: 놀이 영역 뒤로 솟아 산자락이 된다 / 서·동쪽 끝과 카메라 쪽 끝도 조금 올려 경계를 감춘다
	H += Smoothstep(-2100.0, -4200.0, Y) * (700.0 + Fbm(X, Y, 2600.0, 13) * 380.0)
	H += Smoothstep(-4600.0, -6500.0, X) * 500.0 + Smoothstep(5700.0, 7500.0, X) * 600.0
	H += Smoothstep(1700.0, 3500.0, Y) * 250.0
	# 방앗간 언덕
	H += np.exp(-(((X - 4000.0) / 900.0) ** 2 + ((Y + 1650.0) / 650.0) ** 2)) * 160.0
	# 광장·길은 평평하게
	Flat = np.maximum(Smoothstep(1.25, 0.95, EllipseValue(X, Y, PLAZA)), Smoothstep(320.0, 160.0, SegmentDistance(X, Y, PATH)))
	H = H * (1.0 - Flat)
	# 연못: 가장자리에서 완만히 파여 -110
	PondT = EllipseValue(X, Y, POND)
	H = H - Smoothstep(1.15, 0.55, PondT) * (110.0 + H)
	return X, Y, H


def BuildWeights(X, Y, H):
	# 레이어: 0 풀, 1 흙길, 2 자갈 광장, 3 이끼 언덕(경사·연못가)
	GY, GX = np.gradient(H, TERRAIN_SIZE / (TERRAIN_RES - 1))
	Slope = np.sqrt(GX * GX + GY * GY)
	Noise = Fbm(X, Y, 500.0, 71, 3)
	Plaza = Smoothstep(1.02 + Noise * 0.06, 0.9 + Noise * 0.06, EllipseValue(X, Y, PLAZA))
	PathD = np.minimum(SegmentDistance(X, Y, PATH), SegmentDistance(X, Y, PATH_MILL) + 40.0)
	Path = Smoothstep(150.0 + Noise * 40.0, 95.0 + Noise * 40.0, PathD) * (1.0 - Plaza)
	Moss = np.maximum(Smoothstep(0.2, 0.45, Slope + Noise * 0.08), Smoothstep(1.35, 1.0, EllipseValue(X, Y, POND)) * 0.85)
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


def WritePrefab(Name, Entities):
	Folder = os.path.join(CONTENT, *PREFABS.split("/"))
	os.makedirs(Folder, exist_ok=True)
	with open(os.path.join(Folder, f"{Name}.eprefab"), "w", encoding="utf-8", newline="\n") as File:
		json.dump({"Entities": Entities, "NextId": len(Entities) + 1, "Version": 1}, File, indent=2, ensure_ascii=False)
		File.write("\n")


PLAYER_RADIUS, PLAYER_HALF = 32.0, 53.0   # 캡슐 바닥 = 중심 - 85
SLIME_RADIUS, SLIME_HALF   = 38.0, 4.0     # 캡슐 바닥 = 중심 - 42


def WritePrefabs():
	Foot = -(PLAYER_RADIUS + PLAYER_HALF)
	Player = [
		{"Name": "Player", "Parent": -1, "Components": {
			"CharacterMovementComponent": {
				"AirControl": 0.35, "CapsuleHalfHeight": PLAYER_HALF, "CapsuleRadius": PLAYER_RADIUS, "FaceControlYaw": False,
				"GravityScale": 1.0, "JumpZVelocity": 0.0, "Mass": 70.0, "MaxSlopeAngle": 50.0, "MaxStepHeight": 35.0,
				"MaxWalkSpeed": 430.0, "PushForce": 2000.0, "KnockbackDeceleration": 2600.0},
			"ScriptComponent": {"ExecutionLocation": 2, "ScriptAsset": "Scripts/Demo/HD2D/HD2DPlayer.lua", "PropertyOverrides": json.dumps({
				"CameraDistance": CAMERA_DISTANCE, "MinX": PLAY_MIN[0] + 900.0, "MaxX": PLAY_MAX[0] - 900.0,
				"MinY": PLAY_MIN[1] + 500.0, "MaxY": PLAY_MAX[1] - 650.0}, ensure_ascii=False)},
			"PrefabLinkComponent": Link(1), "TransformComponent": Transform((0, 0, 100))}},
		{"Name": "Visual", "Parent": 0, "Components": {"PrefabLinkComponent": Link(2), "TransformComponent": Transform()}},
		{"Name": "Body", "Parent": 1, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Hero.esprite", "IdleDown0"), "FlipbookComponent": Flipbook("Sprites/HD2D/Hero_IdleDown.eflipbook"),
			"PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, Foot))}},
		{"Name": "Shadow", "Parent": 1, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
			"PrefabLinkComponent": Link(4), "TransformComponent": Transform((0, 0, Foot + 1.5), FLAT, (0.75, 1.0, 0.75))}},
	]
	for Index in range(5):
		Player.append({"Name": f"Heart{Index}", "Parent": 1, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "HeartFull", Lit=False, Shadows=False, Visible=False),
			"PrefabLinkComponent": Link(5 + Index), "TransformComponent": Transform(((Index - 2) * 38.0, 8.0, Foot + 205.0), None, (0.7, 1.0, 0.7))}})
	WritePrefab("Player", Player)

	SlimeFoot = -(SLIME_RADIUS + SLIME_HALF)
	WritePrefab("Slime", [
		{"Name": "Slime", "Parent": -1, "Components": {
			"CharacterMovementComponent": {
				"AirControl": 1.0, "CapsuleHalfHeight": SLIME_HALF, "CapsuleRadius": SLIME_RADIUS, "FaceControlYaw": False,
				"GravityScale": 1.0, "JumpZVelocity": 0.0, "Mass": 30.0, "MaxSlopeAngle": 50.0, "MaxStepHeight": 25.0,
				"MaxWalkSpeed": 300.0, "PushForce": 500.0, "KnockbackDeceleration": 2400.0, "ClientPrediction": False},
			"ScriptComponent": {"ExecutionLocation": 0, "PropertyOverrides": "", "ScriptAsset": "Scripts/Demo/HD2D/HD2DSlime.lua"},
			"PrefabLinkComponent": Link(1), "TransformComponent": Transform()}},
		{"Name": "Visual", "Parent": 0, "Components": {"PrefabLinkComponent": Link(2), "TransformComponent": Transform()}},
		{"Name": "Body", "Parent": 1, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Slime.esprite", "Idle0"), "FlipbookComponent": Flipbook("Sprites/HD2D/Slime_Idle.eflipbook"),
			"PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, SlimeFoot))}},
		{"Name": "Shadow", "Parent": 1, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
			"PrefabLinkComponent": Link(4), "TransformComponent": Transform((0, 0, SlimeFoot + 1.5), FLAT, (0.8, 1.0, 0.8))}},
	])
	# 효과 조각: 스크립트 없음 — HD2DGame.lua가 만든 직후 콜백에서 모양을 정하고 수명이 끝나면 지운다
	WritePrefab("FxSprite", [
		{"Name": "FxSprite", "Parent": -1, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Spark0", Lit=False, Shadows=False, Blend=0, Visible=False),
			"FlipbookComponent": Flipbook(""), "PrefabLinkComponent": Link(1), "TransformComponent": Transform()}},
	])


# ---- 씬 배치 --------------------------------------------------------------------------------------------------------
TIME_OF_DAY = 16.9          # 해 질 녘 (간이 모델 6~18시) — 고도 약 11도, 긴 그림자 + 따뜻한 빛
MAX_SUN_ELEVATION = 40.0
SUN_AZIMUTH = 118.0         # 해가 있는 쪽 = 카메라 쪽 왼편(-X, +Y) → 스프라이트 앞면이 빛을 받고 그림자는 안쪽 오른편으로
NORTH_AZIMUTH = SUN_AZIMUTH - 90.0 - (TIME_OF_DAY - 6.0) / 12.0 * 180.0

CAMERA_PITCH = -28.0
CAMERA_FOV = 24.0
CAMERA_DISTANCE = 2900.0
PLAYER_START = (-2050.0, 250.0)
VIEW_STARTS = {
	"Village": (-2050.0, 250.0),
	"Market":  (-3100.0, -600.0),
	"Field":   (1700.0, 0.0),
	"Pond":    (2500.0, -300.0),
	"Mill":    (3800.0, -700.0),
}
SPAWN_POINTS = [(1000.0, -700.0), (1700.0, -250.0), (2300.0, 450.0), (3150.0, -250.0), (3400.0, 750.0), (4300.0, -650.0),
				(4700.0, 0.0), (1500.0, -1450.0), (3400.0, -1350.0), (900.0, 650.0)]


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

	# ---- 지형 + 폴리지 + 연못 (연못 둘레 상자만 — 바깥 지형은 수면보다 높다)
	S.Add("Terrain", {"TerrainComponent": {
		"Asset": "Terrain/Demo/HD2D.eterrain", "Size": [TERRAIN_SIZE, TERRAIN_SIZE], "HeightRange": HEIGHT_RANGE,
		"Layer0Material": f"{MAT_DIR}/TerrainGrass.emat", "Layer1Material": f"{MAT_DIR}/TerrainPath.emat",
		"Layer2Material": f"{MAT_DIR}/TerrainCobble.emat", "Layer3Material": f"{MAT_DIR}/TerrainMoss.emat",
		"Layer0Tiling": 450.0, "Layer1Tiling": 380.0, "Layer2Tiling": 300.0, "Layer3Tiling": 600.0,
		"CastShadows": True, "Collision": True}})
	S.Add("Foliage", {"FoliageComponent": {"Asset": "Foliage/Demo/HD2D.efoliage", "Visible": True}})
	PX, PY, PRX, PRY = POND
	S.Add("Pond", {"WaterBodyComponent": {
		"Size": [PRX * 2.6, PRY * 2.6, 200.0], "ScatterColor": [0.02, 0.07, 0.07], "Absorption": [0.35, 0.12, 0.1],
		"NormalStrength": 0.35, "WaveScale": 160.0, "WaveSpeed": 6.0, "FlowDirection": 0.0, "FlowSpeed": 0.0,
		"FoamIntensity": 0.4, "FoamDistance": 12.0, "RefractionStrength": 0.03, "ReflectionIntensity": 1.0, "Roughness": 0.05}},
		(PX, PY, POND_LEVEL - 100.0))
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

	# ---- 마을 (서쪽) ----------------------------------------------------------------------------------------------
	# KayKit 건물 앞(문) = 엔진 -X → Yaw -90이면 카메라(+Y)를 본다
	Face = -90.0
	CX, CY = PLAZA[0], PLAZA[1]
	KK_("Well", "building_well_blue", CX, CY, Face + 15.0, Collide=True, Shrink=0.8)
	Point("Well_Lantern", (CX + 120.0, CY + 90.0, Height(CX, CY) + 240.0), (1.0, 0.65, 0.35), 3.0, 500.0, Flicker={"Style": "Fire", "Seed": 3, "Amount": 0.15})
	# 안쪽 줄: 선술집 · 교회 · 집 (광장을 본다)
	Back = [("Tavern", "building_tavern_yellow", -3500.0, -1450.0, Face + 8.0, KS),
			("Church", "building_church_green", -2300.0, -1650.0, Face, KS * 1.15),
			("HomeB", "building_home_B_red", -1250.0, -1450.0, Face - 10.0, KS)]
	for Name, Id, X, Y, Yaw, Scale in Back:
		KK_(Name, Id, X, Y, Yaw, Scale, Collide=True)
	# 서쪽: 대장간(광장을 봄) + 집, 동쪽 앞: 시장 천막
	KK_("Blacksmith", "building_blacksmith_blue", -3950.0, -250.0, 0.0 + 180.0, KS, Collide=True)
	KK_("HomeA", "building_home_A_blue", -3900.0, 750.0, Face - 30.0, KS * 0.95, Collide=True)
	KK_("Market", "building_market_red", -1250.0, -550.0, 0.0, KS * 0.9, Collide=True)
	# 창문 불빛 (건물 앞면 앞쪽에 따뜻한 점광원) + 굴뚝 연기
	Windows = [(-3500.0, -1150.0, 260.0), (-3200.0, -1180.0, 200.0), (-2300.0, -1330.0, 330.0), (-1250.0, -1180.0, 230.0),
			   (-3700.0, -250.0, 200.0), (-3700.0, 750.0, 180.0)]
	for Index, (X, Y, Z) in enumerate(Windows):
		Point(f"Window_{Index}", (X, Y, Height(X, Y) + Z), (1.0, 0.68, 0.38), 3.5, 520.0, Flicker={"Style": "Fire", "Seed": 80 + Index, "Amount": 0.08, "Speed": 0.5})
	# 대장간 화로: 불꽃 + 연기 + 주황 불빛(그림자)
	FX_, FY_ = -3650.0, -60.0
	FZ = Height(FX_, FY_)
	PH_("Forge", "barrel_stove", FX_, FY_, 0.0, 1.1, Sink=2.0, Collide=True)
	Particles("Forge_Fire", "Particles/Demo/AlleyStoveFire.eparticle", (FX_, FY_, FZ + 88.0))
	Particles("Forge_Smoke", "Particles/Demo/AlleyStoveSmoke.eparticle", (FX_, FY_, FZ + 125.0))
	Point("Forge_Light", (FX_ + 30.0, FY_ + 40.0, FZ + 140.0), (1.0, 0.46, 0.16), 16.0, 900.0, True, Flicker={"Style": "Fire", "Seed": 11})
	S.Add("Forge_Sound", {"AudioSourceComponent": {"ClipAsset": "Audio/Demo/CampfireCrackle.wav", "Volume": 0.5, "Pitch": 1.2, "Loop": True,
		"PlayOnStart": True, "Spatial": True, "MinDistance": 200.0, "MaxDistance": 2200.0}}, (FX_, FY_, FZ + 80.0))
	for Index, (Id, DX, DY, Yaw) in enumerate((("weaponrack", 230.0, 120.0, 90.0), ("resource_lumber", 180.0, -260.0, 20.0), ("barrel", 260.0, 330.0, 0.0),
											   ("crate_A_big", 310.0, -150.0, 25.0), ("crate_B_small", 300.0, -80.0, 50.0))):
		KK_(f"Smithy_{Id}_{Index}", Id, -3950.0 + DX, -250.0 + DY, Yaw, KS * 0.9, Collide=True)
	PH_("Smithy_Anvil_Stump", "tree_stump_02", -3560.0, -330.0, 30.0, 0.5, Sink=4.0, Collide=True)
	PH_("Smithy_Hammer", "sledgehammer_01", -3520.0, -330.0, 70.0, 1.0, Z=Height(-3560, -330) + 38.0, Roll=90.0)
	# 시장: 천막 노점 + 피크닉 탁자 위 바구니·항아리, 통·상자 무더기
	for Index, (X, Y, Yaw) in enumerate(((-1650.0, 250.0, Face + 20.0), (-1100.0, 380.0, Face - 15.0))):
		KK_(f"Stall_{Index}", "tent", X, Y, Yaw, KS * 1.1, Collide=True, Shrink=0.6)
	Table = PH_("Market_Table", "wooden_picnic_table", -1380.0, 120.0, 10.0, 1.0, Collide=True)
	for G, (Id, DX, DY) in enumerate((("wicker_basket_01", -20.0, -70.0), ("jug_01", 15.0, -15.0), ("wicker_basket_02", -10.0, 45.0), ("jug_01", 20.0, 90.0))):
		S.Model(f"Market_Goods_{G}", f"{PH}/{Id}/{Id}.gltf", (DX, DY, 75.0), Rng.uniform(0, 360), 1.0, Parent=Table)
	PH_("Market_Barrels", "wooden_barrels_01", -800.0, -900.0, 200.0, 0.9, Sink=3.0, Collide=True)
	PH_("Market_CrateA", "wooden_crate_02", -1650.0, -950.0, 20.0, 1.0, Collide=True)
	PH_("Market_CrateB", "wooden_crate_01", -1580.0, -870.0, 65.0, 1.0, Z=Height(-1650, -950) + 44.0)
	for Index, (Id, X, Y) in enumerate((("sack", -1500.0, 330.0), ("sack", -1460.0, 360.0), ("bucket_water", -2050.0, -560.0), ("wheelbarrow", -2900.0, 250.0),
										("barrel", -3150.0, -1050.0), ("barrel", -3060.0, -1080.0), ("crate_A_big", -1500.0, -1180.0), ("sack", -2750.0, -1220.0))):
		KK_(f"Village_{Id}_{Index}", Id, X, Y, Rng.uniform(0, 360), KS, Collide=Id != "sack")
	# 광장 벤치 (우물을 봄) + 화분·꽃
	for Index, (DX, DY) in enumerate(((-520.0, 200.0), (520.0, 220.0))):
		PH_(f"Plaza_Bench_{Index}", "painted_wooden_bench", CX + DX, CY + DY, -90.0, 1.0, Collide=True)  # 앉는 쪽이 카메라(+Y)
	for Index, (X, Y) in enumerate(((-3150.0, -1150.0), (-2650.0, -1300.0), (-1950.0, -1300.0), (-1500.0, -1150.0), (-3720.0, 520.0))):
		PH_(f"Planter_{Index}", "planter_box_01", X, Y, 0.0, 1.0, Collide=True)
		for F in range(3):
			PH_(f"Planter_{Index}_Flower_{F}", "flower_gazania", X + (F - 1) * 35.0, Y, Rng.uniform(0, 360), 1.6, Z=Height(X, Y) + 32.0)
	# 깃발 기둥
	for Index, (X, Y) in enumerate(((-2950.0, -800.0), (-1850.0, -820.0))):
		KK_(f"Flag_{Index}", "flag_blue", X, Y, Face, KS * 1.6)
	# 등불 기둥: 광장 둘레 + 마을 출구
	for X, Y in ((-3150.0, 350.0), (-1600.0, -850.0), (-3100.0, -850.0), (-1750.0, 600.0), (-650.0, 300.0), (-650.0, -250.0)):
		LanternPost(X, Y, Shadows=(X, Y) == (-1750.0, 600.0))
	# 울타리: 마을 동쪽 경계(가운데 = 길 문) + 카메라 쪽 앞줄 일부
	#   울타리 조각은 육각 말판 가장자리에 놓여 있다: 로컬 X 방향으로 길고 원점에서 -Y로 100 유닛 떨어짐 → Yaw 90이면 Y 방향 줄, +X로 밀림
	FS = KS * 0.5
	FenceLen = 116.0 * FS
	for Index in range(-3, 4):
		Y = 60.0 + Index * FenceLen
		if Index == 0:
			KK_("Fence_Gate", "fence_wood_straight_gate", -480.0 - 100.0 * FS, Y, 90.0, FS)
			continue
		KK_(f"Fence_East_{Index}", "fence_wood_straight", -480.0 - 100.0 * FS, Y, 90.0, FS, Collide=True, Shrink=1.0)
	for Index in range(4):
		X = -3500.0 + Index * FenceLen + (900.0 if Index >= 2 else 0.0)
		KK_(f"Fence_Front_{Index}", "fence_wood_straight", X, 1150.0 + 100.0 * FS, 0.0, FS)
	# 마을 둘레 나무 (안쪽·서쪽은 크게, 카메라 쪽은 작은 덤불만)
	for Index, (Id, X, Y, Scale) in enumerate((("trees_A_large", -4300.0, -1700.0, KS * 1.2), ("tree_single_B", -2950.0, -1700.0, KS),
												("tree_single_A", -1750.0, -1800.0, KS * 1.1), ("trees_B_medium", -600.0, -1700.0, KS * 1.1),
												("tree_single_A", -4350.0, 300.0, KS), ("trees_A_medium", -4500.0, -800.0, KS * 1.2))):
		KK_(f"VillageTree_{Index}", Id, X, Y, Rng.uniform(0, 360), Scale, Collide=Id.startswith("tree_single"), Shrink=0.25)
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

	# ---- 들판 (동쪽) ----------------------------------------------------------------------------------------------
	# 방앗간 언덕 + 밀밭 + 수레
	MillX, MillY = 4000.0, -1650.0
	KK_("Windmill", "building_windmill_green", MillX, MillY, Face - 20.0, KS * 1.25, Collide=True)
	KK_("Mill_Sacks", "sack", MillX + 350.0, MillY + 380.0, 30.0, KS)
	KK_("Mill_Cart", "wheelbarrow", MillX - 380.0, MillY + 450.0, 60.0, KS, Collide=True)
	Point("Mill_Lantern", (MillX + 150.0, MillY + 330.0, Height(MillX, MillY + 330) + 220.0), (1.0, 0.62, 0.3), 4.0, 600.0, Flicker={"Style": "Fire", "Seed": 90})
	# 모험가 야영지: 천막 둘 + 모닥불(불꽃·불빛 그림자) + 통나무 의자
	CampX, CampY = 1300.0, 820.0
	CZ = Height(CampX, CampY)
	PH_("Camp_FirePit", "stone_fire_pit", CampX, CampY, 15.0, 1.0, Sink=10.0)
	Particles("Camp_Fire", "Particles/Demo/CampfireFire.eparticle", (CampX, CampY, CZ + 2.0))
	Point("Camp_Light", (CampX, CampY, CZ + 80.0), (1.0, 0.52, 0.2), 18.0, 1300.0, True, Flicker={"Style": "Fire", "Seed": 4})
	S.Add("Camp_Sound", {"AudioSourceComponent": {"ClipAsset": "Audio/Demo/CampfireCrackle.wav", "Volume": 0.8, "Pitch": 1.0, "Loop": True,
		"PlayOnStart": True, "Spatial": True, "MinDistance": 200.0, "MaxDistance": 2500.0}}, (CampX, CampY, CZ + 40.0))
	BoxCollider("Camp_FirePit_Collision", (CampX, CampY, CZ + 20.0), (70.0, 70.0, 40.0))
	Reserve(CampX, CampY, 160.0)
	KK_("Camp_Tent_A", "tent", CampX - 420.0, CampY - 80.0, Face + 30.0, KS * 1.3, Collide=True, Shrink=0.7)
	KK_("Camp_Tent_B", "tent", CampX + 440.0, CampY - 140.0, Face - 35.0, KS * 1.2, Collide=True, Shrink=0.7)
	PH_("Camp_Log", "dead_tree_trunk", CampX + 30.0, CampY - 230.0, 92.0, 0.9, Sink=3.0, Collide=True)
	PH_("Camp_Crate", "wooden_crate_02", CampX - 260.0, CampY + 200.0, 30.0, 1.0, Collide=True)
	PH_("Camp_Bucket", "wooden_bucket_01", CampX + 230.0, CampY + 170.0, 0.0, 1.0)
	# 연못가: 자카란다(보라 꽃나무) + 바위 + 덤불 + 꽃
	PH_("Pond_Tree", "jacaranda_tree", PX - 900.0, PY - 450.0, 30.0, 0.55, Sink=10.0)
	BoxCollider("Pond_Tree_Collision", (PX - 900.0, PY - 450.0, Height(PX - 900, PY - 450) + 150.0), (40.0, 40.0, 150.0))
	Reserve(PX - 900.0, PY - 450.0, 160.0)
	for Index, (Id, A, Scale) in enumerate((("rock_moss_set_01", 20.0, 1.0), ("boulder_01", 160.0, 0.6), ("rock_07", 220.0, 1.0),
											 ("rock_moss_set_02", 300.0, 0.9), ("coast_rocks_05", 95.0, 0.35))):
		R = math.radians(A)
		X, Y = PX + math.cos(R) * (PRX + 60.0), PY + math.sin(R) * (PRY + 50.0)
		PH_(f"Pond_Rock_{Index}", Id, X, Y, Rng.uniform(0, 360), Scale, Sink=12.0, Collide=Id in ("boulder_01", "coast_rocks_05"), Shrink=0.6)
	for Index in range(14):
		A = Rng.uniform(0, math.tau)
		X, Y = PX + math.cos(A) * (PRX + Rng.uniform(120.0, 300.0)), PY + math.sin(A) * (PRY + Rng.uniform(100.0, 260.0))
		if Free(X, Y, 50.0):
			PH_(f"Pond_Flower_{Index}", "flower_gazania", X, Y, Rng.uniform(0, 360), Rng.uniform(1.6, 2.2), Sink=1.0)
	PH_("Pond_Fern_0", "fern_02", PX + 650.0, PY - 380.0, 40.0, 1.0, Sink=4.0)
	PH_("Pond_Fern_1", "fern_02", PX - 700.0, PY + 300.0, 110.0, 0.9, Sink=4.0)
	# 길가 등불 + 이정표 대신 통·상자
	for X, Y in ((700.0, -60.0), (2100.0, 450.0), (3300.0, 0.0), (4400.0, 600.0), (3550.0, -950.0)):
		LanternPost(X, Y)
	# 들판 나무·바위·덤불 (길·연못·야영지·소환 지점은 비움)
	for X, Y in SPAWN_POINTS:
		Reserve(X, Y, 150.0)
	Trees = [("tree_single_A", 600.0, -1500.0, 1.1), ("tree_single_B", 1900.0, -1700.0, 1.0), ("trees_A_medium", 5100.0, -1400.0, 1.2),
			 ("tree_single_A", 5000.0, 900.0, 1.0), ("tree_single_B", 2700.0, 1150.0, 0.9), ("trees_B_large", 5600.0, -300.0, 1.3),
			 ("tree_single_A", -200.0, 1000.0, 0.85), ("tree_single_B", 3900.0, 1150.0, 0.9), ("trees_A_large", 1300.0, -2300.0, 1.4),
			 ("trees_B_medium", 2400.0, -2300.0, 1.4), ("trees_A_medium", 4800.0, -2300.0, 1.5), ("tree_single_A", -350.0, -1300.0, 1.0)]
	for Index, (Id, X, Y, Scale) in enumerate(Trees):
		KK_(f"FieldTree_{Index}", Id, X, Y, Rng.uniform(0, 360), KS * Scale, Collide=Id.startswith("tree_single"), Shrink=0.25)
	for Index in range(14):
		for _ in range(40):
			X, Y = Rng.uniform(0.0, 5200.0), Rng.uniform(-1800.0, 1250.0)
			if Free(X, Y, 120.0) and SegmentDistance(np.array(X), np.array(Y), PATH) > 220.0:
				break
		KK_(f"FieldRock_{Index}", f"rock_single_{'ABCD'[Index % 4]}", X, Y, Rng.uniform(0, 360), KS * Rng.uniform(0.9, 1.6), Sink=3.0,
			Collide=Index % 4 in (2, 3), Shrink=0.7)
		Reserve(X, Y, 100.0)
	for Index in range(22):
		for _ in range(40):
			X, Y = Rng.uniform(-300.0, 5300.0), Rng.uniform(-1900.0, 1300.0)
			if Free(X, Y, 90.0) and SegmentDistance(np.array(X), np.array(Y), PATH) > 200.0:
				break
		Id = Rng.choice(["shrub_02", "shrub_03", "shrub_04", "wild_rooibos_bush", "flower_gazania"])
		PH_(f"FieldBush_{Index}", Id, X, Y, Rng.uniform(0, 360), Rng.uniform(1.8, 2.6) if Id == "flower_gazania" else Rng.uniform(0.5, 0.9), Sink=4.0)
		Reserve(X, Y, 70.0)

	# ---- 먼 배경: 안쪽 산·언덕 (카메라 위쪽 화면을 채움)
	for Index, (Id, X, Y, Scale, Yaw) in enumerate((("mountain_B_grass_trees", -3000.0, -6800.0, 22.0, 20.0), ("mountain_A_grass_trees", 1500.0, -7500.0, 26.0, 70.0),
													 ("mountain_B_grass_trees", 6000.0, -6800.0, 21.0, 140.0), ("hills_A_trees", -500.0, -4300.0, 9.0, 0.0),
													 ("hills_A_trees", 3500.0, -4200.0, 8.0, 100.0), ("hills_A_trees", -5200.0, -3800.0, 9.0, 200.0),
													 ("trees_A_large", -1600.0, -3000.0, 7.0, 30.0), ("trees_B_large", 1200.0, -3100.0, 7.0, 80.0),
													 ("trees_A_large", 4300.0, -3000.0, 7.0, 150.0), ("trees_B_large", 6800.0, -2400.0, 7.0, 10.0))):
		KK_(f"Backdrop_{Index}", Id, X, Y, Yaw, Scale, Sink=40.0)

	# ---- 풀 폴리지 (풀 레이어 위만, 물체·길·광장·연못 피함)
	GrassRng = np.random.default_rng(17)
	Dry = []
	P = GrassRng.uniform((-6000.0, -3600.0), (7000.0, 2600.0), size=(150000, 2))
	Keep = (Height.GrassWeight(P[:, 0], P[:, 1]) >= 0.75) & (GrassRng.random(len(P)) < 0.55)
	Keep &= np.hypot(P[:, 0] - Start[0], P[:, 1] - Start[1]) >= 120.0
	for OX, OY, OR in Occupied:
		Keep &= (P[:, 0] - OX) ** 2 + (P[:, 1] - OY) ** 2 >= (OR + 25.0) ** 2
	for X, Y in P[Keep]:
		Z = Height(X, Y)
		N = Height.Normal(X, Y)
		Item = (float(X), float(Y), Z, float(GrassRng.uniform(0, 360)), float(GrassRng.uniform(0.45, 0.85)), float(N[0]), float(N[1]), float(N[2]))
		(Dry if GrassRng.random() < 0.18 else Grass).append(Item)

	# ---- 게임: 관리자 + 카메라 + 플레이어
	S.Add("HD2DGame", {"ScriptComponent": {"ScriptAsset": "Scripts/Demo/HD2D/HD2DGame.lua", "ExecutionLocation": 0,
		"PropertyOverrides": json.dumps({
			"SpawnPoints": ";".join(f"{X:.0f},{Y:.0f},{Height(X, Y) + SLIME_RADIUS + SLIME_HALF + 4.0:.0f}" for X, Y in SPAWN_POINTS),
			"SlimeCount": 7, "AutoPlay": AutoPlay}, ensure_ascii=False)}})
	StartZ = Height(*Start) + PLAYER_RADIUS + PLAYER_HALF + 4.0
	Forward = (0.0, -math.cos(math.radians(-CAMERA_PITCH)), -math.sin(math.radians(-CAMERA_PITCH)))
	Focus = (Start[0], Start[1], StartZ - 85.0 + 70.0)
	S.Add("Camera", {"CameraComponent": {"FovYDegrees": CAMERA_FOV, "NearZ": 50.0, "FarZ": 60000.0, "Primary": True, "Priority": 10}},
		  tuple(Focus[I] - Forward[I] * CAMERA_DISTANCE for I in range(3)), QuatFromEuler(Pitch=CAMERA_PITCH, Yaw=-90.0))
	PlayerIndex = len(S.Entities)
	S.Add("Player", {
		"PrefabInstanceComponent": {"Asset": f"{PREFABS}/Player.eprefab", "Overrides": ""},
		"PrefabLinkComponent": {"Id": "1", "Root": PlayerIndex}},
		(Start[0], Start[1], StartZ))
	return S, Grass, Dry


def Main():
	bViews = "--views" in sys.argv
	HD2DArt.WriteAll(os.path.join(CONTENT, "Sprites", "HD2D"))
	X, Y, H = BuildHeights()
	Weights, _, Stack = BuildWeights(X, Y, H)
	WriteTerrain(os.path.join(CONTENT, "Terrain", "Demo", "HD2D.eterrain"), H, Weights)
	Sampler = FHeightSampler(H, Stack)
	WriteMaterials()
	WritePrefabs()
	Scene, Grass, Dry = BuildScene(Sampler)
	WriteFoliage(os.path.join(CONTENT, "Foliage", "Demo", "HD2D.efoliage"), [(GRASS_TYPE, Grass), (GRASS_DRY_TYPE, Dry)])
	Scene.Save(os.path.join(CONTENT, "Scenes", "Demo", "HD2D.escene"))
	print(f"HD2D 생성: 엔티티 {len(Scene.Entities)}개, 풀 {len(Grass) + len(Dry)}개, 지형 {TERRAIN_RES}² 높이 {H.min():.0f}~{H.max():.0f}cm")
	if bViews:
		for Name, Start in VIEW_STARTS.items():
			Variant, _, _ = BuildScene(Sampler, Start)
			Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_HD2D_{Name}.escene"))
		Variant, _, _ = BuildScene(Sampler, VIEW_STARTS["Field"], AutoPlay=True)
		Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", "_HD2DAutoPlay.escene"))
		print("확인용 변형: Scenes/Demo/_HD2D_*.escene, _HD2DAutoPlay.escene (커밋하지 않음)")


if __name__ == "__main__":
	Main()
