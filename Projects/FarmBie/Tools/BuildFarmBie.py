# FarmBie 콘텐츠 생성기 — 씬·프리팹·도트 아트·지형·머티리얼은 모두 이 스크립트의 생성물이다 (손으로 고치지 않고 여기를 고쳐 다시 만든다).
#   실행: python Projects/FarmBie/Tools/BuildFarmBie.py   (결정적 — 고정 시드)
#   만드는 것: Sprites/FarmBie/(FarmBieArt), Textures/FarmBie/(바닥), Materials/FarmBie/, Terrain/FarmBie/Farm.eterrain, Prefabs/FarmBie/,
#             Scenes/Farm.escene(기본 맵), Scenes/Tests/FarmAutoPlay.escene(자동 검증)
#   좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 카메라는 +Y 쪽 위에서 -Y를 거의 수직(피치 CAMERA_PITCH)으로 내려다본다 → 화면 오른쪽 = +X, 화면 위 = -Y.
#   농장 격자: 칸 TILE cm, 가로 GRID_W × 세로 GRID_H, 원점(왼쪽 위 모서리) = GRID_ORIGIN — 밭·건물·벽·좀비 흐름장이 모두 이 격자를 쓴다
#   집은 화면 위(-Y), 남쪽(+Y) 입구 길에 보부상 자리, 서쪽 입구는 숲으로 가는 길. 울타리 틈(입구)은 좀비 진입로이기도 하다.
import json
import math
import os
import random
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "Tools", "DemoMap"))
sys.path.insert(0, HERE)
import FarmBieArt  # noqa: E402
import ModelBounds  # noqa: E402
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402

CONTENT = os.path.join(ROOT, "Projects", "FarmBie", "Content")
KK      = "Asset/KayKit/Village"
KS      = 4.5   # KayKit 마을 키트 배율 (1 유닛 = 1m 말판 → 도트 캐릭터 약 180cm에 맞춘 크기)
SPRITES = "Sprites/FarmBie"
PREFABS = "Prefabs/FarmBie"
MATS    = "Materials/FarmBie"
SCRIPTS = "Scripts/FarmBie"

# ---- 격자·맵 -------------------------------------------------------------------------------------------------------------
TILE        = 100.0
GRID_W      = 52
GRID_H      = 40
GRID_ORIGIN = (-GRID_W * TILE * 0.5, -GRID_H * TILE * 0.5)  # (-2600, -2000)
PLAY_MIN    = (GRID_ORIGIN[0], GRID_ORIGIN[1])
PLAY_MAX    = (-GRID_ORIGIN[0], -GRID_ORIGIN[1])
FENCE_INSET = 150.0                         # 울타리는 격자 가장자리 안쪽
HOUSE       = (0.0, -1350.0)
ENTRANCES   = {"South": (0.0, PLAY_MAX[1]), "West": (PLAY_MIN[0], 200.0), "East": (PLAY_MAX[0], -300.0), "North": (-1400.0, PLAY_MIN[1])}
ENTRANCE_W  = 400.0
PATHS = [
	[(0.0, -1000.0), (0.0, 0.0), (60.0, 900.0), (0.0, PLAY_MAX[1] + 600.0)],         # 집 → 남쪽 입구(보부상)
	[(0.0, -700.0), (-1200.0, -700.0), (-2000.0, 150.0), (PLAY_MIN[0] - 600.0, 200.0)],  # 집 앞 → 서쪽 숲길
	[(0.0, -700.0), (1700.0, -650.0), (PLAY_MAX[0] + 600.0, -300.0)],                # 동쪽
]
PLAYER_START = (0.0, -700.0)

# ---- 지형 ----------------------------------------------------------------------------------------------------------------
TERRAIN_SIZE = 12000.0
TERRAIN_RES  = 241      # 칸 50cm
HEIGHT_RANGE = 4000.0

