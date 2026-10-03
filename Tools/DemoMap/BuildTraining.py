# 데모 서브맵 "Training"(항구 옆 요새 안뜰 훈련장 — 애니메이션·AI) 생성:
#   요새 키트 조립 glTF + 그래프 머티리얼 + UAL2 마네킹 소켓·노티파이(.emeta) + 애니메이션 그래프 + 비헤이비어 트리 + 씬(Scenes/Demo/Training.escene)
#   실행: python Tools/DemoMap/BuildTraining.py [--overview[=x,y,z,pitch,yaw]]  (먼저 Scripts/FetchDemoAssets.ps1로 Poly Haven 에셋을 받는다)
#   --overview: 플레이어 대신 고정 카메라를 둔 확인용 변형(Scenes/Demo/_TrainingOverview.escene)도 쓴다 — 커밋하지 않는다
#   내비메시(.enav)는 에디터로 굽는다: Verify.ps1 -Target Editor -ExtraArgs "--scene Scenes/Demo/Training.escene --bake-navmesh"
#     (굽기가 씬의 NavMeshAsset을 채워 저장하므로 이 스크립트도 같은 경로를 미리 적는다 — 다시 실행해도 .enav는 그대로 쓰인다)
#   보여 주는 기능: 몽타주 + 노티파이(검술 대련: 공격 노티파이 → 상대 막기/넉백), 소켓 부착(검·방패), 비헤이비어 트리 + Recast 내비게이션
#                   (경비병 순찰 — 계단으로 성벽 위까지, Lua 서비스가 플레이어를 보면 따라오며 시선 IK로 쳐다본다), 발 IK(돌계단),
#                   2D 블렌드 스페이스(방향 이동 훈련 — 목표를 보며 옆걸음·앞뒤), 루트 모션(돌진 레인), 래그돌(교관에게 맞고 쓰러지는 훈련병
#                   → 제자리 리스폰 + 일어나기 몽타주), KayKit 이동 클립 리타기팅("<모델>:<클립>")
#   좌표: 엔진 규약 왼손 Z-up, 1 = 1cm. 안뜰 X [-1245, 1719](남 → 북, 북쪽은 바다 쪽 부두) × Y [-1597, 1597], 남쪽 성벽 가운데 성문 = Hub 포털
#   배치를 바꿀 때는 씬 파일이 아니라 이 스크립트를 고치고 다시 실행한다 (결정적)
import json
import math
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from GltfKit import FGltfKitComposer, EngineToGltf  # noqa: E402
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402

ROOT    = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONTENT = os.path.join(ROOT, "Projects", "Sample", "Content")
PH      = "Asset/PolyHaven"
MAT     = "Materials/Demo/Training"
KIT_OUT = "Asset/DemoKits/Training/TrainingFort.gltf"
ANIM    = "Animations/Demo/Training"
SCRIPTS = "Scripts/Demo/Training"
MANNEQUIN = "Asset/Quaternius/UAL2/UAL2_Standard.glb"
KNIGHT  = "Asset/KayKit/Characters/KnightBare.glb"  # 이동 클립 리타기팅 원본 (화면에는 쓰지 않음)
FORT    = "modular_fort_01"

# 요새 (키트 조각 치수, 2026-10-04 glTF 실측): 얇은 벽 두께 255 · 길이 1482, 성문 길이 741(통로 Y ±171.5), 성벽 위 통로 높이 = 키트 y 7.08 m.
#   키트 바닥은 y 0.40 m에 맞춘다(계단 첫 단 0.63 m, 성문 바닥 0.26 m 사이) → 조각을 40 cm 묻는다
SINK      = 40.0
WALL_T    = 255.0
SEG       = 1482.0
GATE_LEN  = 741.0
WALK_Z    = 708.0 - SINK   # 성벽 위 통로 높이 (엔진 Z)
OUTER_S   = -1500.0        # 남쪽 성벽 바깥면 X
HALF_W    = 1852.5         # 동서 성벽 바깥면 |Y| (남쪽 성벽 = 직선 + 성문 + 직선)
INNER_S   = OUTER_S + WALL_T
INNER_W   = HALF_W - WALL_T
NORTH_END = INNER_S + 2 * SEG   # 동서 성벽 북쪽 끝 X (1719)
QUAY_X    = 2150.0              # 부두 끝 (바다)
SEA_Z     = -170.0


def Model(Id):
	return f"{PH}/{Id}/{Id}.gltf"


def WriteJson(Path, Doc, Compact=False):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=None if Compact else 2, ensure_ascii=False)
		File.write("\n")


def Yawed(X, Y, Yaw):
	R = math.radians(Yaw)
	return X * math.cos(R) - Y * math.sin(R), X * math.sin(R) + Y * math.cos(R)


# ---- 요새 키트 조립 --------------------------------------------------------------------------------------------------
# 조각 로컬(엔진, Yaw 0): 길이(키트 z) → -X, 두께(키트 x, 바깥 → 안) → +Y. 원점 = 조각 바깥면 시작 모서리
STAIR_TREADS = [(0.63 + 0.17447 * I, 0.02 + 0.28724 * I, 0.27 + 0.28724 * I) for I in range(38)]  # (키트 y, z 시작, z 끝) — 계단 디딤판 실측


def BuildFort():
	K = FGltfKitComposer(os.path.join(CONTENT, "Asset", "PolyHaven"), os.path.join(CONTENT, KIT_OUT))

	def Piece(Name, X, Y, Yaw):
		K.Add(FORT, f"{FORT}_{Name}", EngineToGltf(X, Y, -SINK, Yaw))

	# 남쪽 성벽 (바깥 = -X): Yaw -90 → 길이 +Y, 두께 +X
	Piece("wall_thin_straight_01", OUTER_S, -HALF_W, -90.0)
	Piece("wall_thin_gate_01", OUTER_S, -GATE_LEN * 0.5, -90.0)
	Piece("wall_thin_straight_02", OUTER_S, GATE_LEN * 0.5, -90.0)
	# 서쪽 성벽 (바깥 = -Y): Yaw 0 → 길이 -X, 두께 +Y. 북쪽 절반 안쪽 = 계단(남쪽으로 오름), 남쪽 절반 안쪽 = 성벽 위 통로
	Piece("wall_thin_straight_02", NORTH_END, -HALF_W, 0.0)
	Piece("wall_thin_straight_01", NORTH_END - SEG, -HALF_W, 0.0)
	Piece("wall_stairs_straight_01", NORTH_END, -INNER_W - 49.0, 0.0)
	Piece("wall_walkway_straight_01", NORTH_END - SEG, -INNER_W, 0.0)
	# 동쪽 성벽 (바깥 = +Y): Yaw 180 → 길이 +X, 두께 -Y
	Piece("wall_thin_straight_01", INNER_S, HALF_W, 180.0)
	Piece("wall_thin_straight_02", INNER_S + SEG, HALF_W, 180.0)
	return K.Save()


