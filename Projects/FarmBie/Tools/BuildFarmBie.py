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
import FarmBieBuild  # noqa: E402
import FarmBieCrops  # noqa: E402
import FarmBieData  # noqa: E402
import FarmBieUI  # noqa: E402
import FarmBieZombies  # noqa: E402
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
	WriteJson(os.path.join(CONTENT, MATS, "WoodPost.emat"), Material("FarmBieWoodPost", (0.32, 0.2, 0.12, 1.0), 0.85))
	Glow = Material("FarmBieLampGlow", (1.0, 0.8, 0.5, 1.0), 0.6)
	Glow["EmissiveFactor"] = [1.3, 0.8, 0.35]
	WriteJson(os.path.join(CONTENT, MATS, "LampGlow.emat"), Glow)


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


def TileCenter(TX, TY):
	return (GRID_ORIGIN[0] + (TX + 0.5) * TILE, GRID_ORIGIN[1] + (TY + 0.5) * TILE)


def WriteBuildPrefabs():
	# 설치물 프리팹: 루트(FarmStructureComponent — FarmBuild.lua가 칸·내구도를 채움) > Model [+ Collision(플레이어도 막는 것)] [+ 크리스탈 빛]
	Rows = dict(FarmBieData.BUILD_ROWS)
	for Id, Row in list(Rows.items()) + [("Crystal", {"Hp": 400, "Blocks": True, "Solid": True})]:
		Entities = [{"Name": Id, "Parent": -1, "Components": {
			"FarmStructureComponent": {"Kind": Id, "Hp": float(Row["Hp"]), "MaxHp": float(Row["Hp"]), "TX": 0, "TY": 0, "Blocks": Row["Blocks"],
									   "Crystal": Id == "Crystal", "Destroyed": False},
			"PrefabLinkComponent": Link(1), "TransformComponent": Transform()}},
			{"Name": "Model", "Parent": 0, "Components": {"ModelComponent": {"AssetPath": f"{FarmBieBuild.FOLDER}/{Id}.gltf"},
														  "PrefabLinkComponent": Link(2), "TransformComponent": Transform()}}]
		if Row["Solid"]:
			Half = (40.0, 40.0, 70.0) if Id in ("Turret", "Crystal") else (50.0, 24.0, 70.0)
			Entities.append({"Name": "Collision", "Parent": 0, "Components": {"BoxColliderComponent": {"HalfExtents": list(Half)},
																				  "PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, 70))}})
		if Id == "Crystal":
			Entities.append({"Name": "Glow", "Parent": 0, "Components": {
				"PointLightComponent": {"Color": [0.72, 0.45, 1.0], "Intensity": 6.0, "Radius": 650.0, "CastShadows": False},
				"PrefabLinkComponent": Link(4), "TransformComponent": Transform((0, 40, 160))}})
		WritePrefab(f"Build_{Id}", Entities)
	# 온실 (고정 부지 — 모델만, 안쪽은 걸어 다님)
	WritePrefab("Build_Greenhouse", [
		{"Name": "Greenhouse", "Parent": -1, "Components": {"PrefabLinkComponent": Link(1), "TransformComponent": Transform()}},
		{"Name": "Model", "Parent": 0, "Components": {"ModelComponent": {"AssetPath": f"{FarmBieBuild.FOLDER}/Greenhouse.gltf"},
													  "PrefabLinkComponent": Link(2), "TransformComponent": Transform()}},
	])


def WriteZombiePrefabs():
	# 좀비: 루트(FarmZombieComponent — FarmDefense.lua가 Zombies.etable 값을 채움, C++가 움직임) > Body(빌보드 도트) · Shadow · HpBack·HpFill(맞으면 보임)
	for Kind, Info in FarmBieZombies.KINDS.items():
		Size = Info["Cell"]
		WritePrefab(f"Zombie_{Kind}", [
			{"Name": f"Zombie_{Kind}", "Parent": -1, "Components": {
				"FarmZombieComponent": {"Kind": Kind, "SpriteBase": f"{SPRITES}/Zombie_{Kind}", "Hp": 30.0, "MaxHp": 30.0, "Speed": 110.0, "Damage": 6.0,
										"AttackInterval": 1.0, "StructureDamageMul": 1.0, "CropEatTime": 2.5, "BodyRadius": 26.0, "Explode": False,
										"ExplodeRadius": 0.0, "ExplodeDamage": 0.0, "RegenPerSec": 0.0, "SlowOnHit": 0.0, "Boss": False, "AggroTime": 0.0,
										"Alerted": False, "Dead": False},
				"PrefabLinkComponent": Link(1), "TransformComponent": Transform()}},
			{"Name": "Body", "Parent": 0, "Components": {
				"SpriteComponent": Sprite(f"{SPRITES}/Zombie_{Kind}.esprite", "WalkDown0", Billboard=1),
				"FlipbookComponent": Flipbook(f"{SPRITES}/Zombie_{Kind}_WalkDown.eflipbook"),
				"PrefabLinkComponent": Link(2), "TransformComponent": Transform()}},
			{"Name": "Shadow", "Parent": 0, "Components": {
				"SpriteComponent": Sprite(f"{SPRITES}/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
				"PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, 1.5), FLAT, (Size / 32.0 * 0.8, 1.0, Size / 32.0 * 0.8))}},
		])


def ColliderCells(Scene):
	# 씬 콜라이더(울타리·건물·소품·등불)가 덮는 농장 격자 칸 → 좀비가 못 지나가는 칸 (경계 벽·트리거 제외)
	Cells = set()
	for E in Scene.Entities:
		Box = E["Components"].get("BoxColliderComponent")
		if not Box or Box.get("IsTrigger") or E["Name"].startswith("Bound_") or E["Name"].startswith("Merchant"):
			continue
		P = E["Components"]["TransformComponent"]["Position"]
		Q = E["Components"]["TransformComponent"]["Rotation"]
		Yaw = math.atan2(2.0 * (Q[3] * Q[2] + Q[0] * Q[1]), 1.0 - 2.0 * (Q[1] * Q[1] + Q[2] * Q[2]))
		HX, HY = Box["HalfExtents"][0], Box["HalfExtents"][1]
		CY, SY = math.cos(Yaw), math.sin(Yaw)
		Steps = 20.0
		IX = max(1, int(HX * 2 / Steps))
		IY = max(1, int(HY * 2 / Steps))
		for A in range(IX + 1):
			for Bv in range(IY + 1):
				LX = -HX + 2 * HX * A / IX
				LY = -HY + 2 * HY * Bv / IY
				X = P[0] + LX * CY - LY * SY
				Y = P[1] + LX * SY + LY * CY
				TX = int(math.floor((X - GRID_ORIGIN[0]) / TILE))
				TY = int(math.floor((Y - GRID_ORIGIN[1]) / TILE))
				if 0 <= TX < GRID_W and 0 <= TY < GRID_H:
					Cells.add((TX, TY))
	return Cells


STATIC_BLOCKED = [""]


def AttractTiles():
	# 집 앞마당 3×2칸 (좀비가 크리스탈을 모르면 작물과 함께 여기로 모여든다)
	TX = int(math.floor((SLEEP_SPOT[0] - GRID_ORIGIN[0]) / TILE))
	TY = int(math.floor((SLEEP_SPOT[1] - GRID_ORIGIN[1]) / TILE))
	return ";".join(f"{TX + DX},{TY + DY}" for DX in (-1, 0, 1) for DY in (0, 1))


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
		# 밤 등불 (FarmTime이 낮밤 LampScale로 켠다 — 어둠 속 좀비가 보이게)
		{"Name": "Lantern", "Parent": 0, "Components": {
			"PointLightComponent": {"Color": [1.0, 0.78, 0.5], "Intensity": 0.0, "Radius": 750.0, "CastShadows": False},
			"PrefabLinkComponent": Link(5), "TransformComponent": Transform((0, 30, 60))}},
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
	B.Place("Well", "building_well_blue", WELL[0], WELL[1], -90.0, KS * 0.7, Collide=True)
	B.Place("Barn", "building_home_B_red", 1150.0, -1350.0, -90.0, KS * 0.9, Collide=True, Shrink=0.95)
	B.Place("ShippingBin", "crate_A_big", SHIPPING_BIN[0], SHIPPING_BIN[1], 10.0, KS * 1.6, Collide=True)
	B.Place("Lumber", "resource_lumber", -500.0, -1100.0, 90.0, KS * 0.9, Collide=True)
	B.Place("Barrel_0", "barrel", 330.0, -1080.0, 0.0, KS)
	B.Place("Barrel_1", "barrel", 440.0, -1110.0, 30.0, KS)
	B.Place("Wheelbarrow", "wheelbarrow", -1300.0, -800.0, 30.0, KS * 0.9)
	B.Place("Bucket", "bucket_water", -820.0, -1050.0, 0.0, KS)
	B.Place("Sack_0", "sack", 1500.0, -1050.0, 20.0, KS)
	B.Place("Sack_1", "sack", 1570.0, -1000.0, 80.0, KS)
	# 온실 부지: 귀퉁이 말뚝 4 (짓기 전 표시 — FarmBuild.lua가 지으면 숨김)
	TX, TY, W, H = GREENHOUSE_TILES
	X0, Y0 = TileCenter(TX, TY)
	X1, Y1 = TileCenter(TX + W - 1, TY + H - 1)
	for I, (X, Y) in enumerate(((X0 - 50, Y0 - 50), (X1 + 50, Y0 - 50), (X0 - 50, Y1 + 50), (X1 + 50, Y1 + 50))):
		B.S.Add(f"GreenhouseStake_{I}", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MATS}/WoodPost.emat"}},
				(X, Y, B.Height(X, Y) + 40.0), None, (0.1, 0.1, 0.8))
	B.Reserve((X0 + X1) * 0.5, (Y0 + Y1) * 0.5, 250.0)
	# 작업대 (제작 — FarmCraft.lua)
	B.S.Add("Workbench", {"ModelComponent": {"AssetPath": f"{FarmBieBuild.FOLDER}/Workbench.gltf"}}, (WORKBENCH[0], WORKBENCH[1], B.Height(*WORKBENCH)),
			QuatFromEuler(Yaw=0.0))
	B.BoxCollider("Workbench_Collision", (WORKBENCH[0], WORKBENCH[1], B.Height(*WORKBENCH) + 45.0), (75.0, 35.0, 45.0))
	B.Reserve(WORKBENCH[0], WORKBENCH[1], 120.0)
	# 오늘 밤 진입로 경고 표지 (FarmDefense.lua가 낮에 그 밤 진입로만 보이게)
	for Name, (X, Y) in ENTRANCE_SPAWNS.items():
		IX = X + (220.0 if Name in ("North", "South") else 0.0)
		IY = Y + (230.0 if Name in ("West", "East") else 0.0)
		B.S.Add(f"Warn_{Name}", {"SpriteComponent": Sprite(f"{SPRITES}/Fx.esprite", "Warn", Billboard=2, Visible=False, Shadows=True)},
				(IX, IY, B.Height(IX, IY)), None, (1.4, 1.4, 1.4))
	# 남쪽 입구 보부상 천막 자리 (F3에서 보부상이 온다)
	B.Place("MerchantTent", "tent", 700.0, 1650.0, -90.0, KS * 1.3, Collide=True)
	B.Place("MerchantCrate", "crate_B_small", 450.0, 1720.0, 15.0, KS * 1.4)


def AddEnvironment(B, TimeOfDay, TerrainAsset="Terrain/FarmBie/Farm.eterrain", Min=PLAY_MIN, Max=PLAY_MAX):
	S = B.S
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.95, 0.86], "Intensity": 5.4}}, (0, 0, 3000), QuatFromEuler(Pitch=-50, Yaw=60.0))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {"MoonIntensity": 0.2, "MoonColor": [0.6, 0.72, 1.0], "NightSkyColor": [0.008, 0.012, 0.026], "StarIntensity": 0.3},
		"TimeOfDayComponent": {"TimeOfDay": TimeOfDay, "DayLengthMinutes": 0.0, "MaxSunElevation": 60.0, "NorthAzimuth": 180.0, "AnimateInEditor": False},
		"SkyLightComponent": {"Intensity": 1.9},
		"HeightFogComponent": {
			"Color": [0.55, 0.6, 0.65], "Density": 0.0004, "HeightFalloff": 0.05, "StartDistance": 3500.0, "MaxOpacity": 0.4,
			"DirectionalInscatteringColor": [0.9, 0.8, 0.6], "Volumetric": False, "VolumetricDistance": 6000.0,
			"VolumetricAlbedo": [0.95, 0.92, 0.88], "VolumetricExtinctionScale": 0.6, "VolumetricAnisotropy": 0.5,
			"VolumetricDirectionalScale": 0.5, "VolumetricLocalLightScale": 0.35},
	})
	S.Add("Terrain", {"TerrainComponent": {
		"Asset": TerrainAsset, "Size": [TERRAIN_SIZE, TERRAIN_SIZE], "HeightRange": HEIGHT_RANGE,
		"Layer0Material": f"{MATS}/GroundGrass.emat", "Layer1Material": f"{MATS}/GroundPath.emat",
		"Layer2Material": f"{MATS}/GroundSoil.emat", "Layer3Material": f"{MATS}/GroundStone.emat",
		"Layer0Tiling": 500.0, "Layer1Tiling": 400.0, "Layer2Tiling": 400.0, "Layer3Tiling": 450.0,
		"CastShadows": True, "Collision": True}})
	# 놀이 영역 밖으로 나가지 않는 보이지 않는 벽 (입구 길은 맵 이동 트리거가 맡는다 — F5)
	for Name, Center, Half in (("Bound_N", (0, Min[1] - 50, 200), (Max[0] + 200, 50, 300)),
							   ("Bound_S", (0, Max[1] + 50, 200), (Max[0] + 200, 50, 300)),
							   ("Bound_W", (Min[0] - 50, 0, 200), (50, Max[1] + 200, 300)),
							   ("Bound_E", (Max[0] + 50, 0, 200), (50, Max[1] + 200, 300))):
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