# ---- 카메라 (거의 수직 탑뷰 HD-2D) -----------------------------------------------------------------------------------------
CAMERA_PITCH    = -70.0
CAMERA_FOV      = 30.0
CAMERA_DISTANCE = 2000.0

# ---- 플레이어 캡슐 -------------------------------------------------------------------------------------------------------
PLAYER_RADIUS = 30.0
PLAYER_HALF   = 55.0


def WriteJson(Path, Doc):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")


def Smoothstep(E0, E1, X):
	T = np.clip((X - E0) / (E1 - E0), 0.0, 1.0)
	return T * T * (3.0 - 2.0 * T)


def SegmentDistance(X, Y, Points):
	D = np.full(X.shape, np.inf)
	for (AX, AY), (BX, BY) in zip(Points[:-1], Points[1:]):
		VX, VY = BX - AX, BY - AY
		L2 = VX * VX + VY * VY
		T = np.clip(((X - AX) * VX + (Y - AY) * VY) / L2, 0.0, 1.0)
		D = np.minimum(D, np.hypot(X - (AX + VX * T), Y - (AY + VY * T)))
	return D


def Noise2(X, Y, Scale, Seed):
	# 값 노이즈 (부드러운 보간) — 지형 굴곡·경계 흔들림용
	Rng = np.random.default_rng(Seed)
	G = Rng.random((64, 64))
	FX, FY = X / Scale, Y / Scale
	IX, IY = np.floor(FX).astype(int), np.floor(FY).astype(int)
	TX, TY = FX - IX, FY - IY
	TX, TY = TX * TX * (3 - 2 * TX), TY * TY * (3 - 2 * TY)
	A, B = G[IY % 64, IX % 64], G[IY % 64, (IX + 1) % 64]
	C, D = G[(IY + 1) % 64, IX % 64], G[(IY + 1) % 64, (IX + 1) % 64]
	return (A * (1 - TX) + B * TX) * (1 - TY) + (C * (1 - TX) + D * TX) * TY


def BuildTerrain():
	Axis = np.linspace(-TERRAIN_SIZE * 0.5, TERRAIN_SIZE * 0.5, TERRAIN_RES)
	X, Y = np.meshgrid(Axis, Axis)
	# 놀이 영역(울타리 안팎)은 평평, 바깥은 언덕으로 솟아 화면 끝을 막는다
	OutX = np.maximum(0.0, np.abs(X) - (PLAY_MAX[0] + 300.0))
	OutY = np.maximum(0.0, np.abs(Y) - (PLAY_MAX[1] + 300.0))
	Out = np.hypot(OutX, OutY)
	PathD = np.min(np.stack([SegmentDistance(X, Y, P) for P in PATHS]), axis=0)
	Valley = Smoothstep(500.0, 250.0, PathD)  # 길이 언덕을 뚫고 나가는 골짜기
	H = Smoothstep(0.0, 1800.0, Out) * (420.0 + Noise2(X, Y, 900.0, 3) * 380.0) * (1.0 - Valley * 0.8)
	H += (Noise2(X, Y, 450.0, 5) - 0.5) * 12.0  # 놀이 영역 안 아주 약한 굴곡
	# 레이어: 0 풀, 1 흙길, 2 밭 흙, 3 돌(집 앞 마당)
	N = Noise2(X, Y, 160.0, 9)
	Path = Smoothstep(100.0 + N * 30.0, 60.0 + N * 30.0, PathD)
	# 밭은 괭이로 간 칸만 흙(타일 스프라이트 — F2). 지형 레이어 2 = 마당 맨흙(집 앞 넓게 + 풀 사이 얼룩), 3 = 우물가 돌
	Yard = Smoothstep(1.05, 0.8, np.hypot((X - HOUSE[0]) / 650.0, (Y - HOUSE[1] - 250.0) / 380.0) + N * 0.15)
	Patch = Smoothstep(0.72, 0.8, Noise2(X, Y, 380.0, 13)) * 0.7
	Soil = np.clip(np.maximum(Yard, Patch) * (1.0 - Path), 0.0, 1.0)
	Stone = Smoothstep(260.0, 180.0, np.hypot(X + 950.0, Y + 1250.0)) * (1.0 - Path) * (1.0 - Soil)
	Grass = np.clip(1.0 - Path - Soil - Stone, 0.0, 1.0)
	Stack = np.stack([Grass, Path, Soil, Stone], axis=-1)
	Stack /= np.maximum(Stack.sum(axis=-1, keepdims=True), 1e-6)
	Bytes = np.floor(Stack * 255.0 + 0.5).astype(np.int32)
	Bytes[..., 0] += 255 - Bytes.sum(axis=-1)
	Bytes = np.clip(Bytes, 0, 255).astype(np.uint32)
	Weights = Bytes[..., 0] | (Bytes[..., 1] << 8) | (Bytes[..., 2] << 16) | (Bytes[..., 3] << 24)
	return X, Y, H, Weights


