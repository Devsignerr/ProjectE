# HD-2D 데모 지도 그림 (지도 화면·미니맵 공용 — 맵 생성기가 장면을 다 만든 뒤 부른다. 결정적).
#   WriteMinimap(Content, Info, Classify, Scene, Height, Trees): 놀이 영역을 내려다본 약도 PNG(UI/Demo/HD2D/Maps/<맵 id>.png)
#     - Classify(X, Y) → 지면 종류 배열 (아래 K_* — 맵마다 지형 함수로 정한다), Height(X, Y) → 높이(언덕 음영, 생략 가능)
#     - Scene: 생성한 FScene — 루트의 막는 상자 콜라이더(Bound_*·연못 벽 제외, 가로·세로 반폭 70cm 이상)를 건물 지붕으로 그린다
#     - Trees: [(X, Y, 반지름 cm)] 나무 (폴리지 자리)
#   Info(MapId, Bounds, Title, Landmarks): 게임에 넘길 정보 (HD2DMetaGen.AddMeta → MetaUI 엔티티 속성) — 지명 = [(이름, X, Y, 종류)] 종류 Place/Exit
#   좌표: 그림 위 = -Y(화면 안쪽), 오른쪽 = +X (게임 카메라와 같은 방향). 그림 한 도트 = Cell cm, 파일은 Scale배 최근접 확대(UI 샘플러가 선형)
#   다른 지역 생성기도 같은 함수를 쓴다 — Classify만 그 맵 지형에 맞게 넘기면 된다.
import math
import os

import numpy as np
from PIL import Image

UI_DIR = "UI/Demo/HD2D"

# 지면 종류
K_GRASS, K_PATH, K_PLAZA, K_ROCK, K_WATER, K_VOID, K_FIELD, K_FLOOR, K_TILE, K_MOSS, K_SAND = range(11)

# 양피지 바탕 위 옅은 색 (sRGB) — 옥토패스풍 지도: 채도를 눌러 종이에 그린 듯
PAPER = np.array((226, 206, 160), dtype=np.float32)
PALETTE = {
	K_GRASS: (126, 156, 88), K_PATH: (196, 160, 104), K_PLAZA: (178, 170, 150), K_ROCK: (132, 116, 100), K_WATER: (92, 146, 170),
	K_VOID: (40, 36, 46), K_FIELD: (214, 180, 92), K_FLOOR: (122, 112, 104), K_TILE: (150, 148, 152), K_MOSS: (96, 132, 104),
	K_SAND: (220, 198, 146),
}
PAPER_MIX = {K_VOID: 0.0, K_WATER: 0.18}  # 종이색을 섞는 비율 (기본 0.3)
INK = (58, 40, 30)
ROOF, ROOF_L, ROOF_D = (172, 84, 62), (206, 122, 88), (112, 52, 40)
TREE, TREE_L, TREE_D = (70, 112, 62), (112, 152, 84), (40, 70, 42)


def Info(MapId, Bounds, Title, Landmarks=()):
	MinX, MinY, MaxX, MaxY = Bounds
	return {"MapId": MapId, "Image": f"{UI_DIR}/Maps/{MapId}.png", "Bounds": (MinX, MinY, MaxX, MaxY), "Title": Title,
			"Landmarks": list(Landmarks)}


def _QuatYaw(Q):
	X, Y, Z, W = Q
	return math.atan2(2.0 * (W * Z + X * Y), 1.0 - 2.0 * (Y * Y + Z * Z))


SKIP_PREFIXES = ("Bound_", "Pond_Wall", "Creek", "Stairs")  # 지도에 그리지 않는 콜라이더 (경계·물가 벽·계단)
WOOD_PREFIXES = ("Bridge",)                                  # 나무 다리 (지붕 대신 널빤지 색)
WOOD, WOOD_D = (170, 122, 74), (110, 74, 44)


def _Obstacles(Scene):
	# (가운데 X, Y, 반폭 X, Y, Yaw 라디안, 나무인가) — 루트 엔티티의 막는 상자 콜라이더 중 건물·벽 크기만
	Out = []
	for E in Scene.Entities:
		if E.get("Parent", -1) != -1:
			continue
		Box = E["Components"].get("BoxColliderComponent")
		Name = E.get("Name", "")
		if not Box or Box.get("IsTrigger") or Name.startswith(SKIP_PREFIXES):
			continue
		HX, HY = Box["HalfExtents"][0], Box["HalfExtents"][1]
		T = E["Components"]["TransformComponent"]
		Sc = T.get("Scale", [1, 1, 1])
		HX, HY = HX * abs(Sc[0]), HY * abs(Sc[1])
		if min(HX, HY) < 70.0:
			continue
		Out.append((T["Position"][0], T["Position"][1], HX, HY, _QuatYaw(T.get("Rotation", [0, 0, 0, 1])), Name.startswith(WOOD_PREFIXES)))
	return Out


