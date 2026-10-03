# 데모 서브맵 "Lighting"(항구 창고를 고친 선술집 홀) 생성: 머티리얼(.emat) + 임포트 설정 + 씬(Scenes/Demo/Lighting.escene)
#   실행: python Tools/DemoMap/BuildLighting.py [--overview]  (먼저 Scripts/FetchDemoAssets.ps1로 Poly Haven 에셋을 받는다)
#   --overview: 플레이어 대신 고정 카메라를 둔 확인용 변형(Scenes/Demo/_LightingOverview.escene)도 쓴다 — 커밋하지 않는다
#   보여 주는 기능: 창으로 드는 햇빛 + 볼류메트릭 안개(빛줄기), DDGI 조도 볼륨, RTAO, RT 반사(광택 마루), 면광원(창·바 조명),
#                   IES 프로파일(벽 워셔·다운라이트·바 조명 띠), 발광 머티리얼
#   좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 홀 안쪽 X [-1000, 1000](길이) × Y [-600, 600](폭) × Z [0, 650], 창은 남쪽(-Y) 벽
#   배치를 바꿀 때는 씬 파일이 아니라 이 스크립트를 고치고 다시 실행한다 (결정적 — 난수 없음)
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402
from AssetFixes import GLASS_FIX, FrameModel, WriteGlassFixedModel  # noqa: E402

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTENT = os.path.join(ROOT, "Projects", "Sample", "Content")
PH      = "Asset/PolyHaven"
MAT_DIR = "Materials/Demo/Lighting"

HALF_X, HALF_Y, HEIGHT = 1000.0, 600.0, 650.0
WALL = 40.0  # 벽 두께
WINDOW_X = [-750.0, -375.0, 0.0, 375.0, 750.0]
WINDOW_W, SILL_Z, HEAD_Z = 180.0, 100.0, 480.0


def Model(Id):
	# 유리가 알파 없는 jpg라 그림을 가리는 액자는 공용 고친 사본 (AssetFixes — Gallery와 같은 파일)
	return FrameModel(Id)


def FaceYaw(DX, DY):
	# Poly Haven 모델 정면 = 엔진 -X(glTF +Z) → 정면이 (DX, DY) 방향을 보게 하는 Yaw
	return math.degrees(math.atan2(-DY, -DX))


# ---- 머티리얼 -------------------------------------------------------------------------------------------------------
# 표면 텍스처: (Poly Haven 텍스처 Id, 타일 한 장 크기 cm, 거칠기 배율, 색 배율)
SURFACES = {
	"Floor":    ("herringbone_parquet", 130.0, 0.45, [1.0, 1.0, 1.0]),  # 광택 마루 (RT 반사)
	"Brick":    ("red_brick", 160.0, 1.0, [0.95, 0.92, 0.9]),
	"Ceiling":  ("dark_wooden_planks", 220.0, 1.0, [1.0, 1.0, 1.0]),
	"Beam":     ("rough_wood", 160.0, 1.0, [0.8, 0.75, 0.7]),
	"Trim":     ("wood_table_worn", 110.0, 0.7, [1.0, 1.0, 1.0]),
	"Cobble":   ("cobblestone_floor_04", 220.0, 1.0, [1.0, 1.0, 1.0]),
}


def WriteJson(Path, Doc):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")


