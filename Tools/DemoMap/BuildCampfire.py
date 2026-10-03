# 데모 서브맵 "Campfire"(해 질 녘 밀수꾼 해변 캠프 — 파티클 쇼케이스) 생성:
#   지형(.eterrain) + 지형/풀 머티리얼 + 파티클(.eparticle v2, 이미터·모듈·곡선) + 파티클 텍스처(절차 생성 플립북) + 소리(절차 생성)
#   + 폴리지(.efoliage) + 씬(Scenes/Demo/Campfire.escene)
#   실행: python Tools/DemoMap/BuildCampfire.py [--overview[=<시점 이름>,... | =x,y,지면 위 높이,pitch,yaw]]
#         (먼저 Scripts/FetchDemoAssets.ps1로 Poly Haven 에셋을 받는다)
#   --overview: 플레이어 대신 고정 카메라를 둔 확인용 변형(Scenes/Demo/_CampfireOverview*.escene)도 쓴다 — 커밋하지 않는다
#   보여 주는 기능: 나이아가라식 파티클 — 모닥불(불꽃 플립북 + 곡선 노이즈 + 부력, 불티 GPU(속도 정렬·버스트·바닥 충돌), 연기(정렬 반투명
#                   플립북·바람), 재), 등불 불꽃(로컬 공간), 바위 물보라(GPU 버스트 + 바다 높이 충돌로 사라짐 + 물안개), 반딧불(GPU 깜빡임 곡선
#                   + 고정 경계), 신호 조명탄(스크립트 Timer.Every + 코루틴 → 로켓·폭발 버스트·낙하산 조명탄 불꽃/연기 + 따라 내려오는 붉은 빛)
#                   + 불빛 깜빡임(FlickerLight.lua), 볼류메트릭 안개 속 불빛, 바다(물) + 대기 박명 하늘 + 구름, 공간 음향(모닥불·파도)
#   좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 바다는 +X(서쪽 — 해가 바다로 진다), 만(灣)의 양쪽 곶은 ±Y, 뒤(-X)는 모래 언덕과 언덕.
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
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402
from AssetFixes import AlphaModel  # noqa: E402

ROOT     = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTENT  = os.path.join(ROOT, "Projects", "Sample", "Content")
PH       = "Asset/PolyHaven"
MAT_DIR  = "Materials/Demo/Campfire"
FX_DIR   = "Particles/Demo"

# ---- 지형 ----------------------------------------------------------------------------------------------------------
TERRAIN_SIZE = 25600.0  # cm (가로·세로)
TERRAIN_RES  = 513      # 칸 50cm
HEIGHT_RANGE = 12000.0  # cm (16비트 전체 범위, 가운데 = 위치 Z = 0)
SEA_LEVEL    = 0.0
HEADLAND_Y   = 6200.0   # 곶 안쪽 절벽이 서는 |Y|
HEADLAND_TIP = 9200.0   # 곶 끝 X
PLATEAU      = 1500.0   # 곶 위 높이


def Smoothstep(E0, E1, X):
	T = np.clip((X - E0) / (E1 - E0), 0.0, 1.0)
	return T * T * (3.0 - 2.0 * T)


def ValueNoise(X, Y, Scale, Seed):
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


def ShoreX(Y):
	# 물가 선 X: 만 가운데가 가장 안쪽(1800cm), 곶으로 갈수록 바다 쪽으로 휜다
	A = np.abs(Y)
	return 1800.0 + 2600.0 * (np.minimum(A, 7000.0) / 6000.0) ** 2 + 150.0 * np.sin(np.asarray(Y) / 900.0)


def BuildHeights():
	Coords = (np.arange(TERRAIN_RES) / (TERRAIN_RES - 1) - 0.5) * TERRAIN_SIZE
	X, Y = np.meshgrid(Coords, Coords)  # 배열 [행(Y), 열(X)]
	A = np.abs(Y)
	Shore = ShoreX(Y)
	Inland = Shore - X  # 물가에서 육지 쪽 거리
	# 해변: 물가에서 완만히 오르고(3.5%), 뒤로 모래 언덕(물결 노이즈), 더 뒤로 풀 언덕
	Land = np.maximum(Inland, 0.0) * 0.035
	Land += Smoothstep(1400.0, 3600.0, Inland) * (330.0 + Fbm(X, Y, 1600.0, 11) * 160.0)
	Land += Smoothstep(4200.0, 9500.0, Inland) * (1900.0 + Fbm(X, Y, 4000.0, 21) * 700.0)
	Land += Fbm(X, Y, 700.0, 31, 3) * 12.0 * Smoothstep(0.0, 600.0, Inland)  # 모래 잔물결
	# 바다 바닥: 물가 바깥으로 6% 내려가다 -700에서 평평
	Sea = np.maximum(-np.maximum(-Inland, 0.0) * 0.06, -700.0 + Fbm(X, Y, 2500.0, 41) * 60.0)
	H = np.where(Inland > 0.0, Land, Sea)
	# 곶 (양쪽): 안쪽 면은 가파른 절벽, 끝은 바다로 떨어진다
	Edge = Fbm(X, Y, 1800.0, 51) * 350.0
	HeadMask = Smoothstep(HEADLAND_Y - 300.0, HEADLAND_Y + 700.0, A + Edge) * Smoothstep(HEADLAND_TIP + 600.0, HEADLAND_TIP - 600.0, X + Edge)
	Plateau = PLATEAU + Fbm(X, Y, 2200.0, 61) * 180.0 + Smoothstep(0.0, -6000.0, X) * 900.0
	H = np.maximum(H, -700.0 * (1.0 - HeadMask) + Plateau * HeadMask)
	return X, Y, H


def BuildWeights(X, Y, H):
	# 레이어: 0 마른 모래, 1 젖은 모래(물가), 2 바위(경사), 3 풀(언덕·곶 위)
	GY, GX = np.gradient(H, TERRAIN_SIZE / (TERRAIN_RES - 1))
	Slope = np.sqrt(GX * GX + GY * GY)
	Noise = Fbm(X, Y, 600.0, 71, 3)
	Wet = Smoothstep(45.0, 10.0, H + Noise * 12.0)
	Rock = Smoothstep(0.45, 0.85, Slope + Noise * 0.12)
	Grass = Smoothstep(420.0, 650.0, H + Noise * 120.0) * (1.0 - Rock)
	W2 = Rock
	W3 = Grass * (1 - W2)
	W1 = Wet * (1 - W2) * (1 - W3)
	W0 = np.clip(1 - W1 - W2 - W3, 0, 1)
	Stack = np.stack([W0, W1, W2, W3], axis=-1)
	Stack = Stack / np.maximum(Stack.sum(axis=-1, keepdims=True), 1e-6)
	Bytes = np.floor(Stack * 255.0 + 0.5).astype(np.int32)
	Bytes[..., 0] += 255 - Bytes.sum(axis=-1)
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
	def __init__(self, H, Slope):
		self.H, self.Slope = H, Slope
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


# ---- 머티리얼 / 임포트 설정 ----------------------------------------------------------------------------------------
TERRAIN_TEXTURES = {
	"Sand":    ("coast_sand_01", [1.0, 0.97, 0.92]),    # Hub와 같은 원본
	"WetSand": ("damp_beach_sand", [0.85, 0.85, 0.85]),
	"Rock":    ("cliff_side", [0.9, 0.88, 0.85]),
	"Grass":   ("aerial_grass_rock", [0.95, 0.92, 0.75]),
}

# 임포트 설정: 이 맵에서 새로 받은 에셋은 모두 1.3만 삼각형 이하(2026-10-04 glTF 실측) — 상한 없음.
#   스캔 바위·절벽·통나무는 Hub/Forest가 쓰는 .eimport 상한(coast_* 4~6만, coastal_cliff_* 15만, dead_tree_trunk* 1.2~1.4만)을 그대로 쓴다


def WriteJson(Path, Doc, Compact=False):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=None if Compact else 2, ensure_ascii=False)
		File.write("\n")


def PlainMaterial(Name, Base, Rough, Emissive=(0.0, 0.0, 0.0)):
	return {"Name": Name, "BaseColorFactor": list(Base), "EmissiveFactor": list(Emissive), "Metallic": 0.0, "Roughness": Rough,
			"NormalScale": 1.0, "OcclusionStrength": 1.0, "BaseColorTexture": "", "MetallicRoughnessTexture": "", "NormalTexture": "",
			"OcclusionTexture": "", "EmissiveTexture": ""}


def WriteMaterials():
	for Name, (Id, Tint) in TERRAIN_TEXTURES.items():
		Rel = f"../../../{PH}/{Id}/{Id}"
		WriteJson(os.path.join(CONTENT, MAT_DIR, f"Terrain{Name}.emat"), {
			"Name": f"CampfireTerrain{Name}", "BaseColorFactor": Tint + [1.0], "EmissiveFactor": [0.0, 0.0, 0.0],
			"Metallic": 1.0, "Roughness": 1.0, "NormalScale": 1.0, "OcclusionStrength": 1.0,
			"BaseColorTexture": f"{Rel}_diff_2k.jpg",
			"MetallicRoughnessTexture": f"{Rel}_arm_2k.jpg",  # Poly Haven ARM: R=AO, G=거칠기, B=금속
			"NormalTexture": f"{Rel}_nor_gl_2k.jpg", "OcclusionTexture": f"{Rel}_arm_2k.jpg", "EmissiveTexture": "",
		})
	# 모래 언덕 풀(엔진 내장 foliage:grass): 정점 색 × 마른 해변 풀 색
	WriteJson(os.path.join(CONTENT, MAT_DIR, "DuneGrass.emat"), PlainMaterial("CampfireDuneGrass", (1.25, 1.05, 0.62, 1.0), 0.9))