def WriteTerrain(Path, H, Weights):
	import base64
	H16 = np.clip(np.round((H / HEIGHT_RANGE + 0.5) * 65535.0), 0, 65535).astype("<u2")
	Doc = {"Heights": base64.b64encode(H16.tobytes()).decode("ascii"), "Resolution": TERRAIN_RES, "Version": 1,
		   "Weights": base64.b64encode(Weights.astype("<u4").tobytes()).decode("ascii")}
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent="\t")
		File.write("\n")


class FHeight:
	def __init__(self, H):
		self.H = H
		self.Cell = TERRAIN_SIZE / (TERRAIN_RES - 1)

	def __call__(self, PX, PY):
		FX = (PX + TERRAIN_SIZE * 0.5) / self.Cell
		FY = (PY + TERRAIN_SIZE * 0.5) / self.Cell
		IX = int(np.clip(math.floor(FX), 0, TERRAIN_RES - 2))
		IY = int(np.clip(math.floor(FY), 0, TERRAIN_RES - 2))
		TX, TY = FX - IX, FY - IY
		H = self.H
		Top = H[IY, IX] * (1 - TX) + H[IY, IX + 1] * TX
		Bot = H[IY + 1, IX] * (1 - TX) + H[IY + 1, IX + 1] * TX
		return float(Top * (1 - TY) + Bot * TY)


# ---- 머티리얼 ------------------------------------------------------------------------------------------------------------
def Material(Name, Base=(1.0, 1.0, 1.0, 1.0), Rough=0.95, Texture=""):
	return {"Name": Name, "BaseColorFactor": list(Base), "EmissiveFactor": [0.0, 0.0, 0.0], "Metallic": 0.0, "Roughness": Rough,
			"NormalScale": 1.0, "OcclusionStrength": 1.0, "BaseColorTexture": Texture, "MetallicRoughnessTexture": "", "NormalTexture": "",
			"OcclusionTexture": "", "EmissiveTexture": ""}


def WriteMaterials():
	FarmBieArt.WriteGroundTextures(os.path.join(CONTENT, "Textures", "FarmBie"))
	for Name in FarmBieArt.GROUND_TEXTURES:
		WriteJson(os.path.join(CONTENT, MATS, f"Ground{Name}.emat"), Material(f"FarmBieGround{Name}", Texture=f"../../Textures/FarmBie/{Name}.png"))


# ---- 프리팹 --------------------------------------------------------------------------------------------------------------
def Link(Id):
	return {"Id": str(Id), "Root": -1}


def Transform(Position=(0.0, 0.0, 0.0), Rotation=None, Scale=(1.0, 1.0, 1.0)):
	return {"Position": [float(V) for V in Position], "Rotation": Rotation or [0.0, 0.0, 0.0, 1.0], "Scale": [float(V) for V in Scale]}