def WriteBaseMaterials():
	# 이 폴더는 생성물 전용 — 이전 실행의 타일 인스턴스(크기가 바뀌어 이름이 달라진 것)를 지우고 다시 쓴다
	Folder = os.path.join(CONTENT, MAT_DIR)
	if os.path.isdir(Folder):
		for Name in os.listdir(Folder):
			if Name.endswith(".emat"):
				os.remove(os.path.join(Folder, Name))
	# 타일 PBR 그래프 (VectorParameter 출력 2~5 = x~w): 내장 큐브 UV(면마다 0~1)에 Tiling을 곱한다 — 면 크기별 인스턴스가 Tiling만 덮어쓴다(탄젠트 노멀이 UV와 맞음)
	Graph = {
		"Name": "TiledPBR",
		"BlendMode": "Opaque",
		"Parameters": [
			{"Name": "BaseTexture", "Type": "Texture", "Value": "", "Usage": "Color"},
			{"Name": "ArmTexture", "Type": "Texture", "Value": "", "Usage": "Linear"},
			{"Name": "NormalTexture", "Type": "Texture", "Value": "", "Usage": "Normal"},
			{"Name": "Tiling", "Type": "Vector", "Value": [1.0, 1.0, 0.0, 0.0]},
			{"Name": "Tint", "Type": "Vector", "Value": [1.0, 1.0, 1.0, 1.0]},
			{"Name": "RoughnessScale", "Type": "Scalar", "Value": 1.0},
		],
		"Graph": {
			"Nodes": [
				{"Id": "uv0", "Type": "TexCoord"},
				{"Id": "tiling", "Type": "VectorParameter", "Parameter": "Tiling"},
				{"Id": "tilingXY", "Type": "Append", "Inputs": {"A": "tiling:2", "B": "tiling:3"}},
				{"Id": "uv", "Type": "Multiply", "Inputs": {"A": "uv0", "B": "tilingXY"}},
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
	WriteJson(os.path.join(CONTENT, MAT_DIR, "TiledPBR.emat"), Graph)

	def Plain(Name, Base, Alpha=1.0, Emissive=(0.0, 0.0, 0.0), Rough=0.9, Blend=None):
		Mat = {
			"Name": Name, "BaseColorFactor": list(Base) + [Alpha], "EmissiveFactor": list(Emissive),
			"Metallic": 0.0, "Roughness": Rough, "NormalScale": 1.0, "OcclusionStrength": 1.0,
			"BaseColorTexture": "", "MetallicRoughnessTexture": "", "NormalTexture": "", "OcclusionTexture": "", "EmissiveTexture": "",
		}
		if Blend:
			Mat["BlendMode"] = Blend
		WriteJson(os.path.join(CONTENT, MAT_DIR, f"{Name}.emat"), Mat)

	Plain("WindowGlass", (0.8, 0.88, 0.9), Alpha=0.08, Rough=0.04, Blend="Translucent")
	Plain("BarStripGlow", (0.0, 0.0, 0.0), Emissive=(1.2, 0.75, 0.35))  # 바 위 조명 띠 (면광원 자리)


class FMaterials:
	# 표면 + 면 크기 → 타일 인스턴스(.emat) 경로. 같은 키는 한 파일
	def __init__(self):
		self.Written = {}

	def Get(self, Surface, U, V):
		Id, Tile, RoughScale, Tint = SURFACES[Surface]
		TU, TV = round(U / Tile, 2), round(V / Tile, 2)
		Name = f"{Surface}_{int(round(U))}x{int(round(V))}"
		if Name not in self.Written:
			Rel = f"../../../{PH}/{Id}/{Id}"
			Doc = {
				"Name": Name, "Parent": "TiledPBR.emat",
				"Parameters": [
					{"Name": "BaseTexture", "Type": "Texture", "Value": f"{Rel}_diff_2k.jpg"},
					{"Name": "ArmTexture", "Type": "Texture", "Value": f"{Rel}_arm_2k.jpg"},
					{"Name": "NormalTexture", "Type": "Texture", "Value": f"{Rel}_nor_gl_2k.jpg"},
					{"Name": "Tiling", "Type": "Vector", "Value": [max(TU, 0.05), max(TV, 0.05), 0.0, 0.0]},
					{"Name": "Tint", "Type": "Vector", "Value": Tint + [1.0]},
					{"Name": "RoughnessScale", "Type": "Scalar", "Value": RoughScale},
				],
			}
			WriteJson(os.path.join(CONTENT, MAT_DIR, f"{Name}.emat"), Doc)
			self.Written[Name] = True
		return f"{MAT_DIR}/{Name}.emat"


# 임포트 설정: 스캔/고밀도 에셋은 쿠킹 때 LOD0 삼각형 상한으로 줄인다 (실제 glTF 삼각형 수 기준)
IMPORT_SETTINGS = {
	"wooden_candlestick": {"MaxTriangles": 4000},   # 원본 21만 삼각형 (polycount 표기 3328)
	"wine_bottles_01":    {"MaxTriangles": 12000},
}


def WriteImportSettings():
	for Id, Settings in IMPORT_SETTINGS.items():
		WriteJson(os.path.join(CONTENT, "Asset", "PolyHaven", Id, f"{Id}.gltf.eimport"), Settings)


USED_FRAMES = ["hanging_picture_frame_02"]  # 이 맵이 쓰는 유리 고침 액자 (사본은 Gallery와 공유 — 같은 내용)


def WriteGlassFixedModels():
	for Id in USED_FRAMES:
		assert Id in GLASS_FIX
		WriteGlassFixedModel(CONTENT, Id)


# ---- 씬 -------------------------------------------------------------------------------------------------------------
def BuildScene(Overview=False):
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

	def Prop(Name, Id, X, Y, Z=0.0, Yaw=0.0, Scale=1.0, Collider=None, Parent=-1):
		# Collider = (반 크기, 오프셋) 모델 로컬 cm — 스케일이 곱해진다
		Extra = None
		if Collider:
			Half, Offset = Collider
			Extra = {"BoxColliderComponent": {"HalfExtents": list(Half), "Offset": list(Offset)}}
		return S.Model(Name, Model(Id), (X, Y, Z), Yaw, Scale, Parent, Extra)

	# ---- 환경: 늦은 오후 해(남서쪽 낮은 고도) + 대기 하늘 + 실내 먼지 안개(볼류메트릭 — 빛줄기)
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.86, 0.68], "Intensity": 5.5}}, (0, 0, 2000), QuatFromEuler(Pitch=-34, Yaw=62))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {},
		"SkyLightComponent": {"Intensity": 1.0},
		"HeightFogComponent": {
			"Color": [0.5, 0.45, 0.4], "Density": 0.004, "HeightFalloff": 0.0, "StartDistance": 0.0, "MaxOpacity": 0.3,
			"DirectionalInscatteringColor": [0.0, 0.0, 0.0],
			"Volumetric": True, "VolumetricDistance": 3000.0, "VolumetricAlbedo": [0.9, 0.85, 0.8], "VolumetricExtinctionScale": 1.0,
			"VolumetricAnisotropy": 0.55, "VolumetricDirectionalScale": 2.5, "VolumetricLocalLightScale": 0.4},
	})

	# DDGI: 홀보다 조금 큰 상자 (바깥에도 프로브 한 층). 반사 캡처는 두지 않는다 — 굽기는 DDGI 없이 하늘빛만 담아
	#   어두운 실내 거친 면에 푸른 반사가 얹혔다(2026-10-04 비교). 거친 면 반사는 DDGI/하늘 경로에 맡긴다
	S.Add("HallProbes", {"IrradianceVolumeComponent": {
		"HalfExtents": [HALF_X + 80.0, HALF_Y + 80.0, HEIGHT * 0.5 + 60.0], "Spacing": 120.0, "Intensity": 1.0, "FadeDistance": 30.0,
		"NormalBias": 10.0, "ViewBias": 20.0, "Hysteresis": 0.98, "RaysPerProbe": 256, "ProbeUpdateBudget": 0, "MaxRayDistance": 10000.0,
		"Relocation": True, "Classification": True, "Priority": 0, "DebugProbes": 0, "DebugProbeRadius": 8.0}},
		(0, 0, HEIGHT * 0.5))

	# ---- 건물: 바닥/천장/벽(남쪽 벽은 창 사이 조각) + 들보 + 바깥 자갈 마당
	Box("Yard", "Cobble", (0, 0, -15.0), (8000.0, 8000.0, 20.0))
	Box("Floor", "Floor", (0, 0, -10.0), (2 * HALF_X, 2 * HALF_Y, 20.0))
	Box("Ceiling", "Ceiling", (0, 0, HEIGHT + 15.0), (2 * HALF_X + 2 * WALL, 2 * HALF_Y + 2 * WALL, 30.0))
	Box("Wall_North", "Brick", (0, HALF_Y + WALL * 0.5, HEIGHT * 0.5), (2 * HALF_X + 2 * WALL, WALL, HEIGHT))
	Box("Wall_West", "Brick", (-HALF_X - WALL * 0.5, 0, HEIGHT * 0.5), (WALL, 2 * HALF_Y, HEIGHT))
	Box("Wall_East", "Brick", (HALF_X + WALL * 0.5, 0, HEIGHT * 0.5), (WALL, 2 * HALF_Y, HEIGHT))
	SouthY = -HALF_Y - WALL * 0.5
	Box("Wall_South_Low", "Brick", (0, SouthY, SILL_Z * 0.5), (2 * HALF_X + 2 * WALL, WALL, SILL_Z))
	Box("Wall_South_High", "Brick", (0, SouthY, (HEAD_Z + HEIGHT) * 0.5), (2 * HALF_X + 2 * WALL, WALL, HEIGHT - HEAD_Z))
	Edges = [-HALF_X - WALL] + [E for X in WINDOW_X for E in (X - WINDOW_W * 0.5, X + WINDOW_W * 0.5)] + [HALF_X + WALL]
	for Index in range(0, len(Edges), 2):
		X0, X1 = Edges[Index], Edges[Index + 1]
		Box(f"Wall_South_Pier_{Index // 2}", "Brick", ((X0 + X1) * 0.5, SouthY, (SILL_Z + HEAD_Z) * 0.5), (X1 - X0, WALL, HEAD_Z - SILL_Z))

	# 창: 나무 틀(문틀·문지방·가운데 세로살·가로살) + 얇은 유리(그림자/TLAS 밖 — 햇빛은 지나간다) + 바깥 하늘빛 면광원
	OpenH = HEAD_Z - SILL_Z
	for Index, WX in enumerate(WINDOW_X):
		Window = S.Add(f"Window_{Index}", {}, (WX, SouthY, SILL_Z))
		Box(f"Window_{Index}_Sill", "Trim", (0, -6.0, -3.0), (WINDOW_W + 24.0, WALL + 22.0, 8.0), Parent=Window)
		Box(f"Window_{Index}_Head", "Trim", (0, 0, OpenH + 2.0), (WINDOW_W + 16.0, WALL + 8.0, 10.0), Parent=Window)
		for Side in (-1, 1):
			Box(f"Window_{Index}_Jamb", "Trim", (Side * (WINDOW_W * 0.5 - 4.0), 0, OpenH * 0.5), (12.0, WALL + 8.0, OpenH), Parent=Window)
		Box(f"Window_{Index}_Mullion", "Trim", (0, 0, OpenH * 0.5), (7.0, 9.0, OpenH), Collide=False, Parent=Window)
		Box(f"Window_{Index}_Transom", "Trim", (0, 0, OpenH * 0.68), (WINDOW_W - 12.0, 9.0, 7.0), Collide=False, Parent=Window)
		S.Add(f"Window_{Index}_Glass", {
			"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MAT_DIR}/WindowGlass.emat"},
			"BoxColliderComponent": {}}, (0, 6.0, OpenH * 0.5), None, ((WINDOW_W - 12.0) / 100.0, 0.01, OpenH / 100.0), Window)
		S.Add(f"Window_{Index}_SkyLight", {"AreaLightComponent": {
			"Shape": 0, "Color": [0.7, 0.82, 1.0], "Intensity": 12.0, "Width": WINDOW_W - 12.0, "Height": OpenH, "Radius": 900.0,
			"TwoSided": False, "BarnDoorAngle": 85.0, "BarnDoorLength": 10.0, "CastShadows": False,
			"IesProfile": "", "UseIesIntensity": False, "IesIntensityScale": 1.0, "CookieTexture": "", "CookieScale": [1, 1], "CookiePanSpeed": [0, 0]}},
			(0, WALL * 0.5 + 2.0, OpenH * 0.5), QuatFromEuler(Yaw=90.0), Parent=Window)

	# 지붕 들보 (폭 방향) + 벽 기둥
	BeamH = 45.0
	for Index in range(8):
		BX = -875.0 + Index * 250.0
		Box(f"Beam_{Index}", "Beam", (BX, 0, HEIGHT - BeamH * 0.5), (30.0, 2 * HALF_Y, BeamH), Collide=False)
	for Index, PX in enumerate([-875.0, -375.0, 125.0, 625.0]):
		Box(f"Post_North_{Index}", "Beam", (PX, HALF_Y - 14.0, HEIGHT * 0.5), (30.0, 28.0, HEIGHT))
	Box("Beam_Wall_North", "Beam", (0, HALF_Y - 14.0, HEIGHT - BeamH - 20.0), (2 * HALF_X, 28.0, 40.0), Collide=False)
	Box("Beam_Wall_South", "Beam", (0, -HALF_Y + 14.0, HEIGHT - BeamH - 20.0), (2 * HALF_X, 28.0, 40.0), Collide=False)

	# ---- 동쪽 끝: 바 (붙박이 카운터 + 뒷장 + 술통) — 위에 면광원 조명 띠
	BarX = 760.0
	Box("Bar_Counter", "Trim", (BarX, 0, 50.0), (60.0, 700.0, 100.0))
	Box("Bar_Top", "Trim", (BarX - 4.0, 0, 104.0), (78.0, 720.0, 8.0), Collide=False)
	Box("Bar_FootRail", "Beam", (BarX - 40.0, 0, 18.0), (10.0, 680.0, 10.0), Collide=False)
	Box("Bar_Soffit", "Trim", (BarX - 10.0, 0, 452.0), (110.0, 760.0, 24.0), Collide=False)
	S.Add("Bar_Strip", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MAT_DIR}/BarStripGlow.emat"}},
		(BarX - 10.0, 0, 439.5), None, (0.14, 6.8, 0.01))
	S.Add("Bar_StripLight", {"AreaLightComponent": {
		"Shape": 0, "Color": [1.0, 0.75, 0.5], "Intensity": 16.0, "Width": 680.0, "Height": 14.0, "Radius": 900.0,
		"TwoSided": False, "BarnDoorAngle": 88.0, "BarnDoorLength": 10.0, "CastShadows": True,
		"IesProfile": "AreaLights/Linear_Batwing.ies", "UseIesIntensity": False, "IesIntensityScale": 1.0,
		"CookieTexture": "", "CookieScale": [1, 1], "CookiePanSpeed": [0, 0]}},
		(BarX - 10.0, 0, 438.0), QuatFromEuler(Pitch=-90.0))
	for Index, SY in enumerate([-270.0, -135.0, 0.0, 135.0, 270.0]):
		Prop(f"Bar_Stool_{Index}", "wooden_stool_02", BarX - 90.0, SY, 0.0, 90.0 + Index * 23.0, 3.3)
	# 카운터 위: 램프·술병·잔
	Prop("Bar_Lamp_A", "vintage_oil_lamp", BarX - 5.0, -250.0, 108.0, 20.0, 0.55)
	Prop("Bar_Bottles", "wine_bottles_01", BarX + 8.0, 40.0, 108.0, 0.0, 1.0)
	Prop("Bar_Goblets", "brass_goblets", BarX - 5.0, -60.0, 108.0, 35.0, 1.0)
	Prop("Bar_Jug", "jug_01", BarX + 5.0, 220.0, 108.0, 140.0, 1.0)
	# 뒷장 (동쪽 벽): 책장 2개 + 가운데 고딕 장 + 술통
	ShelfYaw = FaceYaw(-1, 0)
	Prop("BackBar_Shelf_A", "wooden_bookshelf_worn", HALF_X - 32.0, -250.0, 0.0, ShelfYaw, 1.0, ((29, 69, 103), (0, 0, 103)))
	Prop("BackBar_Shelf_B", "wooden_bookshelf_worn", HALF_X - 32.0, 250.0, 0.0, ShelfYaw, 1.0, ((29, 69, 103), (0, 0, 103)))
	Prop("BackBar_Cabinet", "GothicCabinet_01", HALF_X - 50.0, 0.0, 0.0, ShelfYaw, 0.85, ((57, 86, 118), (-10, 0, 118)))
	Prop("BackBar_Bottles_A", "wine_bottles_01", HALF_X - 32.0, -282.0, 206.0, 0.0, 1.0)
	Prop("BackBar_Bottles_B", "wine_bottles_01", HALF_X - 32.0, 218.0, 206.0, 0.0, 1.0)
	Prop("BackBar_Basket", "wicker_basket_01", HALF_X - 30.0, 0.0, 201.0, 15.0, 1.3)
	Barrels = [("wine_barrel_01", 905.0, -505.0, 0.0), ("wooden_crate_01", 905.0, -505.0, 87.0), ("barrel_03", 820.0, -520.0, 0.0),
			   ("barrel_03", 900.0, 515.0, 0.0), ("wooden_crate_02", 770.0, 530.0, 0.0), ("wooden_crate_01", 770.0, 528.0, 45.0)]
	for Index, (Id, X, Y, Z) in enumerate(Barrels):
		Half = (36, 36, 44) if Id != "wooden_crate_02" else (58, 26, 23)
		Prop(f"Bar_Store_{Index}", Id, X, Y, Z, 15.0 + Index * 37.0, 1.0, (Half, (0, 0, Half[2])) if Z == 0.0 else None)
	Prop("Bar_Menu", "standing_chalkboard_01", 520.0, -470.0, 0.0, FaceYaw(-0.7, 0.7), 1.0, ((38, 45, 75), (0, 0, 75)))
	Prop("Bar_Dartboard", "dartboard", HALF_X - 1.0, 470.0, 175.0, ShelfYaw, 1.0)

	# ---- 가운데: 둥근 탁자 6개 + 의자 + 탁자 위 램프/촛대/잔
	TableScale = 0.78
	TableTop = 100.0 * TableScale
	Tables = [(-480.0, -230.0), (0.0, -230.0), (460.0, -230.0), (-480.0, 190.0), (0.0, 190.0), (460.0, 190.0)]
	for Index, (TX, TY) in enumerate(Tables):
		Prop(f"Table_{Index}", "round_wooden_table_01", TX, TY, 0.0, Index * 31.0, TableScale, ((60, 60, 50), (0, 0, 50)))
		for Seat in range(3):
			Angle = math.radians(Index * 47.0 + Seat * 120.0 + 20.0)
			CX, CY = TX + math.cos(Angle) * 95.0, TY + math.sin(Angle) * 95.0
			Prop(f"Table_{Index}_Chair_{Seat}", "gallinera_chair", CX, CY, 0.0, FaceYaw(TX - CX, TY - CY), 0.92)
		if Index % 2 == 0:
			Prop(f"Table_{Index}_Lamp", "vintage_oil_lamp", TX + 12.0, TY - 8.0, TableTop, Index * 40.0, 0.5)
			S.Add(f"Table_{Index}_LampLight", {"PointLightComponent": {
				"Color": [1.0, 0.62, 0.3], "Intensity": 1.6, "Radius": 320.0, "CastShadows": False}}, (TX + 12.0, TY - 8.0, TableTop + 26.0))
			Prop(f"Table_{Index}_Goblets", "brass_goblets", TX - 25.0, TY + 20.0, TableTop, Index * 70.0, 0.8)
		else:
			Prop(f"Table_{Index}_Candle", "wooden_candlestick", TX - 10.0, TY + 15.0, TableTop, 0.0, 1.0)
			Prop(f"Table_{Index}_Jug", "jug_01", TX + 25.0, TY - 20.0, TableTop, Index * 50.0, 1.0)
			Prop(f"Table_{Index}_Plant", "potted_plant_04", TX + 5.0, TY - 35.0, TableTop, 0.0, 1.2)

	# 샹들리에 (천장에 매단 랜턴형) + 점광원 (가운데 것만 그림자)
	for Index, CX in enumerate([-500.0, 0.0, 500.0]):
		Prop(f"Chandelier_{Index}", "lantern_chandelier_01", CX, -20.0, HEIGHT - 2.0, 0.0, 1.5)
		S.Add(f"Chandelier_{Index}_Light", {"PointLightComponent": {
			"Color": [1.0, 0.68, 0.38], "Intensity": 7.0, "Radius": 1000.0, "CastShadows": Index == 1}}, (CX, -20.0, HEIGHT - 95.0))

	# ---- 북쪽 벽: 갤러리 (그림 + 벽 등 + IES 워셔), 고딕 장, 괘종시계
	WallFace = HALF_Y - 1.0
	GalleryYaw = FaceYaw(0, -1)
	Paintings = [(-250.0, "fancy_picture_frame_01", 2.2), (-10.0, "hanging_picture_frame_02", 1.9), (375.0, "fancy_picture_frame_01", 2.2)]
	for Index, (PX, Id, Scale) in enumerate(Paintings):
		Prop(f"Gallery_Painting_{Index}", Id, PX, WallFace, 200.0, GalleryYaw, Scale)
		Prop(f"Gallery_Sconce_{Index}", "industrial_wall_sconce", PX, WallFace, 300.0, GalleryYaw, 1.4)
		S.Add(f"Gallery_Washer_{Index}", {"PointLightComponent": {
			"Color": [1.0, 0.8, 0.6], "Intensity": 5.0, "Radius": 450.0, "CastShadows": False,
			"IesProfile": "AreaLights/Wallwash_Asym.ies", "UseIesIntensity": False, "IesIntensityScale": 1.0,
			"CookieTexture": "", "CookieScale": [1, 1], "CookiePanSpeed": [0, 0]}},
			(PX, WallFace - 30.0, 300.0), QuatFromEuler(Pitch=-90.0, Yaw=-90.0))
	Prop("Gallery_Clock", "vintage_grandfather_clock_01", 530.0, HALF_Y - 30.0, 0.0, GalleryYaw, 1.0, ((22, 32, 110), (3, 6, 110)))

	# ---- 서쪽 끝: 라운지 (소파·안락의자·거울·화분·청동 고래) + 돌아가는 포털
	Prop("Lounge_Sofa", "Sofa_01", -880.0, 400.0, 0.0, FaceYaw(1, -0.3), 1.0, ((33, 79, 40), (3, 0, 40)))
	Prop("Lounge_ArmChair_A", "ArmChair_01", -640.0, 470.0, 0.0, FaceYaw(-1, -0.6), 1.0, ((39, 42, 53), (3, 0, 53)))
	Prop("Lounge_ArmChair_B", "ArmChair_01", -700.0, 260.0, 0.0, FaceYaw(-0.8, 1), 1.0, ((39, 42, 53), (3, 0, 53)))
	Prop("Lounge_Cabinet", "GothicCabinet_01", -560.0, HALF_Y - 60.0, 0.0, GalleryYaw, 0.85, ((57, 86, 118), (-10, 0, 118)))
	Prop("Lounge_Whale", "bronze_whale_statue", -560.0, HALF_Y - 60.0, 236.0 * 0.85, GalleryYaw, 0.55)
	Prop("Lounge_Painting", "fancy_picture_frame_01", -HALF_X + 1.0, 400.0, 200.0, FaceYaw(1, 0), 2.0)
	Prop("BackBar_Mirror", "ornate_mirror_01", HALF_X - 1.0, 0.0, 300.0, ShelfYaw, 2.4)
	Prop("Lounge_Lantern", "wooden_lantern_01", -760.0, 380.0, 0.0, 30.0, 1.4)
	S.Add("Lounge_LanternLight", {"PointLightComponent": {"Color": [1.0, 0.6, 0.3], "Intensity": 1.4, "Radius": 300.0, "CastShadows": False}},
		(-760.0, 380.0, 45.0))
	# 다운라이트 (원판 면광원 + 좁은 IES) — 라운지 소파 위
	S.Add("Lounge_Downlight", {"AreaLightComponent": {
		"Shape": 1, "Color": [1.0, 0.85, 0.7], "Intensity": 12.0, "Width": 24.0, "Height": 24.0, "Radius": 800.0,
		"TwoSided": False, "BarnDoorAngle": 90.0, "BarnDoorLength": 10.0, "CastShadows": True,
		"IesProfile": "AreaLights/Downlight_Narrow.ies", "UseIesIntensity": True, "IesIntensityScale": 1.0,
		"CookieTexture": "", "CookieScale": [1, 1], "CookiePanSpeed": [0, 0]}},
		(-780.0, 380.0, HEIGHT - 2.0), QuatFromEuler(Pitch=-90.0))
	# 남서쪽 창가: 짐 더미
	Prop("Corner_Crate_A", "wooden_crate_02", -880.0, -500.0, 0.0, 8.0, 1.0, ((58, 26, 23), (0, 0, 23)))
	Prop("Corner_Crate_B", "wooden_crate_01", -870.0, -495.0, 45.0, -12.0, 1.0)
	Prop("Corner_Barrel", "barrel_03", -900.0, -380.0, 0.0, 40.0, 1.0, ((32, 32, 46), (0, 0, 46)))
	Prop("Corner_Basket", "wicker_basket_01", -780.0, -520.0, 0.0, 70.0, 1.2)

	# 창 아래 벤치 (햇빛 자리)
	for Index, BX in enumerate([-375.0, 375.0]):
		Prop(f"Window_Bench_{Index}", "painted_wooden_bench", BX, -HALF_Y + 32.0, 0.0, FaceYaw(0, 1), 1.0, ((24, 58, 22), (0, 0, 22)))

	# 뒷장 위 벽 등 (동쪽 벽) — 바 끝이 어둡지 않게
	for Index, SY in enumerate([-250.0, 250.0]):
		Prop(f"BackBar_Sconce_{Index}", "industrial_wall_sconce", HALF_X - 1.0, SY, 270.0, ShelfYaw, 1.4)
		S.Add(f"BackBar_SconceLight_{Index}", {"PointLightComponent": {
			"Color": [1.0, 0.72, 0.45], "Intensity": 3.0, "Radius": 500.0, "CastShadows": False}}, (HALF_X - 35.0, SY, 285.0))

	# 포털 양옆: 술통 + 등불
	for Side in (-1, 1):
		Prop(f"Portal_Barrel_{Side + 1}", "wine_barrel_01", -HALF_X + 45.0, -20.0 + Side * 160.0, 0.0, 30.0 * Side, 1.0, ((36, 36, 44), (0, 0, 44)))
		Prop(f"Portal_Lantern_{Side + 1}", "wooden_lantern_01", -HALF_X + 45.0, -20.0 + Side * 160.0, 87.0, 15.0, 1.1)

	# 돌아가는 포털: 서쪽 벽 가운데 성문 (DemoPortal.lua — Hub로, 돌아가기 자리 기억 안 함)
	Portal = S.Add("Portal_Hub", {"ScriptComponent": {"ScriptAsset": "Scripts/Demo/DemoPortal.lua", "ExecutionLocation": 0,
		"PropertyOverrides": json.dumps({"TargetScene": {"Asset": "Scenes/Demo/Hub.escene"}, "Label": "Hub로", "Ready": True,
			"RememberReturn": False}, ensure_ascii=False)}},
		(-HALF_X + 12.0, -20.0, 0.0), QuatFromEuler(Yaw=0.0))
	S.Model("Portal_Hub_Door", Model("large_castle_door"), (0, 0, -3), 0.0, 1.0, Parent=Portal)
	S.Add("Portal_Hub_Glow", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Demo/PortalGlow.emat"}},
		(18, 0, 148), None, (0.02, 1.85, 2.85), Parent=Portal)
	S.Add("Portal_Hub_Light", {"PointLightComponent": {"Color": [0.5, 0.75, 1.0], "Intensity": 1.5, "Radius": 350.0, "CastShadows": False}}, (80, 0, 220), Parent=Portal)

	if Overview:
		# 확인용 고정 카메라 (남서쪽 구석 높이에서 홀 전체를 본다)
		S.Add("OverviewCamera", {"CameraComponent": {"FovYDegrees": 75.0, "NearZ": 5.0, "FarZ": 20000.0, "Primary": True, "Priority": 100}},
			OVERVIEW_VIEW[:3], QuatFromEuler(Pitch=OVERVIEW_VIEW[3], Yaw=OVERVIEW_VIEW[4]))
	else:
		# 플레이어 (탁자 두 줄 사이에서 바 쪽(+X)을 보며 시작 — 3인칭 카메라가 포털 빛에 너무 붙지 않게) — 프리팹 인스턴스는 루트만 저장 (PrefabLink.Root = 자기 인덱스)
		PlayerIndex = len(S.Entities)
		S.Add("Player", {
			"PrefabInstanceComponent": {"Asset": "Prefabs/Demo/DemoPlayer.eprefab", "Overrides": ""},
			"PrefabLinkComponent": {"Id": "1", "Root": PlayerIndex}},
			(-380.0, -20.0, 110.0))
	return S, Mats


OVERVIEW_VIEW = (-960.0, -150.0, 430.0, -17.0, 8.0)  # 확인용 카메라 X, Y, Z, Pitch, Yaw (--overview=x,y,z,pitch,yaw로 바꿈)


def Main():
	global OVERVIEW_VIEW
	for Arg in sys.argv:
		if Arg.startswith("--overview="):
			OVERVIEW_VIEW = tuple(float(V) for V in Arg.split("=", 1)[1].split(","))
	WriteBaseMaterials()
	WriteImportSettings()
	WriteGlassFixedModels()
	Scene, Mats = BuildScene()
	Scene.Save(os.path.join(CONTENT, "Scenes", "Demo", "Lighting.escene"))
	print(f"Lighting 생성: 엔티티 {len(Scene.Entities)}개, 타일 머티리얼 {len(Mats.Written)}개")
	if any(Arg.startswith("--overview") for Arg in sys.argv):
		Overview, _ = BuildScene(Overview=True)
		Overview.Save(os.path.join(CONTENT, "Scenes", "Demo", "_LightingOverview.escene"))
		print("확인용 변형: Scenes/Demo/_LightingOverview.escene (커밋하지 않음)")


if __name__ == "__main__":
	Main()