# ---- 파티클 텍스처 (절차 생성, 결정적) ---------------------------------------------------------------------------------
def _Noise2(Size, Cells, Seed, Octaves=4):
	# 이음매 없는(주기) 값 노이즈 0~1
	Rng = np.random.default_rng(Seed)
	Y, X = (np.mgrid[0:Size, 0:Size] + 0.5) / Size
	Sum, Amp, Norm = np.zeros((Size, Size)), 1.0, 0.0
	for Octave in range(Octaves):
		C = Cells * 2 ** Octave
		G = Rng.random((C, C))
		FX, FY = X * C, Y * C
		IX, IY = np.floor(FX).astype(int), np.floor(FY).astype(int)
		TX, TY = FX - IX, FY - IY
		TX, TY = TX * TX * (3 - 2 * TX), TY * TY * (3 - 2 * TY)
		A, B = G[IY % C, IX % C], G[IY % C, (IX + 1) % C]
		Cc, D = G[(IY + 1) % C, IX % C], G[(IY + 1) % C, (IX + 1) % C]
		Sum += ((A * (1 - TX) + B * TX) * (1 - TY) + (Cc * (1 - TX) + D * TX) * TY) * Amp
		Norm += Amp
		Amp *= 0.5
	return Sum / Norm


def WriteParticleTextures():
	Folder = os.path.join(CONTENT, FX_DIR, "Textures")
	os.makedirs(Folder, exist_ok=True)
	Frame = 128
	# 불꽃 플립북 4x4: 아래가 넓고 위로 갈수록 가늘게 흔들리는 혀 모양 (색은 흰색, 알파 = 모양 × 뜨거운 속). 텍스처 V = 0이 위
	Flame = np.zeros((Frame * 4, Frame * 4, 4), dtype=np.float32)
	for Index in range(16):
		N = _Noise2(Frame, 4, 100 + Index)
		N2 = _Noise2(Frame, 8, 200 + Index)
		V, U = (np.mgrid[0:Frame, 0:Frame] + 0.5) / Frame
		H = 1.0 - V  # 0 = 아래, 1 = 위
		Sway = (N[:, Frame // 2][:, None] - 0.5) * 0.35 * H  # 위로 갈수록 크게 흔들림
		Width = 0.34 * (1.0 - H) ** 0.65 + 0.02
		Dx = np.abs(U - 0.5 - Sway) / np.maximum(Width, 1e-3)
		Shape = np.clip(1.0 - Dx, 0.0, 1.0) * Smoothstep(0.0, 0.12, H) * Smoothstep(1.0, 0.55 + 0.3 * N2, H)
		Alpha = np.clip(Shape ** 0.8 * (0.65 + 0.7 * N2) - 0.08, 0.0, 1.0)
		Core = np.clip(1.0 - Dx * 1.6, 0.0, 1.0) * Smoothstep(0.05, 0.25, H) * Smoothstep(0.7, 0.2, H)
		Rgb = 0.75 + 0.25 * Core  # 가운데가 조금 더 밝다 (색은 입자 색이 정한다)
		Row, Col = divmod(Index, 4)
		Flame[Row * Frame:(Row + 1) * Frame, Col * Frame:(Col + 1) * Frame] = np.dstack([Rgb, Rgb, Rgb, Alpha])
	_SaveRgba(os.path.join(Folder, "CampfireFlame.png"), Flame)
	# 연기 플립북 4x4: 노이즈로 깎은 뭉게 덩어리 + 위쪽이 조금 밝은 음영
	Smoke = np.zeros((Frame * 4, Frame * 4, 4), dtype=np.float32)
	for Index in range(16):
		N = _Noise2(Frame, 3, 300 + Index, 5)
		V, U = (np.mgrid[0:Frame, 0:Frame] + 0.5) / Frame * 2.0 - 1.0
		R = np.sqrt(U * U + V * V)
		Blob = np.clip((1.0 - R) * 1.4 + (N - 0.5) * 1.6 - 0.15, 0.0, 1.0)
		Alpha = Blob ** 1.5 * np.clip((0.95 - R) / 0.25, 0.0, 1.0)
		Shade = np.clip(0.78 - V * 0.18 + (N - 0.5) * 0.3, 0.45, 1.0)
		Row, Col = divmod(Index, 4)
		Smoke[Row * Frame:(Row + 1) * Frame, Col * Frame:(Col + 1) * Frame] = np.dstack([Shade, Shade, Shade, Alpha])
	_SaveRgba(os.path.join(Folder, "CampfireSmoke.png"), Smoke)


def _SaveRgba(Path, Rgba):
	Image.fromarray(np.clip(Rgba * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGBA").save(Path, optimize=False)


# ---- 파티클 (.eparticle v2) -------------------------------------------------------------------------------------------
def V(*Values):
	Out = [float(X) for X in Values] + [0.0] * (4 - len(Values))
	return Out[:4]


def Const(*Values):
	return {"Mode": 0, "A": V(*Values)}


def Rand(A, B):
	A = A if isinstance(A, (list, tuple)) else (A,)
	B = B if isinstance(B, (list, tuple)) else (B,)
	return {"Mode": 1, "A": V(*A), "B": V(*B)}


def Curve(*Keys):
	# Keys: (시간 0~1, (값...))
	return {"Mode": 2, "A": V(*Keys[0][1]), "Curve": [{"T": float(T), "V": V(*Value)} for T, Value in Keys]}


def Mod(Name, **Inputs):
	return {"Module": Name, "Enabled": True, "Inputs": Inputs}


def Sprite(Blend=1, Texture="", Cols=1, Rows=1, Velocity=False, Stretch=0.02):
	return {"Type": 0, "Enabled": True, "BlendMode": Blend, "Texture": Texture, "SubImageColumns": Cols, "SubImageRows": Rows,
			"Alignment": 1 if Velocity else 0, "VelocityStretch": Stretch, "Mesh": "primitive:sphere", "RibbonWidth": 1.0}


def Emitter(Name, Seed, Max, EmitterUpdate, Spawn, Update, Renderer, Sim="CPU", Duration=1.0, Loop=True, Local=False, Bounds=None):
	Node = {"Name": Name, "Enabled": True, "SimTarget": Sim, "LocalSpace": Local, "Duration": Duration, "Loop": Loop,
			"MaxParticles": Max, "Seed": Seed}
	if Bounds:
		Node["FixedBounds"] = {"Min": list(Bounds[0]), "Max": list(Bounds[1])}
	Node.update({"EmitterUpdate": EmitterUpdate, "ParticleSpawn": Spawn, "ParticleUpdate": Update, "Renderers": [Renderer]})
	return Node


SHAPE_POINT, SHAPE_SPHERE, SHAPE_BOX, SHAPE_CYLINDER = 0, 1, 2, 3
FLAME_TEX = "Textures/CampfireFlame.png"
SMOKE_TEX = "Textures/CampfireSmoke.png"


def Shape(Kind, Radius=0.0, Box=(50.0, 50.0, 50.0), Height=0.0, Offset=(0.0, 0.0, 0.0), Surface=False):
	return Mod("ShapeLocation", Shape=Const(Kind), Radius=Const(Radius), BoxSize=Const(*Box), Height=Const(Height),
			   SurfaceOnly=Const(1.0 if Surface else 0.0), Offset=Const(*Offset))


def Init(Lifetime, Color, Size, Rotation=Const(0.0), Mass=Const(1.0)):
	return Mod("InitializeParticle", Lifetime=Lifetime, Color=Color, SpriteSize=Size, SpriteRotation=Rotation, Mass=Mass)


def CampfireSystem(GroundZ):
	# 모닥불 하나 = 이미터 5개. GroundZ = 이미터 원점 기준 모래 높이(불티 충돌 평면은 월드 Z라 씬이 알려 준다)
	Flames = Emitter("Flames", 11, 140,
		[Mod("SpawnRate", SpawnRate=Const(48.0))],
		[Init(Rand(0.45, 0.85), Rand((0.8, 0.27, 0.06, 0.85), (1.1, 0.42, 0.1, 1.0)), Rand((20.0, 34.0), (32.0, 58.0)), Rand(-14.0, 14.0)),
		 Shape(SHAPE_CYLINDER, Radius=24.0, Height=6.0, Offset=(0.0, 0.0, 8.0)),
		 Mod("AddVelocity", Velocity=Rand((-12.0, -12.0, 55.0), (12.0, 12.0, 105.0)))],
		[Mod("AccelerationForce", Acceleration=Const(6.0, 2.0, 130.0)),
		 Mod("Drag", Drag=Const(1.4)),
		 Mod("CurlNoiseForce", Strength=Const(260.0), Frequency=Const(0.025), PanSpeed=Const(0.0, 0.0, 90.0)),
		 Mod("ScaleColor", Scale=Curve((0.0, (0.5, 0.5, 0.5, 0.0)), (0.1, (1.0, 1.0, 1.0, 1.0)), (0.45, (0.85, 0.62, 0.45, 0.9)), (1.0, (0.35, 0.1, 0.03, 0.0)))),
		 Mod("ScaleSpriteSize", Scale=Curve((0.0, (0.75, 0.7)), (0.3, (1.1, 1.0)), (1.0, (0.35, 0.55)))),
		 Mod("SpriteRotationRate", RotationRate=Rand(-35.0, 35.0)),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0))],
		Sprite(1, FLAME_TEX, 4, 4))
	Glow = Emitter("CoreGlow", 12, 24,
		[Mod("SpawnRate", SpawnRate=Const(12.0))],
		[Init(Rand(0.6, 1.0), Rand((0.3, 0.1, 0.025, 0.3), (0.42, 0.15, 0.035, 0.4)), Rand((110.0, 110.0), (150.0, 150.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_SPHERE, Radius=12.0, Offset=(0.0, 0.0, 22.0)),
		 Mod("AddVelocity", Velocity=Const(0.0, 0.0, 15.0))],
		[Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 0.0)), (0.3, (1.0, 1.0, 1.0, 1.0)), (1.0, (1.0, 1.0, 1.0, 0.0))))],
		Sprite(1))
	# 불티: GPU, 속도 방향으로 늘인 스프라이트. 꾸준한 생성 + 장작이 터지는 버스트(주기 3.7초 안 1.3초), 식으며 떨어져 모래에 닿으면 구르다 꺼진다
	Embers = Emitter("Embers", 13, 900,
		[Mod("SpawnRate", SpawnRate=Const(26.0)), Mod("SpawnBurstInstantaneous", SpawnCount=Rand(30.0, 45.0), SpawnTime=Const(1.3))],
		[Init(Rand(1.4, 3.6), Rand((6.0, 2.0, 0.45, 1.0), (9.0, 3.2, 0.8, 1.0)), Rand((0.9, 0.9), (1.8, 1.8)), Const(0.0), Rand(0.6, 1.4)),
		 Shape(SHAPE_CYLINDER, Radius=26.0, Height=10.0, Offset=(0.0, 0.0, 18.0)),
		 Mod("AddVelocityInCone", ConeAxis=Const(0.0, 0.0, 1.0), ConeAngle=Const(28.0), Speed=Rand(140.0, 400.0))],
		[Mod("AccelerationForce", Acceleration=Const(22.0, 8.0, 45.0)),
		 Mod("GravityForce", Gravity=Const(0.0, 0.0, -95.0)),
		 Mod("Drag", Drag=Const(0.85)),
		 Mod("CurlNoiseForce", Strength=Const(700.0), Frequency=Const(0.012), PanSpeed=Const(0.0, 0.0, 70.0)),
		 Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 1.0)), (0.12, (1.35, 1.35, 1.35, 1.0)), (0.28, (0.8, 0.75, 0.7, 1.0)), (0.45, (1.1, 1.0, 0.9, 1.0)),
									   (0.75, (0.45, 0.25, 0.15, 0.85)), (1.0, (0.15, 0.04, 0.02, 0.0)))),
		 Mod("Collision", PlaneHeight=Const(GroundZ), Restitution=Const(0.15), Friction=Const(0.7), KillOnCollide=Const(0.0))],
		Sprite(1, Velocity=True, Stretch=0.018), Sim="GPU", Duration=3.7,
		Bounds=((-900.0, -900.0, -150.0), (900.0, 900.0, 1400.0)))
	# 연기: CPU 정렬 반투명 플립북. 처음엔 불빛을 받은 주황 → 회색으로 식으며 바람(+X 바다 → 육지 쪽은 -X지만 해풍이 잦아든 저녁 — 만 안쪽 +Y로)
	Smoke = Emitter("Smoke", 14, 90,
		[Mod("SpawnRate", SpawnRate=Const(9.0))],
		[Init(Rand(6.0, 9.0), Rand((0.09, 0.09, 0.095, 0.42), (0.13, 0.13, 0.14, 0.55)), Rand((55.0, 55.0), (80.0, 80.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_CYLINDER, Radius=18.0, Height=10.0, Offset=(0.0, 0.0, 75.0)),
		 Mod("AddVelocity", Velocity=Rand((-6.0, -6.0, 70.0), (6.0, 6.0, 110.0)))],
		[Mod("AccelerationForce", Acceleration=Const(-14.0, 30.0, 16.0)),
		 Mod("Drag", Drag=Const(0.35)),
		 Mod("CurlNoiseForce", Strength=Const(55.0), Frequency=Const(0.006), PanSpeed=Const(0.0, 15.0, 25.0)),
		 Mod("SpriteRotationRate", RotationRate=Rand(-16.0, 16.0)),
		 Mod("ScaleColor", Scale=Curve((0.0, (3.0, 1.6, 0.8, 0.0)), (0.07, (2.6, 1.5, 0.85, 1.0)), (0.3, (1.1, 1.08, 1.1, 0.8)), (1.0, (1.05, 1.08, 1.2, 0.0)))),
		 Mod("ScaleSpriteSize", Scale=Curve((0.0, (0.6, 0.6)), (1.0, (5.5, 5.5)))),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0))],
		Sprite(0, SMOKE_TEX, 4, 4))
	# 재: 작은 회색 조각이 연기 기둥을 타고 맴돌며 오른다 (GPU 반투명 — 정렬 없음, 작아서 티 안 남)
	Ash = Emitter("Ash", 15, 200,
		[Mod("SpawnRate", SpawnRate=Const(10.0))],
		[Init(Rand(4.0, 7.0), Rand((0.18, 0.17, 0.16, 0.8), (0.3, 0.28, 0.26, 0.9)), Rand((1.2, 1.2), (2.2, 2.2)), Rand(0.0, 360.0)),
		 Shape(SHAPE_CYLINDER, Radius=20.0, Height=10.0, Offset=(0.0, 0.0, 70.0)),
		 Mod("AddVelocityInCone", ConeAxis=Const(0.0, 0.0, 1.0), ConeAngle=Const(20.0), Speed=Rand(60.0, 130.0))],
		[Mod("AccelerationForce", Acceleration=Const(-10.0, 25.0, 10.0)),
		 Mod("Drag", Drag=Const(0.6)),
		 Mod("VortexForce", Axis=Const(0.0, 0.0, 1.0), Center=Const(0.0, 0.0, 0.0), Amount=Const(60.0), PullIn=Const(10.0)),
		 Mod("CurlNoiseForce", Strength=Const(120.0), Frequency=Const(0.01), PanSpeed=Const(0.0, 0.0, 20.0)),
		 Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 0.0)), (0.1, (1.0, 1.0, 1.0, 1.0)), (1.0, (1.0, 1.0, 1.0, 0.0))))],
		Sprite(0), Sim="GPU", Bounds=((-600.0, -400.0, -50.0), (400.0, 1200.0, 1500.0)))
	return {"Name": "CampfireFire", "Version": 2, "Emitters": [Glow, Flames, Smoke, Embers, Ash]}