# ---- 머티리얼 --------------------------------------------------------------------------------------------------------
def TexturePath(Id, Map, Folder=None):
	if Folder:
		return f"../../../{PH}/{Folder}/textures/{Id}_{Map}_2k.jpg"
	return f"../../../{PH}/{Id}/{Id}_{Map}_2k.jpg"


def WorldUvGraph(Id, Scale, Wall, BaseScale, RoughScale, Tint=(1.0, 1.0, 1.0), Folder=None):
	# 월드 좌표 UV 그래프 (Alley와 같은 식): 바닥 UV = 월드 XY × Scale, 벽 UV = (X + Y, -Z) × Scale
	Nodes = [
		{"Id": "pos", "Type": "WorldPosition"},
		{"Id": "scale", "Type": "ScalarParameter", "Parameter": "WorldScale"},
		{"Id": "p", "Type": "Multiply", "Inputs": {"A": "pos", "B": "scale"}},
	]
	if Wall:
		Nodes += [
			{"Id": "ps", "Type": "Split", "Inputs": {"A": "p"}},
			{"Id": "u", "Type": "Add", "Inputs": {"A": "ps:0", "B": "ps:1"}},
			{"Id": "v", "Type": "Multiply", "Inputs": {"A": "ps:2", "B": -1.0}},
			{"Id": "uv", "Type": "Append", "Inputs": {"A": "u", "B": "v"}},
		]
	else:
		Nodes.append({"Id": "uv", "Type": "ComponentMask", "Channels": "xy", "Inputs": {"A": "p"}})
	Nodes += [
		{"Id": "col", "Type": "TextureSample", "Texture": "Albedo", "Inputs": {"UV": "uv"}},
		{"Id": "arm", "Type": "TextureSample", "Texture": "Arm", "Inputs": {"UV": "uv"}},
		{"Id": "nrm", "Type": "TextureSample", "Texture": "Normal", "Inputs": {"UV": "uv"}},
		{"Id": "tint", "Type": "VectorParameter", "Parameter": "Tint"},
		{"Id": "base", "Type": "Multiply", "Inputs": {"A": "col:1", "B": "tint:1"}},
		{"Id": "baseS", "Type": "Multiply", "Inputs": {"A": "base", "B": BaseScale}},
		{"Id": "rough", "Type": "Multiply", "Inputs": {"A": "arm:3", "B": RoughScale}},
	]
	Arm = TexturePath(Id, "arm", Folder)
	return {
		"BlendMode": "Opaque",
		"Parameters": [
			{"Name": "Albedo", "Type": "Texture", "Value": TexturePath(Id, "diff", Folder), "Usage": "Color"},
			{"Name": "Arm", "Type": "Texture", "Value": Arm, "Usage": "Linear"},
			{"Name": "Normal", "Type": "Texture", "Value": TexturePath(Id, "nor_gl", Folder), "Usage": "Normal"},
			{"Name": "WorldScale", "Type": "Scalar", "Value": Scale},
			{"Name": "Tint", "Type": "Vector", "Value": list(Tint) + [1.0]},
		],
		"Graph": {"Nodes": Nodes, "Output": {"BaseColor": "baseS", "Roughness": "rough", "Normal": "nrm:1", "AmbientOcclusion": "arm:2", "Metallic": 0.0}},
	}


def WriteMaterials():
	Folder = os.path.join(CONTENT, MAT)
	if os.path.isdir(Folder):
		for Name in os.listdir(Folder):
			if Name.endswith(".emat"):
				os.remove(os.path.join(Folder, Name))
	Graphs = {
		"YardDirt": WorldUvGraph("raked_dirt", 1.0 / 320.0, False, 0.9, 1.0, (1.0, 0.95, 0.88)),
		"YardGravel": WorldUvGraph("sandy_gravel", 1.0 / 300.0, False, 0.85, 1.0),
		"YardCobble": WorldUvGraph("cobblestone_floor_04", 1.0 / 240.0, False, 0.9, 1.0),
		"QuayStone": WorldUvGraph("modular_fort_01_wall", 1.0 / 400.0, True, 0.9, 1.0, Folder=FORT),
	}
	for Name, Graph in Graphs.items():
		WriteJson(os.path.join(Folder, f"{Name}.emat"), {"Name": Name, **Graph})


# ---- 마네킹 소켓·노티파이 (.emeta) ----------------------------------------------------------------------------------------
# 노티파이 시각(초)은 클립 길이(Sword_Regular_A 0.43 / B 0.53 / C 2.0 / Combo 3.0 / Melee_Hook 0.47 / Shield_OneShot 0.83)에 맞춘 칼끝 정점
NOTIFIES = {
	"Sword_Regular_A": [("SwordHit", 0.27)],
	"Sword_Regular_B": [("SwordHit", 0.33)],
	"Sword_Regular_C": [("SwordHit", 0.62), ("SwordFinisher", 1.12)],
	"Sword_Regular_Combo": [("SwordHit", 0.42), ("SwordHit", 1.05), ("SwordFinisher", 1.95)],
	"Melee_Hook": [("PunchHit", 0.27)],
	"Shield_OneShot": [("SwordHit", 0.32)],
}


def _LoadGlb(Path):
	with open(Path, "rb") as File:
		Data = File.read()
	Length = struct.unpack("<I", Data[12:16])[0]
	return json.loads(Data[20:20 + Length])


def _QuatToMatrix(Q):
	X, Y, Z, W = Q
	return np.array([[1 - 2 * (Y * Y + Z * Z), 2 * (X * Y - Z * W), 2 * (X * Z + Y * W)],
					 [2 * (X * Y + Z * W), 1 - 2 * (X * X + Z * Z), 2 * (Y * Z - X * W)],
					 [2 * (X * Z - Y * W), 2 * (Y * Z + X * W), 1 - 2 * (X * X + Y * Y)]])


def _MatrixToQuat(M):
	# 열벡터 회전 행렬 → [X, Y, Z, W]
	T = M[0, 0] + M[1, 1] + M[2, 2]
	if T > 0:
		S = math.sqrt(T + 1.0) * 2
		return [(M[2, 1] - M[1, 2]) / S, (M[0, 2] - M[2, 0]) / S, (M[1, 0] - M[0, 1]) / S, 0.25 * S]
	if M[0, 0] > M[1, 1] and M[0, 0] > M[2, 2]:
		S = math.sqrt(1.0 + M[0, 0] - M[1, 1] - M[2, 2]) * 2
		return [0.25 * S, (M[0, 1] + M[1, 0]) / S, (M[0, 2] + M[2, 0]) / S, (M[2, 1] - M[1, 2]) / S]
	if M[1, 1] > M[2, 2]:
		S = math.sqrt(1.0 + M[1, 1] - M[0, 0] - M[2, 2]) * 2
		return [(M[0, 1] + M[1, 0]) / S, 0.25 * S, (M[1, 2] + M[2, 1]) / S, (M[0, 2] - M[2, 0]) / S]
	S = math.sqrt(1.0 + M[2, 2] - M[0, 0] - M[1, 1]) * 2
	return [(M[0, 2] + M[2, 0]) / S, (M[1, 2] + M[2, 1]) / S, 0.25 * S, (M[1, 0] - M[0, 1]) / S]


