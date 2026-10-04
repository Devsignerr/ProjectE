# 데모 서브맵 "Workshop"(항구 창고 작업장 — 물리 쇼케이스) 생성: 머티리얼(.emat) + 데칼 텍스처 + 마당 건너편 공장 파사드 조립 glTF
#   + 임포트 설정 + 씬(Scenes/Demo/Workshop.escene)
#   실행: python Tools/DemoMap/BuildWorkshop.py [--camera X,Y,Z,Pitch,Yaw[,Fov]]  (먼저 Scripts/FetchDemoAssets.ps1로 Poly Haven 에셋을 받는다)
#   --camera: 고정 확인 카메라를 더한 변형(Scenes/Demo/_WorkshopCamera.escene)도 쓴다 — 커밋하지 않는다
#   보여 주는 물리 (Scripts/Demo/Workshop/*.lua):
#     밀기     — 시작 위치 앞 나무 상자 피라미드(동적, 캐릭터 PushForce로 밀림)
#     경사로   — 드럼통 3개가 판자 경사로를 굴러 내려간다 (캡슐 콜라이더, RollingResistance)
#     트리거   — 드럼통(또는 플레이어)이 크레인 위험 구역 트리거에 들어가면(OnTriggerEnter) 매달린 짐을 당겨
#                구 관절이 BreakForce를 넘어 끊어지고(OnJointBreak) 짐이 상자 더미로 떨어진다
#     도미노   — 지붕 트러스에 거리 관절(밧줄)로 매단 트랙터 타이어가 흔들려 선반 9개를 도미노로 넘어뜨린다
#     래그돌   — 마지막 선반이 전화하던 마네킹을 넘어뜨린다 (RagdollComponent, entity:EnableRagdoll)
#     관절     — 흔들리는 펜던트 등(구 관절) + 두 줄에 매단 형광등(경첩 관절, 한 평면으로만 흔들림), 등 아래 빛도 함께 움직인다
#     사격     — E / 마우스 잠금 중 왼쪽 클릭: 카메라 레이캐스트 + AddImpulse (WorkshopManager.lua)
#   좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 창고 안쪽 X [-1600, 1600](길이) × Y [-650, 650](폭) × Z [0, 900], 큰 문은 남쪽(-Y) 벽 가운데,
#   서쪽 벽 = Hub 포털, 마당 건너 Y = -2400에 공장 파사드. 배치를 바꿀 때는 씬 파일이 아니라 이 스크립트를 고치고 다시 실행한다 (결정적)
#   동적 물체는 콜라이더 바닥이 받침 면에 정확히 닿게(겹침·틈 없이) 놓아 시작 때 튀지 않게 한다
import json
import math
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from BuildAlley import FAC, FFacade, BuildFactory  # noqa: E402 (공장 파사드 키트 조립 규칙 재사용)
from GltfKit import FGltfKitComposer, EngineToGltf  # noqa: E402
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTENT = os.path.join(ROOT, "Projects", "Sample", "Content")
PH      = "Asset/PolyHaven"
MAT     = "Materials/Demo/Workshop"
KIT_OUT = "Asset/DemoKits/Workshop"
SCRIPTS = "Scripts/Demo/Workshop"

HALF_X, HALF_Y, HEIGHT = 1600.0, 650.0, 900.0
WALL      = 40.0
BRICK_Z   = 150.0             # 아래 벽돌 띠 높이, 그 위는 골함석
DOOR_HALF = 350.0             # 남쪽 큰 문 반폭
DOOR_H    = 560.0
TRUSS_X   = [-1400.0 + 400.0 * I for I in range(8)]
SKYLIGHT_Y = [(-330.0, -170.0), (170.0, 330.0)]  # 지붕 채광 띠 (X [-1450, 1450])
SKYLIGHT_X = 1450.0

CRANE_X, CRANE_Z = 500.0, 735.0   # 천장 크레인 모델 원점 (레일 높이 ≈ 690~730)
HOOK = (CRANE_X, -48.0, CRANE_Z - 391.0)  # 모델 고리 끝 (glTF 실측: 원점 기준 Y -48, Z -391)

# 임포트 설정: 고밀도 에셋은 쿠킹 때 LOD0 삼각형 상한으로 줄인다 (2026-10-04 glTF 실측 삼각형 수)
IMPORT_SETTINGS = {
	"overhead_crane":          {"MaxTriangles": 30000},  # 89964
	"tool_cart":               {"MaxTriangles": 10000},  # 29394
	"ladder_sectioned_01":     {"MaxTriangles": 8000},   # 29140
	"portable_generator":      {"MaxTriangles": 10000},  # 26419
	"caged_hanging_light":     {"MaxTriangles": 6000},   # 22893 (10개)
	"old_drill_press":         {"MaxTriangles": 8000},   # 21842
	"power_box_01":            {"MaxTriangles": 6000},   # 21272
	"industrial_storage_cart": {"MaxTriangles": 8000},   # 18902
	"rusted_wheel_rim_01":     {"MaxTriangles": 5000},   # 16440
	# glTF가 BLEND인데 알파가 없는 불투명 물체(알파 없는 JPG, 색 알파 1) — 반투명으로 그려지면 사전 패스·움직임 벡터·그림자가 빠지고
	# TAA 반응형이라 지터로 떨린다(2026-10-05 사용자 보고). 마스크는 알파 1이라 잘리는 곳 없이 불투명 경로로 그려진다
	"plastic_crate_02":        {"BlendAsMasked": True},
}


def Model(Id):
	return f"{PH}/{Id}/{Id}.gltf"


def WriteJson(Path, Doc):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")


def FaceYaw(DX, DY):
	# Poly Haven 모델 정면 = 엔진 -X → 정면이 (DX, DY) 방향을 보게 하는 Yaw
	return math.degrees(math.atan2(-DY, -DX))


def QMul(A, B):
	# 해밀턴 곱 [X, Y, Z, W] (A * B = B 먼저)
	AX, AY, AZ, AW = A
	BX, BY, BZ, BW = B
	return [AW * BX + AX * BW + AY * BZ - AZ * BY, AW * BY - AX * BZ + AY * BW + AZ * BX,
			AW * BZ + AX * BY - AY * BX + AZ * BW, AW * BW - AX * BX - AY * BY - AZ * BZ]


def QRotate(Q, V):
	R = QMul(QMul(Q, [V[0], V[1], V[2], 0.0]), [-Q[0], -Q[1], -Q[2], Q[3]])
	return (R[0], R[1], R[2])


# ---- 머티리얼 -------------------------------------------------------------------------------------------------------
# 표면: (Poly Haven 텍스처 Id, 타일 한 장 크기 cm, 거칠기 배율, 색 배율)
SURFACES = {
	"Concrete":   ("hangar_concrete_floor", 360.0, 1.0, [3.6, 3.6, 3.6]),  # 원본 알베도 ≈ 0.03 (너무 검다)
	"Brick":      ("factory_brick", 220.0, 1.0, [0.9, 0.9, 0.9]),
	"Corrugated": ("corrugated_iron_02", 240.0, 1.0, [1.9, 1.95, 2.0]),
	"Roof":       ("corrugated_iron_02", 300.0, 1.0, [1.3, 1.3, 1.3]),
	"Steel":      ("rusty_metal_02", 140.0, 0.9, [0.55, 0.55, 0.6]),
	"Planks":     ("weathered_planks", 200.0, 1.0, [1.8, 1.8, 1.8]),
	"Asphalt":    ("asphalt_02", 450.0, 1.0, [0.9, 0.9, 0.9]),
}