WELL = (-950.0, -1250.0)
MERCHANT_SPOT = (520.0, 1480.0)  # 남쪽 입구 천막 앞
SHIPPING_BIN = (380.0, -950.0)
GREENHOUSE_TILES = (38, 23, 6, 4)   # 온실 부지 (TX, TY, 가로, 세로 칸) — 밭 동쪽
CRYSTAL_START = (18, 11)            # 크리스탈 처음 칸 (집 서쪽 마당)
WORKBENCH = (760.0, -1120.0)        # 작업대 (헛간 앞)
# 밤 진입로 (울타리 틈 바깥쪽 — 격자 안): 이름, 좀비가 나오는 자리
ENTRANCE_SPAWNS = {"North": (ENTRANCES["North"][0], PLAY_MIN[1] + 70.0), "South": (ENTRANCES["South"][0], PLAY_MAX[1] - 70.0),
				   "West": (PLAY_MIN[0] + 70.0, ENTRANCES["West"][1]), "East": (PLAY_MAX[0] - 70.0, ENTRANCES["East"][1])}
SLEEP_SPOT = (HOUSE[0], HOUSE[1] + 260.0)  # 집 문 앞 (잠자기 상호작용)
LAMPS = [(-420.0, -1000.0), (420.0, -1200.0), (-1300.0, -600.0), (1300.0, -600.0), (-900.0, 500.0), (900.0, 500.0), (0.0, 1250.0), (-2000.0, 150.0), (2000.0, -300.0)]