def LanternFlameSystem():
	# 등불 심지 불꽃: 로컬 공간(등불을 따라감), 작고 빠른 혀 + 은은한 빛무리
	Flame = Emitter("Wick", 21, 24,
		[Mod("SpawnRate", SpawnRate=Const(30.0))],
		[Init(Rand(0.22, 0.38), Rand((5.0, 2.2, 0.6, 1.0), (6.0, 2.8, 0.8, 1.0)), Rand((3.0, 5.5), (4.0, 7.5)), Rand(-6.0, 6.0)),
		 Shape(SHAPE_SPHERE, Radius=0.6),
		 Mod("AddVelocity", Velocity=Rand((-1.5, -1.5, 8.0), (1.5, 1.5, 14.0)))],
		[Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 0.0)), (0.15, (1.0, 1.0, 1.0, 1.0)), (1.0, (0.6, 0.3, 0.15, 0.0)))),
		 Mod("ScaleSpriteSize", Scale=Curve((0.0, (0.8, 0.8)), (1.0, (0.4, 0.7)))),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0))],
		Sprite(1, FLAME_TEX, 4, 4), Local=True)
	Halo = Emitter("Halo", 22, 8,
		[Mod("SpawnRate", SpawnRate=Const(5.0))],
		[Init(Const(0.8), Const(1.4, 0.55, 0.14, 0.35), Rand((26.0, 26.0), (32.0, 32.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_POINT)],
		[Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 0.0)), (0.4, (1.0, 1.0, 1.0, 1.0)), (1.0, (1.0, 1.0, 1.0, 0.0))))],
		Sprite(1), Local=True)
	return {"Name": "CampfireLanternFlame", "Version": 2, "Emitters": [Halo, Flame]}


