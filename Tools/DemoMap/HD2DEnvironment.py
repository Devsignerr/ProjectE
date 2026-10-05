# 데모 서브맵 "HD2D" 배경 꾸미기 도우미 (BuildHD2D.py가 부른다) — 옥토패스 트래블러풍 디오라마 마을
#   머티리얼: 월드 좌표 박스 투영 그래프(HD2DTriplanar) 인스턴스로 내장 큐브 건축면(회벽·목재·석재·지붕)의 텍스처 밀도를 크기와 무관하게 맞춘다
#             (축 정렬 면 기준 — 건물은 Yaw 0/±90 근처로 놓는다. 기울어진 지붕은 가까운 축 면으로 투영)
#   건물: 내장 큐브로 짓는 반목조(하프팀버) 집 — 석재 1층 + 내민 회벽 2층 + 목재 골조(기둥·띠·가새) + 창(불 켠 창 = 발광) + 덧창·화분 + 박공 지붕
#         박공 삼각형 = 45도 돌린 큐브(마름모 기둥)를 부모의 Z 비균등 스케일로 눌러 지붕 기울기에 맞춘다(법선 = 역전치 — 엔진이 올바르게 변환)
#   그 밖: 노점(줄무늬 차양), 깃발 줄, 빨랫줄, 나무 울타리, 돌 옹벽·계단, 나무 다리, 폭포, 환경 파티클(꽃잎·빛 먼지·굴뚝 연기·폭포 물보라)
#   좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 카메라는 +Y 쪽에서 -Y를 본다 → 건물 앞면 = 로컬 +Y (Yaw로 돌림)
#   결정적: 무작위는 호출자가 넘긴 random.Random만 쓴다
import json
import math
import os

import numpy as np
from PIL import Image

from SceneBuilder import QuatFromEuler
from BuildCampfire import Const, Curve, Emitter, Init, Mod, Rand, Shape, Sprite as PSprite, SHAPE_BOX, SHAPE_CYLINDER, SHAPE_SPHERE, _Noise2  # noqa: E402

PH      = "Asset/PolyHaven"
MAT_DIR = "Materials/Demo/HD2D"
FX_DIR  = "Particles/Demo/HD2D"
CUBE    = "primitive:cube"
SPHERE  = "primitive:sphere"


def WriteJson(Path, Doc):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")


def Mat(Name):
	return f"{MAT_DIR}/{Name}.emat"


# ---- 머티리얼 -------------------------------------------------------------------------------------------------------
def TriplanarGraph(VertexColor=False):
	# 월드 좌표 박스 투영: 위를 보는 면(|N.z| > 0.75) = XY, 그 밖은 X를 보면 YZ, Y를 보면 XZ (경계는 날카롭게 — 큐브 면 단위라 섞일 일 없음)
	Nodes = [
		{"Id": "pos", "Type": "WorldPosition"},
		{"Id": "scale", "Type": "ScalarParameter", "Parameter": "WorldScale"},
		{"Id": "p", "Type": "Multiply", "Inputs": {"A": "pos", "B": "scale"}},
		{"Id": "ps", "Type": "Split", "Inputs": {"A": "p"}},
		{"Id": "nz", "Type": "Multiply", "Inputs": {"A": "ps:2", "B": -1.0}},
		{"Id": "uvXY", "Type": "Append", "Inputs": {"A": "ps:0", "B": "ps:1"}},
		{"Id": "uvYZ", "Type": "Append", "Inputs": {"A": "ps:1", "B": "nz"}},
		{"Id": "uvXZ", "Type": "Append", "Inputs": {"A": "ps:0", "B": "nz"}},
		{"Id": "n", "Type": "WorldNormal"},
		{"Id": "an", "Type": "Abs", "Inputs": {"A": "n"}},
		{"Id": "ns", "Type": "Split", "Inputs": {"A": "an"}},
		{"Id": "dxy", "Type": "Subtract", "Inputs": {"A": "ns:0", "B": "ns:1"}},
		{"Id": "dxyS", "Type": "Multiply", "Inputs": {"A": "dxy", "B": 60.0}},
		{"Id": "mx", "Type": "Saturate", "Inputs": {"A": "dxyS"}},
		{"Id": "uvWall", "Type": "Lerp", "Inputs": {"A": "uvXZ", "B": "uvYZ", "Alpha": "mx"}},
		{"Id": "dz", "Type": "Subtract", "Inputs": {"A": "ns:2", "B": 0.75}},
		{"Id": "dzS", "Type": "Multiply", "Inputs": {"A": "dz", "B": 60.0}},
		{"Id": "mz", "Type": "Saturate", "Inputs": {"A": "dzS"}},
		{"Id": "uv", "Type": "Lerp", "Inputs": {"A": "uvWall", "B": "uvXY", "Alpha": "mz"}},
		{"Id": "col", "Type": "TextureSample", "Texture": "Albedo", "Inputs": {"UV": "uv"}},
		{"Id": "arm", "Type": "TextureSample", "Texture": "Arm", "Inputs": {"UV": "uv"}},
		{"Id": "nrm", "Type": "TextureSample", "Texture": "Normal", "Inputs": {"UV": "uv"}},
		{"Id": "tint", "Type": "VectorParameter", "Parameter": "Tint"},
		{"Id": "base0", "Type": "Multiply", "Inputs": {"A": "col:1", "B": "tint:1"}},
		{"Id": "rs", "Type": "ScalarParameter", "Parameter": "RoughnessScale"},
		{"Id": "rough", "Type": "Multiply", "Inputs": {"A": "arm:3", "B": "rs"}},
	]
	if VertexColor:
		# 엔진 폴리지 메시(foliage:tree/pine/bush)는 정점 색(줄기 갈색·잎 초록, 어두움)을 가진다 → 텍스처 × 정점 색 × Tint
		Nodes += [{"Id": "vc", "Type": "VertexColor"}, {"Id": "base", "Type": "Multiply", "Inputs": {"A": "base0", "B": "vc:1"}}]
	else:
		Nodes.append({"Id": "base", "Type": "Multiply", "Inputs": {"A": "base0", "B": 1.0}})
	return {
		"Name": "HD2DTriplanarVC" if VertexColor else "HD2DTriplanar", "BlendMode": "Opaque",
		"Parameters": [
			{"Name": "Albedo", "Type": "Texture", "Value": "", "Usage": "Color"},
			{"Name": "Arm", "Type": "Texture", "Value": "", "Usage": "Linear"},
			{"Name": "Normal", "Type": "Texture", "Value": "", "Usage": "Normal"},
			{"Name": "WorldScale", "Type": "Scalar", "Value": 0.005},
			{"Name": "Tint", "Type": "Vector", "Value": [1.0, 1.0, 1.0, 1.0]},
			{"Name": "RoughnessScale", "Type": "Scalar", "Value": 1.0},
		],
		# 폴리지 메시는 UV가 없어 탄젠트가 없다 → 노멀 맵은 정점 색 변형에서 쓰지 않는다
		"Graph": {"Nodes": Nodes, "Output": dict({"BaseColor": "base", "Roughness": "rough", "AmbientOcclusion": "arm:2", "Metallic": 0.0},
												 **({} if VertexColor else {"Normal": "nrm:1"}))},
	}


def TriplanarInstance(Name, Stem, Size, Tint, Rough=1.0, Parent="HD2DTriplanar.emat"):
	# Stem: Content 기준이 아닌 이 머티리얼 폴더 기준 "<폴더>/<줄기>" (…_diff_2k.jpg 등을 붙인다), Size = 텍스처 한 장이 덮는 cm
	Rel = f"../../../{PH}/{Stem}"
	return {"Name": f"HD2D{Name}", "Parent": Parent, "Parameters": [
		{"Name": "Albedo", "Type": "Texture", "Value": f"{Rel}_diff_2k.jpg"},
		{"Name": "Arm", "Type": "Texture", "Value": f"{Rel}_arm_2k.jpg"},
		{"Name": "Normal", "Type": "Texture", "Value": f"{Rel}_nor_gl_2k.jpg"},
		{"Name": "WorldScale", "Type": "Scalar", "Value": 1.0 / Size},
		{"Name": "Tint", "Type": "Vector", "Value": list(Tint) + [1.0]},
		{"Name": "RoughnessScale", "Type": "Scalar", "Value": Rough},
	]}


def Plain(Name, Base, Rough=0.8, Emissive=(0.0, 0.0, 0.0), Texture="", Blend=None, Metallic=0.0):
	Doc = {"Name": f"HD2D{Name}"}
	if Blend:
		Doc["BlendMode"] = Blend
	Doc.update({"BaseColorFactor": list(Base), "EmissiveFactor": list(Emissive), "Metallic": Metallic, "Roughness": Rough,
				"NormalScale": 1.0, "OcclusionStrength": 1.0, "BaseColorTexture": Texture, "MetallicRoughnessTexture": "",
				"NormalTexture": "", "OcclusionTexture": "", "EmissiveTexture": ""})
	return Doc


def WaterfallGraph():
	# 폭포 면: 세로 줄무늬 노이즈 두 겹을 아래로 흘리고(Panner) 밝은 줄 = 거품, 알파 = 줄무늬 (반투명, 큐브 앞면 UV)
	Nodes = [
		{"Id": "uv0", "Type": "TexCoord"},
		{"Id": "uvA", "Type": "Multiply", "Inputs": {"A": "uv0", "B": [1.6, 0.35]}},
		{"Id": "panA", "Type": "Panner", "Inputs": {"UV": "uvA", "Speed": [0.0, -0.55]}},
		{"Id": "uvB", "Type": "Multiply", "Inputs": {"A": "uv0", "B": [2.6, 0.7]}},
		{"Id": "panB", "Type": "Panner", "Inputs": {"UV": "uvB", "Speed": [0.03, -1.05]}},
		{"Id": "a", "Type": "TextureSample", "Texture": "Streaks", "Inputs": {"UV": "panA"}},
		{"Id": "b", "Type": "TextureSample", "Texture": "Streaks", "Inputs": {"UV": "panB"}},
		{"Id": "sum", "Type": "Add", "Inputs": {"A": "a:2", "B": "b:2"}},
		{"Id": "half", "Type": "Subtract", "Inputs": {"A": "sum", "B": 0.95}},
		{"Id": "sharp", "Type": "Multiply", "Inputs": {"A": "half", "B": 2.2}},
		{"Id": "foam", "Type": "Saturate", "Inputs": {"A": "sharp"}},
		{"Id": "deep", "Type": "VectorParameter", "Parameter": "DeepColor"},
		{"Id": "white", "Type": "VectorParameter", "Parameter": "FoamColor"},
		{"Id": "color", "Type": "Lerp", "Inputs": {"A": "deep:1", "B": "white:1", "Alpha": "foam"}},
		{"Id": "op0", "Type": "Multiply", "Inputs": {"A": "foam", "B": 0.6}},
		{"Id": "op", "Type": "Add", "Inputs": {"A": "op0", "B": 0.32}},
		{"Id": "glow", "Type": "Multiply", "Inputs": {"A": "color", "B": 0.05}},
	]
	return {
		"Name": "HD2DWaterfall", "BlendMode": "Translucent", "TwoSided": True,
		"Parameters": [
			{"Name": "Streaks", "Type": "Texture", "Value": "Textures/WaterfallStreaks.png", "Usage": "Linear"},
			{"Name": "DeepColor", "Type": "Vector", "Value": [0.06, 0.13, 0.14, 1.0]},
			{"Name": "FoamColor", "Type": "Vector", "Value": [0.85, 0.9, 0.92, 1.0]},
		],
		"Graph": {"Nodes": Nodes, "Output": {"BaseColor": "color", "Roughness": 0.15, "Metallic": 0.0, "Opacity": "op", "Emissive": "glow"}},
	}