def Sprite(Asset, Slice="", Lit=True, Shadows=True, Blend=3, Visible=True, Cutoff=0.5, Color=(1, 1, 1, 1), Billboard=0):
	# Blend: 0 알파, 1 프리멀티플라이드, 2 가산, 3 마스크 / Billboard: 0 없음, 1 전체(거의 수직 탑뷰 — 몸을 카메라 쪽으로 눕힘), 2 세로축
	return {"Sprite": Asset, "Slice": Slice, "Color": list(Color), "FlipX": False, "FlipY": False, "SortingLayer": "", "OrderInLayer": 0,
			"Lit": Lit, "CastShadows": Shadows, "Size": [0.0, 0.0], "Visible": Visible, "Blend": Blend, "AlphaCutoff": Cutoff, "SliceMode": 0,
			"Billboard": Billboard}


def Flipbook(Path):
	return {"Flipbook": Path, "Speed": 1.0, "Playing": True, "StartTime": 0.0}


FLAT = [math.sin(math.radians(-45.0)), 0.0, 0.0, math.cos(math.radians(-45.0))]  # 스프라이트 평면(XZ)을 바닥(XY)에 눕힘 (Roll -90)


def Mover(Radius, Half, Speed, Mass, **Extra):
	M = {"AirControl": 1.0, "CapsuleHalfHeight": Half, "CapsuleRadius": Radius, "FaceControlYaw": False, "GravityScale": 1.0, "JumpZVelocity": 0.0,
		 "Mass": Mass, "MaxSlopeAngle": 50.0, "MaxStepHeight": 25.0, "MaxWalkSpeed": Speed, "PushForce": 500.0, "KnockbackDeceleration": 2400.0,
		 "ClientPrediction": False}
	M.update(Extra)
	return M


def Script(Path, Location=0, **Props):
	return {"ExecutionLocation": Location, "ScriptAsset": Path, "PropertyOverrides": json.dumps(Props, ensure_ascii=False)}


def WritePrefab(Name, Entities):
	WriteJson(os.path.join(CONTENT, *PREFABS.split("/"), f"{Name}.eprefab"), {"Entities": Entities, "NextId": len(Entities) + 1, "Version": 1})