def SeaSpraySystem():
	# 바위에 부서지는 파도: 주기 5.2초 안 두 번(0초 큰 파도, 2.6초 작은 파도). 로컬 +X = 바다 쪽 → 물보라는 위·육지(-X) 쪽으로.
	# 물방울은 바다 높이(월드 Z = 0) 아래로 떨어지면 사라진다 (충돌 = 죽이기)
	Splash = Emitter("Splash", 31, 500,
		[Mod("SpawnBurstInstantaneous", SpawnCount=Rand(230.0, 270.0), SpawnTime=Const(0.0)), Mod("SpawnBurstInstantaneous", SpawnCount=Rand(120.0, 150.0), SpawnTime=Const(2.6))],
		[Init(Rand(0.8, 1.7), Rand((0.42, 0.46, 0.5, 0.5), (0.55, 0.6, 0.65, 0.75)), Rand((4.0, 4.0), (10.0, 10.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_BOX, Box=(60.0, 160.0, 10.0)),
		 Mod("AddVelocityInCone", ConeAxis=Const(-0.45, 0.0, 1.0), ConeAngle=Const(32.0), Speed=Rand(320.0, 760.0))],
		[Mod("GravityForce", Gravity=Const(0.0, 0.0, -980.0)),
		 Mod("Drag", Drag=Const(1.3)),
		 Mod("ScaleSpriteSize", Scale=Curve((0.0, (0.5, 0.5)), (1.0, (3.2, 3.2)))),
		 Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 1.0)), (0.6, (1.0, 1.0, 1.0, 0.7)), (1.0, (1.0, 1.0, 1.0, 0.0)))),
		 Mod("Collision", PlaneHeight=Const(-5.0), Restitution=Const(0.0), Friction=Const(0.0), KillOnCollide=Const(1.0))],
		Sprite(0, SMOKE_TEX, 4, 4), Sim="GPU", Duration=5.2, Bounds=((-700.0, -400.0, -100.0), (200.0, 400.0, 600.0)))
	Splash["ParticleUpdate"].append(Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0)))
	Mist = Emitter("Mist", 32, 60,
		[Mod("SpawnBurstInstantaneous", SpawnCount=Const(14.0), SpawnTime=Const(0.15)), Mod("SpawnBurstInstantaneous", SpawnCount=Const(8.0), SpawnTime=Const(2.75))],
		[Init(Rand(2.5, 4.0), Rand((0.3, 0.33, 0.36, 0.13), (0.36, 0.4, 0.44, 0.2)), Rand((110.0, 110.0), (190.0, 190.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_BOX, Box=(60.0, 160.0, 30.0), Offset=(0.0, 0.0, 120.0)),
		 Mod("AddVelocity", Velocity=Rand((-120.0, -40.0, 30.0), (-50.0, 40.0, 80.0)))],
		[Mod("Drag", Drag=Const(0.5)),
		 Mod("SpriteRotationRate", RotationRate=Rand(-10.0, 10.0)),
		 Mod("ScaleSpriteSize", Scale=Curve((0.0, (0.6, 0.6)), (1.0, (2.4, 2.4)))),
		 Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 0.0)), (0.2, (1.0, 1.0, 1.0, 1.0)), (1.0, (1.0, 1.0, 1.0, 0.0)))),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0))],
		Sprite(0, SMOKE_TEX, 4, 4), Duration=5.2)
	return {"Name": "CampfireSeaSpray", "Version": 2, "Emitters": [Mist, Splash]}


def FirefliesSystem():
	# 반딧불: 모래 언덕 풀 위 넓은 상자. 각자 다른 수명이 같은 깜빡임 곡선을 다른 빠르기로 따라가 박자가 어긋난다
	Blink = Curve((0.0, (1, 1, 1, 0)), (0.08, (1, 1, 1, 1)), (0.16, (1, 1, 1, 0.1)), (0.3, (1, 1, 1, 0)), (0.38, (1, 1, 1, 1)), (0.47, (1, 1, 1, 0.05)),
				  (0.62, (1, 1, 1, 0)), (0.7, (1, 1, 1, 0.9)), (0.8, (1, 1, 1, 0.05)), (0.9, (1, 1, 1, 0.6)), (1.0, (1, 1, 1, 0)))
	Flies = Emitter("Fireflies", 41, 400,
		[Mod("SpawnRate", SpawnRate=Const(28.0))],
		[Init(Rand(7.0, 12.0), Rand((2.2, 3.2, 0.4, 1.0), (3.4, 4.2, 0.8, 1.0)), Rand((3.0, 3.0), (4.5, 4.5))),
		 Shape(SHAPE_BOX, Box=(1400.0, 2200.0, 70.0), Offset=(0.0, 0.0, 110.0)),
		 Mod("AddVelocity", Velocity=Rand((-18.0, -18.0, -6.0), (18.0, 18.0, 6.0)))],
		[Mod("CurlNoiseForce", Strength=Const(45.0), Frequency=Const(0.005), PanSpeed=Const(6.0, 4.0, 0.0)),
		 Mod("Drag", Drag=Const(0.6)),
		 Mod("ScaleColor", Scale=Blink)],
		Sprite(1), Sim="GPU", Duration=10.0, Bounds=((-1700.0, -2500.0, -100.0), (1700.0, 2500.0, 500.0)))
	return {"Name": "CampfireFireflies", "Version": 2, "Emitters": [Flies]}


FLARE_RISE_TIME = 2.8      # 초: 로켓이 꼭대기에 닿는 시각 (= 폭발·낙하산 조명탄 시작)
FLARE_GRAVITY   = 1000.0   # cm/초²
FLARE_APEX      = 0.5 * FLARE_GRAVITY * FLARE_RISE_TIME ** 2  # 3920cm
FLARE_BURN      = 5.5      # 초: 낙하산 조명탄 타는 시간


def FlareLaunchSystem():
	# 한 번 쏘기(반복 없음, 스크립트가 Asset을 비웠다 다시 넣어 재시작): 발사 연기 → 로켓 → 꼭대기에서 붉은 별 폭발
	Speed = FLARE_GRAVITY * FLARE_RISE_TIME
	Puff = Emitter("LaunchSmoke", 51, 30,
		[Mod("SpawnBurstInstantaneous", SpawnCount=Const(18.0), SpawnTime=Const(0.0))],
		[Init(Rand(1.8, 3.0), Rand((0.35, 0.33, 0.32, 0.35), (0.45, 0.43, 0.42, 0.5)), Rand((30.0, 30.0), (50.0, 50.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_SPHERE, Radius=15.0),
		 Mod("AddVelocityInCone", ConeAxis=Const(0.0, 0.0, 1.0), ConeAngle=Const(60.0), Speed=Rand(40.0, 160.0))],
		[Mod("Drag", Drag=Const(1.5)),
		 Mod("ScaleSpriteSize", Scale=Curve((0.0, (0.6, 0.6)), (1.0, (3.0, 3.0)))),
		 Mod("ScaleColor", Scale=Curve((0.0, (4.0, 2.0, 1.4, 1.0)), (0.1, (1.0, 1.0, 1.0, 1.0)), (1.0, (1.0, 1.0, 1.0, 0.0)))),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0))],
		Sprite(0, SMOKE_TEX, 4, 4), Duration=8.0, Loop=False)
	Rocket = Emitter("Rocket", 52, 4,
		[Mod("SpawnBurstInstantaneous", SpawnCount=Const(1.0), SpawnTime=Const(0.0))],
		[Init(Const(FLARE_RISE_TIME), Const(18.0, 5.0, 2.0, 1.0), Const(14.0, 14.0)), Shape(SHAPE_POINT),
		 Mod("AddVelocity", Velocity=Const(0.0, 0.0, Speed))],
		[Mod("GravityForce", Gravity=Const(0.0, 0.0, -FLARE_GRAVITY))],
		Sprite(1, Velocity=True, Stretch=0.05), Duration=8.0, Loop=False)
	Sparks = Emitter("RocketSparks", 53, 120,
		[Mod("SpawnRate", SpawnRate=Curve((0.0, (60.0,)), (FLARE_RISE_TIME / 8.0, (60.0,)), (FLARE_RISE_TIME / 8.0 + 0.001, (0.0,)), (1.0, (0.0,))))],
		[Init(Rand(0.3, 0.7), Rand((8.0, 3.0, 1.0, 1.0), (12.0, 5.0, 1.8, 1.0)), Rand((2.0, 2.0), (3.0, 3.0))),
		 Shape(SHAPE_POINT),
		 Mod("AddVelocity", Velocity=Curve((0.0, (0.0, 0.0, Speed)), (FLARE_RISE_TIME / 8.0, (0.0, 0.0, 0.0)), (1.0, (0.0, 0.0, 0.0)))),
		 Mod("AddVelocityInCone", ConeAxis=Const(0.0, 0.0, -1.0), ConeAngle=Const(25.0), Speed=Rand(80.0, 200.0))],
		[Mod("GravityForce", Gravity=Const(0.0, 0.0, -FLARE_GRAVITY)),
		 Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 1.0)), (1.0, (0.3, 0.1, 0.05, 0.0))))],
		Sprite(1, Velocity=True, Stretch=0.02), Duration=8.0, Loop=False)
	Stars = Emitter("Burst", 54, 260,
		[Mod("SpawnBurstInstantaneous", SpawnCount=Rand(260.0, 300.0), SpawnTime=Const(FLARE_RISE_TIME))],
		[Init(Rand(1.6, 3.2), Rand((16.0, 2.6, 1.2, 1.0), (22.0, 5.5, 2.2, 1.0)), Rand((9.0, 9.0), (16.0, 16.0))),
		 Shape(SHAPE_SPHERE, Radius=12.0, Offset=(0.0, 0.0, FLARE_APEX)),
		 Mod("AddVelocityFromPoint", Origin=Const(0.0, 0.0, FLARE_APEX), Speed=Rand(550.0, 1100.0))],
		[Mod("GravityForce", Gravity=Const(0.0, 0.0, -240.0)),
		 Mod("Drag", Drag=Const(1.1)),
		 Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 1.0)), (0.4, (0.75, 0.6, 0.5, 1.0)), (0.7, (0.9, 0.4, 0.3, 0.8)), (0.8, (0.3, 0.1, 0.05, 0.6)),
									   (0.88, (0.8, 0.3, 0.2, 0.6)), (1.0, (0.1, 0.02, 0.01, 0.0))))],
		Sprite(1, Velocity=True, Stretch=0.03), Sim="GPU", Duration=8.0, Loop=False,
		Bounds=((-2500.0, -2500.0, FLARE_APEX - 2500.0), (2500.0, 2500.0, FLARE_APEX + 2000.0)))
	return {"Name": "CampfireFlareLaunch", "Version": 2, "Emitters": [Puff, Rocket, Sparks, Stars]}