def WriteTextures(Content):
	Folder = os.path.join(Content, MAT_DIR, "Textures")
	os.makedirs(Folder, exist_ok=True)

	def Save(Name, Rgb):
		Image.fromarray(np.clip(Rgb * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGB").save(os.path.join(Folder, Name), optimize=False)

	# 차양 줄무늬 (큐브 면 UV 0~1에 줄 8개, 천 결 노이즈 + 가장자리 어둡게)
	Size = 256
	V, U = (np.mgrid[0:Size, 0:Size] + 0.5) / Size
	Weave = 0.92 + 0.08 * _Noise2(Size, 32, 701, 2)
	for Name, (A, B) in {"AwningRed.png": ((0.62, 0.1, 0.08), (0.92, 0.86, 0.72)), "AwningBlue.png": ((0.12, 0.25, 0.55), (0.92, 0.88, 0.76)),
						 "AwningGreen.png": ((0.16, 0.38, 0.18), (0.93, 0.86, 0.62))}.items():
		Stripe = (np.floor(U * 8.0) % 2)[..., None]
		Rgb = (np.array(A) * Stripe + np.array(B) * (1.0 - Stripe)) * Weave[..., None]
		Save(Name, Rgb)
	# 폭포 줄무늬 (세로로 길게 늘인 노이즈 — 위아래 이음매 없음): R = 거품 정도
	N = _Noise2(Size, 16, 702, 4)
	Streak = np.clip((np.repeat(np.mean(N, axis=0, keepdims=True), Size, axis=0) - 0.5) * 3.0 + 0.5, 0.0, 1.0)
	Streak = np.clip(Streak * 0.75 + (_Noise2(Size, 4, 703, 3) - 0.5) * 0.9, 0.0, 1.0)
	Save("WaterfallStreaks.png", np.dstack([Streak, Streak, Streak]))


def WriteMaterials(Content):
	WriteTextures(Content)
	WriteJson(os.path.join(Content, MAT_DIR, "HD2DTriplanar.emat"), TriplanarGraph())
	WriteJson(os.path.join(Content, MAT_DIR, "HD2DTriplanarVC.emat"), TriplanarGraph(True))
	# 나무(엔진 폴리지 나무·침엽수): 잎 = 풀잎 텍스처 × 정점 색(어두움 → Tint로 밝힘)
	for Name, Tint in {"EnvTreeCrown": (5.2, 7.2, 6.0), "EnvTreeAutumn": (9.0, 6.6, 4.0), "EnvPineCrown": (4.6, 6.6, 6.0)}.items():
		WriteJson(os.path.join(Content, MAT_DIR, f"{Name}.emat"),
				  TriplanarInstance(Name, "leafy_grass/leafy_grass", 140.0, Tint, 1.0, "HD2DTriplanarVC.emat"))
	Fort = "modular_fort_01/textures/modular_fort_01"
	Instances = {
		"EnvPlaster":     ("plastered_wall_04/plastered_wall_04", 260.0, (1.0, 0.94, 0.82), 1.0),
		"EnvPlasterWarm": ("plastered_wall_04/plastered_wall_04", 260.0, (1.0, 0.82, 0.62), 1.0),
		"EnvPlasterRose": ("plastered_wall_04/plastered_wall_04", 260.0, (0.98, 0.78, 0.7), 1.0),
		"EnvTimber":      ("dark_wooden_planks/dark_wooden_planks", 220.0, (0.55, 0.42, 0.33), 1.0),
		"EnvPlanks":      ("weathered_planks/weathered_planks", 200.0, (0.85, 0.72, 0.58), 1.0),
		"EnvStone":       (f"{Fort}_wall", 300.0, (0.95, 0.9, 0.82), 1.0),
		"EnvStoneDark":   (f"{Fort}_wall", 300.0, (0.62, 0.6, 0.56), 1.0),
		"EnvRoofRed":     ("red_brick/red_brick", 150.0, (1.0, 0.55, 0.42), 1.0),
		"EnvRoofSlate":   ("rough_wood/rough_wood", 160.0, (0.42, 0.5, 0.62), 1.0),
		"EnvRoofBrown":   ("dark_wooden_planks/dark_wooden_planks", 180.0, (0.72, 0.5, 0.36), 1.0),
		"EnvCliff":       ("mossy_rock/mossy_rock", 380.0, (0.7, 0.72, 0.66), 1.0),
		"EnvMossRock":    ("mossy_rock/mossy_rock", 400.0, (0.85, 0.9, 0.8), 1.0),
		"EnvSoil":        ("forest_ground_04/forest_ground_04", 220.0, (0.62, 0.5, 0.4), 1.0),
	}
	for Name, (Stem, Size, Tint, Rough) in Instances.items():
		WriteJson(os.path.join(Content, MAT_DIR, f"{Name}.emat"), TriplanarInstance(Name, Stem, Size, Tint, Rough))
	WriteJson(os.path.join(Content, MAT_DIR, "EnvWaterfall.emat"), WaterfallGraph())
	Plains = {
		"EnvWindowLit":   Plain("EnvWindowLit", (0.0, 0.0, 0.0, 1.0), 0.6, (3.2, 1.75, 0.7)),
		"EnvWindowDim":   Plain("EnvWindowDim", (0.0, 0.0, 0.0, 1.0), 0.6, (1.1, 0.55, 0.22)),
		"EnvWindowDark":  Plain("EnvWindowDark", (0.05, 0.07, 0.1, 1.0), 0.08),
		"EnvShutterBlue": Plain("EnvShutterBlue", (0.12, 0.25, 0.42, 1.0), 0.7),
		"EnvShutterGreen": Plain("EnvShutterGreen", (0.15, 0.32, 0.16, 1.0), 0.7),
		"EnvShutterRed":  Plain("EnvShutterRed", (0.45, 0.1, 0.07, 1.0), 0.7),
		"EnvIron":        Plain("EnvIron", (0.06, 0.06, 0.065, 1.0), 0.45, Metallic=1.0),
		"EnvRope":        Plain("EnvRope", (0.18, 0.13, 0.08, 1.0), 0.9),
		"EnvAwningRed":   Plain("EnvAwningRed", (1.0, 1.0, 1.0, 1.0), 0.85, Texture="Textures/AwningRed.png"),
		"EnvAwningBlue":  Plain("EnvAwningBlue", (1.0, 1.0, 1.0, 1.0), 0.85, Texture="Textures/AwningBlue.png"),
		"EnvAwningGreen": Plain("EnvAwningGreen", (1.0, 1.0, 1.0, 1.0), 0.85, Texture="Textures/AwningGreen.png"),
		"EnvCanvas":      Plain("EnvCanvas", (0.36, 0.3, 0.22, 1.0), 0.95),
		"EnvClothWhite":  Plain("EnvClothWhite", (0.86, 0.85, 0.8, 1.0), 0.9),
		"EnvClothBlue":   Plain("EnvClothBlue", (0.25, 0.4, 0.62, 1.0), 0.9),
		"EnvClothRed":    Plain("EnvClothRed", (0.62, 0.16, 0.12, 1.0), 0.9),
		"EnvClothYellow": Plain("EnvClothYellow", (0.9, 0.7, 0.2, 1.0), 0.9),
		"EnvClothGreen":  Plain("EnvClothGreen", (0.22, 0.48, 0.25, 1.0), 0.9),
		"EnvTentGreen":   Plain("EnvTentGreen", (0.09, 0.15, 0.08, 1.0), 0.95),
		"EnvApple":       Plain("EnvApple", (0.55, 0.06, 0.04, 1.0), 0.35),
		"EnvOrange":      Plain("EnvOrange", (0.85, 0.38, 0.05, 1.0), 0.45),
		"EnvCabbage":     Plain("EnvCabbage", (0.2, 0.4, 0.13, 1.0), 0.7),
		"EnvBread":       Plain("EnvBread", (0.62, 0.38, 0.16, 1.0), 0.8),
		"EnvFlowerRed":   Plain("EnvFlowerRed", (0.75, 0.06, 0.05, 1.0), 0.7),
		"EnvFlowerPink":  Plain("EnvFlowerPink", (0.95, 0.42, 0.58, 1.0), 0.7),
		"EnvFlowerYellow": Plain("EnvFlowerYellow", (1.0, 0.78, 0.12, 1.0), 0.7),
		"EnvFlowerWhite": Plain("EnvFlowerWhite", (0.95, 0.93, 0.88, 1.0), 0.7),
		"EnvFlowerPurple": Plain("EnvFlowerPurple", (0.45, 0.25, 0.75, 1.0), 0.7),
		"EnvFlowerBlue":  Plain("EnvFlowerBlue", (0.28, 0.45, 0.95, 1.0), 0.7),
		"EnvLily":        Plain("EnvLily", (0.12, 0.3, 0.08, 1.0), 0.5),
		"EnvWheat":       Plain("EnvWheat", (1.35, 1.0, 0.42, 1.0), 0.85),
		"EnvMeadow":      Plain("EnvMeadow", (0.75, 1.0, 0.45, 1.0), 0.85),
		"EnvLeaf":        Plain("EnvLeaf", (0.2, 0.36, 0.12, 1.0), 0.8),
		"EnvCaveDark":    Plain("EnvCaveDark", (0.004, 0.004, 0.005, 1.0), 1.0),
	}
	for Name, Doc in Plains.items():
		WriteJson(os.path.join(Content, MAT_DIR, f"{Name}.emat"), Doc)


# ---- 파티클 ---------------------------------------------------------------------------------------------------------
SMOKE_TEX = "../Textures/CampfireSmoke.png"


def PetalsSystem(Box, Height, Colors, Rate, Name, Seed):
	# 떨어지는 꽃잎/잎: 넓은 상자 위쪽에서 생겨 바람(+X)·회오리를 타고 천천히 내려온다 (GPU, 정렬 없음 — 작은 불투명에 가까운 조각)
	Fall = Emitter(Name, Seed, 1200,
		[Mod("SpawnRate", SpawnRate=Const(Rate))],
		[Init(Rand(14.0, 20.0), Rand(*Colors), Rand((13.0, 8.0), (20.0, 12.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_BOX, Box=(Box[0], Box[1], 200.0), Offset=(0.0, 0.0, Height)),
		 Mod("AddVelocity", Velocity=Rand((15.0, -10.0, -32.0), (45.0, 10.0, -18.0)))],
		[Mod("CurlNoiseForce", Strength=Const(70.0), Frequency=Const(0.004), PanSpeed=Const(30.0, 0.0, 0.0)),
		 Mod("Drag", Drag=Const(0.4)),
		 Mod("SpriteRotationRate", RotationRate=Rand(-160.0, 160.0)),
		 Mod("ScaleColor", Scale=Curve((0.0, (1, 1, 1, 0)), (0.08, (1, 1, 1, 1)), (0.85, (1, 1, 1, 1)), (1.0, (1, 1, 1, 0)))),
		 Mod("Collision", PlaneHeight=Const(-20.0), Restitution=Const(0.0), Friction=Const(1.0), KillOnCollide=Const(0.0))],
		PSprite(0), Sim="GPU", Duration=10.0,
		Bounds=((-Box[0] * 0.5 - 1500.0, -Box[1] * 0.5 - 1500.0, -300.0), (Box[0] * 0.5 + 1500.0, Box[1] * 0.5 + 1500.0, Height + 600.0)))
	return {"Name": Name, "Version": 2, "Emitters": [Fall]}


def MotesSystem(Box):
	# 햇빛 속 빛 먼지: 따뜻한 작은 가산 점이 천천히 떠다니며 반짝인다
	Twinkle = Curve((0.0, (1, 1, 1, 0)), (0.2, (1, 1, 1, 0.8)), (0.35, (1, 1, 1, 0.25)), (0.55, (1, 1, 1, 1.0)), (0.75, (1, 1, 1, 0.3)), (1.0, (1, 1, 1, 0)))
	Motes = Emitter("Motes", 811, 1600,
		[Mod("SpawnRate", SpawnRate=Const(120.0))],
		[Init(Rand(6.0, 11.0), Rand((2.0, 1.4, 0.7, 0.7), (3.2, 2.2, 1.1, 1.0)), Rand((5.0, 5.0), (8.0, 8.0))),
		 Shape(SHAPE_BOX, Box=(Box[0], Box[1], 380.0), Offset=(0.0, 0.0, 230.0)),
		 Mod("AddVelocity", Velocity=Rand((-8.0, -8.0, -4.0), (12.0, 8.0, 8.0)))],
		[Mod("CurlNoiseForce", Strength=Const(18.0), Frequency=Const(0.006), PanSpeed=Const(5.0, 0.0, 2.0)),
		 Mod("Drag", Drag=Const(0.5)),
		 Mod("ScaleColor", Scale=Twinkle)],
		PSprite(1), Sim="GPU", Duration=10.0,
		Bounds=((-Box[0] * 0.5 - 500.0, -Box[1] * 0.5 - 500.0, -100.0), (Box[0] * 0.5 + 500.0, Box[1] * 0.5 + 500.0, 900.0)))
	return {"Name": "HD2DMotes", "Version": 2, "Emitters": [Motes]}


def ChimneySmokeSystem():
	# 굴뚝 연기: 옅은 회청색 뭉게가 천천히 오르며 +X로 흩어진다 (CPU 정렬 반투명)
	Smoke = Emitter("Smoke", 821, 60,
		[Mod("SpawnRate", SpawnRate=Const(4.0))],
		[Init(Rand(7.0, 10.0), Rand((0.42, 0.42, 0.45, 0.22), (0.55, 0.55, 0.6, 0.32)), Rand((40.0, 40.0), (60.0, 60.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_CYLINDER, Radius=14.0, Height=6.0),
		 Mod("AddVelocity", Velocity=Rand((-4.0, -4.0, 45.0), (4.0, 4.0, 70.0)))],
		[Mod("AccelerationForce", Acceleration=Const(16.0, -4.0, 6.0)),
		 Mod("Drag", Drag=Const(0.3)),
		 Mod("CurlNoiseForce", Strength=Const(30.0), Frequency=Const(0.006), PanSpeed=Const(10.0, 0.0, 15.0)),
		 Mod("SpriteRotationRate", RotationRate=Rand(-12.0, 12.0)),
		 Mod("ScaleColor", Scale=Curve((0.0, (1.6, 1.3, 1.1, 0.0)), (0.1, (1.3, 1.15, 1.05, 1.0)), (1.0, (1.0, 1.0, 1.05, 0.0)))),
		 Mod("ScaleSpriteSize", Scale=Curve((0.0, (0.6, 0.6)), (1.0, (4.2, 4.2)))),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(20.0))],
		PSprite(0, SMOKE_TEX, 4, 4))
	return {"Name": "HD2DChimneySmoke", "Version": 2, "Emitters": [Smoke]}


def WaterfallMistSystem(Width):
	# 폭포 아래 물보라: 떨어진 물이 튀는 작은 방울 + 피어오르는 옅은 안개 (이미터 원점 = 웅덩이 수면)
	Spray = Emitter("Spray", 831, 500,
		[Mod("SpawnRate", SpawnRate=Const(110.0))],
		[Init(Rand(0.5, 1.1), Rand((0.75, 0.8, 0.85, 0.5), (0.9, 0.95, 1.0, 0.8)), Rand((3.0, 3.0), (7.0, 7.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_BOX, Box=(Width, 30.0, 10.0)),
		 Mod("AddVelocityInCone", ConeAxis=Const(0.0, 0.4, 1.0), ConeAngle=Const(40.0), Speed=Rand(150.0, 380.0))],
		[Mod("GravityForce", Gravity=Const(0.0, 0.0, -980.0)),
		 Mod("Drag", Drag=Const(1.0)),
		 Mod("ScaleColor", Scale=Curve((0.0, (1, 1, 1, 1)), (1.0, (1, 1, 1, 0)))),
		 Mod("Collision", PlaneHeight=Const(-40.0), Restitution=Const(0.0), Friction=Const(0.0), KillOnCollide=Const(1.0))],
		PSprite(0, SMOKE_TEX, 4, 4), Sim="GPU", Bounds=((-600.0, -600.0, -200.0), (600.0, 800.0, 600.0)))
	Spray["ParticleUpdate"].append(Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(30.0)))
	Mist = Emitter("Mist", 832, 50,
		[Mod("SpawnRate", SpawnRate=Const(7.0))],
		[Init(Rand(3.0, 5.0), Rand((0.6, 0.65, 0.68, 0.12), (0.7, 0.75, 0.8, 0.2)), Rand((90.0, 90.0), (150.0, 150.0)), Rand(0.0, 360.0)),
		 Shape(SHAPE_BOX, Box=(Width * 1.2, 60.0, 20.0), Offset=(0.0, 0.0, 30.0)),
		 Mod("AddVelocity", Velocity=Rand((-15.0, 10.0, 15.0), (15.0, 40.0, 45.0)))],
		[Mod("Drag", Drag=Const(0.4)),
		 Mod("SpriteRotationRate", RotationRate=Rand(-10.0, 10.0)),
		 Mod("ScaleSpriteSize", Scale=Curve((0.0, (0.6, 0.6)), (1.0, (2.2, 2.2)))),
		 Mod("ScaleColor", Scale=Curve((0.0, (1, 1, 1, 0)), (0.25, (1, 1, 1, 1)), (1.0, (1, 1, 1, 0)))),
		 Mod("SubUVAnimation", Mode=Const(2.0), FrameCount=Const(16.0), FrameRate=Const(20.0))],
		PSprite(0, SMOKE_TEX, 4, 4))
	return {"Name": "HD2DWaterfallMist", "Version": 2, "Emitters": [Mist, Spray]}


def WriteParticles(Content, PlayBox):
	Systems = {
		"HD2DPetals": PetalsSystem(PlayBox, 550.0, ((1.0, 0.62, 0.78, 1.0), (1.0, 0.86, 0.92, 1.0)), 32.0, "HD2DPetals", 801),
		"HD2DLeaves": PetalsSystem(PlayBox, 600.0, ((0.75, 0.42, 0.12, 1.0), (0.95, 0.68, 0.22, 1.0)), 14.0, "HD2DLeaves", 802),
		"HD2DPondPetals": PetalsSystem((900.0, 900.0), 500.0, ((0.62, 0.45, 0.95, 1.0), (0.8, 0.66, 1.0, 1.0)), 9.0, "HD2DPondPetals", 803),
		"HD2DMotes": MotesSystem(PlayBox),
		"HD2DChimneySmoke": ChimneySmokeSystem(),
		"HD2DWaterfallMist": WaterfallMistSystem(180.0),
	}
	for Name, Doc in Systems.items():
		WriteJson(os.path.join(Content, FX_DIR, f"{Name}.eparticle"), Doc)


def Fx(Name):
	return f"{FX_DIR}/{Name}.eparticle"


# ---- 폴리지 종류 (엔진 폴리지 — 꽃 = 작은 구, 밀 = 금빛 풀) ---------------------------------------------------------
def FoliageType(Name, Mesh, Material, MinScale, MaxScale, ZOffset=0.0, Cull=5000.0, Shadow=0.0):
	return {"Name": Name, "Mesh": Mesh, "Material": Material, "Density": 20.0, "MinScale": MinScale, "MaxScale": MaxScale, "MaxSlope": 45.0,
			"MinHeight": -1.0e7, "MaxHeight": 1.0e7, "AlignToNormal": True, "RandomYaw": True, "ZOffset": ZOffset, "CullDistance": Cull,
			"ShadowDistance": Shadow, "Collision": False, "CollisionRadius": 25.0, "CollisionHeight": 400.0}


FLOWER_COLORS = ["Red", "Yellow", "White", "Purple", "Pink", "Blue"]
FLOWER_TYPES = {Color: FoliageType(f"Flower{Color}", SPHERE, Mat(f"EnvFlower{Color}"), 0.05, 0.085, ZOffset=14.0, Cull=4500.0) for Color in FLOWER_COLORS}
WHEAT_TYPE = FoliageType("Wheat", "foliage:grass", Mat("EnvWheat"), 1.25, 1.75, ZOffset=-2.0, Cull=6000.0, Shadow=2500.0)
MEADOW_TYPE = FoliageType("Meadow", "foliage:grass", Mat("EnvMeadow"), 0.8, 1.2, ZOffset=-2.0, Cull=5000.0)
TREE_TYPE = dict(FoliageType("Tree", "foliage:tree", Mat("EnvTreeCrown"), 0.8, 1.3, ZOffset=-10.0, Cull=20000.0, Shadow=9000.0), AlignToNormal=False)
TREE_AUTUMN_TYPE = dict(TREE_TYPE, Name="TreeAutumn", Material=Mat("EnvTreeAutumn"))
PINE_TYPE = dict(FoliageType("Pine", "foliage:pine", Mat("EnvPineCrown"), 0.9, 1.6, ZOffset=-10.0, Cull=20000.0, Shadow=9000.0), AlignToNormal=False)


# ---- 배치 도우미 ----------------------------------------------------------------------------------------------------
class FDressing:
	# 씬(FScene) + 높이 함수 + 점유 기록을 받아 건물·소품을 내장 큐브로 짓는다
	def __init__(self, S, Height, Rng, Reserve, BoxCollider, Point):
		self.S, self.Height, self.Rng = S, Height, Rng
		self.Reserve, self.BoxCollider, self.Point = Reserve, BoxCollider, Point

	def Particles(self, Name, Asset, Pos, Parent=-1, Speed=1.0):
		return self.S.Add(Name, {"ParticleSystemComponent": {"Asset": Asset, "Playing": True, "Speed": Speed}}, Pos, None, (1.0, 1.0, 1.0), Parent)

	def Box(self, Name, Center, Size, Material, Rotation=None, Parent=-1):
		return self.S.Add(Name, {"StaticMeshComponent": {"MeshAsset": CUBE, "MaterialAsset": Mat(Material)}}, Center, Rotation,
						  tuple(V / 100.0 for V in Size), Parent)

	def Ball(self, Name, Center, Diameter, Material, Parent=-1, Squash=1.0):
		return self.S.Add(Name, {"StaticMeshComponent": {"MeshAsset": SPHERE, "MaterialAsset": Mat(Material)}}, Center, None,
						  (Diameter / 100.0, Diameter / 100.0, Diameter * Squash / 100.0), Parent)

	def Group(self, Name, Center, Yaw=0.0, Parent=-1, Scale=(1.0, 1.0, 1.0)):
		return self.S.Add(Name, {}, Center, QuatFromEuler(Yaw=Yaw), Scale, Parent)

	def Beam(self, Name, A, B, Thick, Depth, Material, Parent):
		# 앞면(XZ 평면, 로컬 Y = 면 앞 거리) 위 A(x, z) → B(x, z) 각재 (Pitch로 기울임 — X 축이 Z 쪽으로)
		(AX, AY, AZ), (BX, BY, BZ) = A, B
		DX, DZ = BX - AX, BZ - AZ
		Length = math.hypot(DX, DZ)
		Angle = math.degrees(math.atan2(DZ, DX))
		return self.Box(Name, ((AX + BX) * 0.5, (AY + BY) * 0.5, (AZ + BZ) * 0.5), (Length, Depth, Thick), Material, QuatFromEuler(Pitch=Angle), Parent)

	def SideBeam(self, Name, A, B, Thick, Depth, Material, Parent):
		# 옆면(YZ 평면) 위 A(y, z) → B(y, z) 각재 (Roll로 기울임 — Y 축이 -Z 쪽으로)
		(AX, AY, AZ), (BX, BY, BZ) = A, B
		DY, DZ = BY - AY, BZ - AZ
		Length = math.hypot(DY, DZ)
		Angle = math.degrees(math.atan2(-DZ, DY))
		return self.Box(Name, ((AX + BX) * 0.5, (AY + BY) * 0.5, (AZ + BZ) * 0.5), (Depth, Length, Thick), Material, QuatFromEuler(Roll=Angle), Parent)

	# ---- 창 / 문 ----------------------------------------------------------------------------------------------------
	def Window(self, Name, X, FaceY, Z, W, H, Lit, Shutter, Parent, FlowerBox=False):
		Glass = "EnvWindowLit" if Lit == 2 else ("EnvWindowDim" if Lit == 1 else "EnvWindowDark")
		self.Box(f"{Name}_Glass", (X, FaceY + 1.0, Z), (W, 4.0, H), Glass, None, Parent)
		T = 9.0
		for Index, (CX, CZ, SX, SZ) in enumerate(((X, Z + H * 0.5 + T * 0.5, W + 2 * T, T), (X, Z - H * 0.5 - T * 0.5, W + 2 * T + 8.0, T + 3.0),
												  (X - W * 0.5 - T * 0.5, Z, T, H), (X + W * 0.5 + T * 0.5, Z, T, H),
												  (X, Z, 5.0, H), (X, Z + H * 0.12, W, 5.0))):
			self.Box(f"{Name}_Frame{Index}", (CX, FaceY + 4.0 + (2.0 if Index == 1 else 0.0), CZ), (SX, 8.0 + (4.0 if Index == 1 else 0.0), SZ), "EnvTimber", None, Parent)
		if Shutter:
			for Side in (-1, 1):
				self.Box(f"{Name}_Shutter{Side}", (X + Side * (W * 0.75 + T + 2.0), FaceY + 3.0, Z), (W * 0.5, 4.0, H + 6.0), Shutter, None, Parent)
		if FlowerBox:
			BZ = Z - H * 0.5 - 22.0
			self.Box(f"{Name}_FlowerBox", (X, FaceY + 14.0, BZ), (W + 26.0, 24.0, 20.0), "EnvTimber", None, Parent)
			for F in range(6):
				Color = self.Rng.choice(["EnvFlowerRed", "EnvFlowerPink", "EnvFlowerYellow", "EnvFlowerWhite", "EnvFlowerRed"])
				FX = X + (F - 2.5) * (W + 10.0) / 6.0
				self.Ball(f"{Name}_Leaf{F}", (FX, FaceY + 14.0, BZ + 12.0), 22.0, "EnvLeaf", Parent, 0.7)
				self.Ball(f"{Name}_Flower{F}", (FX + self.Rng.uniform(-4, 4), FaceY + 18.0 + self.Rng.uniform(-3, 3), BZ + 20.0), 11.0, Color, Parent)

	def Door(self, Name, X, FaceY, Z0, W=110.0, H=210.0, Parent=-1, Arch=False):
		self.Box(f"{Name}_Leaf", (X, FaceY + 2.0, Z0 + H * 0.5), (W, 6.0, H), "EnvPlanks", None, Parent)
		for Index, (CX, CZ, SX, SZ) in enumerate(((X, Z0 + H + 9.0, W + 36.0, 18.0), (X - W * 0.5 - 8.0, Z0 + H * 0.5, 16.0, H), (X + W * 0.5 + 8.0, Z0 + H * 0.5, 16.0, H))):
			self.Box(f"{Name}_Frame{Index}", (CX, FaceY + 5.0, CZ), (SX, 12.0, SZ), "EnvStone" if Arch else "EnvTimber", None, Parent)
		self.Box(f"{Name}_Step", (X, FaceY + 25.0, Z0 - 6.0), (W + 50.0, 50.0, 24.0), "EnvStoneDark", None, Parent)
		self.Box(f"{Name}_Handle", (X + W * 0.32, FaceY + 7.0, Z0 + H * 0.48), (6.0, 6.0, 6.0), "EnvIron", None, Parent)

	# ---- 박공 지붕 ----------------------------------------------------------------------------------------------------
	def Roof(self, Name, Root, Top, Span, Length, Ridge, Pitch, Material, GableMat, Eave=38.0, GableOver=30.0, Thick=18.0, Cap=True):
		# 박공 마름모 기둥(45도 돌린 큐브를 부모 Z 스케일 tan(경사)로 눌러 삼각형 단면) + 지붕판 둘 + 용마루. 반환 = 지붕 높이(Rise)
		#   Ridge "X": 용마루가 로컬 X (처마가 카메라 쪽), "Y": 용마루가 로컬 Y (박공 삼각형이 카메라 쪽)
		P = math.radians(Pitch)
		Rise = Span * 0.5 * math.tan(P)
		Prism = self.Group(f"{Name}_GableScale", (0.0, 0.0, Top), 0.0, Root, (1.0, 1.0, math.tan(P)))
		Side = Span / math.sqrt(2.0)
		if GableMat:
			# 마름모 아래 절반은 벽 속에 묻힌다 — 끝면을 벽면보다 2cm 들여 아래층 벽과 같은 면에서 겹쳐 보이지 않게
			if Ridge == "X":
				self.Box(f"{Name}_Gable", (0.0, 0.0, 0.0), (Length - 4.0, Side, Side), GableMat, QuatFromEuler(Roll=45.0), Prism)
			else:
				self.Box(f"{Name}_Gable", (0.0, 0.0, 0.0), (Side, Length - 4.0, Side), GableMat, QuatFromEuler(Pitch=45.0), Prism)
		Run = Span * 0.5 + Eave
		Slope = Run / math.cos(P)
		Along = Run * 0.5  # 경사면 중점의 수평 위치 (용마루에서 처마 쪽으로)
		Zc = Top + Rise - Along * math.tan(P) + (Thick * 0.5) / math.cos(P)
		for Sign in (-1, 1):
			if Ridge == "X":
				Center, Size, Rot = (0.0, Sign * Along, Zc), (Length + GableOver * 2.0, Slope, Thick), QuatFromEuler(Roll=Sign * Pitch)
			else:
				Center, Size, Rot = (Sign * Along, 0.0, Zc), (Slope, Length + GableOver * 2.0, Thick), QuatFromEuler(Pitch=-Sign * Pitch)
			self.Box(f"{Name}_Roof{Sign}", Center, Size, Material, Rot, Root)
		if Cap:
			RidgeZ = Top + Rise + Thick / math.cos(P)
			CapSize = (Length + GableOver * 2.0 + 6.0, 26.0, 20.0) if Ridge == "X" else (26.0, Length + GableOver * 2.0 + 6.0, 20.0)
			self.Box(f"{Name}_RidgeCap", (0.0, 0.0, RidgeZ - 4.0), CapSize, "EnvTimber", QuatFromEuler(Roll=45.0) if Ridge == "X" else QuatFromEuler(Pitch=45.0), Root)
		return Rise

	# ---- 반목조 집 --------------------------------------------------------------------------------------------------
	def House(self, Name, X, Y, W, D, H1=300.0, H2=0.0, Over=25.0, Ridge="X", Pitch=50.0, Roof="EnvRoofRed", Ground="EnvStone",
			  Upper="EnvPlaster", Yaw=0.0, Lit=0.6, Shutter="EnvShutterBlue", Chimney=0.6, DoorX=0.0, Smoke=True, Lantern=False, Z=None,
			  Collide=True, GroundTimber=False, FlowerBoxes=True, GroundWin=(72.0, 92.0), UpperWin=(68.0, 88.0)):
		Rng = self.Rng
		Z = self.Height(X, Y) if Z is None else Z
		Root = self.Group(Name, (X, Y, Z), Yaw)
		Sink = 60.0
		# 1층 (땅속으로 Sink만큼 묻어 경사 지형에서도 뜨지 않게) + 바닥 띠돌
		self.Box(f"{Name}_Ground", (0.0, 0.0, (H1 - Sink) * 0.5), (W, D, H1 + Sink), Ground, None, Root)
		self.Box(f"{Name}_Plinth", (0.0, 0.0, 12.0 - Sink * 0.5), (W + 14.0, D + 14.0, 24.0 + Sink), "EnvStoneDark", None, Root)
		Top = H1
		Wu, Du = W, D
		if H2 > 0.0:
			# 박공이 앞을 보는 집(Ridge Y)은 앞뒤로 내밀지 않는다: 박공 마름모의 아래 절반이 1층보다 앞으로 삐져나오기 때문 (옆으로만 내밈)
			OverY = 0.0 if Ridge == "Y" else Over
			Wu, Du = W + Over * 2.0, D + OverY * 2.0
			self.Box(f"{Name}_Upper", (0.0, 0.0, H1 + H2 * 0.5), (Wu, Du, H2), Upper, None, Root)
			# 내민 2층 아래 받침 장선 (앞면)
			for Index in range(int(W // 90.0) + 1 if OverY > 0.0 else 0):
				JX = -W * 0.5 + 20.0 + Index * (W - 40.0) / max(1, int(W // 90.0))
				self.Box(f"{Name}_Joist{Index}", (JX, D * 0.5 + Over * 0.5, H1 - 7.0), (12.0, Over + 6.0, 14.0), "EnvTimber", None, Root)
			Top = H1 + H2
		# 앞면(+Y) 창 배치: 층마다 너비에 맞춰 고르게
		def Bays(Width, Spacing):
			Count = max(1, int((Width - 80.0) // Spacing))
			return [-Width * 0.5 + Width * (I + 0.5) / Count for I in range(Count)], Count

		def Frame(Prefix, Width, FaceY, Z0, Height, Bays_):
			# 앞면 목재 골조: 아래·위 띠, 모서리 기둥, 창 사이 샛기둥, 양 끝 칸 가새
			T, Dp = 15.0, 8.0
			Y_ = FaceY + Dp * 0.5 - 1.0
			self.Box(f"{Prefix}_Sill", (0.0, Y_, Z0 + T * 0.5), (Width + 4.0, Dp, T), "EnvTimber", None, Root)
			self.Box(f"{Prefix}_Plate", (0.0, Y_, Z0 + Height - T * 0.5), (Width + 4.0, Dp, T), "EnvTimber", None, Root)
			Xs = [-Width * 0.5 + T * 0.5, Width * 0.5 - T * 0.5]
			Xs += [(Bays_[I] + Bays_[I + 1]) * 0.5 for I in range(len(Bays_) - 1)]
			for Index, PX in enumerate(Xs):
				self.Box(f"{Prefix}_Post{Index}", (PX, Y_, Z0 + Height * 0.5), (T, Dp, Height), "EnvTimber", None, Root)
			if len(Bays_) >= 1:
				Edge = Width * 0.5 - T
				Inner = Bays_[0] - 60.0
				for Side in (-1, 1):
					if abs(Inner) < Edge - 40.0:
						A = (Side * Edge, Y_, Z0 + T)
						B = (Side * abs(Inner), Y_, Z0 + Height - T)
						self.Beam(f"{Prefix}_Brace{Side}", A, B, 13.0, Dp, "EnvTimber", Root)

		def SideFrame(Prefix, Depth, FaceX, Z0, Height, Sign):
			T, Dp = 15.0, 8.0
			X_ = FaceX + Sign * (Dp * 0.5 - 1.0)
			self.Box(f"{Prefix}_Sill", (X_, 0.0, Z0 + T * 0.5), (Dp, Depth + 4.0, T), "EnvTimber", None, Root)
			self.Box(f"{Prefix}_Plate", (X_, 0.0, Z0 + Height - T * 0.5), (Dp, Depth + 4.0, T), "EnvTimber", None, Root)
			for Index, PY in enumerate((-Depth * 0.5 + T * 0.5, 0.0, Depth * 0.5 - T * 0.5)):
				self.Box(f"{Prefix}_Post{Index}", (X_, PY, Z0 + Height * 0.5), (Dp, T, Height), "EnvTimber", None, Root)
			self.SideBeam(f"{Prefix}_Brace", (X_, Depth * 0.5 - T, Z0 + T), (X_, Depth * 0.18, Z0 + Height - T), 13.0, Dp, "EnvTimber", Root)
			self.SideBeam(f"{Prefix}_Brace2", (X_, -Depth * 0.5 + T, Z0 + T), (X_, -Depth * 0.18, Z0 + Height - T), 13.0, Dp, "EnvTimber", Root)

		# 1층 창·문
		GBays, _ = Bays(W, 170.0)
		DoorBay = min(range(len(GBays)), key=lambda I: abs(GBays[I] - DoorX))
		for Index, BX in enumerate(GBays):
			if Index == DoorBay:
				self.Door(f"{Name}_Door", BX, D * 0.5, 0.0, Parent=Root, Arch=Ground.startswith("EnvStone"))
				if Lantern:
					LX = BX + 95.0
					self.Box(f"{Name}_LanternArm", (LX, D * 0.5 + 18.0, 245.0), (6.0, 36.0, 6.0), "EnvIron", None, Root)
					self.S.Add(f"{Name}_Lantern", {"ModelComponent": {"AssetPath": f"{PH}/wooden_lantern_01/wooden_lantern_01.gltf"}},
							   (LX, D * 0.5 + 34.0, 205.0), None, (1.1, 1.1, 1.1), Root)
					self.Particles(f"{Name}_LanternFlame", "Particles/Demo/CampfireLanternFlame.eparticle", (LX, D * 0.5 + 34.0, 222.0), Root)
				continue
			Light = 2 if Rng.random() < Lit else (1 if Rng.random() < 0.3 else 0)
			self.Window(f"{Name}_GWin{Index}", BX, D * 0.5, max(H1 * 0.55, GroundWin[1] * 0.5 + 70.0), GroundWin[0], GroundWin[1], Light, Shutter if Rng.random() < 0.7 else None, Root, FlowerBoxes and H2 <= 0.0)
		if GroundTimber:
			Frame(f"{Name}_GFrame", W, D * 0.5, 0.0, H1, GBays)
		if H2 > 0.0:
			UBays, _ = Bays(Wu, 160.0)
			for Index, BX in enumerate(UBays):
				Light = 2 if Rng.random() < Lit else (1 if Rng.random() < 0.3 else 0)
				self.Window(f"{Name}_UWin{Index}", BX, Du * 0.5, H1 + H2 * 0.52, UpperWin[0], UpperWin[1], Light, Shutter if Rng.random() < 0.5 else None, Root,
							FlowerBoxes and Rng.random() < 0.6)
			if Upper != Ground or not Upper.startswith("EnvStone"):
				Frame(f"{Name}_UFrame", Wu, Du * 0.5, H1, H2, UBays)
				for Sign in (-1, 1):
					SideFrame(f"{Name}_USide{Sign}", Du, Sign * Wu * 0.5, H1, H2, Sign)
		# 지붕
		Span, Length = (Du, Wu) if Ridge == "X" else (Wu, Du)
		Rise = self.Roof(Name, Root, Top, Span, Length, Ridge, Pitch, Roof, Upper if H2 > 0.0 else Ground)
		P = math.radians(Pitch)
		if Ridge == "Y":
			# 앞 박공(카메라 쪽 삼각형) 골조: 가운데 왕기둥 + 가로대 + 다락 창
			FaceY = Du * 0.5
			self.Box(f"{Name}_GableBeam", (0.0, FaceY + 3.0, Top + 8.0), (Wu + 4.0, 8.0, 16.0), "EnvTimber", None, Root)
			self.Box(f"{Name}_KingPost", (0.0, FaceY + 3.0, Top + Rise * 0.5), (15.0, 8.0, Rise), "EnvTimber", None, Root)
			self.Box(f"{Name}_Collar", (0.0, FaceY + 3.0, Top + Rise * 0.45), (Span * 0.55, 8.0, 14.0), "EnvTimber", None, Root)
			for Sign in (-1, 1):
				self.Beam(f"{Name}_GableBrace{Sign}", (Sign * 8.0, FaceY + 3.0, Top + Rise * 0.45), (Sign * Span * 0.36, FaceY + 3.0, Top + 12.0), 12.0, 8.0, "EnvTimber", Root)
			if Rise > 180.0:
				self.Window(f"{Name}_Attic", Span * 0.2, FaceY, Top + Rise * 0.22, 46.0, 52.0, 2 if Rng.random() < Lit else 0, None, Root)
		# 굴뚝 (지붕 위로) + 연기
		if Chimney is not None:
			CX = Length * 0.5 * Chimney if Ridge == "X" else Span * 0.25
			CY = -Span * 0.15 if Ridge == "X" else -Length * 0.5 * abs(Chimney)
			Dist = abs(CY) if Ridge == "X" else abs(CX)
			RoofZ = Top + Rise - Dist * math.tan(P)
			TopZ = Top + Rise + 85.0
			self.Box(f"{Name}_Chimney", (CX, CY, (RoofZ - 40.0 + TopZ) * 0.5), (62.0, 62.0, TopZ - RoofZ + 40.0), "EnvStone", None, Root)
			self.Box(f"{Name}_ChimneyCap", (CX, CY, TopZ + 6.0), (78.0, 78.0, 12.0), "EnvStoneDark", None, Root)
			if Smoke:
				self.Particles(f"{Name}_Smoke", Fx("HD2DChimneySmoke"), (CX, CY, TopZ + 20.0), Root)
		if Collide:
			R = math.radians(Yaw)
			HalfX, HalfY = (Wu * 0.5, Du * 0.5) if abs(math.sin(R)) < 0.5 else (Du * 0.5, Wu * 0.5)
			self.BoxCollider(f"{Name}_Collision", (X, Y, Z + Top * 0.5), (HalfX * 0.98, HalfY * 0.98, Top * 0.5 + 10.0), 0.0)
			self.Reserve(X, Y, max(Wu, Du) * 0.6)
		return Root

	# ---- 노점 --------------------------------------------------------------------------------------------------------
	def Stall(self, Name, X, Y, Yaw=0.0, Awning="EnvAwningRed", Goods=("EnvApple", "EnvOrange", "EnvCabbage")):
		W, D = 260.0, 150.0
		Z = self.Height(X, Y)
		Root = self.Group(Name, (X, Y, Z), Yaw)
		for SX in (-1, 1):
			for SY, H in ((1, 215.0), (-1, 265.0)):
				self.Box(f"{Name}_Post{SX}{SY}", (SX * (W * 0.5 - 6.0), SY * (D * 0.5 - 6.0), H * 0.5), (11.0, 11.0, H), "EnvTimber", None, Root)
		self.Box(f"{Name}_Counter", (0.0, D * 0.5 - 30.0, 42.0), (W - 10.0, 56.0, 84.0), "EnvPlanks", None, Root)
		self.Box(f"{Name}_CounterTop", (0.0, D * 0.5 - 28.0, 87.0), (W + 10.0, 66.0, 6.0), "EnvTimber", None, Root)
		self.Box(f"{Name}_Back", (0.0, -D * 0.5 + 15.0, 70.0), (W - 20.0, 26.0, 140.0), "EnvPlanks", None, Root)
		Tilt = math.degrees(math.atan2(50.0, D + 50.0))
		self.Box(f"{Name}_Awning", (0.0, 8.0, 247.0), (W + 50.0, D + 70.0, 4.0), Awning, QuatFromEuler(Roll=Tilt), Root)
		self.Box(f"{Name}_Valance", (0.0, D * 0.5 + 43.0, 207.0), (W + 50.0, 3.0, 26.0), Awning, None, Root)
		# 물건: 상자 셋 + 과일 무더기 (작은 구)
		for Index, Good in enumerate(Goods):
			GX = (Index - (len(Goods) - 1) * 0.5) * 78.0
			self.Box(f"{Name}_Crate{Index}", (GX, D * 0.5 - 28.0, 100.0), (64.0, 46.0, 22.0), "EnvPlanks", None, Root)
			Size = 15.0 if Good != "EnvCabbage" else 21.0
			for K in range(8 if Good != "EnvCabbage" else 5):
				A = K * 2.4
				R = 0.0 if K == 0 else (10.0 + (K % 3) * 5.0)
				self.Ball(f"{Name}_Good{Index}_{K}", (GX + math.cos(A) * R * 1.4, D * 0.5 - 28.0 + math.sin(A) * R * 0.8, 116.0 + (6.0 if K == 0 else 0.0)), Size, Good, Root)
		self.BoxCollider(f"{Name}_Collision", (X, Y, Z + 60.0), (W * 0.5, D * 0.5, 60.0), Yaw)
		self.Reserve(X, Y, 170.0)
		return Root

	# ---- 줄 (깃발·빨래) ---------------------------------------------------------------------------------------------
	def Line(self, Name, A, B, Sag, Items, Spacing, ItemFn):
		# A → B 처진 줄(포물선 근사 6 조각) + 일정 간격 매단 물건(ItemFn(이름, 위치, 진행 방향 Yaw, 번호))
		Segments = 6
		Points = []
		for I in range(Segments + 1):
			T = I / Segments
			Points.append(tuple(A[K] + (B[K] - A[K]) * T for K in range(3)))
			Points[-1] = (Points[-1][0], Points[-1][1], Points[-1][2] - Sag * 4.0 * T * (1.0 - T))
		for I in range(Segments):
			P0, P1 = Points[I], Points[I + 1]
			DX, DY, DZ = P1[0] - P0[0], P1[1] - P0[1], P1[2] - P0[2]
			Horiz = math.hypot(DX, DY)
			Yaw = math.degrees(math.atan2(DY, DX))
			Pitch = math.degrees(math.atan2(DZ, Horiz))
			self.Box(f"{Name}_Rope{I}", ((P0[0] + P1[0]) * 0.5, (P0[1] + P1[1]) * 0.5, (P0[2] + P1[2]) * 0.5), (math.hypot(Horiz, DZ), 1.6, 1.6), "EnvRope",
					 QuatFromEuler(Pitch=Pitch, Yaw=Yaw))
		Length = math.dist(A[:2], B[:2])
		Count = max(1, int(Length // Spacing))
		Yaw = math.degrees(math.atan2(B[1] - A[1], B[0] - A[0]))
		for I in range(Count):
			T = (I + 0.5) / Count
			P = (A[0] + (B[0] - A[0]) * T, A[1] + (B[1] - A[1]) * T, A[2] + (B[2] - A[2]) * T - Sag * 4.0 * T * (1.0 - T))
			ItemFn(f"{Name}_Item{I}", P, Yaw, I)

	def Bunting(self, Name, A, B, Sag=40.0, Colors=("EnvClothRed", "EnvClothYellow", "EnvClothBlue", "EnvClothWhite", "EnvClothGreen")):
		def Flag(N, P, Yaw, I):
			# 마름모 깃발(45도 돌린 얇은 판)의 위 꼭짓점을 줄에 건다
			self.Box(N, (P[0], P[1], P[2] - 17.0), (24.0, 1.2, 24.0), Colors[I % len(Colors)], QuatFromEuler(Pitch=45.0, Yaw=Yaw))
		self.Line(Name, A, B, Sag, None, 38.0, Flag)

	def Laundry(self, Name, A, B, Sag=25.0):
		Cloth = ["EnvClothWhite", "EnvClothBlue", "EnvClothWhite", "EnvCanvas", "EnvClothRed", "EnvClothWhite", "EnvClothYellow"]

		def Item(N, P, Yaw, I):
			W, H = (42.0, 60.0) if I % 3 else (55.0, 38.0)
			self.Box(N, (P[0], P[1], P[2] - H * 0.5 - 1.0), (W, 1.5, H), Cloth[I % len(Cloth)], QuatFromEuler(Yaw=Yaw))
		self.Line(Name, A, B, Sag, None, 70.0, Item)

	# ---- 울타리 / 돌담 / 계단 ---------------------------------------------------------------------------------------
	def Fence(self, Name, A, B, Spacing=190.0, Height=105.0, Collide=True):
		Length = math.dist(A, B)
		Count = max(1, int(round(Length / Spacing)))
		Yaw = math.degrees(math.atan2(B[1] - A[1], B[0] - A[0]))
		Mid = ((A[0] + B[0]) * 0.5, (A[1] + B[1]) * 0.5)
		for I in range(Count + 1):
			T = I / Count
			PX, PY = A[0] + (B[0] - A[0]) * T, A[1] + (B[1] - A[1]) * T
			G = self.Height(PX, PY)
			self.Box(f"{Name}_Post{I}", (PX, PY, G + Height * 0.5 - 8.0), (13.0, 13.0, Height + 16.0), "EnvTimber", QuatFromEuler(Yaw=Yaw + self.Rng.uniform(-6, 6)))
		for I in range(Count):
			T0, T1 = I / Count, (I + 1) / Count
			X0, Y0 = A[0] + (B[0] - A[0]) * T0, A[1] + (B[1] - A[1]) * T0
			X1, Y1 = A[0] + (B[0] - A[0]) * T1, A[1] + (B[1] - A[1]) * T1
			G0, G1 = self.Height(X0, Y0), self.Height(X1, Y1)
			for K, H in enumerate((Height * 0.45, Height * 0.85)):
				Slope = math.degrees(math.atan2(G1 - G0, math.dist((X0, Y0), (X1, Y1))))
				self.Box(f"{Name}_Rail{I}_{K}", ((X0 + X1) * 0.5, (Y0 + Y1) * 0.5, (G0 + G1) * 0.5 + H), (math.dist((X0, Y0), (X1, Y1)) + 10.0, 5.0, 11.0),
						 "EnvPlanks", QuatFromEuler(Pitch=Slope, Yaw=Yaw))
		if Collide:
			self.BoxCollider(f"{Name}_Collision", (Mid[0], Mid[1], self.Height(*Mid) + 60.0), (Length * 0.5, 12.0, 60.0), Yaw)

	def StoneWall(self, Name, A, B, Height=70.0, Thick=55.0, Material="EnvStone", Collide=True, Z=None):
		# 낮은 돌담 (땅에 묻힌 몸 + 갓돌), 경사는 조각마다 따라감
		Length = math.dist(A, B)
		Count = max(1, int(math.ceil(Length / 110.0)))
		Yaw = math.degrees(math.atan2(B[1] - A[1], B[0] - A[0]))
		Rng = self.Rng
		for I in range(Count):
			T0, T1 = I / Count, (I + 1) / Count
			X0, Y0 = A[0] + (B[0] - A[0]) * T0, A[1] + (B[1] - A[1]) * T0
			X1, Y1 = A[0] + (B[0] - A[0]) * T1, A[1] + (B[1] - A[1]) * T1
			CX, CY = (X0 + X1) * 0.5, (Y0 + Y1) * 0.5
			G = (self.Height(CX, CY) if Z is None else Z)
			Seg = math.dist((X0, Y0), (X1, Y1)) + 6.0
			H = Height + Rng.uniform(-10.0, 8.0)
			Rot = QuatFromEuler(Pitch=Rng.uniform(-2, 2), Yaw=Yaw + Rng.uniform(-3, 3), Roll=Rng.uniform(-2, 2))
			self.Box(f"{Name}_Body{I}", (CX, CY, G + (H - 40.0) * 0.5), (Seg, Thick + Rng.uniform(-6, 6), H + 40.0), Material, Rot)
			self.Box(f"{Name}_Cap{I}", (CX, CY, G + H + 5.0), (Seg - 4.0, Thick + 10.0, 12.0), "EnvStoneDark", Rot)
		if Collide:
			Mid = ((A[0] + B[0]) * 0.5, (A[1] + B[1]) * 0.5)
			G = (self.Height(*Mid) if Z is None else Z)
			self.BoxCollider(f"{Name}_Collision", (Mid[0], Mid[1], G + Height * 0.5), (Length * 0.5, Thick * 0.5 + 6.0, Height * 0.5 + 20.0), Yaw)

	# ---- 폭포 --------------------------------------------------------------------------------------------------------
	def Waterfall(self, Name, X, Y, TopZ, BottomZ, Width):
		H = TopZ - BottomZ
		Root = self.Group(Name, (X, Y, BottomZ))
		# 뒤 바위 벽 + 흘러내리는 물 면 두 장(약간 앞으로 기울여 겹침) + 아래 물보라
		self.Box(f"{Name}_Water0", (0.0, 6.0, H * 0.5), (Width, 3.0, H + 20.0), "EnvWaterfall", QuatFromEuler(Roll=-6.0), Root)
		self.Box(f"{Name}_Water1", (0.0, 18.0, H * 0.45), (Width * 0.75, 3.0, H * 0.9), "EnvWaterfall", QuatFromEuler(Roll=-10.0), Root)
		self.Particles(f"{Name}_Mist", Fx("HD2DWaterfallMist"), (0.0, 30.0, 0.0), Root)
		return Root

	# ---- 종탑 (정사각 석탑 + 종 칸 + 십자 박공 지붕) ------------------------------------------------------------------
	def Tower(self, Name, X, Y, Size, Height, Roof="EnvRoofSlate", Z=None):
		Z = self.Height(X, Y) if Z is None else Z
		Root = self.Group(Name, (X, Y, Z))
		self.Box(f"{Name}_Body", (0.0, 0.0, (Height - 60.0) * 0.5), (Size, Size, Height + 60.0), "EnvStone", None, Root)
		for Index, H in enumerate((Height * 0.33, Height * 0.66)):
			self.Box(f"{Name}_Band{Index}", (0.0, 0.0, H), (Size + 12.0, Size + 12.0, 16.0), "EnvStoneDark", None, Root)
		# 종 칸: 앞·옆 아치 구멍(어두운 판) + 종(구)
		BellZ = Height - 120.0
		for Sign in (-1, 1):
			self.Box(f"{Name}_OpenSide{Sign}", (Sign * (Size * 0.5 + 1.0), 0.0, BellZ), (4.0, Size * 0.45, 130.0), "EnvWindowDark", None, Root)
		self.Box(f"{Name}_OpenFront", (0.0, Size * 0.5 + 1.0, BellZ), (Size * 0.45, 4.0, 130.0), "EnvWindowDark", None, Root)
		self.Ball(f"{Name}_Bell", (0.0, Size * 0.5 + 6.0, BellZ + 10.0), 46.0, "EnvIron", Root, 1.1)
		self.Window(f"{Name}_Win", 0.0, Size * 0.5, Height * 0.45, 40.0, 90.0, 1, None, Root)
		self.Box(f"{Name}_Eave", (0.0, 0.0, Height + 8.0), (Size + 24.0, Size + 24.0, 16.0), "EnvStoneDark", None, Root)
		Top = Height + 16.0
		self.Roof(f"{Name}_RoofA", Root, Top, Size + 10.0, Size + 10.0, "X", 62.0, Roof, "EnvStone", Eave=18.0, GableOver=14.0, Cap=False)
		Rise = self.Roof(f"{Name}_RoofB", Root, Top, Size + 10.0, Size + 10.0, "Y", 62.0, Roof, "EnvStone", Eave=18.0, GableOver=14.0, Cap=False)
		self.Box(f"{Name}_Spire", (0.0, 0.0, Top + Rise + 40.0), (8.0, 8.0, 90.0), "EnvIron", None, Root)
		self.Box(f"{Name}_Cross", (0.0, 0.0, Top + Rise + 62.0), (36.0, 8.0, 8.0), "EnvIron", None, Root)
		self.BoxCollider(f"{Name}_Collision", (X, Y, Z + Height * 0.5), (Size * 0.5, Size * 0.5, Height * 0.5), 0.0)
		self.Reserve(X, Y, Size * 0.75)
		return Root

	# ---- 우물 (돌 테두리 + 기둥 둘 + 작은 지붕 + 도르래·두레박) -------------------------------------------------------
	def Well(self, Name, X, Y):
		Z = self.Height(X, Y)
		Root = self.Group(Name, (X, Y, Z), 8.0)
		Outer, T, H = 180.0, 30.0, 85.0
		for Index, (CX, CY, SX, SY) in enumerate(((0.0, (Outer - T) * 0.5, Outer, T), (0.0, -(Outer - T) * 0.5, Outer, T),
												  ((Outer - T) * 0.5, 0.0, T, Outer - 2 * T), (-(Outer - T) * 0.5, 0.0, T, Outer - 2 * T))):
			self.Box(f"{Name}_Rim{Index}", (CX, CY, (H - 20.0) * 0.5), (SX, SY, H + 20.0), "EnvStone", None, Root)
			self.Box(f"{Name}_RimCap{Index}", (CX, CY, H + 5.0), (SX + 8.0, SY + 8.0, 10.0), "EnvStoneDark", None, Root)
		self.Box(f"{Name}_Water", (0.0, 0.0, H - 45.0), (Outer - 2 * T, Outer - 2 * T, 4.0), "EnvWindowDark", None, Root)
		for Sign in (-1, 1):
			self.Box(f"{Name}_Post{Sign}", (Sign * (Outer * 0.5 - 15.0), 0.0, 130.0), (16.0, 16.0, 260.0), "EnvTimber", None, Root)
		self.Box(f"{Name}_Axle", (0.0, 0.0, 205.0), (Outer - 10.0, 9.0, 9.0), "EnvTimber", None, Root)
		self.Box(f"{Name}_Crank", (Outer * 0.5 + 6.0, 0.0, 195.0), (5.0, 5.0, 26.0), "EnvIron", None, Root)
		self.Box(f"{Name}_Rope", (0.0, 0.0, 170.0), (2.0, 2.0, 70.0), "EnvRope", None, Root)
		self.S.Add(f"{Name}_Bucket", {"ModelComponent": {"AssetPath": f"{PH}/wooden_bucket_01/wooden_bucket_01.gltf"}}, (0.0, 0.0, 120.0), None, (1.0, 1.0, 1.0), Root)
		self.Roof(f"{Name}_Roof", Root, 258.0, Outer + 30.0, Outer + 20.0, "X", 40.0, "EnvRoofRed", None, Eave=22.0, GableOver=16.0, Thick=10.0)
		self.BoxCollider(f"{Name}_Collision", (X, Y, Z + 60.0), (Outer * 0.5, Outer * 0.5, 60.0), 8.0)
		self.Reserve(X, Y, 150.0)
		return Root

	# ---- A자 천막 -----------------------------------------------------------------------------------------------------
	def Tent(self, Name, X, Y, Yaw, Length=300.0, Span=260.0, Height=190.0, Material="EnvCanvas"):
		Z = self.Height(X, Y)
		Root = self.Group(Name, (X, Y, Z - 4.0), Yaw)
		Pitch = math.degrees(math.atan2(Height, Span * 0.5))
		self.Roof(f"{Name}_Cloth", Root, 0.0, Span, Length, "Y", Pitch, Material, Material, Eave=6.0, GableOver=8.0, Thick=4.0, Cap=False)
		# 입구: 어두운 안쪽 + 걷어 묶은 천 자락 둘
		self.Box(f"{Name}_Opening", (0.0, Length * 0.5 + 1.0, Height * 0.28), (Span * 0.3, 3.0, Height * 0.56), "EnvTimber", None, Root)
		for Sign in (-1, 1):
			self.Box(f"{Name}_Flap{Sign}", (Sign * Span * 0.17, Length * 0.5 + 4.0, Height * 0.3), (Span * 0.08, 4.0, Height * 0.6), Material,
					 QuatFromEuler(Pitch=Sign * 12.0), Root)
		for Sign in (-1, 1):
			self.Box(f"{Name}_Pole{Sign}", (0.0, Sign * (Length * 0.5 + 6.0), Height * 0.5 + 10.0), (6.0, 6.0, Height + 20.0), "EnvTimber", None, Root)
		self.Box(f"{Name}_RidgePole", (0.0, 0.0, Height + 4.0), (6.0, Length + 30.0, 6.0), "EnvTimber", None, Root)
		self.BoxCollider(f"{Name}_Collision", (X, Y, Z + Height * 0.4), (Span * 0.4, Length * 0.45, Height * 0.4), Yaw)
		self.Reserve(X, Y, max(Length, Span) * 0.55)
		return Root

	# ---- 나무 다리 ---------------------------------------------------------------------------------------------------
	def Bridge(self, Name, X, Y, Yaw, Length, Width, DeckZ):
		# 로컬 X = 다리 길이 방향. 판자(띄엄띄엄 색 다름) + 들보 + 난간 기둥·손잡이 (아치처럼 가운데를 조금 올림)
		Root = self.Group(Name, (X, Y, DeckZ), Yaw)
		Planks = int(Length // 32.0)
		for I in range(Planks):
			T = (I + 0.5) / Planks - 0.5
			Lift = 22.0 * (1.0 - (2.0 * T) ** 2)
			self.Box(f"{Name}_Plank{I}", (T * Length, 0.0, Lift - 5.0), (29.0, Width + self.Rng.uniform(-8, 8), 9.0), "EnvPlanks",
					 QuatFromEuler(Pitch=math.degrees(math.atan(-176.0 * T / Length)), Yaw=self.Rng.uniform(-1.5, 1.5)), Root)
		for Sign in (-1, 1):
			self.Box(f"{Name}_Stringer{Sign}", (0.0, Sign * (Width * 0.5 - 12.0), -18.0), (Length + 20.0, 16.0, 24.0), "EnvTimber", None, Root)
			Posts = 5
			for I in range(Posts):
				T = I / (Posts - 1) - 0.5
				Lift = 22.0 * (1.0 - (2.0 * T) ** 2)
				self.Box(f"{Name}_Post{Sign}_{I}", (T * (Length - 20.0), Sign * (Width * 0.5 + 4.0), Lift + 42.0), (11.0, 11.0, 104.0), "EnvTimber", None, Root)
			for I in range(Posts - 1):
				T0, T1 = I / (Posts - 1) - 0.5, (I + 1) / (Posts - 1) - 0.5
				Z0, Z1 = 22.0 * (1.0 - (2.0 * T0) ** 2) + 88.0, 22.0 * (1.0 - (2.0 * T1) ** 2) + 88.0
				self.Beam(f"{Name}_Rail{Sign}_{I}", (T0 * (Length - 20.0), Sign * (Width * 0.5 + 4.0), Z0), (T1 * (Length - 20.0), Sign * (Width * 0.5 + 4.0), Z1), 9.0, 9.0, "EnvTimber", Root)
		# 걷는 면: 가운데가 높은 판을 두 경사 상자로 근사 (끝은 길 높이)
		R = math.radians(Yaw)
		for Sign in (-1, 1):
			CX, CY = X + math.cos(R) * Sign * Length * 0.25, Y + math.sin(R) * Sign * Length * 0.25
			self.S.Add(f"{Name}_Walk{Sign}", {"BoxColliderComponent": {"HalfExtents": [Length * 0.26, Width * 0.5, 10.0]}},
					   (CX, CY, DeckZ + 1.0), QuatFromEuler(Pitch=Sign * -math.degrees(math.atan2(22.0, Length * 0.5)), Yaw=Yaw))
			RX, RY = X - math.sin(R) * Sign * (Width * 0.5 + 6.0), Y + math.cos(R) * Sign * (Width * 0.5 + 6.0)
			self.BoxCollider(f"{Name}_RailCollision{Sign}", (RX, RY, DeckZ + 60.0), (Length * 0.5, 8.0, 60.0), Yaw)
		return Root

	# ---- 풍차 (석탑 + 지붕 + 앞을 보는 날개 넷 — 날개는 CircleWalker(반지름 0)로 앞뒤 축 둘레를 돈다) --------------------
	def Windmill(self, Name, X, Y, Yaw=0.0, Size=380.0, Height=720.0, Speed=-14.0):
		Z = self.Height(X, Y)
		Root = self.Group(Name, (X, Y, Z), Yaw)
		# 몸통: 아래가 넓은 석탑 세 단 + 띠
		for Index, (H0, H1_, S_) in enumerate(((-60.0, 260.0, Size), (260.0, 520.0, Size * 0.88), (520.0, Height, Size * 0.78))):
			self.Box(f"{Name}_Body{Index}", (0.0, 0.0, (H0 + H1_) * 0.5), (S_, S_, H1_ - H0), "EnvStone" if Index < 2 else "EnvPlaster", None, Root)
			self.Box(f"{Name}_Band{Index}", (0.0, 0.0, H1_), (S_ + 14.0, S_ + 14.0, 14.0), "EnvTimber", None, Root)
		self.Door(f"{Name}_Door", 0.0, Size * 0.5, 0.0, 100.0, 200.0, Root, Arch=True)
		self.Window(f"{Name}_Win0", 0.0, Size * 0.44, 390.0, 52.0, 70.0, 2, "EnvShutterGreen", Root)
		self.Window(f"{Name}_Win1", 0.0, Size * 0.39, 620.0, 40.0, 50.0, 1, None, Root)
		Top = Height + 7.0
		self.Roof(f"{Name}_Roof", Root, Top, Size * 0.78 + 20.0, Size * 0.78 + 20.0, "X", 52.0, "EnvRoofBrown", "EnvPlaster", Eave=26.0, GableOver=20.0)
		# 날개 축: 부모를 Roll 90 → 로컬 Z = 월드 +Y(카메라 쪽, Yaw 적용 전 기준), 자식이 로컬 Z 둘레로 돈다
		HubY, HubZ = Size * 0.39 + 40.0, Height - 60.0
		self.Box(f"{Name}_Axle", (0.0, HubY - 30.0, HubZ), (26.0, 70.0, 26.0), "EnvTimber", None, Root)
		Axis = self.S.Add(f"{Name}_SailAxis", {}, (0.0, HubY, HubZ), QuatFromEuler(Roll=90.0), (1.0, 1.0, 1.0), Root)
		Spinner = self.S.Add(f"{Name}_Sails", {"ScriptComponent": {"ScriptAsset": "Scripts/CircleWalker.lua", "ExecutionLocation": 2,
			"PropertyOverrides": json.dumps({"Radius": 0.0, "Speed": Speed, "StartAngle": 15.0, "FacingYaw": 0.0}, ensure_ascii=False)}},
			(0.0, 0.0, 0.0), None, (1.0, 1.0, 1.0), Axis)
		self.Box(f"{Name}_Hub", (0.0, 0.0, 6.0), (40.0, 40.0, 30.0), "EnvTimber", None, Spinner)
		for Index in range(4):
			Arm = self.S.Add(f"{Name}_Arm{Index}", {}, (0.0, 0.0, 12.0), QuatFromEuler(Yaw=Index * 90.0), (1.0, 1.0, 1.0), Spinner)
			L = Size * 1.15
			self.Box(f"{Name}_Arm{Index}_Spar", (L * 0.5, 0.0, 0.0), (L, 12.0, 10.0), "EnvTimber", None, Arm)
			self.Box(f"{Name}_Arm{Index}_Sail", (L * 0.58, 42.0, -3.0), (L * 0.78, 70.0, 2.0), "EnvCanvas", None, Arm)
			for K in range(4):
				self.Box(f"{Name}_Arm{Index}_Lath{K}", (L * 0.22 + K * L * 0.24, 42.0, 2.0), (5.0, 78.0, 4.0), "EnvTimber", None, Arm)
			self.Box(f"{Name}_Arm{Index}_Rail", (L * 0.58, 80.0, 2.0), (L * 0.8, 5.0, 4.0), "EnvTimber", None, Arm)
		self.BoxCollider(f"{Name}_Collision", (X, Y, Z + Height * 0.5), (Size * 0.5, Size * 0.5, Height * 0.5), Yaw)
		self.Reserve(X, Y, Size * 0.8)
		return Root

	# ---- 허수아비 / 이정표 --------------------------------------------------------------------------------------------
	def Scarecrow(self, Name, X, Y, Yaw=0.0):
		Z = self.Height(X, Y)
		Root = self.Group(Name, (X, Y, Z), Yaw)
		self.Box(f"{Name}_Pole", (0.0, 0.0, 85.0), (8.0, 8.0, 190.0), "EnvTimber", None, Root)
		self.Box(f"{Name}_Arms", (0.0, 0.0, 135.0), (130.0, 7.0, 7.0), "EnvTimber", None, Root)
		self.Box(f"{Name}_Coat", (0.0, 2.0, 118.0), (56.0, 26.0, 56.0), "EnvClothRed", QuatFromEuler(Pitch=3.0), Root)
		for Sign in (-1, 1):
			self.Box(f"{Name}_Sleeve{Sign}", (Sign * 46.0, 2.0, 132.0), (38.0, 18.0, 18.0), "EnvClothRed", None, Root)
		self.Ball(f"{Name}_Head", (0.0, 2.0, 168.0), 30.0, "EnvCanvas", Root)
		self.Box(f"{Name}_HatBrim", (0.0, 2.0, 182.0), (54.0, 54.0, 4.0), "EnvWheat", QuatFromEuler(Roll=8.0), Root)
		self.Box(f"{Name}_HatTop", (0.0, 2.0, 192.0), (26.0, 26.0, 18.0), "EnvWheat", None, Root)
		return Root

	def Signpost(self, Name, X, Y, Arrows):
		# Arrows: [(Yaw, 높이)] — 길 쪽을 가리키는 판자
		Z = self.Height(X, Y)
		Root = self.Group(Name, (X, Y, Z))
		self.Box(f"{Name}_Pole", (0.0, 0.0, 100.0), (12.0, 12.0, 220.0), "EnvTimber", None, Root)
		for Index, (Yaw, H) in enumerate(Arrows):
			Arm = self.S.Add(f"{Name}_Arm{Index}", {}, (0.0, 0.0, H), QuatFromEuler(Yaw=Yaw), (1.0, 1.0, 1.0), Root)
			self.Box(f"{Name}_Board{Index}", (48.0, 0.0, 0.0), (90.0, 4.0, 22.0), "EnvPlanks", None, Arm)
			self.Box(f"{Name}_Tip{Index}", (93.0, 0.0, 0.0), (16.0, 4.0, 16.0), "EnvPlanks", QuatFromEuler(Pitch=45.0), Arm)
		self.BoxCollider(f"{Name}_Collision", (X, Y, Z + 90.0), (10.0, 10.0, 90.0), 0.0)
		self.Reserve(X, Y, 60.0)
		return Root

	# ---- 텃밭 (흙 이랑 + 양배추·당근 잎 줄) -------------------------------------------------------------------------
	def Garden(self, Name, X, Y, W, D, Rows=5):
		Z = self.Height(X, Y)
		Root = self.Group(Name, (X, Y, Z))
		Rng = self.Rng
		for Row in range(Rows):
			RY = -D * 0.5 + D * (Row + 0.5) / Rows
			self.Box(f"{Name}_Bed{Row}", (0.0, RY, 2.0), (W, D / Rows * 0.62, 16.0), "EnvSoil", None, Root)
			Crop = ("EnvCabbage", "EnvLeaf", "EnvCabbage", "EnvFlowerYellow", "EnvLeaf")[Row % 5]
			Count = int(W // 48.0)
			for K in range(Count):
				CX = -W * 0.5 + W * (K + 0.5) / Count + Rng.uniform(-5, 5)
				if Crop == "EnvCabbage":
					self.Ball(f"{Name}_Crop{Row}_{K}", (CX, RY, 14.0), Rng.uniform(24.0, 32.0), Crop, Root, 0.75)
				elif Crop == "EnvLeaf":
					self.Ball(f"{Name}_Crop{Row}_{K}", (CX, RY, 16.0), Rng.uniform(16.0, 22.0), Crop, Root, 1.3)
				else:
					self.Ball(f"{Name}_Crop{Row}_{K}", (CX, RY, 14.0), 18.0, "EnvLeaf", Root, 0.8)
					self.Ball(f"{Name}_Bloom{Row}_{K}", (CX, RY, 26.0), 9.0, Crop, Root)
		self.Reserve(X, Y, max(W, D) * 0.55)
		return Root

	# ---- 게시판 / 매단 간판 -------------------------------------------------------------------------------------------
	def NoticeBoard(self, Name, X, Y, Yaw=0.0):
		Z = self.Height(X, Y)
		Root = self.Group(Name, (X, Y, Z), Yaw)
		for Sign in (-1, 1):
			self.Box(f"{Name}_Post{Sign}", (Sign * 85.0, 0.0, 110.0), (12.0, 12.0, 220.0), "EnvTimber", None, Root)
		self.Box(f"{Name}_Board", (0.0, 2.0, 140.0), (170.0, 6.0, 110.0), "EnvPlanks", None, Root)
		self.Box(f"{Name}_Frame", (0.0, 5.0, 198.0), (184.0, 8.0, 10.0), "EnvTimber", None, Root)
		self.Box(f"{Name}_Sill", (0.0, 5.0, 82.0), (184.0, 8.0, 10.0), "EnvTimber", None, Root)
		self.Roof(f"{Name}_Roof", Root, 222.0, 50.0, 200.0, "X", 35.0, "EnvRoofBrown", None, Eave=14.0, GableOver=10.0, Thick=6.0, Cap=False)
		Papers = ((-55.0, 160.0, 34.0, 42.0, -4.0, "EnvClothWhite"), (-8.0, 150.0, 30.0, 38.0, 3.0, "EnvCanvas"), (40.0, 165.0, 36.0, 30.0, -2.0, "EnvClothWhite"),
				  (-35.0, 110.0, 40.0, 28.0, 2.0, "EnvClothYellow"), (30.0, 115.0, 28.0, 36.0, -5.0, "EnvClothWhite"), (62.0, 118.0, 20.0, 26.0, 6.0, "EnvCanvas"))
		for Index, (PX, PZ, W, H, Tilt, Material) in enumerate(Papers):
			self.Box(f"{Name}_Paper{Index}", (PX, 6.0, PZ), (W, 1.0, H), Material, QuatFromEuler(Pitch=Tilt), Root)
			self.Box(f"{Name}_Pin{Index}", (PX, 7.5, PZ + H * 0.4), (3.0, 1.5, 3.0), "EnvClothRed", None, Root)
		self.BoxCollider(f"{Name}_Collision", (X, Y, Z + 100.0), (95.0, 15.0, 100.0), Yaw)
		self.Reserve(X, Y, 120.0)
		return Root

	def HangingSign(self, Name, X, Y, Z, Symbol="Bed", Parent=-1):
		# 벽에서 앞(+Y)으로 내민 쇠 팔 + 매단 판자 + 그림(침대·잔 — 작은 상자 조합)
		Root = self.Group(Name, (X, Y, Z), 0.0, Parent)
		self.Box(f"{Name}_Arm", (0.0, 45.0, 0.0), (6.0, 90.0, 6.0), "EnvIron", None, Root)
		self.Box(f"{Name}_Brace", (0.0, 25.0, -22.0), (5.0, 60.0, 5.0), "EnvIron", QuatFromEuler(Roll=-35.0), Root)
		for Sign in (-1, 1):
			self.Box(f"{Name}_Chain{Sign}", (Sign * 4.0, 70.0, -14.0), (2.0, 2.0, 24.0), "EnvIron", None, Root)
		Board = self.Group(f"{Name}_BoardRoot", (0.0, 70.0, -26.0), 0.0, Root)
		self.Box(f"{Name}_Board", (0.0, 0.0, -30.0), (78.0, 5.0, 60.0), "EnvPlanks", None, Board)
		self.Box(f"{Name}_Edge", (0.0, 0.0, -30.0), (86.0, 4.0, 68.0), "EnvTimber", None, Board)
		for Side in (-1, 1):
			if Symbol == "Bed":
				Parts = ((0.0, -38.0, 46.0, 10.0, "EnvClothRed"), (-20.0, -30.0, 12.0, 10.0, "EnvClothWhite"), (-26.0, -40.0, 4.0, 28.0, "EnvTimber"), (26.0, -42.0, 4.0, 20.0, "EnvTimber"))
			else:
				Parts = ((0.0, -32.0, 22.0, 26.0, "EnvClothYellow"), (14.0, -32.0, 8.0, 12.0, "EnvClothYellow"), (0.0, -18.0, 24.0, 6.0, "EnvClothWhite"))
			for Index, (PX, PZ, W, H, Material) in enumerate(Parts):
				self.Box(f"{Name}_Mark{Side}_{Index}", (PX, Side * 3.5, PZ), (W, 1.5, H), Material, None, Board)
		return Root

	# ---- 벼랑 동굴 입구 (바위 벽 + 무너진 아치 + 어두운 안쪽 + 횃불) ----------------------------------------------------
	def CaveMouth(self, Name, X, Y, Z=None, Kit="Asset/KayKit/Dungeon"):
		Z = self.Height(X, Y) if Z is None else Z
		Root = self.Group(Name, (X, Y, Z))
		Rng = self.Rng
		# 바위 벽 덩어리 (아치 뒤·옆) — 놀이 영역 뒤 경계 너머
		for Index, (DX, DY, SX, SY, SZ, Yaw) in enumerate(((-420.0, -120.0, 360.0, 300.0, 520.0, 14.0), (430.0, -110.0, 380.0, 300.0, 560.0, -12.0),
														   (0.0, -260.0, 700.0, 260.0, 640.0, 3.0), (-720.0, -40.0, 300.0, 260.0, 380.0, 32.0),
														   (720.0, -60.0, 320.0, 240.0, 420.0, -28.0), (0.0, -140.0, 520.0, 160.0, 160.0, 0.0))):
			Top = SZ if Index != 5 else 640.0
			self.Box(f"{Name}_Rock{Index}", (DX, DY, (Top - 40.0) * 0.5 if Index != 5 else 520.0), (SX, SY, Top + 40.0 if Index != 5 else SZ), "EnvCliff",
					 QuatFromEuler(Pitch=Rng.uniform(-5, 5), Yaw=Yaw, Roll=Rng.uniform(-5, 5)), Root)
		# 돌 문틀(기둥 둘 + 상인방 + 쐐기돌) + 안쪽 어둠(입구 너머가 보이지 않게 깊은 검은 상자)
		for Side in (-1, 1):
			self.Box(f"{Name}_Jamb{Side}", (Side * 165.0, 30.0, 140.0), (70.0, 90.0, 320.0), "EnvStone", QuatFromEuler(Yaw=Side * 3.0), Root)
			self.Box(f"{Name}_JambBase{Side}", (Side * 165.0, 34.0, 15.0), (92.0, 104.0, 40.0), "EnvStoneDark", None, Root)
		self.Box(f"{Name}_Lintel", (0.0, 30.0, 330.0), (430.0, 100.0, 70.0), "EnvStone", QuatFromEuler(Pitch=1.5), Root)
		self.Box(f"{Name}_Keystone", (0.0, 36.0, 372.0), (70.0, 100.0, 50.0), "EnvStoneDark", None, Root)
		self.Box(f"{Name}_Dark", (0.0, -80.0, 140.0), (270.0, 200.0, 300.0), "EnvCaveDark", None, Root)
		self.S.Add(f"{Name}_Mist", {"PointLightComponent": {"Color": [0.35, 0.55, 1.0], "Intensity": 1.5, "Radius": 300.0, "CastShadows": False}},
				   (0.0, -40.0, 120.0), None, (1.0, 1.0, 1.0), Root)
		for Side in (-1, 1):
			self.S.Add(f"{Name}_Torch{Side}", {"ModelComponent": {"AssetPath": f"{Kit}/torch_mounted.glb"}}, (Side * 175.0, 72.0, 230.0),
					   QuatFromEuler(Yaw=-90.0), (1.0, 1.0, 1.0), Root)
			self.Particles(f"{Name}_TorchFlame{Side}", "Particles/Demo/CampfireLanternFlame.eparticle", (Side * 175.0, 95.0, 300.0), Root)
		self.S.Add(f"{Name}_Sign", {"StaticMeshComponent": {"MeshAsset": CUBE, "MaterialAsset": Mat("EnvPlanks")}}, (-330.0, 190.0, 120.0),
				   QuatFromEuler(Yaw=8.0, Roll=-6.0), (0.9, 0.05, 0.5), Root)
		self.Box(f"{Name}_SignPost", (-330.0, 190.0, 60.0), (10.0, 10.0, 120.0), "EnvTimber", None, Root)
		return Root


def WriteCastleKit(Content, OutRel):
	# 성채: 둥근 탑 + 얇은 성벽 + 성문 + 성벽 + 둥근 탑 (바깥면 = +Y(카메라 쪽), 로컬 원점 = 서쪽 끝 바깥면). 키트 조각 로컬: 길이 → -X, 두께 → +Y
	#   Yaw 180 → 길이 +X, 두께 -Y (BuildTraining.py 실측과 같은 규약)
	from GltfKit import FGltfKitComposer, EngineToGltf
	Fort = "modular_fort_01"
	K = FGltfKitComposer(os.path.join(Content, *PH.split("/")), os.path.join(Content, *OutRel.split("/")))

	def Piece(Name, X, Y, Yaw):
		K.Add(Fort, f"{Fort}_{Name}", EngineToGltf(X, Y, -40.0, Yaw))

	X = 600.0
	for Name, Length in (("wall_thin_straight_01", 1482.0), ("wall_thin_gate_01", 741.0), ("wall_thin_straight_02", 1482.0)):
		Piece(Name, X, 0.0, 180.0)
		X += Length
	Piece("tower_round", 300.0, -380.0, 0.0)
	Piece("tower_round", X + 300.0, -380.0, 40.0)
	# 성문 안쪽 높은 성벽 (뒤로 보이는 겹)
	Piece("wall_thick_straight_01", 1600.0, -1400.0, 180.0)
	Piece("wall_thick_straight_02", 3056.0, -1400.0, 180.0)
	K.Save()
	return X + 300.0