def WriteBaseMaterials():
	Folder = os.path.join(CONTENT, MAT)
	if os.path.isdir(Folder):  # 생성물 전용 폴더 — 크기가 바뀐 이전 타일 인스턴스를 지운다
		for Name in os.listdir(Folder):
			if Name.endswith(".emat"):
				os.remove(os.path.join(Folder, Name))
	# 박스 매핑 PBR 그래프: 월드 위치를 법선의 우세 축 면에 투영한 UV / TileSize (cm) — 상자 면마다 크기가 달라도(벽 윗면·보 밑면·기둥)
	#   텍스처 밀도가 같고 이웃 상자끼리 이어진다. 예전 "내장 큐브 UV(면마다 0~1) × Tiling"은 Tiling 하나를 가장 큰 면 기준으로 정해
	#   좁은 면이 한 방향으로 수십 배 늘어났다. 면별 UV 방향 = 내장 큐브 면의 탄젠트 방향(PrimitiveShapes AddQuadFace: U = Right, V = -Up)이라
	#   축 정렬 상자는 노멀맵이 정확하다: ±X면 U = -sign(Nx)·Y, ±Y면 U = sign(Ny)·X, V = -Z / ±Z면 U = sign(Nz)·Y, V = -X
	Graph = {
		"Name": "BoxPBR",
		"BlendMode": "Opaque",
		"Parameters": [
			{"Name": "BaseTexture", "Type": "Texture", "Value": "", "Usage": "Color"},
			{"Name": "ArmTexture", "Type": "Texture", "Value": "", "Usage": "Linear"},
			{"Name": "NormalTexture", "Type": "Texture", "Value": "", "Usage": "Normal"},
			{"Name": "TileSize", "Type": "Scalar", "Value": 200.0},
			{"Name": "Tint", "Type": "Vector", "Value": [1.0, 1.0, 1.0, 1.0]},
			{"Name": "RoughnessScale", "Type": "Scalar", "Value": 1.0},
		],
		"Graph": {
			"Nodes": [
				{"Id": "pos", "Type": "WorldPosition"},
				{"Id": "tile", "Type": "ScalarParameter", "Parameter": "TileSize"},
				{"Id": "p", "Type": "Divide", "Inputs": {"A": "pos", "B": "tile"}},
				{"Id": "ps", "Type": "Split", "Inputs": {"A": "p"}},
				{"Id": "negX", "Type": "Multiply", "Inputs": {"A": "ps:0", "B": -1.0}},
				{"Id": "negZ", "Type": "Multiply", "Inputs": {"A": "ps:2", "B": -1.0}},
				{"Id": "n", "Type": "WorldNormal"},
				{"Id": "ns", "Type": "Split", "Inputs": {"A": "n"}},
				{"Id": "ax", "Type": "Abs", "Inputs": {"A": "ns:0"}},
				{"Id": "ay", "Type": "Abs", "Inputs": {"A": "ns:1"}},
				{"Id": "az", "Type": "Abs", "Inputs": {"A": "ns:2"}},
				{"Id": "sx", "Type": "Compare", "Op": "Greater", "Inputs": {"A": "ns:0", "B": 0.0, "True": -1.0, "False": 1.0}},
				{"Id": "sy", "Type": "Compare", "Op": "Greater", "Inputs": {"A": "ns:1", "B": 0.0, "True": 1.0, "False": -1.0}},
				{"Id": "sz", "Type": "Compare", "Op": "Greater", "Inputs": {"A": "ns:2", "B": 0.0, "True": 1.0, "False": -1.0}},
				{"Id": "ux", "Type": "Multiply", "Inputs": {"A": "ps:1", "B": "sx"}},
				{"Id": "uy", "Type": "Multiply", "Inputs": {"A": "ps:0", "B": "sy"}},
				{"Id": "uz", "Type": "Multiply", "Inputs": {"A": "ps:1", "B": "sz"}},
				{"Id": "uvX", "Type": "Append", "Inputs": {"A": "ux", "B": "negZ"}},
				{"Id": "uvY", "Type": "Append", "Inputs": {"A": "uy", "B": "negZ"}},
				{"Id": "uvZ", "Type": "Append", "Inputs": {"A": "uz", "B": "negX"}},
				{"Id": "maxXY", "Type": "Max", "Inputs": {"A": "ax", "B": "ay"}},
				{"Id": "uvXY", "Type": "Compare", "Op": "GreaterEqual", "Inputs": {"A": "ax", "B": "ay", "True": "uvX", "False": "uvY"}},
				{"Id": "uv", "Type": "Compare", "Op": "GreaterEqual", "Inputs": {"A": "az", "B": "maxXY", "True": "uvZ", "False": "uvXY"}},
				{"Id": "base", "Type": "TextureSample", "Texture": "BaseTexture", "Inputs": {"UV": "uv"}},
				{"Id": "arm", "Type": "TextureSample", "Texture": "ArmTexture", "Inputs": {"UV": "uv"}},
				{"Id": "nrm", "Type": "TextureSample", "Texture": "NormalTexture", "Inputs": {"UV": "uv"}},
				{"Id": "tint", "Type": "VectorParameter", "Parameter": "Tint"},
				{"Id": "color", "Type": "Multiply", "Inputs": {"A": "base:1", "B": "tint:1"}},
				{"Id": "roughScale", "Type": "ScalarParameter", "Parameter": "RoughnessScale"},
				{"Id": "rough", "Type": "Multiply", "Inputs": {"A": "arm:3", "B": "roughScale"}},
			],
			"Output": {"BaseColor": "color", "Roughness": "rough", "Metallic": "arm:4", "AmbientOcclusion": "arm:2", "Normal": "nrm:1"},
		},
	}
	WriteJson(os.path.join(Folder, "BoxPBR.emat"), Graph)

	def Plain(Name, Base, Alpha=1.0, Emissive=(0.0, 0.0, 0.0), Rough=0.9, Blend=None, Texture=""):
		Mat = {"Name": Name}
		if Blend:
			Mat["BlendMode"] = Blend
		Mat.update({
			"BaseColorFactor": list(Base) + [Alpha], "EmissiveFactor": list(Emissive), "Metallic": 0.0, "Roughness": Rough,
			"NormalScale": 1.0, "OcclusionStrength": 1.0, "BaseColorTexture": Texture, "MetallicRoughnessTexture": "",
			"NormalTexture": "", "OcclusionTexture": "", "EmissiveTexture": "",
		})
		WriteJson(os.path.join(Folder, f"{Name}.emat"), Mat)

	Plain("SkylightGlass", (0.8, 0.86, 0.9), Alpha=0.1, Rough=0.05, Blend="Translucent")
	Plain("Rope", (0.16, 0.12, 0.08), Rough=0.95)
	Plain("Interior", (0.03, 0.028, 0.026), Rough=0.9)
	Plain("DecalHazard", (1.0, 1.0, 1.0), Alpha=0.9, Rough=0.6, Texture="Hazard.png")


class FMaterials:
	# 표면 → 박스 매핑 인스턴스(.emat) 경로 (면 크기와 무관 — 텍스처 밀도는 TileSize)
	def __init__(self):
		self.Written = {}

	def Get(self, Surface):
		Id, Tile, RoughScale, Tint = SURFACES[Surface]
		Name = Surface
		if Name not in self.Written:
			Rel = f"../../../{PH}/{Id}/{Id}"
			WriteJson(os.path.join(CONTENT, MAT, f"{Name}.emat"), {
				"Name": Name, "Parent": "BoxPBR.emat",
				"Parameters": [
					{"Name": "BaseTexture", "Type": "Texture", "Value": f"{Rel}_diff_2k.jpg"},
					{"Name": "ArmTexture", "Type": "Texture", "Value": f"{Rel}_arm_2k.jpg"},
					{"Name": "NormalTexture", "Type": "Texture", "Value": f"{Rel}_nor_gl_2k.jpg"},
					{"Name": "TileSize", "Type": "Scalar", "Value": Tile},
					{"Name": "Tint", "Type": "Vector", "Value": Tint + [1.0]},
					{"Name": "RoughnessScale", "Type": "Scalar", "Value": RoughScale},
				],
			})
			self.Written[Name] = True
		return f"{MAT}/{Name}.emat"