def FlareHeadSystem():
	# 낙하산 조명탄 불꽃 (스크립트가 엔티티를 천천히 내린다). 반복 없음 — 탈 시간 동안만 생성
	Fire = Emitter("Flare", 61, 80,
		[Mod("SpawnRate", SpawnRate=Const(45.0))],
		[Init(Rand(0.15, 0.3), Rand((26.0, 4.0, 2.0, 1.0), (34.0, 7.0, 3.0, 1.0)), Rand((10.0, 14.0), (18.0, 24.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_SPHERE, Radius=3.0),
		 Mod("AddVelocity", Velocity=Rand((-15.0, -15.0, -10.0), (15.0, 15.0, 25.0)))],
		[Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 1.0)), (1.0, (0.4, 0.2, 0.1, 0.0)))),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0))],
		Sprite(1, FLAME_TEX, 4, 4), Duration=FLARE_BURN, Loop=False)
	Halo = Emitter("Halo", 62, 12,
		[Mod("SpawnRate", SpawnRate=Const(8.0))],
		[Init(Const(0.5), Rand((1.6, 0.2, 0.1, 0.22), (2.0, 0.3, 0.14, 0.3)), Rand((240.0, 240.0), (300.0, 300.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_POINT)],
		[Mod("ScaleColor", Scale=Curve((0.0, (1.0, 1.0, 1.0, 0.0)), (0.3, (1.0, 1.0, 1.0, 1.0)), (1.0, (1.0, 1.0, 1.0, 0.0))))],
		Sprite(1), Duration=FLARE_BURN, Loop=False, Local=True)
	Smoke = Emitter("Smoke", 63, 120,
		[Mod("SpawnRate", SpawnRate=Const(18.0))],
		[Init(Rand(3.0, 4.5), Rand((0.55, 0.22, 0.2, 0.3), (0.7, 0.3, 0.26, 0.42)), Rand((24.0, 24.0), (36.0, 36.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_SPHERE, Radius=6.0),
		 Mod("AddVelocity", Velocity=Rand((-8.0, -8.0, 20.0), (8.0, 8.0, 45.0)))],
		[Mod("AccelerationForce", Acceleration=Const(-25.0, 20.0, 8.0)),
		 Mod("Drag", Drag=Const(0.4)),
		 Mod("CurlNoiseForce", Strength=Const(50.0), Frequency=Const(0.008), PanSpeed=Const(0.0, 0.0, 10.0)),
		 Mod("ScaleSpriteSize", Scale=Curve((0.0, (0.7, 0.7)), (1.0, (5.0, 5.0)))),
		 Mod("ScaleColor", Scale=Curve((0.0, (1.6, 1.0, 1.0, 0.0)), (0.08, (1.4, 1.0, 1.0, 1.0)), (1.0, (0.6, 0.6, 0.65, 0.0)))),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0))],
		Sprite(0, SMOKE_TEX, 4, 4), Duration=FLARE_BURN, Loop=False)
	return {"Name": "CampfireFlareHead", "Version": 2, "Emitters": [Halo, Smoke, Fire]}


def WriteParticles(CampGroundZ):
	Folder = os.path.join(CONTENT, FX_DIR)
	Systems = {
		"CampfireFire": CampfireSystem(CampGroundZ),
		"CampfireLanternFlame": LanternFlameSystem(),
		"CampfireSeaSpray": SeaSpraySystem(),
		"CampfireFireflies": FirefliesSystem(),
		"CampfireFlareLaunch": FlareLaunchSystem(),
		"CampfireFlareHead": FlareHeadSystem(),
	}
	for Name, Doc in Systems.items():
		WriteJson(os.path.join(Folder, f"{Name}.eparticle"), Doc)


# ---- 소리 (절차 생성 — 끊김 없는 반복) -----------------------------------------------------------------------------
def _BandNoise(Rng, N, Rate, Center, Width):
	Spectrum = np.fft.rfft(Rng.standard_normal(N))
	Freq = np.fft.rfftfreq(N, 1.0 / Rate)
	Band = np.exp(-((np.log(np.maximum(Freq, 1.0)) - math.log(Center)) ** 2) / (2 * Width ** 2))
	Out = np.fft.irfft(Spectrum * Band, N)
	return Out / np.max(np.abs(Out))


def _WriteWav(Path, Mix, Rate):
	Pcm = (Mix / np.max(np.abs(Mix)) * 0.7 * 32767.0).astype("<i2")
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with wave.open(Path, "wb") as File:
		File.setnchannels(1)
		File.setsampwidth(2)
		File.setframerate(Rate)
		File.writeframes(Pcm.tobytes())


def WriteSounds():
	Rate = 32000
	# 모닥불: 낮은 불길 울림 + 무작위 탁탁 튀는 소리(짧은 고역 잡음 터짐)
	Rng = np.random.default_rng(9)
	N = int(10.0 * Rate)
	Roar = _BandNoise(Rng, N, Rate, 180.0, 0.8) * 0.35
	Pops = np.zeros(N)
	Hiss = _BandNoise(Rng, N, Rate, 3500.0, 0.6)
	for _ in range(110):
		Start = int(Rng.uniform(0, N))
		Length = int(Rate * Rng.uniform(0.004, 0.03))
		Env = np.exp(-np.arange(Length) / (Length * 0.25))
		Index = (Start + np.arange(Length)) % N
		Pops[Index] += Hiss[Index] * Env * Rng.uniform(0.3, 1.0)
	_WriteWav(os.path.join(CONTENT, "Audio", "Demo", "CampfireCrackle.wav"), Roar + Pops * 0.8, Rate)
	# 파도: 바다 쏴아 소리(대역 잡음) × 주기 5.2초 밀려왔다 빠지는 세기 (물보라 파티클 주기와 같음)
	Rng = np.random.default_rng(10)
	N = int(15.6 * Rate)  # 5.2초 × 3
	Wash = _BandNoise(Rng, N, Rate, 600.0, 1.1)
	Rumble = _BandNoise(Rng, N, Rate, 90.0, 0.6)
	T = np.arange(N) / Rate
	Phase = (T % 5.2) / 5.2
	Swell = np.exp(-((Phase - 0.08) % 1.0) * 6.0) * 0.9 + np.exp(-((Phase - 0.58) % 1.0) * 7.0) * 0.5 + 0.12
	_WriteWav(os.path.join(CONTENT, "Audio", "Demo", "CampfireWaves.wav"), Wash * Swell + Rumble * (0.25 + 0.3 * Swell), Rate)


# ---- 폴리지 ----------------------------------------------------------------------------------------------------------
def WriteFoliage(Path, Types):
	Doc = {"Revision": 0, "Types": [], "Version": 1}
	for Type, Instances in Types:
		Entry = dict(Type)
		Entry["InstanceCount"] = len(Instances)
		Data = b"".join(struct.pack("<8f", *Instance) for Instance in Instances)
		Entry["Instances"] = base64.b64encode(Data).decode("ascii")
		Doc["Types"].append(Entry)
	WriteJson(Path, Doc)


GRASS_TYPE = {
	"Name": "DuneGrass", "Mesh": "foliage:grass", "Material": f"{MAT_DIR}/DuneGrass.emat", "Density": 20.0,
	"MinScale": 0.4, "MaxScale": 0.8, "MaxSlope": 35.0, "MinHeight": -1.0e7, "MaxHeight": 1.0e7, "AlignToNormal": True, "RandomYaw": True,
	"ZOffset": -2.0, "CullDistance": 5000.0, "ShadowDistance": 0.0, "Collision": False, "CollisionRadius": 25.0, "CollisionHeight": 400.0,
}


# ---- 씬 배치 --------------------------------------------------------------------------------------------------------
# 잎 카드 알파가 별도 맵인 에셋: Hub(BuildHub.py ALPHA_FIX)가 쓴 알파 고친 사본 <Id>.alpha.gltf(+ .eimport)를 같이 쓴다
#   (원본 색 JPG에는 알파가 없어 잎 카드가 어두운 판이 된다)
ALPHA_FIXED = {"wild_rooibos_bush", "fern_02"}


def Model(Id):
	return AlphaModel(Id) if Id in ALPHA_FIXED else f"{PH}/{Id}/{Id}.gltf"


def FaceYaw(DX, DY):
	# Poly Haven 모델 정면 = 엔진 -X(glTF +Z) → 정면이 (DX, DY) 방향을 보게 하는 Yaw
	return math.degrees(math.atan2(-DY, -DX))


# 해 질 녘: 간이 시간대 모델(6시 일출·18시 일몰)에서 18.1시 = 해가 수평선 아래 약 1도 — 실제 여름 저녁 7시 반쯤의 박명.
#   태양 방위는 바다(+X) 조금 남쪽(-Y)으로 지도록 북쪽 방위를 맞춘다 (방위 = 북 + 90 + 낮 각도)
TIME_OF_DAY = 18.1
MAX_SUN_ELEVATION = 40.0
SUN_AZIMUTH = 352.0
NORTH_AZIMUTH = SUN_AZIMUTH - 90.0 - (TIME_OF_DAY - 6.0) / 12.0 * 180.0

CAMP = (0.0, 0.0)
PORTAL = (-1900.0, -1500.0)
SHIP = (6600.0, -2300.0)
PIER_Z = SEA_LEVEL - 170.0
PIER_DECK = PIER_Z + 267.0
LAUNCHER_OFFSET = (-1500.0, 0.0, 480.0)  # 배 기준 (고물 갑판 근처)


def BuildScene(Height, Overview=None):
	Rng = random.Random(23)
	S = FScene()
	Grass = []
	Ground = lambda X, Y: Height(X, Y)  # noqa: E731
	CampZ = Height(*CAMP)

	def Place(Name, Id, X, Y, Yaw=0.0, Scale=1.0, Sink=0.0, Z=None, Pitch=0.0, Roll=0.0, Parent=-1):
		Base = Ground(X, Y) if Z is None else Z
		S_ = Scale if isinstance(Scale, (list, tuple)) else (Scale, Scale, Scale)
		return S.Add(Name, {"ModelComponent": {"AssetPath": Model(Id)}}, (X, Y, Base - Sink), QuatFromEuler(Pitch, Yaw, Roll), S_, Parent)

	def Particles(Name, Asset, Pos, Rotation=None, Speed=1.0, Parent=-1, Playing=True):
		return S.Add(Name, {"ParticleSystemComponent": {"Asset": Asset, "Playing": Playing, "Speed": Speed}}, Pos, Rotation, (1, 1, 1), Parent)

	def Point(Name, Pos, Color, Intensity, Radius, Shadows=False, Flicker=None, Parent=-1):
		Comps = {"PointLightComponent": {"Color": list(Color), "Intensity": Intensity, "Radius": Radius, "CastShadows": Shadows}}
		if Flicker:
			Comps["ScriptComponent"] = FlickerScript(Intensity, **Flicker)
		return S.Add(Name, Comps, Pos, Parent=Parent)

	def Collider(Name, Center, Size, Yaw=0.0):
		return S.Add(Name, {"BoxColliderComponent": {"HalfExtents": [Size[0] * 0.5, Size[1] * 0.5, Size[2] * 0.5]}}, Center, QuatFromEuler(Yaw=Yaw))

	# ---- 환경: 박명 하늘(대기) + 별 + 엷은 구름 + 바다 안개(볼류메트릭 — 불빛 무리)
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.82, 0.62], "Intensity": 4.0}}, (0, 0, 3000), QuatFromEuler(Pitch=-10, Yaw=170))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {"MoonIntensity": 0.35, "MoonColor": [0.6, 0.72, 1.0], "NightSkyColor": [0.008, 0.012, 0.026], "StarIntensity": 0.8},
		"VolumetricCloudComponent": {"Coverage": 0.24, "CloudType": 0.35, "WindSpeed": 6.0, "WindDirection": 60.0},
		"TimeOfDayComponent": {"TimeOfDay": TIME_OF_DAY, "DayLengthMinutes": 0.0, "MaxSunElevation": MAX_SUN_ELEVATION,
							   "NorthAzimuth": NORTH_AZIMUTH, "AnimateInEditor": False},
		"SkyLightComponent": {"Intensity": 4.0},
		"HeightFogComponent": {
			"Color": [0.09, 0.1, 0.14], "Density": 0.0018, "HeightFalloff": 0.05, "StartDistance": 0.0, "MaxOpacity": 0.75,
			"DirectionalInscatteringColor": [0.0, 0.0, 0.0], "Volumetric": True, "VolumetricDistance": 6000.0,
			"VolumetricAlbedo": [0.9, 0.92, 0.95], "VolumetricExtinctionScale": 1.0, "VolumetricAnisotropy": 0.4,
			"VolumetricDirectionalScale": 0.6, "VolumetricLocalLightScale": 0.25},
	})

	# ---- 지형 + 폴리지 + 바다 (지형 전체를 덮는 물 상자 하나 — 수면 위 지형이 가린다)
	S.Add("Terrain", {"TerrainComponent": {
		"Asset": "Terrain/Demo/Campfire.eterrain", "Size": [TERRAIN_SIZE, TERRAIN_SIZE], "HeightRange": HEIGHT_RANGE,
		"Layer0Material": f"{MAT_DIR}/TerrainSand.emat", "Layer1Material": f"{MAT_DIR}/TerrainWetSand.emat",
		"Layer2Material": f"{MAT_DIR}/TerrainRock.emat", "Layer3Material": f"{MAT_DIR}/TerrainGrass.emat",
		"Layer0Tiling": 400.0, "Layer1Tiling": 350.0, "Layer2Tiling": 900.0, "Layer3Tiling": 600.0,
		"CastShadows": True, "Collision": True}})
	S.Add("Foliage", {"FoliageComponent": {"Asset": "Foliage/Demo/Campfire.efoliage", "Visible": True}})
	S.Add("Sea", {"WaterBodyComponent": {
		"Size": [80000.0, 80000.0, 1400.0], "ScatterColor": [0.012, 0.05, 0.065], "Absorption": [0.42, 0.1, 0.07],
		"NormalStrength": 0.5, "WaveScale": 380.0, "WaveSpeed": 16.0, "FlowDirection": 0.0, "FlowSpeed": 0.0,
		"FoamIntensity": 0.9, "FoamDistance": 30.0, "RefractionStrength": 0.04, "ReflectionIntensity": 1.0, "Roughness": 0.06}},
		(28000.0, 0.0, SEA_LEVEL - 700.0))
	S.Add("WavesSound", {"AudioSourceComponent": {
		"ClipAsset": "Audio/Demo/CampfireWaves.wav", "Volume": 0.7, "Pitch": 1.0, "Loop": True, "PlayOnStart": True,
		"Spatial": True, "MinDistance": 800.0, "MaxDistance": 6000.0}}, (ShoreXf(0.0) + 200.0, 0.0, 50.0))

	# ---- 겹침 방지
	Occupied = []

	def Free(X, Y, Radius):
		return all((X - OX) ** 2 + (Y - OY) ** 2 >= (Radius + OR) ** 2 for OX, OY, OR in Occupied)

	def Reserve(X, Y, Radius):
		Occupied.append((X, Y, Radius))

	# ---- 모닥불 (만 가운데 모래밭): 돌 화덕 + 장작 + 냄비, 둘레에 통나무 의자
	CX, CY = CAMP
	Place("FirePit", "stone_fire_pit", CX, CY, 15.0, 1.0, Sink=12.0)
	# 장작: Forest가 나눈 마른 가지 변형(.part.gltf — 원본은 변형 3개가 한 파일에 나란히)을 화덕 안에 엇갈려 쌓는다
	def Branch(Name, Variant, X, Y, Z, Yaw, Pitch=0.0, Scale=1.0):
		return S.Add(Name, {"ModelComponent": {"AssetPath": f"{PH}/dry_branches_medium_01/dry_branches_medium_01_{Variant}.part.gltf"}},
					 (X, Y, Z), QuatFromEuler(Pitch=Pitch, Yaw=Yaw), (Scale, Scale, Scale))
	for Index, (Variant, Yaw, Pitch) in enumerate((("b", 10.0, 0.0), ("c", 70.0, 0.0), ("b", 130.0, 6.0), ("c", 190.0, -6.0), ("c", 250.0, 8.0))):
		Branch(f"FirePit_Wood_{Index}", Variant, CX, CY, CampZ - 4.0 + Index * 3.0, Yaw, Pitch, 0.6)
	Particles("Campfire_FX", f"{FX_DIR}/CampfireFire.eparticle", (CX, CY, CampZ + 2.0))
	Point("Campfire_Light", (CX, CY, CampZ + 70.0), (1.0, 0.5, 0.18), 18.0, 1500.0, Shadows=True, Flicker={"Style": "Fire", "Seed": 4})
	Point("Campfire_Light_Low", (CX + 10.0, CY, CampZ + 35.0), (1.0, 0.36, 0.08), 7.0, 500.0, Flicker={"Style": "Fire", "Seed": 8, "Speed": 1.6})
	S.Add("Campfire_Sound", {"AudioSourceComponent": {
		"ClipAsset": "Audio/Demo/CampfireCrackle.wav", "Volume": 0.8, "Pitch": 1.0, "Loop": True, "PlayOnStart": True,
		"Spatial": True, "MinDistance": 200.0, "MaxDistance": 2500.0}}, (CX, CY, CampZ + 40.0))
	Collider("FirePit_Collision", (CX, CY, CampZ + 20.0), (140.0, 140.0, 40.0))
	Reserve(CX, CY, 420.0)
	# 냄비는 화덕 돌 위
	Place("FirePit_Pot", "pot_enamel_01", CX - 8.0, CY + 58.0, 70.0, 1.0, Z=CampZ + 10.0, Roll=-6.0)
	# 통나무 의자 셋 (불을 둘러싸게) + 앉을 자리 소품
	Logs = [("Log_West", "dead_tree_trunk", -240.0, 20.0, 4.0, 1.0, 3.0), ("Log_South", "dead_tree_trunk", 40.0, -250.0, 92.0, 1.0, 3.0),
			("Log_North", "dead_tree_trunk_02", 70.0, 290.0, 76.0, 0.75, 12.0)]
	for Name, Id, DX, DY, Yaw, Scale, Sink in Logs:
		Place(Name, Id, CX + DX, CY + DY, Yaw, Scale, Sink=Sink)
	Place("Hat", "fishermans_hat", CX - 236.0, CY + 70.0, 30.0, 1.0, Z=CampZ + 23.0, Roll=4.0)
	Place("Jug", "jug_01", CX - 205.0, CY - 95.0, 200.0, 1.0)
	Place("Stool_A", "folding_wooden_stool", CX + 205.0, CY + 150.0, FaceYaw(-205, -150) + 180.0, 1.0)
	Place("Stool_B", "wooden_stool_01", CX + 225.0, CY - 120.0, 20.0, 1.0)
	Place("Basket", "wicker_basket_01", CX + 150.0, CY - 255.0, 10.0, 1.0)
	Place("ChopStump", "tree_stump_01", CX - 160.0, CY - 330.0, 50.0, 0.6, Sink=4.0)
	Place("Hatchet", "hatchet", CX - 150.0, CY - 330.0, 70.0, 1.0, Z=Ground(CX - 160.0, CY - 330.0) + 22.0, Pitch=0.0, Roll=90.0)
	# 장작더미 (패 놓은 가지를 나란히 쌓음)
	for Index in range(7):
		Row, Col = divmod(Index, 4)
		Branch(f"Firewood_{Index}", "bc"[Index % 2], CX - 300.0 + (Col - 1.5) * 13.0 + Row * 6.0 + Rng.uniform(-3.0, 3.0), CY - 250.0,
			   Ground(CX - 300.0, CY - 250.0) + 1.0 + Row * 11.0, 105.0 + Rng.uniform(-8.0, 8.0))

	# ---- 밀수품 더미 (모닥불 뒤 육지 쪽): 통 무더기, 상자, 보물 상자, 등불, 구명환, 대포
	Place("Cache_Barrels", "wooden_barrels_01", -760.0, 560.0, 205.0, 1.0, Sink=4.0)
	Collider("Cache_Barrels_Collision", (-760.0, 560.0, Ground(-760, 560) + 50.0), (380.0, 420.0, 100.0), 205.0)
	Crate = Place("Cache_Crate_A", "wooden_crate_02", -420.0, 360.0, 115.0, 1.0, Sink=2.0)
	Place("Cache_Crate_B", "wooden_crate_01", -10.0, 8.0, 25.0, 1.0, Z=44.0, Parent=Crate)
	Place("Cache_Lantern", "wooden_lantern_01", 34.0, -6.0, 0.0, 1.0, Z=44.0, Parent=Crate)
	Place("Cache_Crate_C", "wooden_crate_02", -470.0, 470.0, 160.0, 1.0, Sink=2.0)
	Place("Cache_Chest", "treasure_chest", -260.0, 420.0, FaceYaw(260, -420), 1.0, Sink=3.0)
	Place("Cache_WineBarrel", "wine_barrel_01", -560.0, 200.0, 40.0, 1.0, Sink=2.0)
	Place("Cache_Barrel", "barrel_03", -610.0, 290.0, 0.0, 1.0, Sink=2.0)
	Place("Cache_Lifebuoy", "lifebuoy", -545.0, 180.0, 20.0, 1.0, Z=Ground(-545, 180) + 44.0, Roll=12.0, Pitch=-5.0)
	Place("Cache_Bucket", "wooden_bucket_01", -360.0, 230.0, 0.0, 1.0)
	Place("Cache_Cannon", "cannon_01", -350.0, -720.0, FaceYaw(1.0, 0.25), 1.0, Sink=3.0)
	Collider("Cache_Collision", (-450.0, 400.0, Ground(-450, 400) + 45.0), (260.0, 220.0, 90.0), 30.0)
	for X, Y, R in ((-760.0, 560.0, 260.0), (-450.0, 400.0, 170.0), (-560.0, 230.0, 80.0), (-350.0, -720.0, 150.0), (-160.0, -330.0, 70.0)):
		Reserve(X, Y, R)
	# 등불 (상자 위) + 기둥 등불 둘(그루터기 위) — 심지 불꽃 파티클 + 깜빡이는 작은 점광원
	Lanterns = [("Cache", -420.0 + 34.0 * math.cos(math.radians(115)) + 6.0 * math.sin(math.radians(115)),
				 360.0 + 34.0 * math.sin(math.radians(115)) - 6.0 * math.cos(math.radians(115)), Ground(-420, 360) - 2.0 + 44.0)]
	for Index, (X, Y) in enumerate(((-1321.0, -940.0), (-1125.0, -1168.0), (720.0, -620.0))):  # 시작 길 양옆 둘 + 부두 쪽
		Place(f"LanternPost_{Index}", "tree_stump_01", X, Y, Rng.uniform(0, 360), 0.5, Sink=6.0)
		Base = Ground(X, Y) - 6.0 + 19.0
		Place(f"LanternPost_{Index}_Lantern", "wooden_lantern_01", X, Y, Rng.uniform(0, 360), 1.2, Z=Base)
		Lanterns.append((f"Post{Index}", X, Y, Base))
		Reserve(X, Y, 90.0)
	# 작은 나무 부두 (배에서 오는 거룻배 선착장) + 끝에 등불
	PierY = -1750.0
	PierX = ShoreXf(PierY) - 550.0 + 705.0
	S.Model("Pier", Model("modular_wooden_pier"), (PierX, PierY, PIER_Z), 0.0, 1.0)  # 데크 윗면 = 원점 + 267cm
	Place("Pier_Barrel", "barrel_03", PierX + 900.0, PierY - 90.0, 0.0, 1.0, Z=PIER_DECK)
	Place("Pier_Crate", "wooden_crate_01", PierX + 780.0, PierY + 80.0, 20.0, 1.0, Z=PIER_DECK)
	Place("Pier_Lantern", "wooden_lantern_01", PierX + 1120.0, PierY + 100.0, 0.0, 1.2, Z=PIER_DECK)
	Lanterns.append(("Pier", PierX + 1120.0, PierY + 100.0, PIER_DECK))
	Reserve(PierX - 500.0, PierY, 200.0)
	for Index, (Name, X, Y, Base) in enumerate(Lanterns):
		Scale = 1.0 if Name == "Cache" else 1.2
		Particles(f"Lantern_{Name}_Flame", f"{FX_DIR}/CampfireLanternFlame.eparticle", (X, Y, Base + 17.0 * Scale))
		Point(f"Lantern_{Name}_Light", (X, Y, Base + 22.0 * Scale), (1.0, 0.6, 0.28), 2.5, 550.0, Flicker={"Style": "Fire", "Seed": 20 + Index, "Amount": 0.2})

	# ---- 물가: 떠밀려 온 통나무, 조개, 바위(물보라), 갈매기 대신 부표
	Place("Driftwood_A", "dead_tree_trunk_02", ShoreXf(-1000.0) - 260.0, -1000.0, 25.0, 1.0, Sink=25.0)
	Place("Driftwood_B", "dead_tree_trunk", ShoreXf(1500.0) - 180.0, 1500.0, -30.0, 1.3, Sink=6.0)
	Place("Driftwood_C", "dead_tree_trunk", ShoreXf(-2600.0) - 420.0, -2600.0, 70.0, 1.1, Sink=6.0)
	for Index in range(7):
		Y = Rng.uniform(-2500.0, 2500.0)
		X = ShoreXf(Y) - Rng.uniform(80.0, 420.0)
		Place(f"Shell_{Index}", "lambis_shell", X, Y, Rng.uniform(0, 360), Rng.uniform(1.0, 1.6), Sink=1.0)
	# 물가 바위 무더기: 만 양 끝 (곶 발치) — 파도가 부서진다
	SprayRocks = [(-3400.0, 1.6, 30.0), (3500.0, 1.5, 210.0), (-4700.0, 1.4, 120.0), (4800.0, 1.3, 300.0)]
	for Index, (Y, Scale, Yaw) in enumerate(SprayRocks):
		X = ShoreXf(Y) + 150.0
		Place(f"ShoreRock_{Index}", "coast_rocks_05", X, Y, Yaw, Scale * 1.3, Z=-35.0)
		Particles(f"ShoreRock_{Index}_Spray", f"{FX_DIR}/CampfireSeaSpray.eparticle", (X + 180.0, Y, 20.0), Speed=0.85 + 0.1 * Index)
	for Index in range(9):
		Y = Rng.choice([-1, 1]) * Rng.uniform(2600.0, 5600.0)
		X = ShoreXf(Y) + Rng.uniform(-500.0, 300.0)
		Place(f"BeachRock_{Index}", "sand_rocks_small_01", X, Y, Rng.uniform(0, 360), Rng.uniform(0.6, 1.1), Sink=8.0)
	# 곶: 안쪽 면 절벽 + 곶 끝 절벽 + 곶 아래 큰 바위
	for Side in (-1, 1):
		S.Model(f"Cliff_Inner_{Side}", Model("coastal_cliff_01"), (4600.0, Side * (HEADLAND_Y - 150.0), -150.0), -90.0 * Side, 1.3)
		S.Model(f"Cliff_Tip_{Side}", Model("coastal_cliff_02"), (HEADLAND_TIP + 150.0, Side * (HEADLAND_Y + 2600.0), -250.0), 0.0, 1.6)
		Place(f"CliffRock_{Side}", "coast_land_rocks_03", 7200.0, Side * (HEADLAND_Y - 600.0), 40.0 * Side, 1.6, Z=-80.0)
	# 신호 화톳불 (북쪽 곶 위 — 만 건너편에 보인다)
	BX, BY = 5600.0, HEADLAND_Y + 900.0
	BZ = Ground(BX, BY)
	Place("Beacon", "barrel_stove", BX, BY, 0.0, 1.2, Sink=3.0)
	Particles("Beacon_Fire", "Particles/Demo/AlleyStoveFire.eparticle", (BX, BY, BZ + 95.0))
	Particles("Beacon_Smoke", "Particles/Demo/AlleyStoveSmoke.eparticle", (BX, BY, BZ + 130.0))
	Point("Beacon_Light", (BX, BY, BZ + 150.0), (1.0, 0.45, 0.14), 30.0, 1500.0, Flicker={"Style": "Fire", "Seed": 31})

	# ---- 배 (만 입구에 닻을 내린 밀수선) + 고물 등불 + 신호 조명탄 발사대
	SX, SY = SHIP
	S.Model("Ship", Model("ship_pinnace"), (SX, SY, SEA_LEVEL - 70.0), 63.0, 1.0)
	Yaw = math.radians(63.0)

	def ShipPoint(DX, DY, DZ):
		return (SX + DX * math.cos(Yaw) - DY * math.sin(Yaw), SY + DX * math.sin(Yaw) + DY * math.cos(Yaw), SEA_LEVEL - 70.0 + DZ)
	Point("Ship_SternLantern", ShipPoint(-1750.0, 0.0, 620.0), (1.0, 0.62, 0.3), 14.0, 900.0, Flicker={"Style": "Fire", "Seed": 41, "Amount": 0.15})
	Point("Ship_DeckLantern", ShipPoint(300.0, 0.0, 420.0), (1.0, 0.6, 0.28), 8.0, 700.0, Flicker={"Style": "Fire", "Seed": 42, "Amount": 0.15})
	Launcher = ShipPoint(*LAUNCHER_OFFSET)
	S.Add("Flare_Launcher", {
		"ParticleSystemComponent": {"Asset": "", "Playing": True, "Speed": 1.0},
		"ScriptComponent": {"ScriptAsset": "Scripts/Demo/CampfireFlare.lua", "ExecutionLocation": 2, "PropertyOverrides": json.dumps({
			"LaunchAsset": {"Asset": f"{FX_DIR}/CampfireFlareLaunch.eparticle"}, "HeadAsset": {"Asset": f"{FX_DIR}/CampfireFlareHead.eparticle"},
			"Apex": FLARE_APEX, "RiseTime": FLARE_RISE_TIME, "BurnTime": FLARE_BURN, "Intensity": 900.0}, ensure_ascii=False)}},
		Launcher)
	S.Add("Flare_Light", {
		"PointLightComponent": {"Color": [1.0, 0.18, 0.08], "Intensity": 0.0, "Radius": 9000.0, "CastShadows": False},
		"ParticleSystemComponent": {"Asset": "", "Playing": True, "Speed": 1.0}},
		(Launcher[0], Launcher[1], Launcher[2] + FLARE_APEX))
	# 항로 부표 (초록 등)
	Place("Buoy", "ocean_buoy", 4300.0, 2300.0, 30.0, 1.0, Z=SEA_LEVEL - 45.0, Roll=4.0)
	Point("Buoy_Light", (4300.0, 2300.0, SEA_LEVEL + 130.0), (0.3, 1.0, 0.45), 3.0, 700.0, Flicker={"Style": "Neon", "Seed": 51})

	# ---- 모래 언덕: 덤불 + 엔진 폴리지 풀 + 반딧불
	for Index in range(40):
		for _ in range(40):
			Y = Rng.uniform(-5600.0, 5600.0)
			X = ShoreXf(Y) - Rng.uniform(1500.0, 5200.0)
			if Free(X, Y, 120.0) and Height.SlopeAt(X, Y) < 0.5 and math.hypot(X - PORTAL[0], Y - PORTAL[1]) > 400.0:
				break
		Id = Rng.choice(["wild_rooibos_bush", "shrub_02", "fern_02"]) if Index % 3 else "wild_rooibos_bush"
		Place(f"DuneBush_{Index}", Id, X, Y, Rng.uniform(0, 360), Rng.uniform(0.6, 1.0), Sink=6.0)
		Reserve(X, Y, 100.0)
	GrassRng = np.random.default_rng(29)
	for X, Y in GrassRng.uniform(-12000.0, 12000.0, size=(160000, 2)):
		Inland = ShoreXf(Y) - X
		if Inland < 1100.0 or abs(Y) > HEADLAND_Y + 4000.0:
			continue
		Keep = float(Smoothstep(1100.0, 2400.0, Inland)) * (0.5 if Inland < 6000.0 else 0.2)
		if GrassRng.random() > Keep or Height.SlopeAt(X, Y) > 0.55 or not Free(X, Y, 60.0):
			continue
		if math.hypot(X - PORTAL[0], Y - PORTAL[1]) < 350.0:
			continue
		Z = Height(X, Y)
		N = Height.Normal(X, Y)
		Grass.append((float(X), float(Y), Z, float(GrassRng.uniform(0, 360)), float(GrassRng.uniform(0.4, 0.8)), float(N[0]), float(N[1]), float(N[2])))
	Particles("Fireflies", f"{FX_DIR}/CampfireFireflies.eparticle", (-2600.0, 0.0, Ground(-2600.0, 0.0)))

	# ---- 돌아가는 포털 (모래 언덕 발치, 바다를 본다)
	PX, PY = PORTAL
	Portal = S.Add("Portal_Hub", {"ScriptComponent": {"ScriptAsset": "Scripts/Demo/DemoPortal.lua", "ExecutionLocation": 0,
		"PropertyOverrides": json.dumps({"TargetScene": {"Asset": "Scenes/Demo/Hub.escene"}, "Label": "Hub로", "Ready": True,
			"RememberReturn": False}, ensure_ascii=False)}},
		(PX, PY, Ground(PX, PY)), QuatFromEuler(Yaw=FaceYaw(-1.0, -0.3) + 180.0))
	S.Model("Portal_Hub_Door", Model("large_castle_door"), (0, 0, -3), 0.0, 1.0, Parent=Portal)
	for Side in (-1, 1):
		S.Model("Portal_Hub_Post", Model("tree_stump_01"), (40, Side * 150, -25), 40.0 * Side, 0.45, Parent=Portal)
		S.Model("Portal_Hub_Lantern", Model("wooden_lantern_01"), (40, Side * 150, 8), 15.0 * Side, 1.3, Parent=Portal)
	S.Add("Portal_Hub_Glow", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Demo/PortalGlow.emat"}},
		(18, 0, 148), None, (0.02, 1.85, 2.85), Parent=Portal)
	S.Add("Portal_Hub_Light", {"PointLightComponent": {"Color": [0.5, 0.75, 1.0], "Intensity": 1.5, "Radius": 350.0, "CastShadows": False}},
		(80, 0, 220), Parent=Portal)

	if Overview:
		S.Add("OverviewCamera", {"CameraComponent": {"FovYDegrees": 70.0, "NearZ": 10.0, "FarZ": 60000.0, "Primary": True, "Priority": 100}},
			Overview[:3], QuatFromEuler(Pitch=Overview[3], Yaw=Overview[4]))
	else:
		# 플레이어: 포털 앞에서 모닥불과 바다(배)를 보며 시작 — 프리팹 인스턴스는 루트만 저장 (PrefabLink.Root = 자기 인덱스)
		StartX, StartY = PX + 450.0, PY + 250.0
		PlayerIndex = len(S.Entities)
		S.Add("Player", {
			"PrefabInstanceComponent": {"Asset": "Prefabs/Demo/DemoPlayer.eprefab", "Overrides": ""},
			"PrefabLinkComponent": {"Id": "1", "Root": PlayerIndex}},
			(StartX, StartY, Ground(StartX, StartY) + 110.0), QuatFromEuler(Yaw=math.degrees(math.atan2(CY - StartY, CX - StartX))))
	return S, Grass


