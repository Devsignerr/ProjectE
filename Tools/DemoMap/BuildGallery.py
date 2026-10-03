# 데모 서브맵 "Gallery"(조각 전시 홀 — 머티리얼 갤러리) 생성: 머티리얼(.emat) + 데칼 텍스처 + 임포트 설정 + 씬(Scenes/Demo/Gallery.escene)
#   실행: python Tools/DemoMap/BuildGallery.py [--overview[=x,y,z,pitch,yaw]]  (먼저 Scripts/FetchDemoAssets.ps1로 Poly Haven 에셋을 받는다)
#   --overview: 플레이어 대신 고정 카메라를 둔 확인용 변형(Scenes/Demo/_GalleryOverview.escene)도 쓴다 — 커밋하지 않는다
#   보여 주는 기능: 머티리얼 그래프(삼평면 받침대·영상 작품·빛 기둥·투조 칸막이·유리), 머티리얼 인스턴스(Parent 체인 — 금속 거칠기 사다리·
#                   색 바꿈), 블렌드 모드(Opaque/Masked/Translucent/Additive) + 양면, 천창 햇빛 + 볼류메트릭 안개, DDGI + RTAO,
#                   RT 반사(광택 석재 바닥), 데칼(물웅덩이·때·금), IES 그림 조명, 텍스처 밉 스트리밍(2k 텍스처 다수)
#   좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 홀 안쪽 X [-1400, 1400](길이) × Y [-650, 650](폭) × Z [0, 760], 가운데 줄에 천창 3개
#   배치를 바꿀 때는 씬 파일이 아니라 이 스크립트를 고치고 다시 실행한다 (결정적 — 데칼 텍스처 난수도 시드 고정)
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(__file__))
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402
from AssetFixes import GLASS_FIX, FrameModel, WriteGlassFixedModel  # noqa: E402

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTENT = os.path.join(ROOT, "Projects", "Sample", "Content")
PH      = "Asset/PolyHaven"
MAT_DIR = "Materials/Demo/Gallery"
TEX_REL = "../../../Asset/PolyHaven"  # MAT_DIR 기준 Poly Haven 폴더

HALF_X, HALF_Y, HEIGHT = 1400.0, 650.0, 760.0
WALL = 40.0
SKY_X = [-820.0, 0.0, 820.0]          # 천창 가운데 X (거의 이어진 유리 지붕 — 사이는 구조 보)
SKY_HX, SKY_HY = 350.0, 250.0         # 천창 구멍 반 크기
SKY_WELL = 160.0                      # 천창 우물 높이 (천장 위)
COVE_INTENSITY = 90.0
BAY_X = [-1200.0, -800.0, -400.0, 0.0, 400.0, 800.0, 1200.0]  # 긴 벽 칸 가운데 (벽기둥 사이)


def Model(Id):
	return f"{PH}/{Id}/{Id}.gltf"


def FaceYaw(DX, DY):
	# Poly Haven 모델 정면 = 엔진 -X(glTF +Z) → 정면이 (DX, DY) 방향을 보게 하는 Yaw
	return math.degrees(math.atan2(-DY, -DX))


def AimRotation(From, To):
	# 로컬 +X(빛 방향)가 From → To를 향하는 회전 (UE 부호: -Pitch = 아래)
	DX, DY, DZ = To[0] - From[0], To[1] - From[1], To[2] - From[2]
	Yaw = math.degrees(math.atan2(DY, DX))
	Pitch = math.degrees(math.atan2(DZ, math.hypot(DX, DY)))
	return QuatFromEuler(Pitch=Pitch, Yaw=Yaw)


def WriteJson(Path, Doc):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")


def MatPath(Name):
	return os.path.join(CONTENT, MAT_DIR, f"{Name}.emat")


def Tex(Id, Map):
	return f"{TEX_REL}/{Id}/{Id}_{Map}_2k.jpg"


# ---- 머티리얼 -------------------------------------------------------------------------------------------------------
# 상자 건축면: Lighting의 타일 PBR 그래프(TiledPBR)를 부모로 하는 인스턴스 — (텍스처 Id, 타일 한 장 크기 cm, 거칠기 배율, 색 배율)
SURFACES = {
	"Floor":     ("marble_01", 260.0, 0.32, [1.0, 0.97, 0.93]),          # 광택 석회암 판석 (RT 반사)
	"Inlay":     ("granite_tile", 180.0, 0.45, [0.55, 0.55, 0.58]),       # 바닥 띠 (짙은 화강암)
	"Wall":      ("plastered_wall_04", 300.0, 1.0, [0.72, 0.84, 0.8]),   # 긴 벽: 짙은 세이지색 미장 (미술관 색 벽)
	"EndWall":   ("plastered_wall_04", 300.0, 1.0, [1.25, 1.2, 1.1]),     # 끝 벽·천장: 따뜻한 흰 미장
	"Trim":      ("plastered_wall_04", 200.0, 0.8, [1.45, 1.4, 1.3]),     # 벽기둥·코니스·천창 우물 (흰 몰딩)
	"Base":      ("granite_tile", 120.0, 0.6, [0.42, 0.42, 0.44]),        # 걸레받이
}


