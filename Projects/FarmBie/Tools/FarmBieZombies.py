# FarmBie 좀비·전투 효과 도트 아트 (CC0, 절차 생성). BuildFarmBie.py가 부른다.
#   좀비: 기본 4종(Walker 걷는 좀비 / Runner 달리는 좀비 / Brute 거구 / Bomber 자폭) + 계절 변종 4종(봄 Mossling / 여름 Scorcher / 가을 Harvester / 겨울 Frostbite)
#   방향 2(Side 오른쪽 — 왼쪽은 좌우 반전, Down 정면 — 위로 갈 때도 정면) × 동작(Walk 4, Attack 2, Die 3). 플립북 이름 = Zombie_<종류>_<동작><방향>
#   효과(Fx.esprite에 더함): Arrow·Bolt·Pellet(투사체 — 오른쪽(+X)이 앞), Hit0~2(맞음 불꽃), Boom0~4(폭발) → Fx_Hit / Fx_Boom 플립북
#   규약: 1 도트 = 6cm(농부와 같음), 피벗 = 발
import os

from HD2DArt import FAtlas, FCanvas, WriteFlipbook  # noqa: E402 — BuildFarmBie.py가 경로에 넣는다

OUTLINE = (24, 18, 20)

# 종류: 크기(칸), 몸 폭 배율, 팔레트, 특징
KINDS = {
	"Walker":    {"Cell": 32, "Pal": {"Skin": (128, 156, 106), "SkinD": (88, 112, 74), "Shirt": (80, 96, 132), "ShirtD": (56, 66, 96),
								   "Pants": (92, 78, 62), "PantsD": (66, 54, 44), "Eye": (250, 230, 90)}, "Trait": None},
	"Runner":    {"Cell": 32, "Pal": {"Skin": (150, 140, 120), "SkinD": (104, 94, 80), "Shirt": (150, 140, 120), "ShirtD": (110, 96, 84),
								   "Pants": (60, 52, 58), "PantsD": (40, 34, 40), "Eye": (255, 70, 60)}, "Trait": "Ribs"},
	"Brute":     {"Cell": 42, "Pal": {"Skin": (112, 132, 104), "SkinD": (76, 94, 70), "Shirt": (120, 84, 70), "ShirtD": (86, 58, 48),
								   "Pants": (70, 64, 80), "PantsD": (48, 44, 56), "Eye": (250, 200, 80)}, "Trait": "Stitches"},
	"Bomber":    {"Cell": 34, "Pal": {"Skin": (150, 170, 96), "SkinD": (106, 124, 66), "Shirt": (150, 170, 96), "ShirtD": (106, 124, 66),
								   "Pants": (90, 80, 60), "PantsD": (64, 56, 40), "Eye": (255, 240, 120)}, "Trait": "Pustules"},
	"Mossling":  {"Cell": 32, "Pal": {"Skin": (110, 150, 96), "SkinD": (70, 108, 64), "Shirt": (70, 112, 70), "ShirtD": (48, 82, 50),
								   "Pants": (84, 72, 56), "PantsD": (60, 50, 40), "Eye": (220, 255, 140)}, "Trait": "Moss"},
	"Scorcher":  {"Cell": 32, "Pal": {"Skin": (120, 90, 80), "SkinD": (80, 56, 50), "Shirt": (70, 50, 46), "ShirtD": (50, 34, 32),
								   "Pants": (50, 40, 40), "PantsD": (34, 26, 26), "Eye": (255, 170, 40)}, "Trait": "Embers"},
	"Harvester": {"Cell": 32, "Pal": {"Skin": (140, 150, 100), "SkinD": (100, 108, 70), "Shirt": (170, 110, 60), "ShirtD": (128, 80, 44),
								   "Pants": (96, 84, 70), "PantsD": (70, 60, 50), "Eye": (255, 150, 60)}, "Trait": "Straw"},
	"Frostbite": {"Cell": 42, "Pal": {"Skin": (150, 186, 206), "SkinD": (104, 136, 166), "Shirt": (90, 110, 150), "ShirtD": (64, 80, 116),
								   "Pants": (70, 80, 110), "PantsD": (50, 58, 84), "Eye": (180, 240, 255)}, "Trait": "Ice"},
}
# 보스 (Bosses.etable과 같은 이름 — 더 큰 칸 + 고유 장식)
KINDS.update({
	"Rotgiant":        {"Cell": 64, "Pal": {"Skin": (120, 140, 96), "SkinD": (84, 100, 66), "Shirt": (110, 80, 66), "ShirtD": (78, 56, 46),
									     "Pants": (70, 60, 64), "PantsD": (48, 40, 44), "Eye": (255, 220, 90)}, "Trait": "Giant"},
	"Broodmother":     {"Cell": 60, "Pal": {"Skin": (170, 150, 120), "SkinD": (124, 106, 84), "Shirt": (150, 120, 100), "ShirtD": (110, 86, 70),
									     "Pants": (90, 70, 66), "PantsD": (64, 50, 46), "Eye": (255, 120, 80)}, "Trait": "Brood"},
	"PlagueScarecrow": {"Cell": 56, "Pal": {"Skin": (150, 140, 90), "SkinD": (110, 100, 62), "Shirt": (110, 140, 70), "ShirtD": (80, 104, 50),
									     "Pants": (100, 84, 60), "PantsD": (72, 60, 44), "Eye": (200, 255, 90)}, "Trait": "PlagueStraw"},
	"BloomLich":       {"Cell": 64, "Pal": {"Skin": (180, 200, 170), "SkinD": (130, 150, 120), "Shirt": (70, 120, 80), "ShirtD": (46, 86, 56),
									     "Pants": (60, 96, 66), "PantsD": (40, 70, 46), "Eye": (255, 160, 220)}, "Trait": "Lich"},
	"SunDevourer":     {"Cell": 64, "Pal": {"Skin": (150, 70, 40), "SkinD": (100, 44, 28), "Shirt": (90, 40, 30), "ShirtD": (60, 26, 20),
									     "Pants": (70, 34, 26), "PantsD": (48, 22, 18), "Eye": (255, 240, 120)}, "Trait": "Sun"},
	"HarvestReaper":   {"Cell": 64, "Pal": {"Skin": (220, 214, 196), "SkinD": (170, 164, 150), "Shirt": (40, 32, 44), "ShirtD": (26, 20, 30),
									     "Pants": (34, 28, 38), "PantsD": (22, 18, 26), "Eye": (255, 140, 40)}, "Trait": "Reaper"},
	"FrostColossus":   {"Cell": 72, "Pal": {"Skin": (170, 210, 236), "SkinD": (120, 160, 196), "Shirt": (110, 150, 196), "ShirtD": (80, 112, 156),
									     "Pants": (90, 120, 160), "PantsD": (64, 88, 124), "Eye": (220, 250, 255)}, "Trait": "Colossus"},
})
POSES = {"Walk": 4, "Attack": 2, "Die": 3}
POSE_TIMES = {"Walk": (7.0, "Loop"), "Attack": (6.0, "Loop"), "Die": (8.0, "Once")}