def WritePrefabs():
	Foot = -(PLAYER_RADIUS + PLAYER_HALF)
	WritePrefab("Player", [
		{"Name": "Player", "Parent": -1, "Components": {
			"CharacterMovementComponent": Mover(PLAYER_RADIUS, PLAYER_HALF, 420.0, 70.0, MaxStepHeight=30.0, KnockbackDeceleration=1800.0),
			"ScriptComponent": Script(f"{SCRIPTS}/FarmPlayer.lua", 2, CameraDistance=CAMERA_DISTANCE,
									  MinX=PLAY_MIN[0] + 900.0, MaxX=PLAY_MAX[0] - 900.0, MinY=PLAY_MIN[1] + 300.0, MaxY=PLAY_MAX[1] - 500.0),
			"PrefabLinkComponent": Link(1), "TransformComponent": Transform((0, 0, 100))}},
		{"Name": "Visual", "Parent": 0, "Components": {"PrefabLinkComponent": Link(2), "TransformComponent": Transform()}},
		{"Name": "Body", "Parent": 1, "Components": {
			"SpriteComponent": Sprite(f"{SPRITES}/Farmer.esprite", "IdleDown0", Billboard=1),
			"FlipbookComponent": Flipbook(f"{SPRITES}/Farmer_IdleDown.eflipbook"),
			"PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, Foot))}},
		{"Name": "Shadow", "Parent": 1, "Components": {
			"SpriteComponent": Sprite(f"{SPRITES}/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
			"PrefabLinkComponent": Link(4), "TransformComponent": Transform((0, 0, Foot + 1.5), FLAT, (0.75, 1.0, 0.75))}},
	])


# ---- 씬 ------------------------------------------------------------------------------------------------------------------
class FBuilder:
	def __init__(self, Height):
		self.S = FScene()
		self.Height = Height
		self.Rng = random.Random(7)
		self.Taken = []  # 점유 원 (X, Y, R) — 나무·소품이 겹치지 않게
		self.BoundsCache = {}
		ModelBounds._CONVERT = lambda P: (-P[2] * 100.0, P[0] * 100.0, P[1] * 100.0)

	def Free(self, X, Y, R):
		return all(math.hypot(X - TX, Y - TY) > R + TR for TX, TY, TR in self.Taken)

	def Reserve(self, X, Y, R):
		self.Taken.append((X, Y, R))

	def Bounds(self, Asset):
		if Asset not in self.BoundsCache:
			self.BoundsCache[Asset] = ModelBounds.ComputeBounds(os.path.join(CONTENT, *Asset.split("/")))
		return self.BoundsCache[Asset]

	def BoxCollider(self, Name, Center, Half, Yaw=0.0):
		return self.S.Add(Name, {"BoxColliderComponent": {"HalfExtents": [float(V) for V in Half]}}, Center, QuatFromEuler(Yaw=Yaw))

	def Place(self, Name, Id, X, Y, Yaw=0.0, Scale=KS, Collide=False, Shrink=0.85, Sink=0.0):
		# KayKit 모델 + (선택) 경계 상자 콜라이더 (모델 로컬 경계를 배율·Yaw로 돌려 놓음)
		Asset = f"{KK}/{Id}.gltf"
		Base = self.Height(X, Y) - Sink
		Index = self.S.Add(Name, {"ModelComponent": {"AssetPath": Asset}}, (X, Y, Base), QuatFromEuler(Yaw=Yaw), (Scale, Scale, Scale))
		Lo, Hi = self.Bounds(Asset)
		CX, CY, CZ = [(Lo[I] + Hi[I]) * 0.5 * Scale for I in range(3)]
		HX, HY, HZ = [(Hi[I] - Lo[I]) * 0.5 * Scale for I in range(3)]
		R = math.radians(Yaw)
		WX, WY = X + CX * math.cos(R) - CY * math.sin(R), Y + CX * math.sin(R) + CY * math.cos(R)
		if Collide:
			self.BoxCollider(f"{Name}_Collision", (WX, WY, Base + CZ), (HX * Shrink, HY * Shrink, max(HZ, 60.0)), Yaw)
		self.Reserve(WX, WY, max(HX, HY) * 0.9)
		return Index


def InEntrance(X, Y, Pad=0.0):
	for EX, EY in ENTRANCES.values():
		if abs(EY) == PLAY_MAX[1] and abs(X - EX) < ENTRANCE_W * 0.5 + Pad and abs(Y - EY) < 900.0:
			return True
		if abs(EX) == PLAY_MAX[0] and abs(Y - EY) < ENTRANCE_W * 0.5 + Pad and abs(X - EX) < 900.0:
			return True
	return False


def AddFence(B):
	# 울타리 둘레 (입구 틈 제외). fence_wood_straight 모델은 로컬 -Y로 1.0 유닛 치우친 말판 가장자리 조각 → 배율 FS로 길이 FS×115cm
	FS = 2.4
	Lo, Hi = B.Bounds(f"{KK}/fence_wood_straight.gltf")
	Length = (Hi[0] - Lo[0]) * FS * 0.98
	OffsetY = (Lo[1] + Hi[1]) * 0.5 * FS  # 조각 가운데까지 로컬 Y 치우침
	X0, Y0 = PLAY_MIN[0] + FENCE_INSET, PLAY_MIN[1] + FENCE_INSET
	X1, Y1 = PLAY_MAX[0] - FENCE_INSET, PLAY_MAX[1] - FENCE_INSET
	Sides = [((X0, Y0), (X1, Y0)), ((X1, Y0), (X1, Y1)), ((X1, Y1), (X0, Y1)), ((X0, Y1), (X0, Y0))]
	Count = 0
	for (AX, AY), (BX, BY) in Sides:
		Len = math.hypot(BX - AX, BY - AY)
		Steps = int(Len // Length)
		Yaw = math.degrees(math.atan2(BY - AY, BX - AX))
		for I in range(Steps):
			T = (I + 0.5) / Steps
			CX, CY = AX + (BX - AX) * T, AY + (BY - AY) * T
			if InEntrance(CX, CY, Length * 0.4):
				continue
			# 조각 가운데가 (CX, CY)에 오도록 로컬 Y 치우침을 되돌림
			R = math.radians(Yaw)
			PX, PY = CX + OffsetY * math.sin(R), CY - OffsetY * math.cos(R)
			B.S.Add(f"Fence_{Count}", {"ModelComponent": {"AssetPath": f"{KK}/fence_wood_straight.gltf"}},
					(PX, PY, B.Height(CX, CY)), QuatFromEuler(Yaw=Yaw), (FS, FS, FS))
			B.BoxCollider(f"Fence_{Count}_Collision", (CX, CY, B.Height(CX, CY) + 70.0), (Length * 0.5, 12.0, 70.0), Yaw)
			Count += 1


def AddTrees(B):
	# 울타리 바깥 숲 띠 + 언덕 (화면 가장자리 채움) — 입구 길목은 비움
	Kinds = ["trees_A_large", "trees_B_large", "trees_A_medium", "trees_B_medium", "tree_single_A", "tree_single_B"]
	Rng = B.Rng
	Count = 0
	for _ in range(2600):
		X = Rng.uniform(-5400.0, 5400.0)
		Y = Rng.uniform(-4800.0, 4800.0)
		Inside = abs(X) < PLAY_MAX[0] - 40.0 and abs(Y) < PLAY_MAX[1] - 40.0
		if Inside or InEntrance(X, Y, 250.0) or not B.Free(X, Y, 180.0):
			continue
		PathD = min(float(SegmentDistance(np.array([X]), np.array([Y]), P)[0]) for P in PATHS)
		if PathD < 320.0:
			continue
		Kind = Rng.choice(Kinds)
		Scale = KS * Rng.uniform(0.85, 1.2)
		B.Place(f"Tree_{Count}", Kind, X, Y, Rng.uniform(0, 360), Scale, Sink=10.0)
		Count += 1
	# 마당 안 그늘 나무 몇 그루 (충돌 있음)
	for Index, (X, Y) in enumerate([(-2050.0, -1500.0), (2100.0, -1550.0), (-2150.0, 1450.0), (2200.0, 1500.0), (1250.0, -1500.0)]):
		B.Place(f"YardTree_{Index}", "tree_single_B" if Index % 2 else "tree_single_A", X, Y, Index * 70.0, KS * 1.1, Collide=True, Shrink=0.3)


def AddFarmstead(B):
	X, Y = HOUSE
	B.Place("House", "building_home_A_blue", X, Y, -90.0, KS, Collide=True, Shrink=0.95)
	B.Place("Well", "building_well_blue", -950.0, -1250.0, -90.0, KS * 0.7, Collide=True)
	B.Place("Barn", "building_home_B_red", 1150.0, -1350.0, -90.0, KS * 0.9, Collide=True, Shrink=0.95)
	B.Place("ShippingBin", "crate_A_big", 380.0, -950.0, 10.0, KS * 1.6, Collide=True)
	B.Place("Lumber", "resource_lumber", -500.0, -1100.0, 90.0, KS * 0.9, Collide=True)
	B.Place("Barrel_0", "barrel", 330.0, -1080.0, 0.0, KS)
	B.Place("Barrel_1", "barrel", 440.0, -1110.0, 30.0, KS)
	B.Place("Wheelbarrow", "wheelbarrow", -1300.0, -800.0, 30.0, KS * 0.9)
	B.Place("Bucket", "bucket_water", -820.0, -1050.0, 0.0, KS)
	B.Place("Sack_0", "sack", 1500.0, -1050.0, 20.0, KS)
	B.Place("Sack_1", "sack", 1570.0, -1000.0, 80.0, KS)
	# 남쪽 입구 보부상 천막 자리 (F3에서 보부상이 온다)
	B.Place("MerchantTent", "tent", 700.0, 1650.0, -90.0, KS * 1.3, Collide=True)
	B.Place("MerchantCrate", "crate_B_small", 450.0, 1720.0, 15.0, KS * 1.4)


def AddEnvironment(B, TimeOfDay):
	S = B.S
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.95, 0.86], "Intensity": 4.6}}, (0, 0, 3000), QuatFromEuler(Pitch=-50, Yaw=60.0))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {"MoonIntensity": 0.2, "MoonColor": [0.6, 0.72, 1.0], "NightSkyColor": [0.008, 0.012, 0.026], "StarIntensity": 0.3},
		"TimeOfDayComponent": {"TimeOfDay": TimeOfDay, "DayLengthMinutes": 0.0, "MaxSunElevation": 60.0, "NorthAzimuth": 0.0, "AnimateInEditor": False},
		"SkyLightComponent": {"Intensity": 2.6},
		"HeightFogComponent": {
			"Color": [0.55, 0.6, 0.65], "Density": 0.0004, "HeightFalloff": 0.05, "StartDistance": 3500.0, "MaxOpacity": 0.4,
			"DirectionalInscatteringColor": [0.9, 0.8, 0.6], "Volumetric": False, "VolumetricDistance": 6000.0,
			"VolumetricAlbedo": [0.95, 0.92, 0.88], "VolumetricExtinctionScale": 0.6, "VolumetricAnisotropy": 0.5,
			"VolumetricDirectionalScale": 0.5, "VolumetricLocalLightScale": 0.35},
	})
	S.Add("Terrain", {"TerrainComponent": {
		"Asset": "Terrain/FarmBie/Farm.eterrain", "Size": [TERRAIN_SIZE, TERRAIN_SIZE], "HeightRange": HEIGHT_RANGE,
		"Layer0Material": f"{MATS}/GroundGrass.emat", "Layer1Material": f"{MATS}/GroundPath.emat",
		"Layer2Material": f"{MATS}/GroundSoil.emat", "Layer3Material": f"{MATS}/GroundStone.emat",
		"Layer0Tiling": 500.0, "Layer1Tiling": 400.0, "Layer2Tiling": 400.0, "Layer3Tiling": 450.0,
		"CastShadows": True, "Collision": True}})
	# 놀이 영역 밖으로 나가지 않는 보이지 않는 벽 (입구 길은 맵 이동 트리거가 맡는다 — F5)
	for Name, Center, Half in (("Bound_N", (0, PLAY_MIN[1] - 50, 200), (PLAY_MAX[0] + 200, 50, 300)),
							   ("Bound_S", (0, PLAY_MAX[1] + 50, 200), (PLAY_MAX[0] + 200, 50, 300)),
							   ("Bound_W", (PLAY_MIN[0] - 50, 0, 200), (50, PLAY_MAX[1] + 200, 300)),
							   ("Bound_E", (PLAY_MAX[0] + 50, 0, 200), (50, PLAY_MAX[1] + 200, 300))):
		B.BoxCollider(Name, Center, Half)