def WriteBaseMaterials():
	# 이 폴더는 생성물 전용 — 이전 실행의 인스턴스를 지우고 다시 쓴다
	Folder = os.path.join(CONTENT, MAT_DIR)
	if os.path.isdir(Folder):
		for Name in os.listdir(Folder):
			if Name.endswith(".emat"):
				os.remove(os.path.join(Folder, Name))

	# 1) 삼평면 석재/미장 (받침대): 월드 위치 → 축별 UV 3개, |법선|^k 가중 — 상자 크기와 무관하게 결이 이어진다
	WriteJson(MatPath("TriplanarStone"), {
		"Name": "TriplanarStone", "BlendMode": "Opaque",
		"Parameters": [
			{"Name": "BaseTexture", "Type": "Texture", "Value": Tex("granite_tile", "diff"), "Usage": "Color"},
			{"Name": "ArmTexture", "Type": "Texture", "Value": Tex("granite_tile", "arm"), "Usage": "Linear"},
			{"Name": "Tint", "Type": "Vector", "Value": [1.0, 1.0, 1.0, 1.0]},
			{"Name": "WorldScale", "Type": "Scalar", "Value": 0.006},
			{"Name": "RoughnessScale", "Type": "Scalar", "Value": 1.0},
		],
		"Graph": {
			"Nodes": [
				{"Id": "pos", "Type": "WorldPosition"},
				{"Id": "scale", "Type": "ScalarParameter", "Parameter": "WorldScale"},
				{"Id": "p", "Type": "Multiply", "Inputs": {"A": "pos", "B": "scale"}},
				{"Id": "uvX", "Type": "ComponentMask", "Channels": "yz", "Inputs": {"A": "p"}},
				{"Id": "uvY", "Type": "ComponentMask", "Channels": "xz", "Inputs": {"A": "p"}},
				{"Id": "uvZ", "Type": "ComponentMask", "Channels": "xy", "Inputs": {"A": "p"}},
				{"Id": "bX", "Type": "TextureSample", "Texture": "BaseTexture", "Inputs": {"UV": "uvX"}},
				{"Id": "bY", "Type": "TextureSample", "Texture": "BaseTexture", "Inputs": {"UV": "uvY"}},
				{"Id": "bZ", "Type": "TextureSample", "Texture": "BaseTexture", "Inputs": {"UV": "uvZ"}},
				{"Id": "aX", "Type": "TextureSample", "Texture": "ArmTexture", "Inputs": {"UV": "uvX"}},
				{"Id": "aY", "Type": "TextureSample", "Texture": "ArmTexture", "Inputs": {"UV": "uvY"}},
				{"Id": "aZ", "Type": "TextureSample", "Texture": "ArmTexture", "Inputs": {"UV": "uvZ"}},
				{"Id": "n", "Type": "WorldNormal"},
				{"Id": "absN", "Type": "Abs", "Inputs": {"A": "n"}},
				{"Id": "wRaw", "Type": "Power", "Inputs": {"A": "absN", "B": 6.0}},
				{"Id": "wSum", "Type": "Dot", "Inputs": {"A": "wRaw", "B": 1.0}},
				{"Id": "w", "Type": "Divide", "Inputs": {"A": "wRaw", "B": "wSum"}},
				{"Id": "ws", "Type": "Split", "Inputs": {"A": "w"}},
				{"Id": "cX", "Type": "Multiply", "Inputs": {"A": "bX:1", "B": "ws:0"}},
				{"Id": "cY", "Type": "Multiply", "Inputs": {"A": "bY:1", "B": "ws:1"}},
				{"Id": "cZ", "Type": "Multiply", "Inputs": {"A": "bZ:1", "B": "ws:2"}},
				{"Id": "cXY", "Type": "Add", "Inputs": {"A": "cX", "B": "cY"}},
				{"Id": "c", "Type": "Add", "Inputs": {"A": "cXY", "B": "cZ"}},
				{"Id": "tint", "Type": "VectorParameter", "Parameter": "Tint"},
				{"Id": "color", "Type": "Multiply", "Inputs": {"A": "c", "B": "tint:1"}},
				{"Id": "rX", "Type": "Multiply", "Inputs": {"A": "aX:3", "B": "ws:0"}},
				{"Id": "rY", "Type": "Multiply", "Inputs": {"A": "aY:3", "B": "ws:1"}},
				{"Id": "rZ", "Type": "Multiply", "Inputs": {"A": "aZ:3", "B": "ws:2"}},
				{"Id": "rXY", "Type": "Add", "Inputs": {"A": "rX", "B": "rY"}},
				{"Id": "r", "Type": "Add", "Inputs": {"A": "rXY", "B": "rZ"}},
				{"Id": "roughScale", "Type": "ScalarParameter", "Parameter": "RoughnessScale"},
				{"Id": "rough", "Type": "Multiply", "Inputs": {"A": "r", "B": "roughScale"}},
			],
			"Output": {"BaseColor": "color", "Roughness": "rough", "Metallic": 0.0},
		},
	})
	# 받침대 인스턴스 (색 바꿈): 흰 칠 / 짙은 화강암
	WriteJson(MatPath("Plinth_White"), {"Name": "Plinth_White", "Parent": "TriplanarStone.emat", "Parameters": [
		{"Name": "BaseTexture", "Type": "Texture", "Value": Tex("plastered_wall_04", "diff")},
		{"Name": "ArmTexture", "Type": "Texture", "Value": Tex("plastered_wall_04", "arm")},
		{"Name": "Tint", "Type": "Vector", "Value": [1.55, 1.52, 1.45, 1.0]},
		{"Name": "WorldScale", "Type": "Scalar", "Value": 0.004}]})
	WriteJson(MatPath("Plinth_Granite"), {"Name": "Plinth_Granite", "Parent": "TriplanarStone.emat", "Parameters": [
		{"Name": "Tint", "Type": "Vector", "Value": [0.6, 0.6, 0.62, 1.0]},
		{"Name": "RoughnessScale", "Type": "Scalar", "Value": 0.55}]})

	# 2) 견본 공 부모 그래프: 색·금속·거칠기 파라미터만 — 인스턴스가 덮어쓴다 (Parent 체인 2단: Base → Gold → Gold_Rxx)
	WriteJson(MatPath("SampleBase"), {
		"Name": "SampleBase", "BlendMode": "Opaque",
		"Parameters": [
			{"Name": "BaseColor", "Type": "Vector", "Value": [0.8, 0.8, 0.8, 1.0]},
			{"Name": "Metallic", "Type": "Scalar", "Value": 0.0},
			{"Name": "Roughness", "Type": "Scalar", "Value": 0.5},
		],
		"Graph": {
			"Nodes": [
				{"Id": "color", "Type": "VectorParameter", "Parameter": "BaseColor"},
				{"Id": "metal", "Type": "ScalarParameter", "Parameter": "Metallic"},
				{"Id": "rough", "Type": "ScalarParameter", "Parameter": "Roughness"},
			],
			"Output": {"BaseColor": "color:1", "Metallic": "metal", "Roughness": "rough"},
		},
	})
	WriteJson(MatPath("SampleGold"), {"Name": "SampleGold", "Parent": "SampleBase.emat", "Parameters": [
		{"Name": "BaseColor", "Type": "Vector", "Value": [1.0, 0.77, 0.34, 1.0]},
		{"Name": "Metallic", "Type": "Scalar", "Value": 1.0}]})
	for Rough in (5, 30, 55, 85):
		WriteJson(MatPath(f"SampleGold_R{Rough:02d}"), {"Name": f"SampleGold_R{Rough:02d}", "Parent": "SampleGold.emat", "Parameters": [
			{"Name": "Roughness", "Type": "Scalar", "Value": Rough / 100.0}]})
	WriteJson(MatPath("SampleCopper"), {"Name": "SampleCopper", "Parent": "SampleGold.emat", "Parameters": [
		{"Name": "BaseColor", "Type": "Vector", "Value": [0.95, 0.6, 0.48, 1.0]},
		{"Name": "Roughness", "Type": "Scalar", "Value": 0.25}]})
	WriteJson(MatPath("SampleLacquer"), {"Name": "SampleLacquer", "Parent": "SampleBase.emat", "Parameters": [
		{"Name": "BaseColor", "Type": "Vector", "Value": [0.42, 0.015, 0.02, 1.0]},
		{"Name": "Roughness", "Type": "Scalar", "Value": 0.03}]})

	# 3) 유리 (Translucent 그래프): 프레넬로 가장자리 불투명도·테두리 빛 — 진열장 유리는 같은 그래프의 인스턴스
	WriteJson(MatPath("SampleGlass"), {
		"Name": "SampleGlass", "BlendMode": "Translucent",
		"Parameters": [
			{"Name": "Tint", "Type": "Vector", "Value": [0.7, 0.9, 1.0, 1.0]},
			{"Name": "RimColor", "Type": "Vector", "Value": [0.45, 0.75, 1.0, 1.0]},
			{"Name": "RimIntensity", "Type": "Scalar", "Value": 0.6},
			{"Name": "MinOpacity", "Type": "Scalar", "Value": 0.12},
			{"Name": "MaxOpacity", "Type": "Scalar", "Value": 0.85},
		],
		"Graph": {
			"Nodes": [
				{"Id": "fresnel", "Type": "Fresnel", "Inputs": {"Exponent": 4.0, "BaseReflectFraction": 0.0}},
				{"Id": "minO", "Type": "ScalarParameter", "Parameter": "MinOpacity"},
				{"Id": "maxO", "Type": "ScalarParameter", "Parameter": "MaxOpacity"},
				{"Id": "opacity", "Type": "Lerp", "Inputs": {"A": "minO", "B": "maxO", "Alpha": "fresnel"}},
				{"Id": "tint", "Type": "VectorParameter", "Parameter": "Tint"},
				{"Id": "rimColor", "Type": "VectorParameter", "Parameter": "RimColor"},
				{"Id": "rimI", "Type": "ScalarParameter", "Parameter": "RimIntensity"},
				{"Id": "rim", "Type": "Multiply", "Inputs": {"A": "rimColor:1", "B": "fresnel"}},
				{"Id": "emissive", "Type": "Multiply", "Inputs": {"A": "rim", "B": "rimI"}},
			],
			"Output": {"BaseColor": "tint:1", "Metallic": 0.0, "Roughness": 0.03, "Emissive": "emissive", "Opacity": "opacity"},
		},
	})
	WriteJson(MatPath("VitrineGlass"), {"Name": "VitrineGlass", "Parent": "SampleGlass.emat", "Parameters": [
		{"Name": "Tint", "Type": "Vector", "Value": [0.85, 0.95, 0.95, 1.0]},
		{"Name": "RimIntensity", "Type": "Scalar", "Value": 0.0},
		{"Name": "MinOpacity", "Type": "Scalar", "Value": 0.04},
		{"Name": "MaxOpacity", "Type": "Scalar", "Value": 0.45}]})
	WriteJson(MatPath("SkylightGlass"), {"Name": "SkylightGlass", "Parent": "SampleGlass.emat", "Parameters": [
		{"Name": "Tint", "Type": "Vector", "Value": [0.9, 0.95, 1.0, 1.0]},
		{"Name": "RimIntensity", "Type": "Scalar", "Value": 0.0},
		{"Name": "MinOpacity", "Type": "Scalar", "Value": 0.06},
		{"Name": "MaxOpacity", "Type": "Scalar", "Value": 0.3}]})

	# 4) 빛 구슬 (Additive 그래프): 가운데가 밝은 코어(1 - 프레넬) × Time 맥동
	WriteJson(MatPath("SampleGlowOrb"), {
		"Name": "SampleGlowOrb", "BlendMode": "Additive",
		"Parameters": [
			{"Name": "GlowColor", "Type": "Vector", "Value": [1.0, 0.2, 0.03, 1.0]},
			{"Name": "GlowIntensity", "Type": "Scalar", "Value": 2.6},
			{"Name": "PulseSpeed", "Type": "Scalar", "Value": 2.2},
		],
		"Graph": {
			"Nodes": [
				{"Id": "fresnel", "Type": "Fresnel", "Inputs": {"Exponent": 1.5, "BaseReflectFraction": 0.0}},
				{"Id": "core", "Type": "OneMinus", "Inputs": {"A": "fresnel"}},
				{"Id": "coreSharp", "Type": "Power", "Inputs": {"A": "core", "B": 2.0}},
				{"Id": "time", "Type": "Time"},
				{"Id": "speed", "Type": "ScalarParameter", "Parameter": "PulseSpeed"},
				{"Id": "phase", "Type": "Multiply", "Inputs": {"A": "time", "B": "speed"}},
				{"Id": "sin", "Type": "Sine", "Inputs": {"A": "phase"}},
				{"Id": "pulse", "Type": "Lerp", "Inputs": {"A": 0.55, "B": 1.0, "Alpha": "sin"}},
				{"Id": "color", "Type": "VectorParameter", "Parameter": "GlowColor"},
				{"Id": "intensity", "Type": "ScalarParameter", "Parameter": "GlowIntensity"},
				{"Id": "g1", "Type": "Multiply", "Inputs": {"A": "color:1", "B": "coreSharp"}},
				{"Id": "g2", "Type": "Multiply", "Inputs": {"A": "g1", "B": "pulse"}},
				{"Id": "emissive", "Type": "Multiply", "Inputs": {"A": "g2", "B": "intensity"}},
			],
			"Output": {"BaseColor": [0.0, 0.0, 0.0], "Emissive": "emissive", "Opacity": 1.0},
		},
	})

	# 5) 영상 작품 (Opaque 그래프, 발광): 흐르는 사인 띠 두 겹(Panner) + Frac 격자 + 시간에 따라 바뀌는 색
	WriteJson(MatPath("VideoArt"), {
		"Name": "VideoArt", "BlendMode": "Opaque",
		"Parameters": [
			{"Name": "ColorA", "Type": "Vector", "Value": [0.05, 0.25, 0.9, 1.0]},
			{"Name": "ColorB", "Type": "Vector", "Value": [1.0, 0.35, 0.1, 1.0]},
			{"Name": "Brightness", "Type": "Scalar", "Value": 2.2},
			{"Name": "Cells", "Type": "Vector", "Value": [32.0, 18.0, 0.0, 0.0]},
		],
		"Graph": {
			"Nodes": [
				{"Id": "uv", "Type": "TexCoord"},
				{"Id": "panA", "Type": "Panner", "Inputs": {"UV": "uv", "Speed": [0.06, 0.015]}},
				{"Id": "panB", "Type": "Panner", "Inputs": {"UV": "uv", "Speed": [-0.035, 0.05]}},
				{"Id": "a", "Type": "Dot", "Inputs": {"A": "panA", "B": [9.0, 5.0]}},
				{"Id": "b", "Type": "Dot", "Inputs": {"A": "panB", "B": [-4.0, 11.0]}},
				{"Id": "sa", "Type": "Sine", "Inputs": {"A": "a"}},
				{"Id": "sb", "Type": "Sine", "Inputs": {"A": "b"}},
				{"Id": "wave", "Type": "Multiply", "Inputs": {"A": "sa", "B": "sb"}},
				{"Id": "wave01", "Type": "Multiply", "Inputs": {"A": "wave", "B": 0.5}},
				{"Id": "waveN", "Type": "Add", "Inputs": {"A": "wave01", "B": 0.5}},
				{"Id": "time", "Type": "Time"},
				{"Id": "t2", "Type": "Multiply", "Inputs": {"A": "time", "B": 0.35}},
				{"Id": "shift", "Type": "Sine", "Inputs": {"A": "t2"}},
				{"Id": "shift01", "Type": "Multiply", "Inputs": {"A": "shift", "B": 0.35}},
				{"Id": "mix", "Type": "Add", "Inputs": {"A": "waveN", "B": "shift01"}},
				{"Id": "mixS", "Type": "Saturate", "Inputs": {"A": "mix"}},
				{"Id": "colA", "Type": "VectorParameter", "Parameter": "ColorA"},
				{"Id": "colB", "Type": "VectorParameter", "Parameter": "ColorB"},
				{"Id": "col", "Type": "Lerp", "Inputs": {"A": "colA:1", "B": "colB:1", "Alpha": "mixS"}},
				{"Id": "cells", "Type": "VectorParameter", "Parameter": "Cells"},
				{"Id": "cellsXY", "Type": "Append", "Inputs": {"A": "cells:2", "B": "cells:3"}},
				{"Id": "grid", "Type": "Multiply", "Inputs": {"A": "uv", "B": "cellsXY"}},
				{"Id": "gridF", "Type": "Frac", "Inputs": {"A": "grid"}},
				{"Id": "gridC", "Type": "Subtract", "Inputs": {"A": "gridF", "B": 0.5}},
				{"Id": "gridD", "Type": "Length", "Inputs": {"A": "gridC"}},
				{"Id": "dot", "Type": "Compare", "Op": "Less", "Inputs": {"A": "gridD", "B": 0.42, "True": 1.0, "False": 0.25}},
				{"Id": "bright", "Type": "ScalarParameter", "Parameter": "Brightness"},
				{"Id": "e1", "Type": "Multiply", "Inputs": {"A": "col", "B": "dot"}},
				{"Id": "e2", "Type": "Multiply", "Inputs": {"A": "e1", "B": "waveN"}},
				{"Id": "emissive", "Type": "Multiply", "Inputs": {"A": "e2", "B": "bright"}},
			],
			"Output": {"BaseColor": [0.01, 0.01, 0.01], "Roughness": 0.25, "Emissive": "emissive"},
		},
	})

	# 6) 빛 기둥 (Additive 그래프): 월드 높이를 따라 올라가는 빛 띠 (Time)
	WriteJson(MatPath("LightColumn"), {
		"Name": "LightColumn", "BlendMode": "Additive", "TwoSided": True,
		"Parameters": [
			{"Name": "GlowColor", "Type": "Vector", "Value": [0.35, 0.75, 1.0, 1.0]},
			{"Name": "GlowIntensity", "Type": "Scalar", "Value": 1.1},
			{"Name": "Speed", "Type": "Scalar", "Value": 2.5},
		],
		"Graph": {
			"Nodes": [
				{"Id": "pos", "Type": "WorldPosition"},
				{"Id": "z", "Type": "ComponentMask", "Channels": "z", "Inputs": {"A": "pos"}},
				{"Id": "zs", "Type": "Multiply", "Inputs": {"A": "z", "B": 0.025}},
				{"Id": "time", "Type": "Time"},
				{"Id": "speed", "Type": "ScalarParameter", "Parameter": "Speed"},
				{"Id": "ts", "Type": "Multiply", "Inputs": {"A": "time", "B": "speed"}},
				{"Id": "phase", "Type": "Subtract", "Inputs": {"A": "zs", "B": "ts"}},
				{"Id": "s", "Type": "Sine", "Inputs": {"A": "phase"}},
				{"Id": "s01", "Type": "Saturate", "Inputs": {"A": "s"}},
				{"Id": "band", "Type": "Power", "Inputs": {"A": "s01", "B": 6.0}},
				{"Id": "level", "Type": "Add", "Inputs": {"A": "band", "B": 0.25}},
				{"Id": "color", "Type": "VectorParameter", "Parameter": "GlowColor"},
				{"Id": "intensity", "Type": "ScalarParameter", "Parameter": "GlowIntensity"},
				{"Id": "g", "Type": "Multiply", "Inputs": {"A": "color:1", "B": "level"}},
				{"Id": "emissive", "Type": "Multiply", "Inputs": {"A": "g", "B": "intensity"}},
			],
			"Output": {"BaseColor": [0.0, 0.0, 0.0], "Emissive": "emissive", "Opacity": 1.0},
		},
	})

	# 7) 투조 황동 칸막이 (Masked + 양면 그래프): UV 격자마다 원 구멍 — 판 한 장(primitive:plane)을 양쪽에서 본다
	WriteJson(MatPath("LatticeScreen"), {
		"Name": "LatticeScreen", "BlendMode": "Masked", "AlphaCutoff": 0.5, "TwoSided": True,
		"Parameters": [
			{"Name": "Cells", "Type": "Vector", "Value": [10.0, 14.0, 0.0, 0.0]},
			{"Name": "HoleRadius", "Type": "Scalar", "Value": 0.36},
			{"Name": "Metal", "Type": "Vector", "Value": [0.78, 0.58, 0.3, 1.0]},
			{"Name": "BaseTexture", "Type": "Texture", "Value": Tex("granite_tile", "diff"), "Usage": "Color"},
		],
		"Graph": {
			"Nodes": [
				{"Id": "uv", "Type": "TexCoord"},
				{"Id": "cells", "Type": "VectorParameter", "Parameter": "Cells"},
				{"Id": "cellsXY", "Type": "Append", "Inputs": {"A": "cells:2", "B": "cells:3"}},
				{"Id": "grid", "Type": "Multiply", "Inputs": {"A": "uv", "B": "cellsXY"}},
				{"Id": "f", "Type": "Frac", "Inputs": {"A": "grid"}},
				{"Id": "c", "Type": "Subtract", "Inputs": {"A": "f", "B": 0.5}},
				{"Id": "d", "Type": "Length", "Inputs": {"A": "c"}},
				{"Id": "radius", "Type": "ScalarParameter", "Parameter": "HoleRadius"},
				{"Id": "mask", "Type": "Compare", "Op": "Greater", "Inputs": {"A": "d", "B": "radius", "True": 1.0, "False": 0.0}},
				{"Id": "grain", "Type": "TextureSample", "Texture": "BaseTexture", "Inputs": {"UV": "grid"}},
				{"Id": "metal", "Type": "VectorParameter", "Parameter": "Metal"},
				{"Id": "grainL", "Type": "Lerp", "Inputs": {"A": 0.75, "B": 1.25, "Alpha": "grain:2"}},
				{"Id": "color", "Type": "Multiply", "Inputs": {"A": "metal:1", "B": "grainL"}},
				{"Id": "rough", "Type": "Lerp", "Inputs": {"A": 0.25, "B": 0.5, "Alpha": "grain:2"}},
			],
			"Output": {"BaseColor": "color", "Metallic": 1.0, "Roughness": "rough", "OpacityMask": "mask"},
		},
	})

	# 고정 PBR (그래프 없음): 황동 명판, 데칼
	def Plain(Name, Base, Alpha=1.0, Metallic=0.0, Rough=0.9, Texture="", Blend=None):
		Mat = {
			"Name": Name, "BaseColorFactor": list(Base) + [Alpha], "EmissiveFactor": [0.0, 0.0, 0.0],
			"Metallic": Metallic, "Roughness": Rough, "NormalScale": 1.0, "OcclusionStrength": 1.0,
			"BaseColorTexture": Texture, "MetallicRoughnessTexture": "", "NormalTexture": "", "OcclusionTexture": "", "EmissiveTexture": "",
		}
		if Blend:
			Mat["BlendMode"] = Blend
		WriteJson(MatPath(Name), Mat)

	Plain("PlaqueBrass", (0.82, 0.62, 0.32), Metallic=1.0, Rough=0.35)
	Plain("ScreenFrame", (0.03, 0.03, 0.035), Metallic=0.6, Rough=0.4)
	Plain("DecalGrime", (0.22, 0.19, 0.16), Alpha=0.7, Rough=0.85, Texture="Grime.png")
	Plain("DecalCrack", (0.08, 0.07, 0.06), Rough=0.9, Texture="Crack.png")
	Plain("DecalPuddle", (0.16, 0.17, 0.19), Alpha=0.85, Rough=0.03, Texture="../../../Decals/Puddle.png")