def ShoreXf(Y):
	return float(ShoreX(np.float64(Y)))


def FlickerScript(Intensity, Style="Fire", Seed=1, Amount=0.35, Speed=1.0):
	return {"ScriptAsset": "Scripts/Demo/FlickerLight.lua", "ExecutionLocation": 2,
			"PropertyOverrides": json.dumps({"Style": Style, "BaseIntensity": Intensity, "Seed": Seed, "Amount": Amount, "Speed": Speed}, ensure_ascii=False)}


# 확인용 시점: 이름 → (X, Y, 지면 위 높이, Pitch, Yaw). --overview=<이름>[,<이름>...] 또는 --overview=x,y,높이,pitch,yaw
OVERVIEW_VIEWS = {
	"start":  (PORTAL[0] + 300.0, PORTAL[1] + 150.0, 170.0, -6.0, 38.0),     # 시작 자리: 모닥불 너머 바다·배
	"camp":   (-620.0, -380.0, 150.0, -12.0, 32.0),                          # 모닥불 가까이
	"fire":   (-240.0, -150.0, 95.0, -8.0, 32.0),                            # 불꽃·불티·연기 근접
	"wide":   (-3600.0, -3400.0, 650.0, -8.0, 38.0),                         # 모래 언덕 위에서 만 전체
	"spray":  (2500.0, -2400.0, 180.0, -5.0, -30.0),                         # 바위 물보라
	"cache":  (-150.0, 120.0, 140.0, -14.0, 145.0),                          # 밀수품 더미 쪽
	"pier":   (1200.0, -2600.0, 250.0, -8.0, 40.0),                          # 부두와 배
	"flare":  (-300.0, -200.0, 150.0, 10.0, -16.0),                          # 배 위 신호 조명탄
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
				Views.append(("Overview", tuple(float(V_) for V_ in Value.split(","))))
	X, Y, H = BuildHeights()
	Weights, Slope = BuildWeights(X, Y, H)
	WriteTerrain(os.path.join(CONTENT, "Terrain", "Demo", "Campfire.eterrain"), H, Weights)
	Sampler = FHeightSampler(H, Slope)
	WriteMaterials()
	WriteParticleTextures()
	WriteParticles(Sampler(*CAMP))
	WriteSounds()
	Scene, Grass = BuildScene(Sampler)
	WriteFoliage(os.path.join(CONTENT, "Foliage", "Demo", "Campfire.efoliage"), [(GRASS_TYPE, Grass)])
	Scene.Save(os.path.join(CONTENT, "Scenes", "Demo", "Campfire.escene"))
	print(f"Campfire 생성: 엔티티 {len(Scene.Entities)}개, 풀 {len(Grass)}개, 지형 {TERRAIN_RES}² 높이 {H.min():.0f}~{H.max():.0f}cm, "
		  f"모닥불 지면 {Sampler(*CAMP):.0f}cm, 북쪽 방위 {NORTH_AZIMUTH:.1f}")
	for Name, View in Views:
		View = (View[0], View[1], Sampler(View[0], View[1]) + View[2], View[3], View[4])
		OverviewScene, _ = BuildScene(Sampler, View)
		OverviewScene.Save(os.path.join(CONTENT, "Scenes", "Demo", f"_Campfire{Name}.escene"))
		print(f"확인용 변형: Scenes/Demo/_Campfire{Name}.escene (커밋하지 않음)")


if __name__ == "__main__":
	Main()