def WriteHazardTexture():
	# 위험 구역 바닥 표시: 노랑/검정 사선 줄무늬 테두리 (안쪽은 투명). 결정적
	Size, Border = 512, 0.16
	Y, X = (np.mgrid[0:Size, 0:Size] + 0.5) / Size
	Edge = np.minimum(np.minimum(X, 1.0 - X), np.minimum(Y, 1.0 - Y))
	Stripe = (np.floor((X + Y) * 10.0) % 2.0) < 1.0
	Rgb = np.where(Stripe[..., None], np.array([0.95, 0.72, 0.05]), np.array([0.05, 0.05, 0.05]))
	Alpha = np.clip((Border - Edge) / 0.01, 0.0, 1.0) * np.clip(Edge / 0.004, 0.0, 1.0)
	Rgba = np.dstack([np.clip(Rgb * 255.0 + 0.5, 0, 255), np.clip(Alpha * 255.0 + 0.5, 0, 255)]).astype(np.uint8)
	Image.fromarray(Rgba, "RGBA").save(os.path.join(CONTENT, MAT, "Hazard.png"), optimize=False)


def WriteImportSettings():
	for Id, Settings in IMPORT_SETTINGS.items():
		WriteJson(os.path.join(CONTENT, "Asset", "PolyHaven", Id, f"{Id}.gltf.eimport"), Settings)


# ---- 마당 건너편 공장 파사드 (BuildAlley.py와 같은 공장 키트 규칙) ------------------------------------------------------
YARD_FACADE_Y = -2400.0
YARD_FACTORY = dict(StartCol=0, Floors=3,
					Ground=["std", "door_c_l", "std", "garage", "std", "door_rec_l", "std", "std", "door_c_s"],
					Upper=["centered_medium", "centered_large", "centered_double", "centered_medium", "centered_medium", "centered_large",
						   "centered_small", "centered_double", "centered_large", "centered_medium"])


def BuildYardFacade():
	K = FGltfKitComposer(os.path.join(CONTENT, PH), os.path.join(CONTENT, KIT_OUT, "WorkshopYard.gltf"))
	# 띠는 엔진 (-1500, Y)에서 +X로, 정면(로컬 +z) = 엔진 +Y (창고 쪽)
	Facade = FFacade(FAC, EngineToGltf(-1500.0, YARD_FACADE_Y, 0.0, -90.0))
	BuildFactory(K, Facade, **YARD_FACTORY)
	for X, Type in Facade.Doors:
		if Type == "door_c_s":
			K.Add("rollershutter_door", "rollershutter_door", Facade.At(X, 0.0, -0.02) @ np.diag([1.16, 0.92, 1.0, 1.0]))
	return K.Save()