def WriteDecalTextures():
	# 데칼 텍스처 (RGBA, 알파 = 덮는 정도) — 시드 고정 난수라 다시 실행해도 같은 파일
	Folder = os.path.join(CONTENT, MAT_DIR)
	os.makedirs(Folder, exist_ok=True)
	Rng = np.random.default_rng(1729)
	Size = 512

	def ValueNoise(Cells):
		Grid = Rng.random((Cells + 1, Cells + 1))
		Img = Image.fromarray((Grid * 255).astype(np.uint8)).resize((Size, Size), Image.BICUBIC)
		return np.asarray(Img).astype(np.float32) / 255.0

	# 때/발자국 얼룩: 여러 옥타브 잡음 × 가장자리로 갈수록 0
	Noise = ValueNoise(4) * 0.5 + ValueNoise(16) * 0.3 + ValueNoise(64) * 0.2
	Y, X = np.mgrid[0:Size, 0:Size].astype(np.float32) / (Size - 1) * 2.0 - 1.0
	Radial = np.clip(1.0 - np.sqrt(X * X + Y * Y), 0.0, 1.0)
	Alpha = np.clip((Noise - 0.42) * 2.6, 0.0, 1.0) * np.clip(Radial * 1.8, 0.0, 1.0)
	Rgba = np.zeros((Size, Size, 4), np.uint8)
	Rgba[..., 0:3] = (np.clip(0.75 + Noise[..., None] * 0.35, 0, 1) * 255).astype(np.uint8)
	Rgba[..., 3] = (Alpha * 255).astype(np.uint8)
	Image.fromarray(Rgba, "RGBA").save(os.path.join(Folder, "Grime.png"))

	# 바닥 금: 가지 치는 무작위 걸음 선 (흐림 + 가장자리 페이드)
	Crack = Image.new("L", (Size, Size), 0)
	Draw = ImageDraw.Draw(Crack)

	def Walk(X0, Y0, Angle, Length, Width, Depth):
		Px, Py = X0, Y0
		for Step in range(Length):
			Angle += Rng.normal(0.0, 0.35)
			Nx, Ny = Px + math.cos(Angle) * 6.0, Py + math.sin(Angle) * 6.0
			Draw.line([(Px, Py), (Nx, Ny)], fill=255, width=max(1, int(round(Width))))
			Px, Py = Nx, Ny
			Width = max(1.0, Width * 0.985)
			if Depth < 3 and Rng.random() < 0.06:
				Walk(Px, Py, Angle + Rng.choice([-1.0, 1.0]) * Rng.uniform(0.5, 1.1), int(Length * 0.45), Width * 0.7, Depth + 1)

	Walk(40.0, 256.0, 0.05, 75, 4.0, 0)
	Crack = Crack.filter(ImageFilter.GaussianBlur(0.8))
	CrackA = np.asarray(Crack).astype(np.float32) / 255.0
	Edge = np.clip(1.0 - np.abs(X) ** 6, 0.0, 1.0) * np.clip(1.0 - np.abs(Y) ** 6, 0.0, 1.0)
	Rgba = np.zeros((Size, Size, 4), np.uint8)
	Rgba[..., 0:3] = 200
	Rgba[..., 3] = (np.clip(CrackA * 1.4, 0.0, 1.0) * Edge * 255).astype(np.uint8)
	Image.fromarray(Rgba, "RGBA").save(os.path.join(Folder, "Crack.png"))