# glTF(+Y 위, m, 오른손) → 엔진(Z 위, cm, 왼손): (x, y, z) → (-z, x, y) — FGltfLoader::ConvertPosition과 같은 축 (거울상)
_C = np.array([[0.0, 0.0, -1.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]])


SWORD_SCALE  = 0.85
SHIELD_SCALE = 0.75


def MannequinSockets():
	# 바인드 포즈 뼈 월드 위치로 무기 소켓을 계산한다 (뼈 로컬 = 엔진 축 변환된 노드 공간, cm)
	Gltf = _LoadGlb(os.path.join(CONTENT, MANNEQUIN))
	Nodes = Gltf["nodes"]
	Parent = {Child: I for I, Node in enumerate(Nodes) for Child in Node.get("children", [])}
	Index = {Node.get("name"): I for I, Node in enumerate(Nodes)}

	def World(I):
		M = np.eye(4)
		while True:
			Node = Nodes[I]
			L = np.eye(4)
			L[:3, :3] = _QuatToMatrix(Node.get("rotation", [0, 0, 0, 1])) @ np.diag(Node.get("scale", [1, 1, 1]))
			L[:3, 3] = Node.get("translation", [0, 0, 0])
			M = L @ M
			if I not in Parent:
				return M
			I = Parent[I]

	def P(Name):
		return _C @ World(Index[Name])[:3, 3] * 100.0

	def Socket(Name, Bone, Origin, AxisZ, AxisY, Scale=1.0):
		# 무기 모델 축(엔진 모델 공간) Z → AxisZ, Y → AxisY 가 되도록, 모델 원점을 Origin(월드 cm)에
		W = World(Index[Bone])
		Rb = _C @ W[:3, :3] @ _C.T       # 뼈 회전 (엔진 축)
		Tb = _C @ W[:3, 3] * 100.0
		Z = AxisZ / np.linalg.norm(AxisZ)
		Y = AxisY - Z * np.dot(AxisY, Z)
		Y /= np.linalg.norm(Y)
		X = np.cross(Y, Z)
		Rw = np.stack([X, Y, Z], axis=1)
		Rl = Rb.T @ Rw
		Pl = Rb.T @ (Origin - Tb)
		return {"Name": Name, "Bone": Bone, "Position": [round(float(V), 3) for V in Pl],
				"Rotation": [round(float(V), 5) for V in _MatrixToQuat(Rl)], "Scale": [Scale, Scale, Scale]}

	Sockets = []
	for Side, Sign in (("r", 1.0), ("l", -1.0)):
		Hand, Index1, Pinky, Middle = P(f"hand_{Side}"), P(f"index_01_{Side}"), P(f"pinky_01_{Side}"), P(f"middle_01_{Side}")
		Blade = Index1 - Pinky                       # 주먹 축: 새끼 → 검지 (칼날이 엄지 쪽으로 나온다)
		Finger = Middle - Hand                       # 손가락 방향 (날 평면 = 코등 방향)
		Palm = np.cross(Finger, Blade) * Sign        # 손바닥 쪽 (T 포즈에서 아래)
		Palm /= np.linalg.norm(Palm)
		Grip = Middle + Palm * 3.5 - Finger / np.linalg.norm(Finger) * 1.0
		# 에스톡(0.85배, 길이 1.27 m): 날 = 모델 +Z, 손잡이 가운데 z ≈ -9 → 모델 원점을 날 쪽으로
		Bz = Blade / np.linalg.norm(Blade)
		Sockets.append(Socket("Sword" if Side == "r" else "SwordL", f"hand_{Side}", Grip + Bz * 9.0 * SWORD_SCALE, Blade, Finger, SWORD_SCALE))
	# 방패(0.75배): 왼 아래팔 바깥(손등 쪽)에 붙는다. 방패 높이(모델 Z) = 엄지 쪽, 폭(모델 Y) = 손 → 팔꿈치, 앞(장식 면, 모델 +X) = 손등 쪽
	Lower, HandL = P("lowerarm_l"), P("hand_l")
	Arm = HandL - Lower
	Thumb = P("index_01_l") - P("pinky_01_l")
	Back = np.cross(P("middle_01_l") - HandL, Thumb)  # T 포즈에서 위 (손등)
	Back /= np.linalg.norm(Back)
	Center = Lower + Arm * 0.62 + Back * 10.0
	Shield = Socket("Shield", "lowerarm_l", Center, Thumb, -Arm, SHIELD_SCALE)
	return Sockets + [Shield]


def WriteMannequinMeta():
	Meta = {"Version": 1,
			"Clips": [{"Clip": Clip, "Notifies": [{"Name": N, "Time": T} for N, T in Items]} for Clip, Items in NOTIFIES.items()],
			"Sockets": MannequinSockets()}
	WriteJson(os.path.join(CONTENT, MANNEQUIN + ".emeta"), Meta)


# ---- 애니메이션 그래프 / 비헤이비어 트리 -------------------------------------------------------------------------------------
def KnightClip(Name):
	return f"{KNIGHT}:{Name}"


def WriteAnimGraphs():
	Folder = os.path.join(CONTENT, ANIM)
	# 경비병: 1D 블렌드 (Speed cm/s) — 방패 든 대기 / 걷기 / 뛰기 (KayKit 이동 클립 리타기팅)
	WriteJson(os.path.join(Folder, "Guard.eanimgraph"), {
		"Editor": {"PreviewModel": MANNEQUIN}, "EntryState": "Locomotion",
		"Parameters": [{"Name": "Speed", "Type": "Float", "Default": 0.0}],
		"States": [{"Name": "Locomotion", "BlendParameter": "Speed", "Samples": [
			{"Clip": "Idle_Shield_Loop", "Position": 0.0},
			{"Clip": KnightClip("Walking_A"), "Position": 150.0, "Rate": 1.25},
			{"Clip": KnightClip("Running_A"), "Position": 420.0, "Rate": 1.1}]}],
		"Transitions": [], "Version": 3})
	# 방향 이동 훈련: 2D 블렌드 (MoveX = 오른쪽, MoveY = 앞, cm/s — 몸 기준)
	WriteJson(os.path.join(Folder, "StrafeDrill.eanimgraph"), {
		"Editor": {"PreviewModel": MANNEQUIN}, "EntryState": "Move",
		"Parameters": [{"Name": "MoveX", "Type": "Float", "Default": 0.0}, {"Name": "MoveY", "Type": "Float", "Default": 0.0}],
		"States": [{"Name": "Move", "BlendParameter": "MoveX", "BlendParameterY": "MoveY", "Samples": [
			{"Clip": "Idle_Shield_Loop", "Position": [0.0, 0.0]},
			{"Clip": KnightClip("Walking_A"), "Position": [0.0, 150.0], "Rate": 1.25},
			{"Clip": KnightClip("Running_A"), "Position": [0.0, 420.0], "Rate": 1.1},
			{"Clip": KnightClip("Walking_Backwards"), "Position": [0.0, -150.0], "Rate": 1.25},
			{"Clip": KnightClip("Running_Strafe_Left"), "Position": [-320.0, 0.0]},
			{"Clip": KnightClip("Running_Strafe_Right"), "Position": [320.0, 0.0]}]}],
		"Transitions": [], "Version": 3})