def AddCamera(B, Start):
	S = B.S
	Z = B.Height(*Start) + 70.0
	Forward = (0.0, -math.cos(math.radians(-CAMERA_PITCH)), -math.sin(math.radians(-CAMERA_PITCH)))
	Focus = (Start[0], Start[1], Z)
	# HD-2D: 미니어처 틸트시프트(화면 위아래 흐림) + 따뜻한 색 보정 + 옅은 비네트
	S.Add("Camera", {
		"CameraComponent": {"FovYDegrees": CAMERA_FOV, "NearZ": 50.0, "FarZ": 40000.0, "Primary": True, "Priority": 10},
		"DepthOfFieldComponent": {"Enabled": True, "FocusDistance": CAMERA_DISTANCE, "FocalRegion": 600.0,
								  "NearTransition": 600.0, "FarTransition": 1500.0, "NearBlurSize": 0.6, "FarBlurSize": 0.8,
								  "PreviewInEditor": False, "Mode": 1, "TiltShiftCenter": 0.5, "TiltShiftBand": 0.5,
								  "TiltShiftTransition": 0.45, "TiltShiftAngle": 0.0, "BokehBladeCount": 6, "BokehRotation": 15.0,
								  "BokehHighlightBoost": 2.0, "BokehHighlightThreshold": 1.0},
		"ColorGradingComponent": {"Enabled": True, "Temperature": 0.1, "Tint": 0.02, "Saturation": 1.1, "Contrast": 1.06,
								  "Lift": [0.0, 0.006, 0.02], "Gamma": [1.0, 1.0, 1.0], "Gain": [1.03, 1.0, 0.97],
								  "LookupTable": "", "LookupTableIntensity": 1.0},
		"VignetteComponent": {"Enabled": True, "Intensity": 0.35, "Size": 0.5, "Smoothness": 0.6, "Roundness": 1.0, "Color": [0.03, 0.02, 0.03]},
	}, tuple(Focus[I] - Forward[I] * CAMERA_DISTANCE for I in range(3)), QuatFromEuler(Pitch=CAMERA_PITCH, Yaw=-90.0))