def AddLamps(B):
	# 등불 기둥 (나무 기둥 + 등 상자 + 점광원 Lamp_<n>) — 낮밤(FarmTime.lua)이 LampScale로 켜고 끈다. 밤 디펜스 시야
	for I, (X, Y) in enumerate(LAMPS):
		Ground = B.Height(X, Y)
		B.S.Add(f"LampPost_{I}", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MATS}/WoodPost.emat"}},
				(X, Y, Ground + 100.0), QuatFromEuler(Yaw=I * 23.0), (0.14, 0.14, 2.0))
		B.S.Add(f"LampPost_{I}_Box", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MATS}/LampGlow.emat"}},
				(X, Y, Ground + 212.0), QuatFromEuler(Yaw=I * 23.0), (0.26, 0.26, 0.3))
		B.S.Add(f"Lamp_{I}", {"PointLightComponent": {"Color": [1.0, 0.72, 0.4], "Intensity": 0.0, "Radius": 900.0, "CastShadows": False}},
				(X, Y + 20.0, Ground + 360.0))  # 높게 — 기둥 바로 아래 스프라이트가 하얗게 날아가지 않게
		B.BoxCollider(f"LampPost_{I}_Collision", (X, Y, Ground + 100.0), (16.0, 16.0, 100.0))
		B.Reserve(X, Y, 60.0)