def WriteBehaviorTree():
	# 경비병: 플레이어를 보면(Lua 서비스가 Target 설정) 돌아서 따라가고, 아니면 자기 순찰 지점(Lua 태스크가 경비병 스크립트에서 받음)을 돈다
	def Node(Type, Id, Params=None, **Extra):
		N = {"Type": Type, "Id": Id}
		if Params:
			N["Params"] = [{"Name": K, "Value": V} for K, V in Params.items()]
		N.update(Extra)
		return N
	Tree = {
		"Version": 1,
		"Blackboard": [{"Name": "Target", "Type": "Entity"}, {"Name": "PatrolPoint", "Type": "Vector"}],
		"Root": Node("Selector", 1, Services=[Node("LuaService", 2, {"Script": f"{SCRIPTS}/GuardSenses.lua", "Properties": ""})], Children=[
			Node("Sequence", 3, Decorators=[Node("Blackboard", 4, {"Key": "Target", "Operation": "IsSet", "AbortMode": "Both"})], Children=[
				Node("RotateTo", 5, {"TargetKey": "Target", "ToleranceDegrees": 10.0}),
				Node("MoveTo", 6, {"TargetKey": "Target", "AcceptanceRadius": 230.0, "RepathDistance": 60.0}),
				Node("Wait", 7, {"WaitTime": 0.4}),
			]),
			Node("Sequence", 8, Children=[
				Node("LuaTask", 9, {"Script": f"{SCRIPTS}/GuardNextWaypoint.lua", "Properties": ""}),
				Node("MoveTo", 10, {"TargetKey": "PatrolPoint", "AcceptanceRadius": 30.0}),
				Node("Wait", 11, {"WaitTime": 2.0, "RandomDeviation": 0.8}),
			]),
		]),
	}
	WriteJson(os.path.join(CONTENT, "AI", "Demo", "TrainingGuard.ebt"), Tree)