# ---- 씬 -------------------------------------------------------------------------------------------------------------
def BuildScene(Camera=None):
	S = FScene()
	Mats = FMaterials()

	def Box(Name, Surface, Center, Size, Collide=True, Rotation=None, Parent=-1):
		# 내장 큐브(100cm) × Size/100. 머티리얼은 박스 매핑(월드 좌표)이라 면 크기와 무관
		SX, SY, SZ = Size
		Comps = {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": Mats.Get(Surface)}}
		if Collide:
			Comps["BoxColliderComponent"] = {}
		return S.Add(Name, Comps, Center, Rotation, (SX / 100.0, SY / 100.0, SZ / 100.0), Parent)

	def Wall(Name, X0, X1, Y0, Y1, Z0=0.0, Z1=HEIGHT):
		# 벽 조각: 아래 벽돌 띠 + 위 골함석
		for Part, Surface, A, B in (("Low", "Brick", Z0, min(Z1, BRICK_Z)), ("High", "Corrugated", max(Z0, BRICK_Z), Z1)):
			if B > A:
				Box(f"{Name}_{Part}", Surface, ((X0 + X1) * 0.5, (Y0 + Y1) * 0.5, (A + B) * 0.5), (X1 - X0, Y1 - Y0, B - A))

	def Collider(Kind, *Args):
		if Kind == "box":
			Half, Offset = Args
			return {"BoxColliderComponent": {"HalfExtents": list(Half), "Offset": list(Offset)}}
		if Kind == "sphere":
			Radius, Offset = Args
			return {"SphereColliderComponent": {"Radius": Radius, "Offset": list(Offset)}}
		Radius, HalfHeight, Offset = Args
		return {"CapsuleColliderComponent": {"Radius": Radius, "HalfHeight": HalfHeight, "Offset": list(Offset)}}

	def Body(Mass, Friction=0.6, Restitution=0.05, Rolling=0.0, LinearDamping=0.05, AngularDamping=0.1, Report=False):
		return {"RigidBodyComponent": {"MotionType": 2, "Mass": Mass, "Friction": Friction, "Restitution": Restitution,
									   "LinearDamping": LinearDamping, "AngularDamping": AngularDamping, "RollingResistance": Rolling,
									   "UseGravity": True, "LockRotation": False, "ReportContacts": Report}}

	def Prop(Name, Id, Pos, Yaw=0.0, Scale=1.0, Col=None, Dyn=None, Rotation=None, Extra=None, Parent=-1):
		# Col = Collider(...) 결과 (모델 로컬 cm — 스케일이 곱해진다), Dyn = Body(...) (없으면 정적)
		Comps = {"ModelComponent": {"AssetPath": Model(Id)}}
		for Part in (Col, Dyn, Extra):
			if Part:
				Comps.update(Part)
		Sc = Scale if isinstance(Scale, (list, tuple)) else (Scale, Scale, Scale)
		return S.Add(Name, Comps, Pos, Rotation if Rotation is not None else QuatFromEuler(Yaw=Yaw), Sc, Parent)

	def Script(Name, Overrides=None, Location=0):
		return {"ScriptComponent": {"ScriptAsset": f"{SCRIPTS}/{Name}.lua", "ExecutionLocation": Location,
									"PropertyOverrides": json.dumps(Overrides or {}, ensure_ascii=False)}}

	def Rope(Name, Anchor, Target, AttachHeight):
		# 밧줄 표시 (WorkshopRope.lua가 매 프레임 늘인다) — 처음 위치 = 매단 점
		Comps = {"StaticMeshComponent": {"MeshAsset": "primitive:capsule", "MaterialAsset": f"{MAT}/Rope.emat"}}
		Comps.update(Script("WorkshopRope", {"Target": Target, "AttachHeight": AttachHeight}, Location=2))
		return S.Add(Name, Comps, Anchor, None, (0.024, 0.024, 1.0))

	def Decal(Name, Pos, Size, Yaw=0.0):
		S.Add(Name, {"DecalComponent": {
			"MaterialAsset": f"{MAT}/DecalHazard.emat", "Size": [Size[0], Size[1], 40.0], "Opacity": 1.0, "SortOrder": 0,
			"AffectBaseColor": True, "AffectNormal": False, "AffectRoughness": True, "FadeStartDistance": 0.0, "FadeEndDistance": 0.0}},
			Pos, QuatFromEuler(Yaw=Yaw))

	# ---- 환경: 맑은 오후 해(남서쪽) + 대기 하늘 + 창고 안 먼지(볼류메트릭 — 채광창·큰 문 빛줄기)
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.92, 0.8], "Intensity": 6.0}}, (0, 0, 3000), QuatFromEuler(Pitch=-42, Yaw=68))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {},
		"SkyLightComponent": {"Intensity": 1.0},
		"HeightFogComponent": {
			"Color": [0.55, 0.55, 0.55], "Density": 0.0005, "HeightFalloff": 0.0, "StartDistance": 0.0, "MaxOpacity": 0.2,
			"DirectionalInscatteringColor": [0.0, 0.0, 0.0],
			"Volumetric": True, "VolumetricDistance": 4000.0, "VolumetricAlbedo": [0.9, 0.87, 0.82], "VolumetricExtinctionScale": 1.0,
			"VolumetricAnisotropy": 0.6, "VolumetricDirectionalScale": 5.0, "VolumetricLocalLightScale": 0.4},
	})
	# DDGI: 창고보다 조금 큰 상자 (바깥에도 프로브 한 층)
	S.Add("HallProbes", {"IrradianceVolumeComponent": {
		"HalfExtents": [HALF_X + 100.0, HALF_Y + 100.0, HEIGHT * 0.5 + 60.0], "Spacing": 160.0, "Intensity": 1.0, "FadeDistance": 40.0,
		"NormalBias": 10.0, "ViewBias": 20.0, "Hysteresis": 0.98, "RaysPerProbe": 256, "ProbeUpdateBudget": 0, "MaxRayDistance": 10000.0,
		"Relocation": True, "Classification": True, "Priority": 0, "DebugProbes": 0, "DebugProbeRadius": 8.0}},
		(0, 0, HEIGHT * 0.5))

	# ---- 땅/건물: 마당 아스팔트, 창고 콘크리트 바닥, 벽(남쪽 큰 문), 지붕(채광 띠 두 줄 + 유리), 기둥·트러스·크레인 런웨이
	Box("Yard", "Asphalt", (0, -800.0, -11.0), (8000.0, 6400.0, 20.0))
	Box("Floor", "Concrete", (0, 0, -10.0), (2 * HALF_X, 2 * HALF_Y, 20.0))
	OX, OY = HALF_X + WALL, HALF_Y + WALL
	Wall("Wall_North", -OX, OX, HALF_Y, OY)
	Wall("Wall_West", -OX, -HALF_X, -HALF_Y, HALF_Y)
	Wall("Wall_East", HALF_X, OX, -HALF_Y, HALF_Y)
	Wall("Wall_South_W", -OX, -DOOR_HALF, -OY, -HALF_Y)
	Wall("Wall_South_E", DOOR_HALF, OX, -OY, -HALF_Y)
	Box("Wall_South_Lintel", "Corrugated", (0, -HALF_Y - WALL * 0.5, (DOOR_H + HEIGHT) * 0.5), (2 * DOOR_HALF, WALL, HEIGHT - DOOR_H))
	Box("Door_Header", "Steel", (0, -HALF_Y + 12.0, DOOR_H + 15.0), (2 * DOOR_HALF + 60.0, 24.0, 30.0), Collide=False)
	for Side in (-1, 1):
		Box("Door_Jamb", "Steel", (Side * (DOOR_HALF + 15.0), -HALF_Y + 12.0, DOOR_H * 0.5), (30.0, 24.0, DOOR_H))
	RoofZ, RoofT = HEIGHT + 15.0, 30.0
	Bands = [(-OY, SKYLIGHT_Y[0][0]), (SKYLIGHT_Y[0][1], SKYLIGHT_Y[1][0]), (SKYLIGHT_Y[1][1], OY)]
	for Index, (Y0, Y1) in enumerate(Bands):
		Box(f"Roof_{Index}", "Roof", (0, (Y0 + Y1) * 0.5, RoofZ), (2 * OX, Y1 - Y0, RoofT))
	for Index, (Y0, Y1) in enumerate(SKYLIGHT_Y):
		for Side in (-1, 1):
			Box(f"Roof_End_{Index}", "Roof", (Side * (SKYLIGHT_X + OX) * 0.5, (Y0 + Y1) * 0.5, RoofZ), (OX - SKYLIGHT_X, Y1 - Y0, RoofT))
		S.Add(f"Skylight_{Index}", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MAT}/SkylightGlass.emat"}},
			  (0, (Y0 + Y1) * 0.5, RoofZ - 4.0), None, (2 * SKYLIGHT_X / 100.0, (Y1 - Y0) / 100.0, 0.02))
	for Index, TX in enumerate(TRUSS_X):
		Box(f"Truss_{Index}", "Steel", (TX, 0, HEIGHT - 22.0), (24.0, 2 * HALF_Y, 44.0), Collide=False)
		Box(f"Truss_{Index}_Tie", "Steel", (TX, 0, HEIGHT - 150.0), (10.0, 2 * HALF_Y, 12.0), Collide=False)
		for Side in (-1, 1):
			Box(f"Column_{Index}", "Steel", (TX, Side * (HALF_Y - 16.0), HEIGHT * 0.5), (32.0, 32.0, HEIGHT))
			Box(f"Truss_{Index}_Strut", "Steel", (TX, Side * 300.0, HEIGHT - 86.0), (8.0, 8.0, 128.0), Collide=False)
	for Side in (-1, 1):
		Box("Crane_Runway", "Steel", (0, Side * 625.0, 705.0), (2 * HALF_X, 34.0, 40.0), Collide=False)
		Box("Purlin", "Steel", (0, Side * 480.0, HEIGHT - 8.0), (2 * HALF_X, 10.0, 16.0), Collide=False)
	S.Model("Crane", Model("overhead_crane"), (CRANE_X, 0.0, CRANE_Z), 0.0, 1.0)

	# ---- 서쪽 벽: Hub 포털 (DemoPortal.lua — Hub로, 돌아가기 자리 기억 안 함)
	Portal = S.Add("Portal_Hub", Script_Portal(), (-HALF_X + 12.0, 0.0, 0.0), QuatFromEuler(Yaw=0.0))
	S.Model("Portal_Hub_Door", Model("large_castle_door"), (0, 0, -3), 0.0, 1.0, Parent=Portal)
	S.Add("Portal_Hub_Glow", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Demo/PortalGlow.emat"}},
		  (18, 0, 148), None, (0.02, 1.85, 2.85), Parent=Portal)
	S.Add("Portal_Hub_Light", {"PointLightComponent": {"Color": [0.5, 0.75, 1.0], "Intensity": 1.5, "Radius": 350.0, "CastShadows": False}},
		  (80, 0, 220), Parent=Portal)
	for Side in (-1, 1):
		Prop(f"Portal_Barrel_{Side + 1}", "barrel_03", (-HALF_X + 50.0, Side * 165.0, 0.0), 25.0 * Side, 1.0,
			 Collider("box", (31, 31, 46.5), (0, 0, 46.5)))

	# ---- 1. 밀기: 시작 위치 앞 나무 상자 피라미드 (2줄 × 3-2-1, 동적 18kg) + 꼭대기 종이 상자
	StackX, StackY = -860.0, 380.0
	CrateCol = Collider("box", (20.4, 41.3, 17.5), (-0.85, 0.0, 16.65))  # wooden_crate_01: X [-21.3, 19.6], Z [-0.8, 34.2]
	Layers = [[-84.0, 0.0, 84.0], [-42.0, 42.0], [0.0]]
	for Row, RX in enumerate((StackX - 21.0, StackX + 21.0)):
		for Layer, Ys in enumerate(Layers):
			for Index, DY in enumerate(Ys):
				Prop(f"Push_Crate_{Row}_{Layer}_{Index}", "wooden_crate_01", (RX, StackY + DY, 0.8 + 35.0 * Layer), 0.0, 1.0, CrateCol, Body(18.0))
	CardboardCol = Collider("box", (25.0, 19.4, 16.9), (-3.3, 0.0, 16.4))  # cardboard_box_01: X [-29.1, 22.5], Z [-0.7, 33.5]
	Prop("Push_Cardboard_Top", "cardboard_box_01", (StackX + 3.3, StackY, 0.5 + 105.0), 90.0, 1.0, CardboardCol, Body(6.0))

	# ---- 2. 경사로 + 드럼통: 받침대(X -1150 ~ -1000, 높이 170)에서 X -300까지 내려가는 판자 경사로, 가운데 Y = -48 (크레인 고리 줄)
	RampY, RampW = HOOK[1], 170.0
	TopX, BotX, TopZ = -1000.0, -300.0, 220.0
	Theta = math.atan2(TopZ, BotX - TopX)
	Length = math.hypot(BotX - TopX, TopZ)
	Normal = (math.sin(Theta), 0.0, math.cos(Theta))
	RampRot = QuatFromEuler(Pitch=-math.degrees(Theta))
	Mid = ((TopX + BotX) * 0.5, RampY, TopZ * 0.5)
	Box("Ramp_Deck", "Planks", (Mid[0] - Normal[0] * 4.0, RampY, Mid[2] - Normal[2] * 4.0), (Length + 6.0, RampW, 8.0), Rotation=RampRot)
	Box("Ramp_Landing", "Planks", (TopX - 75.0, RampY, TopZ - 4.0), (150.0, RampW, 8.0))
	for Side in (-1, 1):
		RailY = RampY + Side * (RampW * 0.5 + 5.0)
		Box("Ramp_Rail", "Steel", (Mid[0] + Normal[0] * 8.0, RailY, Mid[2] + Normal[2] * 8.0), (Length, 10.0, 24.0), Rotation=RampRot)
		Box("Ramp_Landing_Rail", "Steel", (TopX - 75.0, RailY, TopZ + 8.0), (150.0, 10.0, 24.0))
		for LX in (TopX - 145.0, TopX - 5.0, -800.0, -600.0, -420.0):
			SurfaceZ = TopZ - max(0.0, LX - TopX) * math.tan(Theta) - 10.0
			Box("Ramp_Leg", "Steel", (LX, RailY, SurfaceZ * 0.5), (14.0, 14.0, SurfaceZ))
			if Side > 0:  # 다리 쌍 사이 가로대 (판자 아래)
				Box("Ramp_Brace", "Steel", (LX, RampY, SurfaceZ - 8.0), (10.0, RampW + 10.0, 10.0), Collide=False)
	Box("Ramp_Backstop", "Steel", (TopX - 155.0, RampY, TopZ + 20.0), (10.0, RampW, 40.0))
	# 드럼통 (Barrel_01 반지름 28, 높이 88): 옆으로 눕혀(Roll 90 → 축 = Y) 경사면 위에 놓는다 — 시작하자마자 굴러 내려간다
	BarrelRot = QuatFromEuler(Roll=90.0)
	Axis = QRotate(BarrelRot, (0.0, 0.0, 44.0))
	for Index, Along in enumerate((60.0, 200.0, 340.0)):
		Surface = (TopX + Along * math.cos(Theta), RampY, TopZ - Along * math.sin(Theta))
		Center = tuple(Surface[A] + Normal[A] * 28.2 for A in range(3))
		Prop(f"Ramp_Barrel_{Index}", "Barrel_01", tuple(Center[A] - Axis[A] for A in range(3)), Rotation=BarrelRot,
			 Col=Collider("capsule", 28.0, 16.0, (0.0, 0.0, 44.0)), Dyn=Body(30.0, Friction=0.8, Rolling=0.02, LinearDamping=0.0, AngularDamping=0.0))

	# ---- 3. 크레인 위험 구역: 경사로 끝 트리거 → 고리에 매달린 짐(wooden_crate_02, 구 관절 BreakForce)이 끊어져 상자 더미로 떨어진다
	LoadTop = 232.0
	Load = Prop("Crane_Load", "wooden_crate_02", (HOOK[0], HOOK[1], LoadTop - 45.4), 0.0, 1.0,
				Collider("box", (58.0, 26.5, 23.2), (0.0, 0.0, 22.2)), Body(70.0, AngularDamping=0.3),
				Extra={"BallJointComponent": {"Anchor": [0.0, 0.0, 45.4], "Axis": [0.0, 0.0, -1.0], "ConeAngle": 180.0,
											  "BreakForce": 5000.0, "CollideConnected": False},
					   **Script("WorkshopCraneLoad", {"Rope": "Crane_Rope"})})
	Rope("Crane_Rope", HOOK, "Crane_Load", 45.4)
	S.Add("Crane_Trigger", {"BoxColliderComponent": {"HalfExtents": [90.0, 110.0, 90.0], "Offset": [0.0, 0.0, 0.0], "IsTrigger": True},
							**Script("WorkshopCraneTrigger", {"Load": "Crane_Load"})}, (180.0, RampY, 90.0))
	Decal("Crane_Hazard", (HOOK[0], HOOK[1], 0.0), (340.0, 300.0))
	Decal("Trigger_Hazard", (180.0, RampY, 0.0), (190.0, 230.0))
	Prop("Trigger_Sign", "WetFloorSign_01", (180.0, RampY - 160.0, 0.0), FaceYaw(-1, -0.4), 1.0)
	# 짐 아래 상자 더미: 종이 상자 2층 + 플라스틱 상자 (동적)
	PX, PY = HOOK[0], HOOK[1]
	for Index, (DX, DY) in enumerate(((-27.0, -21.0), (27.0, -21.0), (-27.0, 21.0), (27.0, 21.0))):
		Prop(f"Pile_Box_{Index}", "cardboard_box_01", (PX + DX + 3.3 * 0, PY + DY, 0.5), 0.0, 1.0, CardboardCol, Body(6.0))
	for Index, DY in enumerate((-21.0, 21.0)):
		Prop(f"Pile_BoxTop_{Index}", "cardboard_box_01", (PX, PY + DY, 0.5 + 33.8), 0.0, 1.0, CardboardCol, Body(6.0))
	PlasticCol = Collider("box", (20.3, 25.3, 12.7), (0.0, 0.0, 12.7))
	for Index, (DX, DY, Z) in enumerate(((90.0, -10.0, 0.0), (90.0, -10.0, 25.4), (-85.0, 30.0, 0.0), (10.0, 85.0, 0.0))):
		Prop(f"Pile_Plastic_{Index}", "plastic_crate_02", (PX + DX, PY + DY, Z), 0.0, 1.0, PlasticCol, Body(3.0))

	# ---- 4. 도미노: 지붕 트러스(X -600)에 거리 관절로 매단 트랙터 타이어 + 선반(Shelf_01, 25.7 × 100 × 208) 9개 → 마네킹
	DominoY = -430.0
	Pivot = (-600.0, DominoY, HEIGHT - 44.0)
	TyreScale, TyreR = 1.4, 42.0
	HangZ = 222.0                                   # 타이어 가운데가 가장 낮을 때 높이 — 선반 꼭대기 20cm만 친다 (낮게 치면 밑동이 밀려 뒤로 넘어간다)
	RopeLen = Pivot[2] - HangZ - TyreR
	Alpha = math.radians(40.0)                      # 시작 각도 (-X 쪽으로 들어 올림)
	Dir = (math.sin(Alpha), 0.0, math.cos(Alpha))   # 타이어 → 매단 점
	Center = tuple(Pivot[A] - Dir[A] * (RopeLen + TyreR) for A in range(3))
	TyreRot = QMul(QuatFromEuler(Pitch=-math.degrees(Alpha)), QuatFromEuler(Yaw=90.0))
	Prop("Tyre_Swing", "old_tyre", Center, Rotation=TyreRot, Scale=TyreScale,
		 Col=Collider("sphere", 28.5, (0.0, 0.0, 0.0)), Dyn=Body(45.0, Friction=0.9, LinearDamping=0.02, AngularDamping=0.05),
		 Extra={"DistanceJointComponent": {"Anchor": [0.0, 0.0, 30.0], "TargetAnchor": list(Pivot), "MinDistance": 0.0, "MaxDistance": -1.0,
											"SpringFrequency": 0.0, "SpringDamping": 0.5, "BreakForce": 0.0, "CollideConnected": True}})
	Rope("Tyre_Rope", Pivot, "Tyre_Swing", TyreR)
	ShelfCol = Collider("box", (12.85, 50.0, 104.0), (-12.85, 0.0, 104.0))
	FirstShelf = Pivot[0] + TyreR + 25.7 + 3.0
	DominoGap = 90.0  # 선반 높이 208의 0.43배 (선반은 RollingResistance 0 — 닿아 있는 동안 회전을 줄여 사슬이 끊긴다)
	for Index in range(9):
		Prop(f"Domino_Shelf_{Index}", "Shelf_01", (FirstShelf + DominoGap * Index, DominoY, 0.0), 0.0, 1.0, ShelfCol, Body(12.0, Friction=0.6))
	LastShelf = FirstShelf + DominoGap * 8

	# ---- 5. 래그돌 마네킹: 마지막 선반 앞에서 전화 중 (루트 = 키네마틱 캡슐, 자식 Mesh = UAL2 마네킹 + RagdollComponent)
	Mannequin = S.Add("Mannequin", {
		"RigidBodyComponent": {"MotionType": 1, "Mass": 0.0, "UseGravity": False},
		"CapsuleColliderComponent": {"Radius": 24.0, "HalfHeight": 62.0, "Offset": [0.0, 0.0, 88.0]},
		**Script("WorkshopMannequin")}, (LastShelf + 130.0, DominoY, 0.0), QuatFromEuler(Yaw=0.0))
	S.Add("Mesh", {
		"ModelComponent": {"AssetPath": "Asset/Quaternius/UAL2/UAL2_Standard.glb"},
		"AnimationComponent": {"BlendTime": 0.2, "Clip": "Idle_TalkingPhone_Loop", "Loop": True, "Playing": True, "Speed": 1.0},
		"RagdollComponent": {"EnableOnDeath": False, "Mass": 60.0, "RadiusScale": 0.25, "MinRadius": 3.0, "MinBoneLength": 4.0,
							 "SwingLimit": 45.0, "TwistLimit": 30.0, "Friction": 0.8, "ExcludeBones": ""}},
		(0, 0, 0), None, (1, 1, 1), Mannequin)

	# ---- 6. 매단 등: 펜던트 등 2개(구 관절, 처음에 기울여 흔들림 + 그림자 점광원) + 형광등 10개(경첩 관절 — 등 길이 축으로만 흔들림, 아래 면광원)
	for Index, (LX, LY, Tilt) in enumerate(((-250.0, 40.0, 16.0), (1050.0, -40.0, -14.0))):
		Rot = QuatFromEuler(Roll=Tilt)
		Lamp = Prop(f"Pendant_{Index}", "hanging_industrial_lamp", (LX, LY, HEIGHT), Rotation=Rot,
					Col=Collider("sphere", 26.0, (0.0, 0.0, -112.0)), Dyn=Body(6.0, LinearDamping=0.01, AngularDamping=0.01),
					Extra={"BallJointComponent": {"Anchor": [0.0, 0.0, 0.0], "Axis": [0.0, 0.0, -1.0], "ConeAngle": 60.0,
												  "BreakForce": 0.0, "CollideConnected": False}})
		S.Add(f"Pendant_{Index}_Light", {"PointLightComponent": {
			"Color": [1.0, 0.78, 0.5], "Intensity": 45.0, "Radius": 1300.0, "CastShadows": True}}, (0, 0, -126.0), Parent=Lamp)
	for Col_, FX in enumerate((-1200.0, -800.0, 0.0, 900.0, 1300.0)):
		for Row, FY in enumerate((-400.0, 400.0)):
			Tilt = (14.0 if (Col_ + Row) % 2 == 0 else -10.0) if Col_ in (1, 3) else 0.0
			Fixture = Prop(f"Fluorescent_{Col_}_{Row}", "caged_hanging_light", (FX, FY, HEIGHT), Rotation=QuatFromEuler(Pitch=Tilt),
						   Col=Collider("box", (15.0, 56.0, 7.0), (0.0, 0.0, -66.0)), Dyn=Body(10.0, LinearDamping=0.01, AngularDamping=0.03),
						   Extra={"HingeJointComponent": {"Anchor": [0.0, 0.0, 0.0], "Axis": [0.0, 1.0, 0.0], "Limit": True, "MinAngle": -60.0,
														  "MaxAngle": 60.0, "Motor": False, "MotorSpeed": 0.0, "MotorMaxTorque": 0.0, "Friction": 0.05,
														  "BreakForce": 0.0, "CollideConnected": False}})
			S.Add(f"Fluorescent_{Col_}_{Row}_Light", {"AreaLightComponent": {
				"Shape": 0, "Color": [0.92, 0.96, 1.0], "Intensity": 140.0, "Width": 104.0, "Height": 10.0, "Radius": 1600.0,
				"TwoSided": False, "BarnDoorAngle": 88.0, "BarnDoorLength": 6.0, "CastShadows": False,
				"IesProfile": "", "UseIesIntensity": False, "IesIntensityScale": 1.0, "CookieTexture": "", "CookieScale": [1, 1], "CookiePanSpeed": [0, 0]}},
				(0, 0, -74.0), QuatFromEuler(Pitch=-90.0), Parent=Fixture)

	# ---- 7. 벽가 작업장 꾸밈 (정적 큰 기계·선반 + 동적 잡동사니)
	# 북쪽 벽: 철제 선반 4개 (스케일 0.1 — 원본이 10배) + 선반 위 상자/통
	ShelfYaw = FaceYaw(0, -1)
	SteelShelf = Collider("box", (25.1, 54.9, 107.0), (0.0, 0.0, 107.0))
	def Rack(Index, SX, SY):
		# 철제 선반 (폭 110 = X 방향) + 칸마다 상자/통 (정적 꾸밈 — 선반 칸 높이 11.5/62/112.5/163)
		Prop(f"Rack_{Index}", "steel_frame_shelves_01", (SX, SY, 0.0), ShelfYaw, 0.1, SteelShelf)
		for Level, LZ in enumerate((11.5, 62.0, 112.5, 163.0)):
			Seed = Index * 4 + Level
			if Seed % 3 == 0:
				Prop(f"Rack_{Index}_Box_{Level}", "cardboard_box_01", (SX - 22.0, SY + 2.0, LZ + 0.5), 90.0 + Seed * 7.0, 0.9)
				Prop(f"Rack_{Index}_BoxB_{Level}", "cardboard_box_01", (SX + 26.0, SY + 2.0, LZ + 0.5), 85.0 - Seed * 5.0, 0.85)
			elif Seed % 3 == 1:
				Prop(f"Rack_{Index}_Crate_{Level}", "plastic_crate_02", (SX - 20.0, SY + 2.0, LZ), 90.0, 0.95)
				Prop(f"Rack_{Index}_Tin_{Level}", "metal_jerrycan_green", (SX + 30.0, SY + 2.0, LZ), 180.0 + Seed * 9.0, 0.9)
			else:
				Prop(f"Rack_{Index}_Cheese_{Level}", "CheeseBox_01", (SX - 25.0, SY, LZ), Seed * 13.0, 1.6)
				Prop(f"Rack_{Index}_Cheese2_{Level}", "CheeseBox_01", (SX + 20.0, SY + 2.0, LZ), 40.0 + Seed * 11.0, 1.6)

	for Index, SX in enumerate((-480.0, -340.0, 1120.0, 1260.0)):
		Rack(Index, SX, HALF_Y - 32.0)
	# 북동쪽: 작업대 2개 + 바이스 + 해머 + 드릴 프레스 + 공구 수레 + 공구 상자
	BenchYaw = FaceYaw(0, -1)
	for Index, BX in enumerate((285.0, 420.0)):
		Prop(f"Workbench_{Index}", "WoodenTable_03", (BX, HALF_Y - 40.0, 0.0), BenchYaw, 1.0, Collider("box", (29.0, 66.0, 41.5), (-0.6, 0.4, 41.5)))
	Prop("Bench_Vice", "bench_vice_01", (255.0, HALF_Y - 60.0, 83.0 + 8.6), BenchYaw + 10.0, 1.0)
	Prop("Bench_Hammer", "sledgehammer_01", (330.0, HALF_Y - 45.0, 83.0 + 10.4), 0.0, 1.0, Rotation=QMul(QuatFromEuler(Yaw=70.0), QuatFromEuler(Roll=90.0)))
	Prop("Bench_Cheese", "CheeseBox_01", (395.0, HALF_Y - 50.0, 83.0), 25.0, 1.4)
	Prop("Bench_ToolChest", "metal_tool_chest", (455.0, HALF_Y - 45.0, 83.0), BenchYaw + 8.0, 0.8)
	Prop("DrillPress", "old_drill_press", (545.0, HALF_Y - 50.0, 0.0), BenchYaw, 1.0, Collider("box", (30.0, 26.0, 52.5), (12.0, -9.5, 52.5)))
	Prop("ToolCart", "tool_cart", (700.0, HALF_Y - 70.0, 0.0), BenchYaw + 4.0, 1.0, Collider("box", (37.7, 63.7, 48.0), (0.8, -2.1, 48.0)))
	Prop("StorageCart", "industrial_storage_cart", (880.0, HALF_Y - 95.0, 0.0), BenchYaw, 1.0, Collider("box", (55.0, 80.0, 69.0), (0.0, 0.0, 69.0)))
	Prop("StorageCart_Box", "cardboard_box_01", (865.0, HALF_Y - 95.0, 138.5), 80.0, 1.0)
	Prop("PowerBox", "power_box_01", (100.0, HALF_Y - 1.0, 160.0), FaceYaw(0, -1), 1.2)
	Prop("Ladder", "ladder_sectioned_01", (30.0, HALF_Y - 45.0, 0.0), BenchYaw, 1.0, Rotation=QMul(QuatFromEuler(Yaw=BenchYaw), QuatFromEuler(Pitch=-8.0)))
	# 동쪽 벽: 발전기, 가스통, 파란 드럼통 무리(굴러온 드럼통이 부딪힘), 타이어 더미
	Prop("Generator", "portable_generator", (HALF_X - 70.0, 380.0, 0.0), FaceYaw(-1, 0), 1.0, Collider("box", (28.0, 40.0, 29.0), (0.0, -0.5, 29.0)))
	CanCol = Collider("box", (8.4, 18.5, 25.0), (0.0, -1.5, 25.0))
	for Index, (DX, DY, Yaw) in enumerate(((0.0, 0.0, 10.0), (5.0, 45.0, -8.0), (40.0, 22.0, 95.0))):
		Prop(f"Jerrycan_{Index}", "metal_jerrycan_green", (HALF_X - 160.0 + DX, 470.0 + DY, 0.0), Yaw, 1.0, CanCol, Body(6.0))
	TankCol = Collider("box", (16.5, 16.5, 27.6), (0.0, 0.0, 27.6))  # 서 있는 원통은 상자로 (캡슐 바닥은 둥글어 넘어진다)
	for Index, (DX, DY) in enumerate(((0.0, 0.0), (36.0, 8.0), (14.0, 36.0))):
		Prop(f"Propane_{Index}", "propane_tank", (HALF_X - 70.0 + DX - 20.0, 200.0 + DY, 0.0), Index * 47.0, 1.0, TankCol, Body(12.0))
	DrumCol = Collider("box", (24.2, 24.2, 44.0), (0.0, 0.0, 44.0))
	for Index, (DX, DY) in enumerate(((0.0, 0.0), (60.0, 0.0), (30.0, 54.0), (0.0, -60.0), (60.0, -60.0))):
		Prop(f"BlueDrum_{Index}", "Barrel_02", (HALF_X - 150.0 + DX, HOOK[1] + DY, 0.0), Index * 90.0, 1.0, DrumCol, Body(20.0))
	TyreFlat = QuatFromEuler(Pitch=90.0)
	for Index in range(4):
		Prop(f"TyreStack_{Index}", "old_tyre", (HALF_X - 80.0, -280.0, 30.0 + Index * 16.6 - 21.7), Rotation=QMul(QuatFromEuler(Yaw=Index * 33.0), TyreFlat),
			 Col=Collider("box", (8.2, 29.0, 29.0), (0.0, 0.0, 0.0)), Dyn=Body(9.0, Friction=0.9))
	Prop("WheelRim_A", "rusted_wheel_rim_01", (HALF_X - 30.0, -150.0, 20.2), FaceYaw(-1, 0.3), 1.0, Rotation=QMul(QuatFromEuler(Yaw=170.0), QuatFromEuler(Roll=-12.0)))
	# 남쪽 벽 (문 서쪽): 시멘트 포대 더미 + 손수레 + 나무 상자
	BagCol = Collider("box", (34.0, 22.5, 9.0), (0.0, 0.2, 9.0))
	for Index, (DX, DY, Z, Yaw) in enumerate(((0, 0, 0, 0), (0, 48, 0, 3), (72, 0, 0, -2), (72, 48, 0, 1), (35, 24, 18.1, 88), (10, 20, 36.2, 4))):
		Prop(f"CementBag_{Index}", "cement_bag", (-1250.0 + DX, -HALF_Y + 60.0 + DY, Z), Yaw, 1.0, BagCol, Body(25.0, Friction=0.9))
	Prop("HandTruck", "hand_truck", (-1050.0, -HALF_Y + 45.0, 0.0), FaceYaw(0, 1) - 20.0, 1.0)
	Prop("Wall_Crate_A", "wooden_crate_02", (-780.0, -HALF_Y + 40.0, 1.0), 0.0, 1.0, Collider("box", (58.0, 26.5, 23.2), (0.0, 0.0, 22.2)), Body(40.0))
	Prop("Wall_Crate_B", "wooden_crate_01", (-770.0, -HALF_Y + 45.0, 47.3), 90.0, 1.0, CrateCol, Body(18.0))
	# 남쪽 벽 (문 동쪽): 나무 상자 + 수레
	Prop("Wall_Crate_C", "wooden_crate_02", (800.0, -HALF_Y + 40.0, 1.0), 0.0, 1.0, Collider("box", (58.0, 26.5, 23.2), (0.0, 0.0, 22.2)), Body(40.0))
	Prop("Wall_Crate_D", "wooden_crate_02", (800.0, -HALF_Y + 40.0, 47.4), 4.0, 1.0, Collider("box", (58.0, 26.5, 23.2), (0.0, 0.0, 22.2)), Body(40.0))

	# 동쪽 칸 가운데: 덮개 씌운 차(정비 중) + 바퀴 + 남쪽 줄 철제 선반 3개(등 맞댄 2열)
	Prop("Garage_Car", "covered_car", (1120.0, 140.0, 0.0), 6.0, 1.0, Collider("box", (220.0, 92.0, 70.0), (0.0, 0.0, 70.0)))
	Prop("Garage_Rim_A", "rusted_wheel_rim_01", (860.0, 230.0, 20.2), 0.0, 1.0, Rotation=QMul(QuatFromEuler(Yaw=75.0), QuatFromEuler(Roll=-8.0)))
	Prop("Garage_Jack_Tyre", "old_tyre", (845.0, 40.0, 8.3), Rotation=QMul(QuatFromEuler(Yaw=20.0), TyreFlat),
		 Col=Collider("box", (8.2, 29.0, 29.0), (0.0, 0.0, 0.0)), Dyn=Body(9.0, Friction=0.9))
	Prop("Garage_ToolChest", "metal_tool_chest", (1380.0, 160.0, 0.0), FaceYaw(-1, 0), 1.0, Collider("box", (20.3, 34.2, 32.6), (-0.6, -2.6, 32.6)))
	for Index, (SX, SY) in enumerate(((1030.0, -250.0), (1160.0, -250.0), (1290.0, -250.0), (1030.0, -306.0), (1160.0, -306.0), (1290.0, -306.0))):
		Rack(4 + Index, SX, SY)
	# 서쪽 칸: 작업대 섬(등 맞댄 작업대 2개 + 공구) + 드럼통 무리(문 안쪽)
	for Index, (BY, Yaw) in enumerate(((250.0, FaceYaw(0, -1)), (190.0, FaceYaw(0, 1)))):
		Prop(f"Island_Bench_{Index}", "WoodenTable_03", (-380.0, BY, 0.0), Yaw, 1.0, Collider("box", (29.0, 66.0, 41.5), (-0.6, 0.4, 41.5)))
	Prop("Island_Vice", "bench_vice_01", (-420.0, 228.0, 83.0 + 8.6), FaceYaw(0, -1) + 15.0, 1.0)
	Prop("Island_Generator", "portable_generator", (-330.0, 205.0, 83.0), 30.0, 0.7)
	Prop("Island_Drill", "old_drill_press", (-420.0, 180.0, 83.0), FaceYaw(0, 1), 0.8)
	Prop("Island_ToolCart", "tool_cart", (-200.0, 240.0, 0.0), FaceYaw(1, 0.2), 1.0, Collider("box", (37.7, 63.7, 48.0), (0.8, -2.1, 48.0)))
	OilCol = Collider("box", (31.0, 31.0, 46.5), (0.0, 0.0, 46.5))
	for Index, (DX, DY) in enumerate(((0.0, 0.0), (66.0, 0.0), (33.0, 58.0))):
		Prop(f"OilDrum_{Index}", "barrel_03", (450.0 + DX, -HALF_Y + 50.0 + DY, 0.0), Index * 90.0, 1.0, OilCol, Body(30.0))

	# ---- 8. 마당: 문 앞 잡동사니 + 건너편 공장 파사드(조립 glTF) + 파사드 뒤 어두운 실내 상자
	Prop("Yard_Car", "covered_car", (900.0, -1500.0, 0.0), 12.0, 1.0, Collider("box", (220.0, 92.0, 70.0), (0.0, 0.0, 70.0)))
	Prop("Yard_Barrel_A", "barrel_03", (-560.0, -900.0, 0.0), 20.0, 1.0, Collider("box", (31, 31, 46.5), (0, 0, 46.5)))
	Prop("Yard_Barrel_B", "barrel_03", (-490.0, -950.0, 0.0), 70.0, 1.0, Collider("box", (31, 31, 46.5), (0, 0, 46.5)))
	Prop("Yard_Crate", "wooden_crate_02", (520.0, -880.0, 0.0), 25.0, 1.0, Collider("box", (58.0, 26.5, 23.2), (0.0, 0.0, 22.2)))
	Prop("Yard_Tyre", "old_tyre", (-300.0, -1050.0, 8.3), Rotation=QMul(QuatFromEuler(Yaw=40.0), QuatFromEuler(Pitch=90.0)))
	S.Model("Yard_Factory", f"{KIT_OUT}/WorkshopYard.gltf", (0, 0, 0))
	FacadeH = YARD_FACTORY["Floors"] * 300.0
	Box("Yard_Factory_Interior", "Steel", (0.0, YARD_FACADE_Y - 160.0, FacadeH * 0.5), (3000.0, 20.0, FacadeH), Collide=False)
	S.Add("Yard_Factory_Collision", {"BoxColliderComponent": {"HalfExtents": [1500.0, 30.0, FacadeH * 0.5]}}, (0.0, YARD_FACADE_Y - 20.0, FacadeH * 0.5))

	# ---- 관리 스크립트 (안내 + E 사격)
	S.Add("WorkshopManager", Script("WorkshopManager"), (0, 0, 0))

	if Camera:
		X, Y, Z, Pitch, Yaw, *Rest = Camera
		S.Add("_VerifyCamera", {"CameraComponent": {"FovYDegrees": Rest[0] if Rest else 70.0, "NearZ": 5.0, "FarZ": 200000.0, "Primary": True,
													 "Priority": 100}}, (X, Y, Z), QuatFromEuler(Pitch=Pitch, Yaw=Yaw))
	# 플레이어 (상자 피라미드 앞, 창고 안쪽 +X를 본다) — 프리팹 인스턴스는 루트만 저장 (PrefabLink.Root = 자기 인덱스)
	PlayerIndex = len(S.Entities)
	S.Add("Player", {
		"PrefabInstanceComponent": {"Asset": "Prefabs/Demo/DemoPlayer.eprefab", "Overrides": ""},
		"PrefabLinkComponent": {"Id": "1", "Root": PlayerIndex}},
		(-1250.0, StackY, 110.0), QuatFromEuler(Yaw=0.0))
	return S, Mats