def AddGame(B, AutoPlay):
	B.S.Add("FarmGame", {"ScriptComponent": Script(f"{SCRIPTS}/FarmGame.lua", 0, AutoPlay=AutoPlay)})


def AddPlayer(B, Start):
	Z = B.Height(*Start) + PLAYER_RADIUS + PLAYER_HALF + 4.0
	Index = len(B.S.Entities)
	B.S.Entities.append({"Components": {
		"PrefabInstanceComponent": {"Asset": f"{PREFABS}/Player.eprefab", "Overrides": ""},
		"PrefabLinkComponent": {"Id": "1", "Root": Index},
		"TransformComponent": {"Position": [Start[0], Start[1], Z], "Rotation": [0.0, 0.0, 0.0, 1.0], "Scale": [1.0, 1.0, 1.0]}},
		"Name": "Player", "Parent": -1})


def BuildScene(Height, AutoPlay=""):
	B = FBuilder(Height)
	B.Reserve(HOUSE[0], HOUSE[1], 500.0)
	AddEnvironment(B, 10.0)
	AddFarmstead(B)
	AddFence(B)
	AddTrees(B)
	AddGame(B, AutoPlay)
	AddCamera(B, PLAYER_START)
	AddPlayer(B, PLAYER_START)
	return B.S


def Main():
	FarmBieArt.WriteSprites(os.path.join(CONTENT, *SPRITES.split("/")))
	WriteMaterials()
	_, _, H, Weights = BuildTerrain()
	WriteTerrain(os.path.join(CONTENT, "Terrain", "FarmBie", "Farm.eterrain"), H, Weights)
	Height = FHeight(H)
	WritePrefabs()
	Scene = BuildScene(Height)
	Scene.Save(os.path.join(CONTENT, "Scenes", "Farm.escene"))
	os.makedirs(os.path.join(CONTENT, "Scenes", "Tests"), exist_ok=True)
	BuildScene(Height, AutoPlay="Basic").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmAutoPlay.escene"))
	print(f"FarmBie 생성: 엔티티 {len(Scene.Entities)}개, 지형 {TERRAIN_RES}² 높이 {H.min():.0f}~{H.max():.0f}cm")


if __name__ == "__main__":
	Main()