# ---- 씬 -------------------------------------------------------------------------------------------------------------
def BuildScene(Overview=None):
	S = FScene()

	def Box(Name, Center, Size, Material, Collide=True, Parent=-1):
		Comps = {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": f"{MAT}/{Material}.emat"}}
		if Collide:
			Comps["BoxColliderComponent"] = {}
		return S.Add(Name, Comps, Center, None, (Size[0] / 100.0, Size[1] / 100.0, Size[2] / 100.0), Parent)

	def Collider(Name, Lo, Hi, Parent=-1):
		Center = [(A + B) * 0.5 for A, B in zip(Lo, Hi)]
		Half = [(B - A) * 0.5 for A, B in zip(Lo, Hi)]
		return S.Add(Name, {"BoxColliderComponent": {"HalfExtents": Half, "Offset": [0.0, 0.0, 0.0]}}, Center, None, (1, 1, 1), Parent)

	def Prop(Name, Id, X, Y, Z=0.0, Yaw=0.0, Scale=1.0, Half=None, Offset=(0, 0, 0), Parent=-1):
		Extra = {"BoxColliderComponent": {"HalfExtents": list(Half), "Offset": list(Offset)}} if Half else None
		return S.Model(Name, Model(Id), (X, Y, Z), Yaw, Scale, Parent, Extra)

	# ---- 환경: 늦은 오후 해 + 대기 + 구름 + 옅은 안개 (Hub와 같은 하늘)
	# 해 = 북동쪽 바다 위 (빛이 남서로) — 동서 성벽이 남북으로 놓여 안뜰에 긴 그림자가 지지 않는다
	S.Add("Sun", {"DirectionalLightComponent": {"Color": [1.0, 0.95, 0.88], "Intensity": 3.4}}, (0, 0, 2000), QuatFromEuler(Pitch=-48, Yaw=200))
	S.Add("Sky", {
		"SkyAtmosphereComponent": {},
		"VolumetricCloudComponent": {"Coverage": 0.35, "WindSpeed": 10.0},
		"SkyLightComponent": {"Intensity": 1.0},
		"HeightFogComponent": {"Color": [0.55, 0.65, 0.78], "Density": 0.0025, "HeightFalloff": 0.06, "StartDistance": 3000.0, "MaxOpacity": 0.7},
	})
	S.Add("Sea", {"WaterBodyComponent": {
		"Size": [40000.0, 60000.0, 1000.0], "ScatterColor": [0.02, 0.10, 0.12], "Absorption": [0.40, 0.08, 0.06],
		"NormalStrength": 0.45, "WaveScale": 420.0, "WaveSpeed": 18.0, "FlowDirection": 0.0, "FlowSpeed": 0.0,
		"FoamIntensity": 0.8, "FoamDistance": 35.0, "RefractionStrength": 0.04, "ReflectionIntensity": 1.0, "Roughness": 0.07}},
		(QUAY_X + 20000.0, 0.0, SEA_Z - 500.0))
	# 내비메시 (굽기 설정: 계단 단 높이 17.4 cm, 성벽 위 통로까지 이어진다)
	S.Add("NavMesh", {"NavMeshComponent": {"NavMeshAsset": "Scenes/Demo/Training.enav", "AgentRadius": 35.0, "AgentHeight": 180.0,
		"AgentMaxClimb": 40.0, "AgentMaxSlopeDegrees": 45.0, "CellSize": 15.0, "CellHeight": 10.0}})

	# ---- 땅: 안뜰(갈퀴질한 흙) + 성문~부두 자갈길 + 부두(돌) + 성 밖 모래 자갈
	Box("Ground_Yard", ((INNER_S + QUAY_X) * 0.5, 0.0, -150.0), (QUAY_X - INNER_S, 2 * INNER_W, 300.0), "YardDirt")
	Box("Ground_Outside_S", (-4500.0, 0.0, -152.0), (6000.0, 12000.0, 300.0), "YardGravel")
	for Side in (-1, 1):
		Box(f"Ground_Outside_{'W' if Side < 0 else 'E'}", ((OUTER_S + QUAY_X) * 0.5, Side * (HALF_W - WALL_T + 3000.0), -152.0),
			(QUAY_X - OUTER_S, 6000.0, 300.0), "YardGravel")
	Box("Ground_Path", ((INNER_S + QUAY_X) * 0.5, 0.0, 0.6), (QUAY_X - INNER_S, 380.0, 1.2), "YardCobble", Collide=False)
	Box("Ground_Gate", (OUTER_S + WALL_T * 0.5, 0.0, 0.6), (WALL_T + 10.0, 343.0, 1.2), "YardCobble", Collide=False)
	Box("Ground_QuayApron", ((NORTH_END + QUAY_X) * 0.5 + 60.0, 0.0, 0.8), (QUAY_X - NORTH_END - 120.0, 2 * HALF_W + 4000.0, 1.6), "YardCobble", Collide=False)
	# 부두 벽 (바다 쪽 수직면 — 땅 상자의 옆면은 흙이라 돌 벽을 덧댄다)
	Box("Quay_Wall", (QUAY_X - 30.0, 0.0, (SEA_Z - 400.0) * 0.5 + 2.0), (60.0, 2 * HALF_W + 12000.0, -(SEA_Z - 400.0) + 4.0), "QuayStone")

	# ---- 요새 (조립 glTF 하나) + 충돌 상자 (키트 기하 실측)
	S.Model("Fort", KIT_OUT, (0.0, 0.0, 0.0))
	Parapet = 97.0
	Collider("Col_Wall_S_W", (OUTER_S, -HALF_W, -10), (INNER_S, -171.5, WALK_Z))
	Collider("Col_Wall_S_E", (OUTER_S, 171.5, -10), (INNER_S, HALF_W, WALK_Z))
	Collider("Col_Gate_Top", (OUTER_S, -171.5, 420.0), (INNER_S, 171.5, WALK_Z))
	Collider("Col_Wall_S_Parapet", (OUTER_S, -HALF_W, WALK_Z), (OUTER_S + Parapet, HALF_W, WALK_Z + 66.0))
	Collider("Col_Wall_W", (INNER_S, -HALF_W, -10), (NORTH_END, -INNER_W, WALK_Z))
	Collider("Col_Wall_W_Parapet", (INNER_S, -HALF_W, WALK_Z), (NORTH_END, -HALF_W + Parapet, WALK_Z + 66.0))
	Collider("Col_Wall_E", (INNER_S, INNER_W, -10), (NORTH_END, HALF_W, WALK_Z))
	Collider("Col_Wall_E_Parapet", (INNER_S, HALF_W - Parapet, WALK_Z), (NORTH_END, HALF_W, WALK_Z + 66.0))
	# 성벽 위 통로 (서쪽 남쪽 절반) + 안쪽 난간
	Collider("Col_Walkway", (INNER_S, -INNER_W, -10), (NORTH_END - SEG, -INNER_W + 256.0, WALK_Z))
	Collider("Col_Walkway_Rail", (INNER_S, -INNER_W + 264.0, -10), (NORTH_END - SEG, -INNER_W + 307.0, WALK_Z + 45.0))
	# 계단: 디딤판마다 상자 (발 IK가 실제 단을 밟는다) + 위 층계참
	StairY0, StairY1 = -INNER_W, -INNER_W + 256.0  # 디딤판 Y 범위 (키트 x 0.49~3.05)
	for I, (Ky, Z0, Z1) in enumerate(STAIR_TREADS):
		Top = Ky * 100.0 - SINK
		if Top >= WALK_Z - 1.0:
			break
		Collider(f"Col_Step_{I:02d}", (NORTH_END - Z1 * 100.0 - 2.0, StairY0, -10), (NORTH_END - Z0 * 100.0, StairY1, Top))
	Collider("Col_Stair_Landing", (NORTH_END - SEG, StairY0, -10), (NORTH_END - 1062.0, StairY1, WALK_Z))
	Collider("Col_Stair_Rail", (NORTH_END - SEG, -INNER_W + 249.0, -10), (NORTH_END - 1089.0, -INNER_W + 303.0, WALK_Z + 45.0))

	# ---- 소품 (모두 Poly Haven 모델 — 상자 기본 도형 소품 없음)
	def Tilted(Name, Id, X, Y, Z, Pitch=0.0, Yaw=0.0, Roll=0.0, Scale=1.0):
		return S.Add(Name, {"ModelComponent": {"AssetPath": Model(Id)}}, (X, Y, Z), QuatFromEuler(Pitch=Pitch, Yaw=Yaw, Roll=Roll), (Scale, Scale, Scale))

	# 대련장 (서쪽, 계단 앞): 가운데 (350, -820). 무기 탁자(무기를 눕혀 둠) + 벤치 + 물통 + 기대 놓은 방패
	RingX, RingY = 350.0, -820.0
	TableX, TableY = RingX - 420.0, RingY + 400.0
	Prop("Ring_WeaponTable", "wooden_picnic_table", TableX, TableY, 0.0, 0.0, 1.0, (150, 112, 38), (0, 0, 38))
	Tilted("Ring_Table_Mace", "ornate_medieval_mace", TableX - 60.0, TableY - 8.0, 80.0, Pitch=90.0, Yaw=10.0)
	Tilted("Ring_Table_Hammer", "ornate_war_hammer", TableX + 10.0, TableY + 14.0, 78.0, Pitch=90.0, Yaw=-15.0)
	Tilted("Ring_Table_Estoc", "antique_estoc", TableX + 50.0, TableY - 18.0, 82.0, Pitch=90.0, Yaw=4.0, Scale=0.85)
	Prop("Ring_KatanaStand", "katana_stand_01", TableX - 20.0, TableY + 30.0, 75.0, 0.0, 1.8)
	Tilted("Ring_Shield_Lean_A", "kite_shield", RingX + 330.0, RingY - 420.0, 52.0, Roll=-14.0, Yaw=90.0, Scale=0.75)
	Tilted("Ring_Shield_Lean_B", "kite_shield", RingX + 420.0, RingY - 400.0, 52.0, Roll=-12.0, Yaw=110.0, Scale=0.75)
	Prop("Ring_Barrel_A", "barrel_03", RingX + 380.0, RingY - 470.0, 0.0, 10.0, 1.0, (32, 32, 46), (0, 0, 46))
	Prop("Ring_Bench", "painted_wooden_bench", RingX + 60.0, RingY + 400.0, 0.0, 90.0, 1.0, (24, 58, 22), (0, 0, 22))
	Prop("Ring_Bucket", "wooden_bucket_01", RingX + 220.0, RingY + 390.0, 0.0, 70.0, 1.0)
	Prop("Ring_Stool", "folding_wooden_stool", RingX - 200.0, RingY + 420.0, 0.0, 20.0, 1.0)
	# 성벽 위 통로 아래 아치(서쪽 벽 남쪽 절반)에 쌓아 둔 통·상자
	for Index, X in enumerate([-1000.0, -420.0]):
		Prop(f"Arch_Barrel_{Index}", "wine_barrel_01", X - 60.0, -INNER_W + 300.0, 0.0, Index * 50.0, 1.0, (38, 37, 44), (0, 0, 44))
		Prop(f"Arch_Crate_{Index}", "wooden_crate_02", X + 60.0, -INNER_W + 300.0, 0.0, 90.0 + Index * 7.0, 1.0, (58, 26, 23), (0, 0, 23))
		Prop(f"Arch_CrateTop_{Index}", "wooden_crate_01", X + 60.0, -INNER_W + 300.0, 45.0, 80.0 + Index * 20.0, 1.0)
	# 교관 옆 훈련 일정 칠판
	Prop("Drill_Board", "standing_chalkboard_01", -820.0, -420.0, 0.0, -20.0, 1.0, (38, 45, 75), (0, 0, 75))
	# 식사 자리 (북동): 탁자 + 의자 + 주전자·잔 + 화로
	MessX, MessY = 1350.0, 980.0
	Prop("Mess_Table", "wooden_picnic_table", MessX, MessY, 0.0, 90.0, 1.0, (150, 112, 38), (0, 0, 38))
	Prop("Mess_Jug", "jug_01", MessX + 20.0, MessY - 30.0, 75.0, 30.0, 1.0)
	Prop("Mess_Goblets", "brass_goblets", MessX - 30.0, MessY + 40.0, 75.0, 0.0, 1.0)
	Prop("Mess_Lantern", "wooden_lantern_01", MessX, MessY + 110.0, 75.0, 0.0, 1.0)
	Prop("Mess_FirePit", "stone_fire_pit", MessX + 330.0, MessY - 330.0, -12.0, 0.0, 0.9)
	S.Add("Mess_FireLight", {"PointLightComponent": {"Color": [1.0, 0.55, 0.25], "Intensity": 6.0, "Radius": 600.0, "CastShadows": False}},
		(MessX + 330.0, MessY - 330.0, 70.0))
	# 보급 더미 (동쪽 성벽 북쪽 끝)
	Prop("Supply_Barrels", "wooden_barrels_01", 1500.0, 1380.0, 0.0, 200.0, 1.0, (160, 200, 46), (25, -10, 46))
	Prop("Supply_Crate_A", "wooden_crate_02", 1100.0, 1520.0, 0.0, 90.0, 1.0, (58, 26, 23), (0, 0, 23))
	Prop("Supply_Crate_B", "wooden_crate_01", 980.0, 1530.0, 0.0, 15.0, 1.0, (21, 41, 17), (0, 0, 17))
	Prop("Supply_Crate_C", "wooden_crate_01", 990.0, 1535.0, 35.0, 40.0, 1.0)
	Prop("Supply_WineBarrel", "wine_barrel_01", 1850.0, 1500.0, 0.0, 0.0, 1.0, (38, 37, 44), (0, 0, 44))
	Prop("Supply_Ladder", "wooden_ladder", 300.0, INNER_W - 30.0, 0.0, 0.0, 1.6)
	# 무기 벽 (동쪽 성벽 앞, 돌진 레인 옆): 탁자에 무기 + 벽에 기댄 방패
	Prop("Armory_Table", "wooden_picnic_table", -400.0, INNER_W - 140.0, 0.0, 0.0, 1.0, (150, 112, 38), (0, 0, 38))
	Tilted("Armory_Axe", "wooden_axe", -460.0, INNER_W - 150.0, 78.0, Pitch=90.0, Yaw=80.0, Roll=90.0)
	Tilted("Armory_Mace", "ornate_medieval_mace", -360.0, INNER_W - 160.0, 80.0, Pitch=90.0, Yaw=-170.0)
	Tilted("Armory_Estoc", "antique_estoc", -300.0, INNER_W - 120.0, 82.0, Pitch=90.0, Yaw=175.0, Scale=0.85)
	for Index, X in enumerate([-620.0, -560.0, -240.0, -180.0]):
		Tilted(f"Armory_Shield_{Index}", "kite_shield", X, INNER_W - 18.0, 55.0, Roll=14.0, Yaw=-90.0, Scale=0.75)
	# 부두: 대포 + 통 + 사다리
	Prop("Quay_Cannon", "cannon_01", 1950.0, -900.0, 0.0, 180.0, 1.0, (80, 50, 43), (-40, -15, 43))
	Prop("Quay_Barrel", "barrel_03", 1900.0, -620.0, 0.0, 0.0, 1.0, (32, 32, 46), (0, 0, 46))
	Prop("Quay_Bucket", "wooden_bucket_01", 1960.0, -560.0, 0.0, 0.0, 1.0)
	Prop("Quay_Crates", "wooden_crate_02", 1950.0, 500.0, 0.0, 10.0, 1.0, (58, 26, 23), (0, 0, 23))
	# 남쪽 성벽 안쪽: 랜턴 기둥 + 벤치
	for Side in (-1, 1):
		Prop(f"Gate_Bench_{Side + 1}", "painted_wooden_bench", INNER_S + 60.0, Side * 700.0, 0.0, 0.0, 1.0, (24, 58, 22), (0, 0, 22))
		Prop(f"Gate_Stump_{Side + 1}", "tree_stump_01", INNER_S + 90.0, Side * 300.0, -6.0, 40.0 * Side, 0.6)
		Prop(f"Gate_Lantern_{Side + 1}", "wooden_lantern_01", INNER_S + 90.0, Side * 300.0, 17.0, 0.0, 1.3)
		Prop(f"Gate_Planter_{Side + 1}", "planter_box_01", INNER_S + 60.0, Side * 1150.0, 0.0, 0.0, 1.2)
	# 덤불·돌 (성벽 발치)
	for Index, (X, Y, Yaw) in enumerate([(-1100.0, 1450.0, 30.0), (650.0, 1470.0, 120.0), (-1150.0, -1150.0, 200.0), (1650.0, -1150.0, 60.0)]):
		Prop(f"Shrub_{Index}", "shrub_02", X, Y, -20.0, Yaw, 0.5)
	for Index, (X, Y, Yaw, Sc) in enumerate([(1650.0, 1550.0, 20.0, 2.0), (-1180.0, 1540.0, 80.0, 1.6), (-200.0, -1290.0, 40.0, 1.8)]):
		Prop(f"Rock_{Index}", "rock_07", X, Y, -3.0, Yaw, Sc)

	# ---- 마네킹 공통
	def Mannequin(Name, X, Y, Z, Yaw, Clip="Idle_Shield_Loop", Graph=None, Parent=-1, Extra=None, Weapons=("Sword", "Shield"), LocalYaw=180.0, Root=True):
		# 모델은 모델 공간 -X를 본다 → 루트(앞 = +X, Yaw) 아래 Mesh(Yaw 180). Root=False면 모델 엔티티 하나 (Yaw = 앞 방향)
		Comps = {"ModelComponent": {"AssetPath": MANNEQUIN},
				 "AnimationComponent": {"BlendTime": 0.2, "Clip": Clip, "Loop": True, "Playing": True, "Speed": 1.0}}
		if Graph:
			Comps["AnimGraphComponent"] = {"Graph": f"{ANIM}/{Graph}.eanimgraph", "UseCharacterMovement": False}
		if Extra:
			Comps.update(Extra)
		if Root:
			Mesh = S.Add("Mesh", Comps, (0, 0, 0), QuatFromEuler(Yaw=LocalYaw), (1, 1, 1), Parent)
		else:
			Mesh = S.Add(Name, Comps, (X, Y, Z), QuatFromEuler(Yaw=Yaw + 180.0), (1, 1, 1), Parent)
		for Weapon in Weapons:
			Asset = Model("antique_estoc") if Weapon.startswith("Sword") else Model("kite_shield")
			# 무기는 모델 엔티티의 자식으로 두지 않는다 (모델 엔티티 아래는 모델 노드 계층) — 소켓 부착은 계층 부모를 무시한다
			S.Add(f"{Name}_{Weapon}", {"ModelComponent": {"AssetPath": Asset}, "SocketAttachmentComponent": {"Target": Mesh, "Socket": Weapon}},
				(0, 0, 0), None, (1, 1, 1), Parent)
		return Mesh

	def Character(Name, X, Y, Z, Yaw, Script, Props=None, Graph=None, Clip="Idle_Shield_Loop", RootExtra=None, MeshExtra=None, Weapons=("Sword", "Shield")):
		# 루트(스크립트 + 정적/키네마틱 캡슐) → Mesh(모델)
		Comps = {"CapsuleColliderComponent": {"Radius": 30.0, "HalfHeight": 55.0, "Offset": [0.0, 0.0, 88.0], "IsTrigger": False, "Layer": ""}}
		if Script:
			Comps["ScriptComponent"] = {"ScriptAsset": f"{SCRIPTS}/{Script}.lua", "ExecutionLocation": 0,
										"PropertyOverrides": json.dumps(Props or {}, ensure_ascii=False)}
		if RootExtra:
			Comps.update(RootExtra)
		Root = S.Add(Name, Comps, (X, Y, Z), QuatFromEuler(Yaw=Yaw))
		Mannequin(Name, 0, 0, 0, 0, Clip=Clip, Graph=Graph, Parent=Root, Extra=MeshExtra, Weapons=Weapons)
		return Root

	FootIk = {"FootIkComponent": {"Enabled": True, "FootBones": "foot_l,foot_r", "PelvisBone": "pelvis", "TraceUp": 60.0, "TraceDown": 70.0,
								  "MaxAdjust": 40.0, "InterpSpeed": 14.0, "Weight": 1.0, "AlignToGround": True, "MaxAlignAngle": 30.0, "KneeDirection": [0.0, 0.0, 0.0]}}
	LookAt = {"LookAtComponent": {"Enabled": True, "Bones": "spine_03,neck_01,Head", "ForwardAxis": [-1.0, 0.0, 0.0], "MaxAngle": 75.0, "Weight": 1.0, "BlendSpeed": 4.0}}

	# (1) 검술 대련: 서로 마주 본 두 검사 — 공격 몽타주 + 노티파이(SwordHit/SwordFinisher) → 상대가 막기/넉백
	Character("Sparring_A", RingX - 165.0, RingY, 0.0, 0.0, "SparringFighter", {"Partner": "Sparring_B", "Leader": True})
	Character("Sparring_B", RingX + 165.0, RingY, 0.0, 180.0, "SparringFighter", {"Partner": "Sparring_A", "Leader": False})

	# (2) 교관과 훈련병: 교관 훅(PunchHit 노티파이) → 훈련병 사망 → 래그돌 → 제자리 리스폰 + 일어나기(LayToIdle)
	DrillX, DrillY = -550.0, -700.0
	Character("Instructor", DrillX, DrillY - 95.0, 0.0, 90.0, "Instructor", {"Recruit": "Recruit"}, Clip="Idle_FoldArms_Loop", Weapons=())
	Recruit = S.Add("Recruit", {
		"ScriptComponent": {"ScriptAsset": f"{SCRIPTS}/Recruit.lua", "ExecutionLocation": 0, "PropertyOverrides": ""},
		"HealthComponent": {"MaxHealth": 100.0, "Health": 100.0, "Invulnerable": False, "DeathAction": 2, "ScoreValue": 0},
		"ModelComponent": {"AssetPath": MANNEQUIN},
		"AnimationComponent": {"BlendTime": 0.2, "Clip": "Idle_Shield_Loop", "Loop": True, "Playing": True, "Speed": 1.0},
		"RagdollComponent": {"EnableOnDeath": True, "Mass": 70.0, "RadiusScale": 0.18, "MinRadius": 5.0, "MinBoneLength": 6.0,
							 "SwingLimit": 50.0, "TwistLimit": 30.0, "Friction": 0.8, "ExcludeBones": "index,middle,pinky,ring,thumb,ball_leaf"}},
		(DrillX, DrillY + 20.0, 0.0), QuatFromEuler(Yaw=-90.0 + 180.0))
	Prop("Drill_Mat_Crate", "wooden_crate_02", DrillX - 260.0, DrillY - 40.0, 0.0, 80.0, 1.0, (58, 26, 23), (0, 0, 23))
	Prop("Drill_Bucket", "wooden_bucket_01", DrillX - 250.0, DrillY + 120.0, 0.0, 0.0, 1.0)

	# (3) 경비병 2명: 비헤이비어 트리 순찰 (A = 안뜰 한 바퀴, B = 계단으로 성벽 위까지), 플레이어를 보면 따라오며 쳐다본다
	Guard = {"BehaviorTreeComponent": {"Asset": "AI/Demo/TrainingGuard.ebt", "AutoStart": True},
			 "NavAgentComponent": {"MaxSpeed": 150.0, "AcceptanceRadius": 25.0, "TurnSpeedDegrees": 300.0, "OrientToMovement": True},
			 "RigidBodyComponent": {"MotionType": 1, "Mass": 0.0, "Density": 300.0, "Friction": 0.5, "Restitution": 0.0,
									"LinearDamping": 0.05, "AngularDamping": 0.05, "UseGravity": False}}
	GuardMesh = dict(FootIk)
	GuardMesh.update(LookAt)
	Character("Guard_Yard", -1000.0, 120.0, 0.0, 0.0, "TrainingGuard",
			  {"Waypoints": "-1000,0,0; 1300,100,0; 1880,800,0; 1880,-500,0; 1200,-250,0"}, Graph="Guard",
			  RootExtra=Guard, MeshExtra=GuardMesh)
	Character("Guard_Wall", 1800.0, -1200.0, 0.0, 180.0, "TrainingGuard",
			  {"Waypoints": "1850,-1540,0; 300,-1540,668; -1100,-1480,668; 300,-1540,668; 1850,-1540,0; 1800,-1000,0"}, Graph="Guard",
			  RootExtra=Guard, MeshExtra=GuardMesh)

	# (4) 발 IK: 돌계단 중간에서 안뜰을 내려다보는 파수꾼 (왼발 = 아래 단, 오른발 = 위 단 — 골반이 내려가고 발이 단을 딛는다)
	TreadLo, TreadHi = STAIR_TREADS[10], STAIR_TREADS[11]
	Boundary = NORTH_END - (TreadLo[2] + TreadHi[1]) * 50.0
	LookoutMesh = dict(FootIk)
	LookoutMesh.update(LookAt)
	Character("Lookout", Boundary, -INNER_W + 178.0, TreadHi[0] * 100.0 - SINK, 90.0, "Lookout", None,
			  Clip="Idle_FoldArms_Loop", MeshExtra=LookoutMesh, Weapons=())

	# (5) 2D 블렌드 스페이스: 훈련 기둥(나무 그루터기 + 도끼)을 보며 원을 도는 방향 이동 훈련
	PostX, PostY = 750.0, 850.0
	Prop("Drill_Post", "tree_stump_01", PostX, PostY, -8.0, 0.0, 0.75, (50, 50, 30), (0, 0, 15))
	Tilted("Drill_Post_Axe", "wooden_axe", PostX + 8.0, PostY, 30.0, Pitch=-20.0, Yaw=30.0)
	Character("Strafer", PostX, PostY - 280.0, 0.0, 90.0, "StrafeDrill", {"CenterX": PostX, "CenterY": PostY, "Radius": 280.0},
			  Graph="StrafeDrill", RootExtra={"RigidBodyComponent": Guard["RigidBodyComponent"]})

	# (6) 루트 모션: 돌진 레인 (Sword_Dash_RM / Shield_Dash_RM — 클립의 루트 이동이 엔티티를 옮긴다, 끝에서 돌아서 다시)
	LaneY = 300.0 + 900.0
	Mannequin("Dasher", -700.0, LaneY, 0.0, 0.0, Clip="Idle_Shield_Loop", Root=False,
			  Extra={"AnimationComponent": {"BlendTime": 0.2, "Clip": "Idle_Shield_Loop", "Loop": True, "Playing": True, "Speed": 1.0,
											"RootMotionMode": 1, "RootMotionBone": "", "RootMotionRotation": False},
					 "ScriptComponent": {"ScriptAsset": f"{SCRIPTS}/Dasher.lua", "ExecutionLocation": 0,
										 "PropertyOverrides": json.dumps({"MinX": -800.0, "MaxX": 300.0}, ensure_ascii=False)}})
	for Index, X in enumerate([-950.0, 520.0]):
		Prop(f"Lane_Barrel_{Index}", "barrel_03", X, LaneY + 120.0, 0.0, Index * 40.0, 1.0, (32, 32, 46), (0, 0, 46))
		Prop(f"Lane_BarrelB_{Index}", "barrel_03", X, LaneY - 120.0, 0.0, Index * 70.0 + 20.0, 1.0, (32, 32, 46), (0, 0, 46))

	# ---- 돌아가는 포털: 남쪽 성문 (DemoPortal.lua — Hub로, 돌아가기 자리 기억 안 함)
	Portal = S.Add("Portal_Hub", {"ScriptComponent": {"ScriptAsset": "Scripts/Demo/DemoPortal.lua", "ExecutionLocation": 0,
		"PropertyOverrides": json.dumps({"TargetScene": {"Asset": "Scenes/Demo/Hub.escene"}, "Label": "Hub로", "Ready": True,
			"RememberReturn": False}, ensure_ascii=False)}},
		(OUTER_S + 40.0, 0.0, 0.0), QuatFromEuler(Yaw=0.0))
	S.Model("Portal_Hub_Door", Model("large_castle_door"), (0, 0, -3), 0.0, 1.0, Parent=Portal)
	S.Add("Portal_Hub_Glow", {"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Demo/PortalGlow.emat"}},
		(18, 0, 148), None, (0.02, 1.85, 2.85), Parent=Portal)
	S.Add("Portal_Hub_Light", {"PointLightComponent": {"Color": [0.5, 0.75, 1.0], "Intensity": 1.5, "Radius": 350.0, "CastShadows": False}}, (90, 0, 220), Parent=Portal)

	if Overview:
		S.Add("OverviewCamera", {"CameraComponent": {"FovYDegrees": 70.0, "NearZ": 5.0, "FarZ": 50000.0, "Primary": True, "Priority": 100}},
			Overview[:3], QuatFromEuler(Pitch=Overview[3], Yaw=Overview[4]))
	else:
		# 플레이어: 성문 안쪽에서 북쪽(부두) 안뜰을 보며 시작 — 프리팹 인스턴스는 루트만 저장 (PrefabLink.Root = 자기 인덱스)
		PlayerIndex = len(S.Entities)
		S.Add("Player", {
			"PrefabInstanceComponent": {"Asset": "Prefabs/Demo/DemoPlayer.eprefab", "Overrides": ""},
			"PrefabLinkComponent": {"Id": "1", "Root": PlayerIndex}},
			(INNER_S + 380.0, -260.0, 110.0))
	return S


OVERVIEW_VIEW = (-1150.0, -1400.0, 950.0, -28.0, 40.0)  # 확인용 카메라 X, Y, Z, Pitch, Yaw (--overview=x,y,z,pitch,yaw로 바꿈)


def Main():
	View = None
	for Arg in sys.argv:
		if Arg == "--overview":
			View = OVERVIEW_VIEW
		elif Arg.startswith("--overview="):
			View = tuple(float(V) for V in Arg.split("=", 1)[1].split(","))
	Parts = BuildFort()
	WriteMaterials()
	WriteMannequinMeta()
	WriteAnimGraphs()
	WriteBehaviorTree()
	Scene = BuildScene()
	Scene.Save(os.path.join(CONTENT, "Scenes", "Demo", "Training.escene"))
	print(f"Training 생성: 엔티티 {len(Scene.Entities)}개, 요새 조각 {Parts}개")
	if View:
		Overview = BuildScene(Overview=View)
		Overview.Save(os.path.join(CONTENT, "Scenes", "Demo", "_TrainingOverview.escene"))
		print("확인용 변형: Scenes/Demo/_TrainingOverview.escene (커밋하지 않음)")


if __name__ == "__main__":
	Main()