def _Humanoid(C, P, Dir, Pose, Frame, Size, Trait):
	# 기본 사람 모양 (32칸 기준 좌표를 Size/32로 키움). 발 = 아래 끝
	S = Size / 32.0
	W = C.W

	def R(X0, Y0, X1, Y1, Col):
		C.Rect(int(round(W / 2 + (X0 - 16) * S)), int(round(Size - 32 * S + Y0 * S)), int(round(W / 2 + (X1 - 16) * S)), int(round(Size - 32 * S + Y1 * S)), Col)

	def Px(X, Y, Col):
		R(X, Y, X, Y, Col)
	Bob = (0, 1, 0, 1)[Frame % 4] if Pose == "Walk" else 0
	Legs = (1, 0, 2, 0)[Frame % 4] if Pose == "Walk" else 0
	Lunge = (Frame == 1) if Pose == "Attack" else False
	Wide = Trait in ("Stitches", "Ice")
	Belly = Trait == "Pustules"
	Hx = 2 if Wide else 0
	if Dir == "Down":
		# 다리
		LY, RY = (0, -1, 0)[Legs], (0, 0, -1)[Legs]
		R(12 - Hx, 25 + LY, 14 - Hx, 30 + LY, P["Pants"])
		R(17 + Hx, 25 + RY, 19 + Hx, 30 + RY, P["PantsD"])
		R(11 - Hx, 31 + LY, 14 - Hx, 31 + LY, P["PantsD"])
		R(17 + Hx, 31 + RY, 20 + Hx, 31 + RY, P["PantsD"])
		# 몸
		Y = 15 + Bob
		R(10 - Hx, Y, 21 + Hx, Y + 10, P["Shirt"])
		R(10 - Hx, Y + 7, 21 + Hx, Y + 10, P["ShirtD"])
		if Belly:
			R(9, Y + 2, 22, Y + 10, P["Skin"])
			R(10, Y + 3, 21, Y + 9, P["SkinD"])
		# 찢어진 옷 자국
		Px(13, Y + 5, P["SkinD"])
		Px(18, Y + 3, P["SkinD"])
		# 팔 (앞으로 뻗음 — 카메라 쪽이라 몸 아래로 짧게)
		AY = Y + 4 + (2 if Lunge else 0)
		R(7 - Hx, AY, 9 - Hx, AY + 7, P["Skin"])
		R(22 + Hx, AY, 24 + Hx, AY + 7, P["Skin"])
		R(7 - Hx, AY + 7, 9 - Hx, AY + 8, P["SkinD"])
		R(22 + Hx, AY + 7, 24 + Hx, AY + 8, P["SkinD"])
		# 머리 (기울어짐)
		HY = 6 + Bob
		R(11, HY, 20, HY + 8, P["Skin"])
		R(11, HY + 6, 20, HY + 8, P["SkinD"])
		R(12, HY - 1, 19, HY - 1, P["Skin"])
		R(12, HY + 2, 14, HY + 4, (30, 24, 28))
		R(17, HY + 2, 19, HY + 4, (30, 24, 28))
		Px(13, HY + 3, P["Eye"])
		Px(18, HY + 3, P["Eye"])
		R(14, HY + 6, 17, HY + 6, (60, 30, 30))  # 벌린 입
		Px(15, HY + 7, (200, 190, 170))
	else:
		# 옆모습 (오른쪽을 봄): 앞으로 굽은 몸 + 앞으로 뻗은 팔
		Spread = (0, 2, -2)[Legs]
		R(14 + Spread, 25, 16 + Spread, 30, P["PantsD"])
		R(14 - Spread, 25, 16 - Spread, 30, P["Pants"])
		R(14 + Spread, 31, 18 + Spread, 31, P["PantsD"])
		R(14 - Spread, 31, 18 - Spread, 31, P["PantsD"])
		Y = 15 + Bob
		R(11 - Hx, Y, 19 + Hx, Y + 10, P["Shirt"])
		R(11 - Hx, Y + 7, 19 + Hx, Y + 10, P["ShirtD"])
		if Belly:
			R(14, Y + 2, 22, Y + 9, P["Skin"])
			R(15, Y + 3, 21, Y + 8, P["SkinD"])
		Reach = 6 if Lunge else 3
		R(17, Y + 2, 21 + Reach + Hx, Y + 4, P["Skin"])          # 앞 팔
		R(21 + Reach + Hx, Y + 2, 22 + Reach + Hx, Y + 5, P["SkinD"])
		R(15, Y + 4, 19 + Reach, Y + 6, P["SkinD"])              # 뒤 팔
		HY = 7 + Bob
		R(14, HY, 22, HY + 7, P["Skin"])
		R(14, HY + 5, 22, HY + 7, P["SkinD"])
		R(19, HY + 2, 21, HY + 3, (30, 24, 28))
		Px(20, HY + 2, P["Eye"])
		R(20, HY + 5, 22, HY + 5, (60, 30, 30))
	# 특징
	if Trait == "Ribs":
		for K in range(3):
			R(12, 17 + Bob + K * 2, 19, 17 + Bob + K * 2, (220, 210, 190))
	elif Trait == "Stitches":
		for K in range(4):
			Px(13 + K * 2, 18 + Bob, (40, 30, 30))
			Px(14 + K * 2, 19 + Bob, (40, 30, 30))
	elif Trait == "Pustules":
		for X, Y in ((12, 19), (19, 21), (15, 23), (20, 17)):
			R(X, Y + Bob, X + 1, Y + 1 + Bob, (170, 255, 90))
	elif Trait == "Moss":
		for X, Y in ((11, 16), (19, 18), (13, 6), (18, 7), (15, 22)):
			R(X, Y + Bob, X + 1, Y + Bob, (60, 140, 50))
		Px(14, 5 + Bob, (250, 210, 230))
		Px(17, 6 + Bob, (250, 250, 160))
	elif Trait == "Embers":
		for X, Y in ((12, 17), (18, 20), (15, 23), (14, 8), (19, 9)):
			Px(X, Y + Bob, (255, 140, 30))
			Px(X + 1, Y + Bob, (255, 220, 120))
	elif Trait == "Straw":
		R(9, 4 + Bob, 22, 5 + Bob, (220, 180, 90))  # 밀짚모자 챙
		R(12, 1 + Bob, 19, 4 + Bob, (230, 196, 110))
		for X in (10, 13, 18, 21):
			Px(X, 25, (220, 190, 100))
	elif Trait == "Ice":
		for X, Y in ((10, 15), (21, 16), (12, 7), (19, 7)):
			R(X, Y + Bob, X, Y + 2 + Bob, (230, 250, 255))
	elif Trait == "Giant":
		for K in range(5):
			Px(12 + K * 2, 18 + Bob, (40, 30, 30))
			Px(13 + K * 2, 19 + Bob, (40, 30, 30))
		R(9, 21 + Bob, 11, 23 + Bob, (170, 60, 60))   # 터진 상처
		R(20, 16 + Bob, 21, 17 + Bob, (170, 60, 60))
	elif Trait == "Brood":
		for X, Y in ((10, 16), (13, 15), (17, 15), (20, 16), (11, 19), (19, 19)):
			R(X, Y + Bob, X + 1, Y + 1 + Bob, (240, 236, 200))  # 알
			Px(X, Y + Bob, (255, 255, 240))
		R(12, 22 + Bob, 19, 24 + Bob, (200, 120, 110))
	elif Trait == "PlagueStraw":
		R(8, 4 + Bob, 23, 5 + Bob, (170, 150, 70))
		R(11, 0 + Bob, 20, 4 + Bob, (190, 170, 80))
		for X, Y in ((11, 18), (19, 20), (14, 22), (16, 16)):
			R(X, Y + Bob, X + 1, Y + 1 + Bob, (150, 220, 80))
		for X in (7, 24):
			R(X, 17 + Bob, X, 25 + Bob, (180, 150, 80))
	elif Trait == "Lich":
		R(11, 2 + Bob, 20, 4 + Bob, (230, 200, 90))       # 왕관
		for X in (11, 14, 17, 20):
			Px(X, 1 + Bob, (230, 200, 90))
		for X, Y, Col in ((10, 15, (250, 180, 220)), (21, 17, (255, 240, 150)), (13, 21, (250, 180, 220)), (19, 23, (255, 255, 255)), (16, 18, (255, 240, 150))):
			R(X, Y + Bob, X + 1, Y + 1 + Bob, Col)
		R(9, 25, 22, 30, P["ShirtD"])                       # 긴 옷자락
	elif Trait == "Sun":
		for X in range(10, 22, 2):
			R(X, 1 + Bob + (X % 4) // 2, X, 5 + Bob, (255, 170, 40))  # 불꽃 머리
			Px(X, 0 + Bob + (X % 4) // 2, (255, 240, 150))
		for X, Y in ((11, 17), (19, 19), (14, 22), (17, 15), (12, 24)):
			R(X, Y + Bob, X + 1, Y + Bob, (255, 150, 30))
	elif Trait == "Reaper":
		R(9, 3 + Bob, 22, 13 + Bob, P["Shirt"])            # 두건
		R(12, 7 + Bob, 19, 13 + Bob, P["Skin"])            # 해골 얼굴
		R(13, 9 + Bob, 14, 10 + Bob, (20, 10, 10))
		R(17, 9 + Bob, 18, 10 + Bob, (20, 10, 10))
		Px(13, 9 + Bob, P["Eye"])
		Px(18, 9 + Bob, P["Eye"])
		R(9, 24, 22, 31, P["ShirtD"])
		R(26, 4 + Bob, 26, 30, (110, 80, 50))             # 낫 자루
		R(20, 4 + Bob, 27, 5 + Bob, (200, 206, 216))       # 낫 날
		R(19, 6 + Bob, 21, 7 + Bob, (170, 176, 186))
	elif Trait == "Colossus":
		for X, Y in ((8, 14), (23, 15), (11, 6), (20, 6), (15, 3), (9, 22), (22, 22)):
			R(X, Y + Bob, X, Y + 3 + Bob, (235, 250, 255))
			Px(X, Y + Bob, (255, 255, 255))


def DrawZombie(Kind, Dir, Pose, Frame):
	Info = KINDS[Kind]
	Size = Info["Cell"]
	C = FCanvas(Size + 8, Size)
	if Pose == "Die":
		# 쓰러짐: 0 기울어짐, 1 무너짐, 2 바닥에 누움
		Base = FCanvas(Size + 8, Size)
		_Humanoid(Base, Info["Pal"], Dir, "Walk", 0, Size, Info["Trait"])
		Base.Outline(OUTLINE)
		Img = Base.P
		import numpy as np
		if Frame == 0:
			Shift = 2
			C.P[Shift:, :] = Img[:-Shift, :]
		else:
			Squash = 0.5 if Frame == 1 else 0.28
			H = max(2, int(Size * Squash))
			Rows = np.linspace(0, Size - 1, H).astype(int)
			C.P[Size - H:, :] = Img[Rows, :]
			if Frame == 2:
				C.P[..., 3] = (C.P[..., 3] * 0.8).astype(np.uint8)
				C.Ellipse(C.W / 2, Size - 2, Size * 0.35, 2, (90, 30, 40), 160)
		return C
	_Humanoid(C, Info["Pal"], Dir, Pose, Frame, Size, Info["Trait"])
	C.Outline(OUTLINE)
	return C


def DrawShot(Kind):
	if Kind == "Arrow":
		C = FCanvas(18, 5)
		C.Rect(2, 2, 14, 2, (150, 104, 60))
		C.Rect(14, 1, 17, 3, (200, 204, 214))
		C.Px(17, 2, (240, 244, 250))
		C.Rect(0, 1, 2, 1, (230, 230, 230))
		C.Rect(0, 3, 2, 3, (230, 230, 230))
	elif Kind == "Bolt":
		C = FCanvas(16, 5)
		C.Rect(1, 2, 12, 2, (110, 72, 40))
		C.Rect(12, 1, 15, 3, (180, 186, 196))
	elif Kind == "FireArrow":
		C = FCanvas(18, 5)
		C.Rect(2, 2, 14, 2, (150, 104, 60))
		C.Rect(13, 1, 17, 3, (255, 160, 40))
		C.Px(17, 2, (255, 240, 160))
	else:  # Pellet
		C = FCanvas(6, 6)
		C.Ellipse(3, 3, 2.5, 2.5, (230, 210, 140))
		C.Px(2, 2, (255, 250, 220))
	C.Outline((30, 20, 16))
	return C


def DrawHit(Frame):
	C = FCanvas(16, 16)
	R = 3 + Frame * 2
	for K in range(8):
		import math
		A = K / 8 * math.tau
		C.Line(8 + math.cos(A) * (R - 2), 8 + math.sin(A) * (R - 2), 8 + math.cos(A) * R, 8 + math.sin(A) * R, (255, 240 - Frame * 40, 160 - Frame * 50))
	C.Ellipse(8, 8, max(1, 3 - Frame), max(1, 3 - Frame), (255, 255, 230))
	return C


def DrawBoom(Frame):
	C = FCanvas(32, 32)
	R = 6 + Frame * 3.2
	Cols = [(255, 250, 210), (255, 210, 90), (250, 130, 40), (180, 70, 40), (90, 80, 80)]
	C.Ellipse(16, 16, R, R, Cols[min(4, Frame + 1)])
	C.Ellipse(16, 16, R * 0.7, R * 0.7, Cols[min(4, Frame)])
	if Frame < 2:
		C.Ellipse(16, 16, R * 0.35, R * 0.35, Cols[0])
	if Frame >= 3:
		import numpy as np
		C.P[..., 3] = (C.P[..., 3] * (0.7 if Frame == 3 else 0.4)).astype(np.uint8)
	return C


def WriteSprites(Folder, FxAtlas):
	for Kind in KINDS:
		Atlas = FAtlas(1024)
		for Dir in ("Side", "Down"):
			for Pose, Count in POSES.items():
				for Frame in range(Count):
					Atlas.Add(f"{Pose}{Dir}{Frame}", DrawZombie(Kind, Dir, Pose, Frame))
		Atlas.Save(Folder, f"Zombie_{Kind}")
		for Dir in ("Side", "Down"):
			for Pose, Count in POSES.items():
				Fps, Loop = POSE_TIMES[Pose]
				WriteFlipbook(Folder, f"Zombie_{Kind}_{Pose}{Dir}", f"Zombie_{Kind}.esprite", [f"{Pose}{Dir}{I}" for I in range(Count)], Fps, Loop)
	# 효과는 Fx 아틀라스에 함께 (피벗 가운데)
	for Kind in ("Arrow", "Bolt", "Pellet", "FireArrow"):
		FxAtlas.Add(Kind, DrawShot(Kind), (0.5, 0.5))
	for I in range(3):
		FxAtlas.Add(f"Hit{I}", DrawHit(I), (0.5, 0.5))
	for I in range(5):
		FxAtlas.Add(f"Boom{I}", DrawBoom(I), (0.5, 0.5))


def WriteFxFlipbooks(Folder):
	WriteFlipbook(Folder, "Fx_Hit", "Fx.esprite", [f"Hit{I}" for I in range(3)], 15.0, "Once")
	WriteFlipbook(Folder, "Fx_Boom", "Fx.esprite", [f"Boom{I}" for I in range(5)], 12.0, "Once")