class FMaterials:
	# 표면 + 면 크기 → 타일 인스턴스(.emat) 경로 (부모 = Lighting 폴더의 TiledPBR 그래프). 같은 키는 한 파일
	def __init__(self):
		self.Written = {}

	def Get(self, Surface, U, V):
		Id, Tile, RoughScale, Tint = SURFACES[Surface]
		TU, TV = round(U / Tile, 2), round(V / Tile, 2)
		Name = f"{Surface}_{int(round(U))}x{int(round(V))}"
		if Name not in self.Written:
			Doc = {
				"Name": Name, "Parent": "../Lighting/TiledPBR.emat",
				"Parameters": [
					{"Name": "BaseTexture", "Type": "Texture", "Value": Tex(Id, "diff")},
					{"Name": "ArmTexture", "Type": "Texture", "Value": Tex(Id, "arm")},
					{"Name": "NormalTexture", "Type": "Texture", "Value": Tex(Id, "nor_gl")},
					{"Name": "Tiling", "Type": "Vector", "Value": [max(TU, 0.05), max(TV, 0.05), 0.0, 0.0]},
					{"Name": "Tint", "Type": "Vector", "Value": Tint + [1.0]},
					{"Name": "RoughnessScale", "Type": "Scalar", "Value": RoughScale},
				],
			}
			WriteJson(MatPath(Name), Doc)
			self.Written[Name] = True
		return f"{MAT_DIR}/{Name}.emat"


# 임포트 설정: 실제 glTF 삼각형 수 기준(2026-10-04 확인)으로 쿠킹 때 LOD0 상한
IMPORT_SETTINGS = {
	"lion_head":        {"MaxTriangles": 16000},   # 원본 4.7만
	"gothic_statue":    {"MaxTriangles": 16000},   # 원본 2.8만
	"horse_statue_01":  {"MaxTriangles": 14000},   # 원본 2.2만
	"brass_vase_01":    {"MaxTriangles": 8000},    # 원본 2.1만
	"potted_plant_02":  {"MaxTriangles": 16000, "BlendAsMasked": True},  # 원본 7만 (잎 MASK)
	"hanging_picture_frame_03": {"MaxTriangles": 6000},  # 원본 2.4만 (유리 고친 사본 Asset/Gallery 옆)
	"fancy_picture_frame_02":   {"MaxTriangles": 8000},  # 원본 3.5만
}