def WriteMinimap(Content, MapInfo, Classify, Scene=None, Height=None, Trees=(), Cell=40.0, Scale=4):
	MinX, MinY, MaxX, MaxY = MapInfo["Bounds"]
	W, H = int(math.ceil((MaxX - MinX) / Cell)), int(math.ceil((MaxY - MinY) / Cell))
	X, Y = np.meshgrid(MinX + (np.arange(W) + 0.5) * Cell, MinY + (np.arange(H) + 0.5) * Cell)
	Kind = np.asarray(Classify(X, Y)).astype(np.int32)
	Img = np.zeros((H, W, 3), dtype=np.float32)
	for K, Col in PALETTE.items():
		Mix = PAPER_MIX.get(K, 0.3)
		Img[Kind == K] = np.array(Col, dtype=np.float32) * (1.0 - Mix) + PAPER * Mix
	# 언덕 음영 (빛 = 왼쪽 위) + 높이 단계 띠 (200cm마다 옅은 등고선)
	if Height is not None:
		Z = np.vectorize(lambda PX, PY: Height(float(PX), float(PY)))(X, Y).astype(np.float32)
		GY, GX = np.gradient(Z, Cell)
		Shade = np.clip(1.0 + (-GX * 0.55 - GY * 0.75) * 0.9, 0.72, 1.22)
		Land = (Kind != K_WATER) & (Kind != K_VOID)
		Img[Land] *= Shade[Land][:, None]
		Band = np.floor(Z / 200.0)
		Contour = np.zeros_like(Land)
		Contour[1:, :] |= Band[1:, :] != Band[:-1, :]
		Contour[:, 1:] |= Band[:, 1:] != Band[:, :-1]
		Contour &= Land & (Kind != K_PLAZA) & (Kind != K_PATH)
		Img[Contour] = Img[Contour] * 0.86
	# 물: 잔물결 점 (결정적 격자)
	Water = Kind == K_WATER
	Ripple = Water & (((np.arange(W)[None, :] * 3 + np.arange(H)[:, None] * 7) % 11) == 0)
	Img[Ripple] = Img[Ripple] * 0.6 + np.array((210, 236, 240), dtype=np.float32) * 0.4
	# 종류 경계: 물가·벼랑·빈 곳 테두리는 먹선
	Edge = np.zeros((H, W), dtype=bool)
	Strong = (Kind == K_WATER) | (Kind == K_VOID)
	for DY, DX in ((1, 0), (-1, 0), (0, 1), (0, -1)):
		Shift = np.roll(np.roll(Kind, DY, axis=0), DX, axis=1)
		ShiftStrong = np.roll(np.roll(Strong, DY, axis=0), DX, axis=1)
		Edge |= (Shift != Kind) & (Strong | ShiftStrong) & ~Strong
	Img[Edge] = Img[Edge] * 0.45 + np.array(INK, dtype=np.float32) * 0.55
	# 나무: 둥근 수관 + 밝은 위쪽 + 어두운 아래 테두리
	for TX, TY, TR in Trees:
		CX, CY, R = (TX - MinX) / Cell, (TY - MinY) / Cell, max(1.2, TR / Cell)
		X0, X1, Y0, Y1 = int(CX - R - 1), int(CX + R + 2), int(CY - R - 1), int(CY + R + 2)
		for PY in range(max(0, Y0), min(H, Y1)):
			for PX in range(max(0, X0), min(W, X1)):
				D = math.hypot(PX + 0.5 - CX, PY + 0.5 - CY)
				if D <= R:
					Img[PY, PX] = TREE_L if (PY + 0.5 - CY) < -R * 0.35 and D < R * 0.8 else TREE
				elif D <= R + 0.9 and PY + 0.5 > CY:
					Img[PY, PX] = TREE_D
	# 건물: 회전 상자 지붕 (위쪽 밝게, 테두리 진하게)
	if Scene is not None:
		for OX, OY, HX, HY, Yaw, bWood in _Obstacles(Scene):
			C, S = math.cos(Yaw), math.sin(Yaw)
			Ext = (abs(HX * C) + abs(HY * S), abs(HX * S) + abs(HY * C))
			PX0, PX1 = int((OX - Ext[0] - MinX) / Cell) - 1, int((OX + Ext[0] - MinX) / Cell) + 2
			PY0, PY1 = int((OY - Ext[1] - MinY) / Cell) - 1, int((OY + Ext[1] - MinY) / Cell) + 2
			for PY in range(max(0, PY0), min(H, PY1)):
				for PX in range(max(0, PX0), min(W, PX1)):
					WX, WY = MinX + (PX + 0.5) * Cell - OX, MinY + (PY + 0.5) * Cell - OY
					LX, LY = WX * C + WY * S, -WX * S + WY * C
					if abs(LX) <= HX and abs(LY) <= HY:
						Rim = abs(LX) > HX - Cell or abs(LY) > HY - Cell
						if bWood:
							Img[PY, PX] = WOOD_D if Rim else WOOD
						else:
							Img[PY, PX] = ROOF_D if Rim else (ROOF_L if LY < -HY * 0.25 else ROOF)
	# 가장자리: 종이 테두리처럼 어둡게
	Fade = np.minimum(np.minimum(np.arange(W)[None, :], W - 1 - np.arange(W)[None, :]), np.minimum(np.arange(H)[:, None], H - 1 - np.arange(H)[:, None]))
	Img *= np.clip(0.78 + Fade * 0.11, 0.78, 1.0)[..., None]
	Out = np.clip(Img + 0.5, 0, 255).astype(np.uint8)
	Path = os.path.join(Content, *MapInfo["Image"].split("/"))
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	Image.fromarray(Out, "RGB").resize((W * Scale, H * Scale), Image.NEAREST).save(Path, optimize=True)
	print(f"지도: {MapInfo['Image']} ({W}x{H} 도트, 한 도트 {Cell:.0f}cm)")
	return W * Scale, H * Scale