def Script_Portal():
	return {"ScriptComponent": {"ScriptAsset": "Scripts/Demo/DemoPortal.lua", "ExecutionLocation": 0,
								"PropertyOverrides": json.dumps({"TargetScene": {"Asset": "Scenes/Demo/Hub.escene"}, "Label": "Hub로", "Ready": True,
																 "RememberReturn": False}, ensure_ascii=False)}}


def Main():
	WriteBaseMaterials()
	WriteHazardTexture()
	WriteImportSettings()
	Count = BuildYardFacade()
	Scene, Mats = BuildScene()
	Scene.Save(os.path.join(CONTENT, "Scenes", "Demo", "Workshop.escene"))
	print(f"Workshop 생성: 엔티티 {len(Scene.Entities)}개, 타일 머티리얼 {len(Mats.Written)}개, 파사드 조각 {Count}개")
	if len(sys.argv) > 2 and sys.argv[1] == "--camera":
		Camera = [float(V) for V in sys.argv[2].split(",")]
		Variant, _ = BuildScene(Camera)
		Variant.Save(os.path.join(CONTENT, "Scenes", "Demo", "_WorkshopCamera.escene"))
		print("확인용 변형: Scenes/Demo/_WorkshopCamera.escene (커밋하지 않음)")


if __name__ == "__main__":
	Main()