def WriteImportSettings():
	for Id, Settings in IMPORT_SETTINGS.items():
		Folder = os.path.join(CONTENT, "Asset", "Gallery") if Id in GLASS_FIX else os.path.join(CONTENT, "Asset", "PolyHaven", Id)
		WriteJson(os.path.join(Folder, f"{Id}.gltf.eimport"), Settings)


# 액자 유리 고침(알파 없는 jpg 유리가 그림을 가림)은 공용 AssetFixes — 고친 사본 Asset/Gallery/<Id>.gltf (Lighting과 공유)
def WriteGlassFixedModels():
	for Id in sorted(GLASS_FIX):
		WriteGlassFixedModel(CONTENT, Id)


# ---- 씬 -------------------------------------------------------------------------------------------------------------
def BuildScene(Overview=None):
	S = FScene()
	Mats = FMaterials()

	def Box(Name, Surface, Center, Size, Collide=True, Parent=-1):
		# 축 정렬 상자 (내장 큐브 100cm × Size/100). 큐브 옆면 UV = (수평, Z), 윗면/아랫면 UV = (Y, X)
		SX, SY, SZ = Size
		Thin = min(range(3), key=lambda A: Size[A])
		if Thin == 2:
			U, V = SY, SX
		elif Thin == 0:
			U, V = SY, SZ
		else:
			U, V = SX, SZ
		Comps = {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": Mats.Get(Surface, U, V)}}
		if Collide:
			Comps["BoxColliderComponent"] = {}
		return S.Add(Name, Comps, Center, None, (SX / 100.0, SY / 100.0, SZ / 100.0), Parent)

	def MatBox(Name, Material, Center, Size, Collide=True, Parent=-1, Mesh="primitive:cube", Rotation=None):
		Comps = {"StaticMeshComponent": {"MeshAsset": Mesh, "MaterialAsset": f"{MAT_DIR}/{Material}.emat"}}
		if Collide:
			Comps["BoxColliderComponent"] = {}
		return S.Add(Name, Comps, Center, Rotation, (Size[0] / 100.0, Size[1] / 100.0, Size[2] / 100.0), Parent)

	def Prop(Name, Id, X, Y, Z=0.0, Yaw=0.0, Scale=1.0, Collider=None, Parent=-1, Asset=None):
		# Collider = (반 크기, 오프셋) 모델 로컬 cm — 스케일이 곱해진다
		Extra = None
		if Collider:
			Half, Offset = Collider
			Extra = {"BoxColliderComponent": {"HalfExtents": list(Half), "Offset": list(Offset)}}
		return S.Model(Name, Asset or Model(Id), (X, Y, Z), Yaw, Scale, Parent, Extra)

	def Spot(Name, From, To, Intensity=120.0, Outer=22.0, Inner=12.0, Color=(1.0, 0.9, 0.78), Shadows=False, Radius=1100.0):
		return S.Add(Name, {"SpotLightComponent": {
			"Color": list(Color), "Intensity": Intensity, "Radius": Radius, "InnerConeAngle": Inner, "OuterConeAngle": Outer,
			"CastShadows": Shadows, "IesProfile": "", "UseIesIntensity": False, "IesIntensityScale": 1.0,
			"CookieTexture": "", "CookieScale": [1, 1], "CookiePanSpeed": [0, 0]}}, From, AimRotation(From, To))

	def Plinth(Name, X, Y, W, D, H, Material="Plinth_White", Plaque=None):
		# 받침대 (삼평면 그래프 인스턴스) + 앞면 황동 명판(Plaque = 앞 방향 (DX, DY))
		Index = MatBox(Name, Material, (X, Y, H * 0.5), (W, D, H))
		if Plaque:
			PX, PY = Plaque
			Off = (W * 0.5 if PX else D * 0.5) + 0.6
			MatBox(f"{Name}_Plaque", "PlaqueBrass", (X + PX * Off, Y + PY * Off, H * 0.72),
				(1.2 if PX else 16.0, 16.0 if PX else 1.2, 9.0), Collide=False)
		return Index

	# ---- 환경: 높은 해(천창으로 드는 빛줄기) + 대기 하늘 + 옅은 실내 볼류메트릭 안개
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.93, 0.82], "Intensity": 6.0}}, (0, 0, 3000), QuatFromEuler(Pitch=-58, Yaw=70))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {},
		"SkyLightComponent": {"Intensity": 1.0},
		"HeightFogComponent": {
			"Color": [0.5, 0.5, 0.5], "Density": 0.0025, "HeightFalloff": 0.0, "StartDistance": 0.0, "MaxOpacity": 0.2,
			"DirectionalInscatteringColor": [0.0, 0.0, 0.0],
			"Volumetric": True, "VolumetricDistance": 3500.0, "VolumetricAlbedo": [0.9, 0.9, 0.88], "VolumetricExtinctionScale": 1.0,
			"VolumetricAnisotropy": 0.6, "VolumetricDirectionalScale": 2.0, "VolumetricLocalLightScale": 0.12},
	})
	# DDGI: 홀보다 조금 큰 상자. 반사 캡처는 두지 않는다 — 굽기가 DDGI 없이 하늘빛만 담아 실내 거친 면에 푸른 반사가 얹힌다(Lighting 비교)
	S.Add("HallProbes", {"IrradianceVolumeComponent": {
		"HalfExtents": [HALF_X + 80.0, HALF_Y + 80.0, (HEIGHT + SKY_WELL) * 0.5 + 60.0], "Spacing": 140.0, "Intensity": 1.0, "FadeDistance": 30.0,
		"NormalBias": 10.0, "ViewBias": 20.0, "Hysteresis": 0.98, "RaysPerProbe": 256, "ProbeUpdateBudget": 0, "MaxRayDistance": 10000.0,
		"Relocation": True, "Classification": True, "Priority": 0, "DebugProbes": 0, "DebugProbeRadius": 8.0}},
		(0, 0, (HEIGHT + SKY_WELL) * 0.5))

	# ---- 건물: 바닥(석회암 + 화강암 띠) / 벽(긴 벽 색 미장, 끝 벽 흰 미장) / 벽기둥 / 걸레받이 / 코니스 / 천장 + 천창 3개
	Box("Floor", "Floor", (0, 0, -10.0), (2 * HALF_X, 2 * HALF_Y, 20.0))
	for Side in (-1, 1):
		Box(f"Floor_Band_{Side + 1}", "Inlay", (0, Side * (HALF_Y - 260.0), 0.05), (2 * HALF_X - 300.0, 36.0, 0.2), Collide=False)
	for Index, BX in enumerate([-HALF_X + 150.0, HALF_X - 150.0]):
		Box(f"Floor_BandEnd_{Index}", "Inlay", (BX, 0, 0.05), (36.0, 2 * (HALF_Y - 260.0) + 36.0, 0.2), Collide=False)
	Box("Wall_North", "Wall", (0, HALF_Y + WALL * 0.5, HEIGHT * 0.5), (2 * HALF_X + 2 * WALL, WALL, HEIGHT))
	Box("Wall_South", "Wall", (0, -HALF_Y - WALL * 0.5, HEIGHT * 0.5), (2 * HALF_X + 2 * WALL, WALL, HEIGHT))
	Box("Wall_West", "EndWall", (-HALF_X - WALL * 0.5, 0, HEIGHT * 0.5), (WALL, 2 * HALF_Y, HEIGHT))
	Box("Wall_East", "EndWall", (HALF_X + WALL * 0.5, 0, HEIGHT * 0.5), (WALL, 2 * HALF_Y, HEIGHT))
	# 걸레받이 + 코니스 (네 벽)
	for Side in (-1, 1):
		Box(f"Base_Long_{Side + 1}", "Base", (0, Side * (HALF_Y - 2.0), 9.0), (2 * HALF_X, 4.0, 18.0), Collide=False)
		Box(f"Base_End_{Side + 1}", "Base", (Side * (HALF_X - 2.0), 0, 9.0), (4.0, 2 * HALF_Y, 18.0), Collide=False)
		Box(f"Cornice_Long_{Side + 1}", "Trim", (0, Side * (HALF_Y - 12.0), HEIGHT - 20.0), (2 * HALF_X, 24.0, 40.0), Collide=False)
		Box(f"Cornice_End_{Side + 1}", "Trim", (Side * (HALF_X - 12.0), 0, HEIGHT - 20.0), (24.0, 2 * HALF_Y, 40.0), Collide=False)
		Box(f"Rail_Long_{Side + 1}", "Trim", (0, Side * (HALF_Y - 3.0), HEIGHT - 120.0), (2 * HALF_X, 6.0, 8.0), Collide=False)
	# 천장 간접 조명 (코브): 긴 벽 코니스 앞에서 위를 비추는 긴 면광원 — 흰 천장에 반사되어 홀 전체를 부드럽게 밝힌다
	for Side in (-1, 1):
		for Index, CX in enumerate(BAY_X):
			S.Add(f"Cove_Light_{Side + 1}_{Index}", {"AreaLightComponent": {
				"Shape": 0, "Color": [1.0, 0.92, 0.8], "Intensity": COVE_INTENSITY, "Width": 20.0, "Height": 380.0, "Radius": 1000.0,
				"TwoSided": False, "BarnDoorAngle": 88.0, "BarnDoorLength": 10.0, "CastShadows": False,
				"IesProfile": "", "UseIesIntensity": False, "IesIntensityScale": 1.0, "CookieTexture": "", "CookieScale": [1, 1], "CookiePanSpeed": [0, 0]}},
				(CX, Side * (HALF_Y - 45.0), HEIGHT - 110.0), QuatFromEuler(Pitch=90.0))
	# 벽기둥 (긴 벽, 칸 경계)
	for Index in range(len(BAY_X) - 1):
		PX = (BAY_X[Index] + BAY_X[Index + 1]) * 0.5
		for Side in (-1, 1):
			Box(f"Pilaster_{Index}_{Side + 1}", "Trim", (PX, Side * (HALF_Y - 14.0), HEIGHT * 0.5), (56.0, 28.0, HEIGHT))

	# 천장: 천창 구멍을 뺀 판 조각 (천창 줄 바깥 두 판 + 천창 사이 판) + 천창 우물 벽 + 유리 + 가로살(줄무늬 그림자)
	CeilT = 30.0
	CeilZ = HEIGHT + CeilT * 0.5
	SideW = HALF_Y - SKY_HY
	for Side in (-1, 1):
		Box(f"Ceiling_Side_{Side + 1}", "EndWall", (0, Side * (SKY_HY + SideW * 0.5), CeilZ), (2 * HALF_X + 2 * WALL, SideW + WALL, CeilT), Collide=False)
	Edges = [-HALF_X - WALL] + [E for X in SKY_X for E in (X - SKY_HX, X + SKY_HX)] + [HALF_X + WALL]
	for Index in range(0, len(Edges), 2):
		X0, X1 = Edges[Index], Edges[Index + 1]
		Box(f"Ceiling_Mid_{Index // 2}", "EndWall", ((X0 + X1) * 0.5, 0, CeilZ), (X1 - X0, 2 * SKY_HY, CeilT), Collide=False)
	WellZ = HEIGHT + SKY_WELL * 0.5
	for Index, SX in enumerate(SKY_X):
		for Side in (-1, 1):
			Box(f"Skylight_{Index}_WallY_{Side + 1}", "Trim", (SX, Side * (SKY_HY + 10.0), WellZ), (2 * SKY_HX + 40.0, 20.0, SKY_WELL), Collide=False)
			Box(f"Skylight_{Index}_WallX_{Side + 1}", "Trim", (SX + Side * (SKY_HX + 10.0), 0, WellZ), (20.0, 2 * SKY_HY, SKY_WELL), Collide=False)
		GlassZ = HEIGHT + SKY_WELL
		MatBox(f"Skylight_{Index}_Glass", "SkylightGlass", (SX, 0, GlassZ), (2 * SKY_HX, 2 * SKY_HY, 1.0), Collide=False)
		for Bar in range(-4, 5):
			Box(f"Skylight_{Index}_Bar_{Bar + 4}", "Base", (SX + Bar * (SKY_HX / 4.5), 0, GlassZ - 8.0), (6.0, 2 * SKY_HY, 12.0), Collide=False)
		# 천창 아래 바닥 가운데 장식 (화강암 테두리 사각)
		for Side in (-1, 1):
			Box(f"Floor_Medallion_{Index}_Y{Side + 1}", "Inlay", (SX, Side * 150.0, 0.06), (330.0, 18.0, 0.2), Collide=False)
			Box(f"Floor_Medallion_{Index}_X{Side + 1}", "Inlay", (SX + Side * 156.0, 0, 0.06), (18.0, 282.0, 0.2), Collide=False)

	# ---- 가운데 회랑: 천창 아래 큰 조각 3점 (짙은 화강암 받침) + 사이 오토만 의자
	# 서쪽: 청동 고래 / 가운데: 고딕 조각 / 동쪽: 말 조각(확대)
	Plinth("Center_Plinth_Whale", SKY_X[0], 0, 190.0, 150.0, 55.0, "Plinth_Granite", Plaque=(-1, 0))
	Prop("Center_Whale", "bronze_whale_statue", SKY_X[0] - 12.0, 4.0, 55.0, FaceYaw(-1, 0.25), 1.15, ((60, 50, 52), (12, -4, 52)))
	Plinth("Center_Plinth_Gothic", SKY_X[1], 0, 210.0, 210.0, 35.0, "Plinth_Granite", Plaque=(-1, 0))
	# 고딕 조각: 경계 X -37~119, Y -78~69 → 가운데(41, -4)를 받침 가운데로 (정면 -X 그대로 = 서쪽 입구를 본다)
	Prop("Center_Gothic", "gothic_statue", SKY_X[1] - 41.0, 4.0, 35.0, 0.0, 1.0, ((78, 74, 86), (41, -4, 86)))
	Plinth("Center_Plinth_Horse", SKY_X[2], 0, 150.0, 90.0, 95.0, "Plinth_Granite", Plaque=(-1, 0))
	Prop("Center_Horse", "horse_statue_01", SKY_X[2], 0, 95.0, FaceYaw(0.4, -1), 4.6)
	for Index, OX in enumerate([-380.0, 380.0]):
		for Side in (-1, 1):
			Prop(f"Ottoman_{Index}_{Side + 1}", "Ottoman_01", OX, Side * 70.0, 0.0, 90.0, 1.0, ((31, 44, 31), (0, 0, 31)))

	# 조각 조명: 천창 가장자리 천장에서 비추는 스포트 (그림자 없음 — 그림자 스포트는 안개 속에 조각 그림자 선이 떠 보였다. 그림자는 해가 만든다)
	for Index, SX in enumerate(SKY_X):
		Target = (SX, 0.0, 130.0)
		Spot(f"Center_Spot_{Index}_A", (SX - 380.0, -330.0, HEIGHT - 5.0), Target, Intensity=208.0, Outer=16.0, Inner=8.0)
		Spot(f"Center_Spot_{Index}_B", (SX + 380.0, 330.0, HEIGHT - 5.0), Target, Intensity=96.0, Outer=18.0, Inner=9.0, Color=(0.85, 0.9, 1.0))

	# ---- 북쪽 벽: 칸마다 그림(IES 벽 워셔) / 두상·흉상 받침대
	NorthFace = HALF_Y - 1.0
	WallYawN = FaceYaw(0, -1)
	Paintings = [
		(BAY_X[0], "hanging_picture_frame_02", 4.0, 270.0),
		(BAY_X[2], "hanging_picture_frame_01", 3.0, 270.0),
		(BAY_X[3], "fancy_picture_frame_01", 4.8, 300.0),
		(BAY_X[4], "hanging_picture_frame_03", 4.4, 280.0),
		(BAY_X[6], "fancy_picture_frame_02", 3.2, 280.0),
	]
	for Index, (PX, Id, Scale, PZ) in enumerate(Paintings):
		Prop(f"North_Painting_{Index}", Id, PX, NorthFace, PZ, WallYawN, Scale, Asset=FrameModel(Id))
		S.Add(f"North_Washer_{Index}", {"PointLightComponent": {
			"Color": [1.0, 0.88, 0.72], "Intensity": 6.0, "Radius": 520.0, "CastShadows": False,
			"IesProfile": "AreaLights/Wallwash_Asym.ies", "UseIesIntensity": False, "IesIntensityScale": 1.0,
			"CookieTexture": "", "CookieScale": [1, 1], "CookiePanSpeed": [0, 0]}},
			(PX, NorthFace - 90.0, HEIGHT - 60.0), QuatFromEuler(Pitch=-90.0, Yaw=-90.0))
	# 두상/흉상: 흰 받침 110cm (명판은 남쪽 = 관람 쪽)
	Heads = [(BAY_X[1], "marble_bust_01", 1.35), (BAY_X[3] - 150.0, "horse_head", 1.15), (BAY_X[3] + 150.0, "bull_head", 1.1), (BAY_X[5], "lion_head", 1.15)]
	for Index, (HX, Id, Scale) in enumerate(Heads):
		HY = HALF_Y - 120.0
		Plinth(f"North_Plinth_{Index}", HX, HY, 48.0, 48.0, 112.0, Plaque=(0, -1))
		Prop(f"North_Head_{Index}", Id, HX, HY, 112.0, FaceYaw(0.15 * (Index - 1.5), -1), Scale)
		Spot(f"North_Spot_{Index}", (HX + 60.0, HY - 330.0, HEIGHT - 5.0), (HX, HY, 150.0), Intensity=128.0, Outer=11.0, Inner=6.0)
	# 벽 앞 화분 (잎 = 마스크 머티리얼)
	for Index, PX in enumerate([BAY_X[0] + 150.0, BAY_X[6] - 150.0]):
		Prop(f"North_Plant_{Index}", "potted_plant_02", PX, HALF_Y - 70.0, 0.0, 30.0 + Index * 140.0, 1.5, ((25, 25, 40), (5, -4, 40)))

	# ---- 남쪽 벽: 진열장(유리 = Translucent 인스턴스) / 견본 공 진열대 / 도자기·황동 그릇
	SouthY = -HALF_Y + 110.0
	ViewDir = (0, 1)

	def Vitrine(Name, X, Y, W, D, H, GlassH):
		Plinth(Name, X, Y, W, D, H, Plaque=ViewDir)
		MatBox(f"{Name}_Glass", "VitrineGlass", (X, Y, H + GlassH * 0.5), (W - 2.0, D - 2.0, GlassH), Collide=False)
		Spot(f"{Name}_Spot", (X, Y + 300.0, HEIGHT - 5.0), (X, Y, H + 10.0), Intensity=112.0, Outer=12.0, Inner=7.0)

	# 투구 진열장 (DamagedHelmet: 경계 ±90 → 0.22배, 원점 = 가운데라 반 높이만큼 올린다)
	Vitrine("South_Vitrine_Helmet", BAY_X[2], SouthY, 70.0, 70.0, 105.0, 60.0)
	Prop("South_Helmet", None, BAY_X[2], SouthY, 105.0 + 90.0 * 0.22 + 4.0, FaceYaw(0, 1), 0.22, Asset="DamagedHelmet.glb")
	# 코끼리 + 소라 진열장
	Vitrine("South_Vitrine_Elephant", BAY_X[0], SouthY, 80.0, 60.0, 100.0, 50.0)
	Prop("South_Elephant", "carved_wooden_elephant", BAY_X[0] - 15.0, SouthY, 100.0, FaceYaw(0.6, 1), 3.2)
	Prop("South_Shell", "lambis_shell", BAY_X[0] + 22.0, SouthY + 5.0, 100.0, 40.0, 2.2)
	# 도자기 무리 (단 높이가 다른 받침)
	Ceramics = [("ceramic_vase_01", -70.0, 80.0), ("ceramic_vase_02", -20.0, 55.0), ("ceramic_vase_03", 25.0, 95.0), ("ceramic_vase_04", 70.0, 70.0)]
	for Index, (Id, DX, H) in enumerate(Ceramics):
		Plinth(f"South_CeramicPlinth_{Index}", BAY_X[1] + DX, SouthY, 40.0, 40.0, H, Plaque=ViewDir if Index == 0 else None)
		Prop(f"South_Ceramic_{Index}", Id, BAY_X[1] + DX, SouthY, H, Index * 47.0, 1.2)
	Spot("South_Ceramics_Spot", (BAY_X[1], SouthY + 340.0, HEIGHT - 5.0), (BAY_X[1], SouthY, 90.0), Intensity=128.0, Outer=18.0, Inner=10.0)
	# 견본 공 진열대 (가운데 칸): 금 거칠기 사다리 4 → 구리 → 옻칠 → 유리 → 빛 구슬
	Plinth("South_SampleTable", BAY_X[3], SouthY, 360.0, 60.0, 90.0, "Plinth_Granite", Plaque=ViewDir)
	Samples = ["SampleGold_R05", "SampleGold_R30", "SampleGold_R55", "SampleGold_R85", "SampleCopper", "SampleLacquer", "SampleGlass", "SampleGlowOrb"]
	for Index, Mat in enumerate(Samples):
		SX = BAY_X[3] - 157.5 + Index * 45.0
		MatBox(f"South_Sample_{Index}_Stand", "PlaqueBrass", (SX, SouthY, 92.0), (12.0, 12.0, 4.0), Collide=False)
		MatBox(f"South_Sample_{Index}", Mat, (SX, SouthY, 94.0 + 17.0), (34.0, 34.0, 34.0), Collide=False, Mesh="primitive:sphere")
	S.Add("South_Sample_OrbLight", {"PointLightComponent": {"Color": [1.0, 0.5, 0.18], "Intensity": 0.8, "Radius": 160.0, "CastShadows": False}},
		(BAY_X[3] + 157.5, SouthY + 20.0, 112.0))
	for Index, DX in enumerate([-110.0, 110.0]):
		Spot(f"South_Sample_Spot_{Index}", (BAY_X[3] + DX, SouthY + 330.0, HEIGHT - 5.0), (BAY_X[3] + DX * 0.6, SouthY, 105.0),
			Intensity=144.0, Outer=17.0, Inner=10.0, Color=(1.0, 0.97, 0.92))
	# 황동 그릇 무리
	Brass = [("brass_vase_01", -55.0, 60.0), ("brass_pot_01", 0.0, 85.0), ("brass_vase_02", 55.0, 60.0), ("brass_vase_03", 25.0, 85.0)]
	Plinth("South_BrassPlinth_A", BAY_X[4] - 55.0, SouthY, 50.0, 50.0, 60.0, Plaque=ViewDir)
	Plinth("South_BrassPlinth_B", BAY_X[4], SouthY - 5.0, 90.0, 50.0, 85.0)
	Plinth("South_BrassPlinth_C", BAY_X[4] + 55.0, SouthY, 50.0, 50.0, 60.0)
	for Index, (Id, DX, H) in enumerate(Brass):
		Prop(f"South_Brass_{Index}", Id, BAY_X[4] + DX - (12.0 if Id == "brass_pot_01" else 0.0), SouthY - 5.0, H, Index * 61.0, 1.15 if Id != "brass_vase_03" else 1.6)
	Spot("South_Brass_Spot", (BAY_X[4], SouthY + 340.0, HEIGHT - 5.0), (BAY_X[4], SouthY, 90.0), Intensity=128.0, Outer=16.0, Inner=9.0)
	# 고양이 + 상어 + 가오리 (청동/콘크리트 소품)
	Plinth("South_Plinth_Shark", BAY_X[5], SouthY, 110.0, 60.0, 80.0, Plaque=ViewDir)
	Prop("South_Shark", "bronze_shark_statue", BAY_X[5], SouthY, 80.0, FaceYaw(-1, 0.3), 1.0)
	Spot("South_Shark_Spot", (BAY_X[5], SouthY + 330.0, HEIGHT - 5.0), (BAY_X[5], SouthY, 100.0), Intensity=112.0, Outer=14.0, Inner=8.0)
	Plinth("South_Plinth_Ray", BAY_X[6] - 30.0, SouthY, 90.0, 80.0, 70.0, Plaque=ViewDir)
	Prop("South_Ray", "bronze_ray_statue", BAY_X[6] - 30.0, SouthY, 70.0, FaceYaw(-0.5, 1), 1.0)
	Plinth("South_Plinth_Cat", BAY_X[6] + 90.0, SouthY + 10.0, 40.0, 40.0, 100.0)
	Prop("South_Cat", "concrete_cat_statue", BAY_X[6] + 90.0, SouthY + 10.0, 100.0, FaceYaw(-0.3, 1), 1.3)
	Spot("South_Ray_Spot", (BAY_X[6], SouthY + 330.0, HEIGHT - 5.0), (BAY_X[6], SouthY, 90.0), Intensity=112.0, Outer=16.0, Inner=9.0)
	# 남쪽 벽 그림 (진열장 위 높이)
	for Index, (PX, Id, Scale) in enumerate([(BAY_X[1], "fancy_picture_frame_01", 2.8), (BAY_X[5], "hanging_picture_frame_03", 3.0)]):
		Prop(f"South_Painting_{Index}", Id, PX, -HALF_Y + 1.0, 300.0, FaceYaw(0, 1), Scale, Asset=FrameModel(Id))
	# 큰 꽃병 (동쪽 끝 모서리)
	Plinth("South_Plinth_Amphora", HALF_X - 90.0, -HALF_Y + 90.0, 60.0, 60.0, 70.0)
	Prop("South_Amphora", "antique_ceramic_vase_01", HALF_X - 90.0, -HALF_Y + 90.0, 70.0, 20.0, 1.6)

	# ---- 동쪽 끝: 미디어 공간 — 영상 작품(그래프 발광) + 빛 기둥(Additive) + 투조 황동 칸막이(Masked 양면)
	ScreenW, ScreenH, ScreenZ = 420.0, 236.0, 300.0
	MatBox("East_ScreenFrame", "ScreenFrame", (HALF_X - 6.0, 0, ScreenZ), (8.0, ScreenW + 24.0, ScreenH + 24.0), Collide=False)
	# 내장 큐브 옆면 UV: X면 → (Y, Z) 0~1 — 화면 한 장에 그래프 UV 0~1이 맞는다
	MatBox("East_VideoArt", "VideoArt", (HALF_X - 11.0, 0, ScreenZ), (2.0, ScreenW, ScreenH), Collide=False)
	for Index, CY in enumerate([-340.0, -290.0, 290.0, 340.0]):
		MatBox(f"East_LightColumn_{Index}", "LightColumn", (HALF_X - 30.0, CY, 210.0), (5.0, 5.0, 400.0), Collide=False)
	for Side in (-1, 1):
		# 칸막이: 판(법선 +Z)을 세워(Roll 90 → 법선 ∓Y) X 방향으로 길게 — 벽기둥 줄과 나란히 미디어 공간을 감싼다
		CX, CY = HALF_X - 330.0, Side * 420.0
		MatBox(f"East_Lattice_{Side + 1}", "LatticeScreen", (CX, CY, 165.0), (240.0, 300.0, 1.0), Collide=False,
			Mesh="primitive:plane", Rotation=QuatFromEuler(Roll=90.0))
		Box(f"East_Lattice_{Side + 1}_Top", "Base", (CX, CY, 318.0), (246.0, 6.0, 6.0), Collide=False)
		Box(f"East_Lattice_{Side + 1}_Foot", "Base", (CX, CY, 7.0), (246.0, 14.0, 14.0))
		for End in (-1, 1):
			Box(f"East_Lattice_{Side + 1}_Post_{End + 1}", "Base", (CX + End * 123.0, CY, 160.0), (6.0, 6.0, 320.0))
	for Side in (-1, 1):
		Prop(f"East_Ottoman_{Side + 1}", "Ottoman_01", HALF_X - 420.0, Side * 90.0, 0.0, 0.0, 1.0, ((31, 44, 31), (0, 0, 31)))

	# ---- 서쪽 끝: 입구 (돌아가는 포털 = 성문 모양 큰 문) + 화분 + 물 새는 천창 아래 물웅덩이(+ 미끄럼 주의 표지)
	for Side in (-1, 1):
		Prop(f"West_Plant_{Side + 1}", "potted_plant_02", -HALF_X + 70.0, Side * 230.0, 0.0, 70.0 * Side, 1.4, ((25, 25, 40), (5, -4, 40)))
		Prop(f"West_Bench_{Side + 1}", "painted_wooden_bench", -HALF_X + 50.0, Side * 470.0, 0.0, FaceYaw(1, 0), 1.0, ((24, 58, 22), (0, 0, 22)))
	Prop("West_WetSign", "WetFloorSign_01", SKY_X[0] + 140.0, -230.0, 0.0, FaceYaw(1, -0.4), 1.0)

	# 데칼: 천창 아래 물웅덩이(광택), 입구 발길 때, 바닥 금 (로컬 -Z로 찍힘)
	def Decal(Name, Material, Center, Size, Yaw=0.0, Opacity=1.0, Order=0, Normal=False):
		S.Add(Name, {"DecalComponent": {"MaterialAsset": f"{MAT_DIR}/{Material}.emat", "Size": list(Size), "Opacity": Opacity, "SortOrder": Order,
			"AffectBaseColor": True, "AffectNormal": Normal, "AffectRoughness": True, "FadeStartDistance": 0.0, "FadeEndDistance": 0.0}},
			Center, QuatFromEuler(Yaw=Yaw))
	Decal("Decal_Puddle", "DecalPuddle", (SKY_X[0] + 90.0, -150.0, 0.0), (220.0, 170.0, 40.0), Yaw=25.0, Order=2)
	Decal("Decal_Grime_Entry", "DecalGrime", (-HALF_X + 160.0, 0.0, 0.0), (300.0, 420.0, 40.0), Yaw=90.0, Opacity=0.8)
	Decal("Decal_Grime_Center", "DecalGrime", (-260.0, 40.0, 0.0), (380.0, 300.0, 40.0), Yaw=12.0, Opacity=0.5)
	Decal("Decal_Grime_East", "DecalGrime", (520.0, -60.0, 0.0), (340.0, 260.0, 40.0), Yaw=-30.0, Opacity=0.45)
	Decal("Decal_Crack_A", "DecalCrack", (-980.0, 330.0, 0.0), (260.0, 260.0, 40.0), Yaw=35.0, Order=1)
	Decal("Decal_Crack_B", "DecalCrack", (1080.0, -280.0, 0.0), (220.0, 220.0, 40.0), Yaw=-70.0, Order=1)

	# 돌아가는 포털: 서쪽 벽 가운데 큰 문 (DemoPortal.lua — Hub로, 돌아가기 자리 기억 안 함)
	Portal = S.Add("Portal_Hub", {"ScriptComponent": {"ScriptAsset": "Scripts/Demo/DemoPortal.lua", "ExecutionLocation": 0,
		"PropertyOverrides": json.dumps({"TargetScene": {"Asset": "Scenes/Demo/Hub.escene"}, "Label": "Hub로", "Ready": True,
			"RememberReturn": False}, ensure_ascii=False)}},
		(-HALF_X + 12.0, 0.0, 0.0), QuatFromEuler(Yaw=0.0))
	S.Model("Portal_Hub_Door", Model("large_castle_door"), (0, 0, -3), 0.0, 1.0, Parent=Portal)
	S.Add("Portal_Hub_Glow", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Demo/PortalGlow.emat"}},
		(18, 0, 148), None, (0.02, 1.85, 2.85), Parent=Portal)
	S.Add("Portal_Hub_Light", {"PointLightComponent": {"Color": [0.5, 0.75, 1.0], "Intensity": 1.5, "Radius": 350.0, "CastShadows": False}}, (80, 0, 220), Parent=Portal)

	if Overview:
		S.Add("OverviewCamera", {"CameraComponent": {"FovYDegrees": 70.0, "NearZ": 5.0, "FarZ": 20000.0, "Primary": True, "Priority": 100}},
			Overview[:3], QuatFromEuler(Pitch=Overview[3], Yaw=Overview[4]))
	else:
		# 플레이어: 입구 앞에서 동쪽(+X) 회랑을 보며 시작 (포털 축에서 비켜 선다 — 3인칭 카메라가 벽에 막혀 포털 발광면 뒤로 가면 화면이 하얗게 덮인다) — 프리팹 인스턴스는 루트만 저장 (PrefabLink.Root = 자기 인덱스)
		PlayerIndex = len(S.Entities)
		S.Add("Player", {
			"PrefabInstanceComponent": {"Asset": "Prefabs/Demo/DemoPlayer.eprefab", "Overrides": ""},
			"PrefabLinkComponent": {"Id": "1", "Root": PlayerIndex}},
			(-HALF_X + 340.0, -280.0, 110.0))
	return S, Mats


OVERVIEW_VIEW = (-1330.0, -560.0, 560.0, -18.0, 20.0)  # 확인용 카메라 X, Y, Z, Pitch, Yaw (--overview=x,y,z,pitch,yaw로 바꿈)


def Main():
	View = None
	for Arg in sys.argv:
		if Arg == "--overview":
			View = OVERVIEW_VIEW
		elif Arg.startswith("--overview="):
			View = tuple(float(V) for V in Arg.split("=", 1)[1].split(","))
	WriteBaseMaterials()
	WriteDecalTextures()
	WriteGlassFixedModels()
	WriteImportSettings()
	Scene, Mats = BuildScene()
	Scene.Save(os.path.join(CONTENT, "Scenes", "Demo", "Gallery.escene"))
	print(f"Gallery 생성: 엔티티 {len(Scene.Entities)}개, 타일 머티리얼 {len(Mats.Written)}개")
	if View:
		Overview, _ = BuildScene(Overview=View)
		Overview.Save(os.path.join(CONTENT, "Scenes", "Demo", "_GalleryOverview.escene"))
		print("확인용 변형: Scenes/Demo/_GalleryOverview.escene (커밋하지 않음)")


if __name__ == "__main__":
	Main()