def AddTravel(B, Name, X, Y, Target, Spawn, Half=(60.0, 230.0, 150.0)):
	# 맵 이동 트리거 (FarmTravel.lua) — 도착 씬에는 Spawn_<이름> 빈 엔티티(발 자리)
	B.S.Add(Name, {"BoxColliderComponent": {"HalfExtents": [float(V) for V in Half], "IsTrigger": True},
				   "ScriptComponent": Script(f"{SCRIPTS}/FarmTravel.lua", 0, TargetScene=Target, SpawnName=Spawn)},
			(X, Y, B.Height(X, Y) + Half[2] * 0.5))


def AddSpawn(B, Name, X, Y):
	B.S.Add(f"Spawn_{Name}", {}, (X, Y, B.Height(X, Y)))


def CameraBounds(Min, Max):
	return f"{Min[0] + 900.0},{Min[1] + 300.0},{Max[0] - 900.0},{Max[1] - 500.0}"


def AddGame(B, AutoPlay, Map="Farm", Bounds=None):
	B.S.Add("FarmGame", {"FarmDefenseComponent": {"Active": False, "Width": 0, "Height": 0}, "ScriptComponent": Script(f"{SCRIPTS}/FarmGame.lua", 0, AutoPlay=AutoPlay, Map=Map,
												   CameraBounds=Bounds or CameraBounds(PLAY_MIN, PLAY_MAX),
												   SleepSpot=f"{SLEEP_SPOT[0]},{SLEEP_SPOT[1]}", Slot="Test" if AutoPlay else "1",
												   ShipSpot=f"{SHIPPING_BIN[0]},{SHIPPING_BIN[1]}", MerchantSpot=f"{MERCHANT_SPOT[0]},{MERCHANT_SPOT[1]}")})
	# 보부상 (천막 앞 — FarmEconomy.lua가 방문 날만 보이게)
	X, Y = MERCHANT_SPOT
	Z = B.Height(X, Y)
	Root = B.S.Add("Merchant", {}, (X, Y, Z))
	B.S.Add("MerchantBody", {"SpriteComponent": Sprite(f"{SPRITES}/Peddler.esprite", "Idle0", Billboard=1, Visible=False),
							 "FlipbookComponent": Flipbook(f"{SPRITES}/Peddler_Idle.eflipbook")}, (0, 0, 0), Parent=Root)
	B.S.Add("MerchantShadow", {"SpriteComponent": Sprite(f"{SPRITES}/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0, Visible=False)},
			(0, 0, 1.5), FLAT, (1.1, 1.0, 1.1), Parent=Root)
	B.S.Add("MerchantCollision", {"BoxColliderComponent": {"HalfExtents": [45.0, 35.0, 90.0]}}, (X, Y, Z + 90.0))
	# 대상 칸 표시 (바닥에 눕힌 흰 모서리 — FarmField.lua가 옮기고 켠다)
	B.S.Add("TileCursor", {"SpriteComponent": Sprite(f"{SPRITES}/Field.esprite", "Cursor", Lit=False, Shadows=False, Blend=0, Visible=False,
													 Color=(1, 1, 1, 0.85))}, (0, 0, 3.0), FLAT)
	B.S.Add("Hud", {"UIComponent": {"Asset": f"{FarmBieUI.UI_DIR}/HUD.eui", "ZOrder": 0, "Visible": True, "ReceiveInput": False, "KeyboardFocus": False},
					"ScriptComponent": Script(f"{SCRIPTS}/FarmHud.lua", 2)})


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
	AddLamps(B)
	AddFence(B)
	AddTrees(B)
	AddGame(B, AutoPlay)
	AddTravel(B, "Travel_Forest", PLAY_MIN[0] + 60.0, ENTRANCES["West"][1], "Scenes/Forest.escene", "FromFarm")
	AddSpawn(B, "FromForest", PLAY_MIN[0] + 300.0, ENTRANCES["West"][1])
	AddSpawn(B, "Bed", SLEEP_SPOT[0], SLEEP_SPOT[1] + 60.0)
	AddCamera(B, PLAYER_START)
	AddPlayer(B, PLAYER_START)
	return B.S


# ================================================================ 숲 (채집 맵 — F5)
FOREST_MIN = (-2200.0, -1600.0)
FOREST_MAX = (2200.0, 1600.0)
FOREST_PATH = [(FOREST_MAX[0] + 600.0, 0.0), (1400.0, 60.0), (300.0, -120.0), (-600.0, 120.0), (-1500.0, -200.0)]
FOREST_START = (1900.0, 0.0)


def BuildForestTerrain():
	Axis = np.linspace(-TERRAIN_SIZE * 0.5, TERRAIN_SIZE * 0.5, TERRAIN_RES)
	X, Y = np.meshgrid(Axis, Axis)
	OutX = np.maximum(0.0, np.abs(X) - (FOREST_MAX[0] + 250.0))
	OutY = np.maximum(0.0, np.abs(Y) - (FOREST_MAX[1] + 250.0))
	Out = np.hypot(OutX, OutY)
	PathD = SegmentDistance(X, Y, FOREST_PATH)
	Valley = Smoothstep(500.0, 250.0, PathD) * (X > 0)
	H = Smoothstep(0.0, 1600.0, Out) * (500.0 + Noise2(X, Y, 800.0, 21) * 400.0) * (1.0 - Valley * 0.85)
	H += (Noise2(X, Y, 380.0, 23) - 0.5) * 16.0
	N = Noise2(X, Y, 160.0, 29)
	Path = Smoothstep(110.0 + N * 40.0, 60.0 + N * 40.0, PathD)
	Moss = Smoothstep(0.55, 0.7, Noise2(X, Y, 420.0, 31)) * (1.0 - Path)        # 흙(낙엽) 얼룩 = 레이어 2
	Stone = Smoothstep(0.7, 0.8, Noise2(X, Y, 300.0, 37)) * (1.0 - Path) * (1.0 - Moss)
	Grass = np.clip(1.0 - Path - Moss - Stone, 0.0, 1.0)
	Stack = np.stack([Grass, Path, Moss, Stone], axis=-1)
	Stack /= np.maximum(Stack.sum(axis=-1, keepdims=True), 1e-6)
	Bytes = np.floor(Stack * 255.0 + 0.5).astype(np.int32)
	Bytes[..., 0] += 255 - Bytes.sum(axis=-1)
	Bytes = np.clip(Bytes, 0, 255).astype(np.uint32)
	return H, Bytes[..., 0] | (Bytes[..., 1] << 8) | (Bytes[..., 2] << 16) | (Bytes[..., 3] << 24)


def AddNode(B, Type, Index, X, Y, Rng):
	# 숲 자원: 루트(이름 Node_<종류>_<번호> — FarmForage.lua가 찾는다) > Model(모델/스프라이트) [+ Stump] [+ Collision]
	Z = B.Height(X, Y)
	Root = B.S.Add(f"Node_{Type}_{Index}", {}, (X, Y, Z))
	if Type == "Tree":
		Id = Rng.choice(["tree_single_A", "tree_single_B"])
		S = KS * Rng.uniform(0.95, 1.15)
		Yaw = Rng.uniform(0, 360)
		B.S.Add("Model", {"ModelComponent": {"AssetPath": f"{KK}/{Id}.gltf"}}, (0, 0, -10.0), QuatFromEuler(Yaw=Yaw), (S, S, S), Parent=Root)
		B.S.Add("Stump", {"ModelComponent": {"AssetPath": f"{KK}/tree_single_A_cut.gltf"}}, (0, 0, -10.0), QuatFromEuler(Yaw=Yaw), (0.001, 0.001, 0.001), Parent=Root)
		B.S.Add("Collision", {"BoxColliderComponent": {"HalfExtents": [30.0, 30.0, 120.0]}}, (0, 0, 120.0), Parent=Root)
		B.Reserve(X, Y, 170.0)
	elif Type in ("Rock", "BigRock"):
		Id = Rng.choice(["rock_single_A", "rock_single_C", "rock_single_D"]) if Type == "Rock" else "rock_single_B"
		S = KS * (Rng.uniform(1.1, 1.4) if Type == "Rock" else Rng.uniform(2.0, 2.4))
		B.S.Add("Model", {"ModelComponent": {"AssetPath": f"{KK}/{Id}.gltf"}}, (0, 0, 0), QuatFromEuler(Yaw=Rng.uniform(0, 360)), (S, S, S), Parent=Root)
		R = 45.0 if Type == "Rock" else 85.0
		B.S.Add("Collision", {"BoxColliderComponent": {"HalfExtents": [R, R, 60.0]}}, (0, 0, 60.0), Parent=Root)
		B.Reserve(X, Y, R + 70.0)
	else:
		Slice = {"Fiber": "Fiber", "Herb": "Herb", "Mushroom": "Mushroom"}[Type]
		B.S.Add("Model", {"SpriteComponent": Sprite(f"{SPRITES}/Forage.esprite", Slice, Billboard=1)}, (0, 0, 0), Parent=Root)
		B.Reserve(X, Y, 80.0)


def BuildForestScene(Height, AutoPlay=""):
	B = FBuilder(Height)
	B.Rng = random.Random(41)
	Rng = B.Rng
	B.Reserve(FOREST_START[0], FOREST_START[1], 300.0)
	AddEnvironment(B, 10.0, "Terrain/FarmBie/Forest.eterrain", FOREST_MIN, FOREST_MAX)
	# 짙은 숲 그늘 (안개 조금 더)
	for E in B.S.Entities:
		if E["Name"] == "Sky":
			E["Components"]["HeightFogComponent"]["Density"] = 0.0007
			E["Components"]["SkyLightComponent"]["Intensity"] = 2.2
	# 자원 노드: 길에서 떨어진 곳에 흩어 놓는다
	def PathDist(X, Y):
		return float(SegmentDistance(np.array([X]), np.array([Y]), FOREST_PATH)[0])
	Counts = {"Tree": 16, "Rock": 10, "BigRock": 3, "Fiber": 12, "Herb": 10, "Mushroom": 8}
	for Type, Count in Counts.items():
		Placed, Tries = 0, 0
		while Placed < Count and Tries < 4000:
			Tries += 1
			X = Rng.uniform(FOREST_MIN[0] + 250.0, FOREST_MAX[0] - 400.0)
			Y = Rng.uniform(FOREST_MIN[1] + 250.0, FOREST_MAX[1] - 300.0)
			Gap = {"Tree": 230.0, "Rock": 150.0, "BigRock": 220.0}.get(Type, 110.0)
			if PathDist(X, Y) < 180.0 or not B.Free(X, Y, Gap):
				continue
			AddNode(B, Type, Placed, X, Y, Rng)
			Placed += 1
	# 테두리 숲 (장식 — 베지 못함)
	Kinds = ["trees_A_large", "trees_B_large", "trees_A_medium", "trees_B_medium"]
	Count = 0
	for _ in range(2200):
		X = Rng.uniform(-5200.0, 5200.0)
		Y = Rng.uniform(-4600.0, 4600.0)
		# 큰 나무 군락(배율 후 지름 약 9m)이 놀이 영역을 덮지 않게 경계 밖으로 충분히
		Near = abs(X) < FOREST_MAX[0] + 380.0 and abs(Y) < FOREST_MAX[1] + 380.0
		if Near or PathDist(X, Y) < 450.0 or not B.Free(X, Y, 260.0):
			continue
		B.Place(f"Tree_{Count}", Rng.choice(Kinds), X, Y, Rng.uniform(0, 360), KS * Rng.uniform(0.9, 1.25), Sink=10.0)
		Count += 1
	AddGame(B, AutoPlay, "Forest", CameraBounds(FOREST_MIN, FOREST_MAX))
	AddTravel(B, "Travel_Farm", FOREST_MAX[0] - 60.0, 0.0, "Scenes/Farm.escene", "FromForest")
	AddSpawn(B, "FromFarm", FOREST_MAX[0] - 300.0, 0.0)
	AddCamera(B, FOREST_START)
	AddPlayer(B, FOREST_START)
	return B.S


def Main():
	FarmBieArt.WriteSprites(os.path.join(CONTENT, *SPRITES.split("/")))
	WriteMaterials()
	FarmBieData.WriteAll(CONTENT)
	FarmBieCrops.WriteSprites(os.path.join(CONTENT, *SPRITES.split("/")))
	X0, Y0 = PLAY_MIN[0] + FENCE_INSET + 60.0, PLAY_MIN[1] + FENCE_INSET + 60.0
	_, _, H, Weights = BuildTerrain()
	WriteTerrain(os.path.join(CONTENT, "Terrain", "FarmBie", "Farm.eterrain"), H, Weights)
	Height = FHeight(H)
	STATIC_BLOCKED[0] = ";".join(f"{X},{Y}" for X, Y in sorted(ColliderCells(BuildScene(Height))))
	FarmBieData.WriteFarmMap(CONTENT, {"Tile": TILE, "Width": GRID_W, "Height": GRID_H, "OriginX": GRID_ORIGIN[0], "OriginY": GRID_ORIGIN[1],
									   "FarmMinX": X0, "FarmMinY": Y0, "FarmMaxX": -X0, "FarmMaxY": -Y0, "Well": [WELL[0], WELL[1]],
									   "Greenhouse": list(GREENHOUSE_TILES), "GreenhouseCost": FarmBieData.GREENHOUSE["Cost"],
									   "GreenhouseGold": FarmBieData.GREENHOUSE["Gold"], "CrystalStart": list(CRYSTAL_START),
									   "StaticBlocked": STATIC_BLOCKED[0], "AttractTiles": AttractTiles(),
									   "Entrances": [f"{N},{X:.0f},{Y:.0f}" for N, (X, Y) in ENTRANCE_SPAWNS.items()],
									   "Workbench": [WORKBENCH[0], WORKBENCH[1]]})
	FarmBieBuild.WriteModels(CONTENT, GREENHOUSE_TILES[2:])
	WriteZombiePrefabs()
	WriteBuildPrefabs()
	FarmBieUI.WriteHud(CONTENT)
	WritePrefabs()
	Scene = BuildScene(Height)
	Scene.Save(os.path.join(CONTENT, "Scenes", "Farm.escene"))
	FH, FW = BuildForestTerrain()
	WriteTerrain(os.path.join(CONTENT, "Terrain", "FarmBie", "Forest.eterrain"), FH, FW)
	ForestHeight = FHeight(FH)
	BuildForestScene(ForestHeight).Save(os.path.join(CONTENT, "Scenes", "Forest.escene"))
	BuildScene(Height, AutoPlay="Forest").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmForest.escene"))
	os.makedirs(os.path.join(CONTENT, "Scenes", "Tests"), exist_ok=True)
	BuildScene(Height, AutoPlay="Basic").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmAutoPlay.escene"))
	BuildScene(Height, AutoPlay="Time").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmTime.escene"))
	BuildScene(Height, AutoPlay="Boss").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmBoss.escene"))
	BuildScene(Height, AutoPlay="SeasonBoss").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmSeasonBoss.escene"))
	BuildScene(Height, AutoPlay="Defense").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmDefense.escene"))
	BuildScene(Height, AutoPlay="DefenseLoss").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmDefenseLoss.escene"))
	BuildScene(Height, AutoPlay="GameOver").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmGameOver.escene"))
	BuildScene(Height, AutoPlay="Build").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmBuild.escene"))
	BuildScene(Height, AutoPlay="Sanity").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmSanity.escene"))
	BuildScene(Height, AutoPlay="Economy").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmEconomy.escene"))
	BuildScene(Height, AutoPlay="Farm").Save(os.path.join(CONTENT, "Scenes", "Tests", "FarmFarming.escene"))
	if "--views" in sys.argv:
		BuildScene(Height, AutoPlay="BossShot").Save(os.path.join(CONTENT, "Scenes", "_FarmBossShot.escene"))
		BuildScene(Height, AutoPlay="NightShot").Save(os.path.join(CONTENT, "Scenes", "_FarmNightShot.escene"))
		BuildScene(Height, AutoPlay="BuildShot").Save(os.path.join(CONTENT, "Scenes", "_FarmBuildShot.escene"))
		BuildForestScene(ForestHeight, "Shot10").Save(os.path.join(CONTENT, "Scenes", "_ForestShot.escene"))
		# 확인용 (커밋하지 않음): 시각별 화면
		BuildScene(Height, AutoPlay="ShopShot").Save(os.path.join(CONTENT, "Scenes", "_FarmShopShot.escene"))
		BuildScene(Height, AutoPlay="BagShot").Save(os.path.join(CONTENT, "Scenes", "_FarmBagShot.escene"))
		BuildScene(Height, AutoPlay="FieldShot").Save(os.path.join(CONTENT, "Scenes", "_FarmFieldShot.escene"))
		for Hour in ("9", "17.5", "21.5"):
			BuildScene(Height, AutoPlay=f"Shot{Hour}").Save(os.path.join(CONTENT, "Scenes", f"_FarmShot{Hour.replace('.', '_')}.escene"))
	print(f"FarmBie 생성: 엔티티 {len(Scene.Entities)}개, 지형 {TERRAIN_RES}² 높이 {H.min():.0f}~{H.max():.0f}cm")


if __name__ == "__main__":
	Main()
