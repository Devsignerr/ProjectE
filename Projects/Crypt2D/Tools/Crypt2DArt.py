# Crypt2D 자체 제작 픽셀 아트 + 효과음 (외부 에셋 아님 — 이 스크립트가 절차적으로 그리고 합성한다. 라이선스 제약 없음, 커밋 대상).
#   무기(검/도끼/창/석궁/지팡이 — 손잡이가 피벗, +X를 향함), 코인 회전 4프레임, 하트, 상자(닫힘/열림), 휘두르기 호 3프레임, 불꽃 튐,
#   먼지, 석궁 화살, 보스 탄, 포털 4프레임, 횃불 빛무리, 조준점, 흰 점.
#   교회/묘지 팩(GothicVania)과 같은 도트 크기(1px = 4cm)로 그린다. UI 아이콘은 4배 최근접 확대본 (UI 샘플러가 선형이라 흐려지지 않게).
import math
import os
import struct
import wave

from PIL import Image, ImageDraw

# 팔레트 (sRGB)
MetalL, MetalM, MetalD = (226, 232, 240), (164, 174, 190), (92, 100, 118)
GoldL, GoldM, GoldD = (255, 222, 104), (226, 168, 46), (138, 88, 24)
WoodL, WoodM, WoodD = (168, 108, 58), (118, 72, 34), (72, 42, 20)
Wrap = (96, 52, 44)
Outline = (24, 16, 28)


def NewImage(W, H):
	return Image.new("RGBA", (W, H), (0, 0, 0, 0))


def Px(Img, X, Y, Color, A=255):
	if 0 <= X < Img.width and 0 <= Y < Img.height:
		Img.putpixel((X, Y), (Color[0], Color[1], Color[2], A))


def AddOutline(Img):
	# 불투명 픽셀 둘레 1px 외곽선 (팩 캐릭터와 같은 어두운 테두리 — 밝은 배경에서도 읽히게)
	Src = Img.copy()
	for Y in range(Img.height):
		for X in range(Img.width):
			if Src.getpixel((X, Y))[3] > 0:
				continue
			for (DX, DY) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
				NX, NY = X + DX, Y + DY
				if 0 <= NX < Img.width and 0 <= NY < Img.height and Src.getpixel((NX, NY))[3] > 128:
					Img.putpixel((X, Y), Outline + (255,))
					break
	return Img


# ---------------------------------------------------------------- 무기 (가로, 끝이 +X). 반환 (이미지, 손잡이 픽셀 (x, y))
def DrawSword():
	Img = NewImage(24, 9)
	Px(Img, 1, 4, GoldM)
	for X in range(2, 5):
		Px(Img, X, 4, Wrap)
	for Y in range(1, 8):
		Px(Img, 5, Y, GoldL if Y < 4 else GoldD)
	for X in range(6, 21):
		Px(Img, X, 3, MetalL)
		Px(Img, X, 4, MetalM)
		Px(Img, X, 5, MetalD)
	Px(Img, 21, 4, MetalL)
	Px(Img, 21, 3, MetalM)
	Px(Img, 22, 4, MetalM)
	return AddOutline(Img), (3, 4)


def DrawAxe():
	Img = NewImage(24, 15)
	for X in range(1, 18):
		Px(Img, X, 7, WoodL if X % 3 else WoodM)
		Px(Img, X, 8, WoodD)
	for X in range(2, 5):
		Px(Img, X, 7, Wrap)
		Px(Img, X, 8, Wrap)
	# 도끼날: 14~21열, 위아래로 퍼지는 반달
	for X in range(14, 22):
		Half = 2 + (X - 14) * 5 // 7
		for Y in range(7 - Half, 9 + Half):
			Edge = X >= 20
			Px(Img, X, Y, MetalL if Edge else (MetalM if Y < 8 else MetalD))
	return AddOutline(Img), (3, 7)


def DrawSpear():
	Img = NewImage(32, 7)
	for X in range(1, 23):
		Px(Img, X, 3, WoodL if X % 4 else WoodM)
	for X in range(3, 6):
		Px(Img, X, 3, Wrap)
	for X in range(21, 23):
		Px(Img, X, 2, GoldM)
		Px(Img, X, 4, GoldD)
	for X in range(23, 31):
		Half = max(0, (30 - X) // 3)
		for Y in range(3 - Half, 4 + Half):
			Px(Img, X, Y, MetalL if Y <= 3 else MetalD)
	return AddOutline(Img), (4, 3)


def DrawCrossbow():
	Img = NewImage(20, 13)
	for X in range(1, 14):
		Px(Img, X, 6, WoodL)
		Px(Img, X, 7, WoodD)
	for X in range(1, 4):
		Px(Img, X, 8, WoodM)
	# 활 (13열 세로, 위아래로 휨)
	for Y in range(1, 12):
		Bend = 1 if Y in (1, 2, 10, 11) else 0
		Px(Img, 14 - Bend, Y, WoodM if Y % 2 else WoodD)
	for Y in range(2, 11):
		Px(Img, 11, Y, MetalL, 200)  # 시위
	for X in range(6, 18):
		Px(Img, X, 5, MetalM)
	Px(Img, 18, 5, MetalL)
	return AddOutline(Img), (3, 7)


def DrawStaff():
	Img = NewImage(28, 9)
	for X in range(1, 20):
		Px(Img, X, 4, WoodL if X % 5 else WoodM)
	for X in range(3, 6):
		Px(Img, X, 4, Wrap)
	Core, Glow = (190, 240, 255), (90, 170, 255)
	for Y in range(9):
		for X in range(19, 28):
			D = math.hypot(X - 23, Y - 4)
			if D <= 2.0:
				Px(Img, X, Y, Core)
			elif D <= 3.6:
				Px(Img, X, Y, Glow)
	Px(Img, 19, 3, GoldM)
	Px(Img, 19, 5, GoldM)
	return AddOutline(Img), (4, 4)


# ---------------------------------------------------------------- 작은 소품
def DrawCoin(Frame):
	Img = NewImage(9, 9)
	HalfW = [3.6, 2.6, 0.9, 2.6][Frame]
	for Y in range(9):
		for X in range(9):
			NX, NY = (X - 4) / max(HalfW, 0.5), (Y - 4) / 3.6
			if NX * NX + NY * NY <= 1.0:
				Px(Img, X, Y, GoldL if (X - 4 < 0 and Y < 5) else GoldM)
	if Frame == 0:
		Px(Img, 4, 3, (255, 248, 200))
		Px(Img, 4, 4, GoldD)
		Px(Img, 4, 5, GoldD)
	return AddOutline(Img)


def DrawHeart():
	Img = NewImage(11, 10)
	Rows = ["..XX...XX..", ".XXXX.XXXX.", "XXXXXXXXXXX", "XXXXXXXXXXX", ".XXXXXXXXX.",
	        "..XXXXXXX..", "...XXXXX...", "....XXX....", ".....X.....", "..........."]
	for Y, Row in enumerate(Rows):
		for X, C in enumerate(Row):
			if C == 'X':
				Px(Img, X, Y, (255, 120, 130) if (Y < 3 and X in (2, 3, 7)) else (220, 40, 60))
	return AddOutline(Img)


def DrawChest(bOpen):
	Img = NewImage(18, 15)
	Top = 5 if not bOpen else 7
	for Y in range(Top, 14):
		for X in range(1, 17):
			Px(Img, X, Y, WoodM if (Y - Top) % 3 else WoodD)
	for X in range(1, 17):
		Px(Img, X, Top, GoldM)
	for Y in range(Top, 14):
		Px(Img, 1, Y, GoldD)
		Px(Img, 16, Y, GoldD)
	if not bOpen:
		for Y in range(1, Top):
			for X in range(2, 16):
				Px(Img, X, Y, WoodL if Y > 1 else WoodM)
		for X in range(7, 11):
			for Y in range(Top - 1, Top + 3):
				Px(Img, X, Y, GoldL)
	else:
		for Y in range(1, 5):  # 뒤로 젖힌 뚜껑
			for X in range(2, 16):
				Px(Img, X, Y, WoodD if Y < 3 else WoodM)
		for X in range(3, 15):
			Px(Img, X, Top + 1, (255, 230, 120))  # 안쪽 빛
	return AddOutline(Img)


def DrawSlash(Frame):
	# 휘두르기 호 (+X 방향 반달, 가산 블렌드용 흰색 → 하늘색 가장자리). 프레임: 0 가늘고 밝음 / 1 가득 / 2 사라짐
	Size = 40
	Img = NewImage(Size, Size)
	C = (Size - 1) / 2.0
	Inner, Outer = [(13, 17), (11, 19), (14, 19)][Frame]
	Span = [55, 78, 85][Frame]
	Fade = [1.0, 0.95, 0.45][Frame]
	for Y in range(Size):
		for X in range(Size):
			DX, DY = X - C, C - Y
			R = math.hypot(DX, DY)
			A = math.degrees(math.atan2(DY, DX))
			if Inner <= R <= Outer and abs(A) <= Span:
				T = (R - Inner) / max(Outer - Inner, 1)
				Edge = 1.0 - abs(A) / Span
				Alpha = int(255 * Fade * min(1.0, Edge * 2.2) * (0.55 + 0.45 * T))
				Col = (255, 255, 255) if T > 0.55 else (170, 220, 255)
				Px(Img, X, Y, Col, Alpha)
	return Img


def DrawSpark(Frame):
	Img = NewImage(13, 13)
	Len = [3, 6, 5][Frame]
	Col = [(255, 255, 220), (255, 230, 140), (255, 170, 80)][Frame]
	for I in range(-Len, Len + 1):
		Px(Img, 6 + I, 6, Col)
		Px(Img, 6, 6 + I, Col)
	if Frame >= 1:
		for I in range(-(Len // 2), Len // 2 + 1):
			Px(Img, 6 + I, 6 + I, Col, 200)
			Px(Img, 6 + I, 6 - I, Col, 200)
	if Frame == 2:
		Px(Img, 6, 6, (0, 0, 0), 0)
	return Img


def DrawDust(Frame):
	Img = NewImage(16, 10)
	Radii = [2.0, 3.0, 3.6, 3.2][Frame]
	Alpha = [230, 210, 150, 80][Frame]
	for (CX, CY) in ((5, 6), (10, 6), (7.5, 4)):
		for Y in range(10):
			for X in range(16):
				if math.hypot(X - CX, Y - CY) <= Radii:
					Px(Img, X, Y, (200, 190, 210) if Y < CY else (150, 140, 160), Alpha)
	return Img


def DrawBolt():
	Img = NewImage(13, 5)
	for X in range(1, 10):
		Px(Img, X, 2, WoodL)
	for X in range(9, 12):
		Px(Img, X, 2, MetalL)
	Px(Img, 10, 1, MetalM)
	Px(Img, 10, 3, MetalD)
	Px(Img, 1, 1, (230, 230, 230))
	Px(Img, 1, 3, (230, 230, 230))
	return Img


def DrawOrb(Frame):
	Img = NewImage(11, 11)
	Core, Ring = ((255, 220, 255), (200, 80, 255)) if Frame == 0 else ((255, 255, 255), (170, 60, 240))
	for Y in range(11):
		for X in range(11):
			D = math.hypot(X - 5, Y - 5)
			if D <= 2.2:
				Px(Img, X, Y, Core)
			elif D <= 4.2:
				Px(Img, X, Y, Ring, 230 if Frame == 0 else 200)
			elif D <= 5.2:
				Px(Img, X, Y, Ring, 90)
	return Img


def DrawPortal(Frame):
	Img = NewImage(26, 36)
	for Y in range(36):
		for X in range(26):
			NX, NY = (X - 12.5) / 11.5, (Y - 17.5) / 16.5
			D = math.sqrt(NX * NX + NY * NY)
			if D > 1.0:
				continue
			Ang = math.atan2(NY, NX) + Frame * (math.pi / 8)
			Swirl = 0.5 + 0.5 * math.sin(Ang * 3 + D * 9)
			if D > 0.82:
				Px(Img, X, Y, (120, 60, 200) if Swirl > 0.5 else (70, 30, 140))
			else:
				Base = 40 + int(80 * Swirl * (1 - D))
				Px(Img, X, Y, (Base, int(Base * 0.6), min(255, Base * 2 + 40)), 235)
	return Img


def DrawGlow():
	Img = NewImage(48, 48)
	for Y in range(48):
		for X in range(48):
			D = math.hypot(X - 23.5, Y - 23.5) / 23.5
			if D < 1.0:
				A = int(255 * (1.0 - D) ** 2.2)
				Px(Img, X, Y, (255, 190, 110), A)
	return Img


def DrawCrosshair():
	Img = NewImage(15, 15)
	Col = (255, 240, 220)
	for I in range(15):
		if abs(I - 7) >= 3:
			Px(Img, I, 7, Col)
			Px(Img, 7, I, Col)
	Px(Img, 7, 7, (255, 80, 80))
	return AddOutline(Img)


def DrawPixel():
	Img = NewImage(4, 4)
	for Y in range(4):
		for X in range(4):
			Px(Img, X, Y, (255, 255, 255))
	return Img


# ---------------------------------------------------------------- 아틀라스 묶기
def BuildGeneratedAtlas():
	# 반환: (아틀라스 이미지, 슬라이스 목록 [(이름, x, y, w, h, 피벗 또는 None)], 무기 이미지 {이름: 이미지})
	Items = []
	Weapons = {}
	for Name, Fn in (("Sword", DrawSword), ("Axe", DrawAxe), ("Spear", DrawSpear), ("Crossbow", DrawCrossbow), ("Staff", DrawStaff)):
		Img, (GX, GY) = Fn()
		Weapons[Name] = Img
		Items.append((Name, Img, ((GX + 0.5) / Img.width, (Img.height - GY - 0.5) / Img.height)))
	for F in range(4):
		Items.append((f"Coin{F}", DrawCoin(F), None))
	Items.append(("Heart", DrawHeart(), None))
	Items.append(("Chest", DrawChest(False), (0.5, 0.0)))
	Items.append(("ChestOpen", DrawChest(True), (0.5, 0.0)))
	for F in range(3):
		Items.append((f"Slash{F}", DrawSlash(F), None))
		Items.append((f"Spark{F}", DrawSpark(F), None))
	for F in range(4):
		Items.append((f"Dust{F}", DrawDust(F), (0.5, 0.0)))
		Items.append((f"Portal{F}", DrawPortal(F), (0.5, 0.0)))
	Items.append(("Bolt", DrawBolt(), None))
	Items.append(("Orb0", DrawOrb(0), None))
	Items.append(("Orb1", DrawOrb(1), None))
	Items.append(("Glow", DrawGlow(), None))
	Items.append(("Crosshair", DrawCrosshair(), None))
	Items.append(("Pixel", DrawPixel(), None))

	# 선반 묶기 (1px 간격 — 점 필터라 번짐 없음, 경계 확인 쉬움)
	AtlasW = 256
	X = Y = 1
	RowH = 0
	Placed = []
	for (Name, Img, Pivot) in Items:
		if X + Img.width + 1 > AtlasW:
			X = 1
			Y += RowH + 1
			RowH = 0
		Placed.append((Name, X, Y, Img, Pivot))
		X += Img.width + 1
		RowH = max(RowH, Img.height)
	AtlasH = Y + RowH + 1
	Atlas = NewImage(AtlasW, AtlasH)
	Slices = []
	for (Name, PX, PY, Img, Pivot) in Placed:
		Atlas.alpha_composite(Img, (PX, PY))
		Slices.append((Name, PX, PY, Img.width, Img.height, Pivot))
	return Atlas, Slices, Weapons


def Upscale(Img, Factor=4, Square=None):
	Big = Img.resize((Img.width * Factor, Img.height * Factor), Image.NEAREST)
	if Square:
		Canvas = NewImage(Square, Square)
		Canvas.alpha_composite(Big, ((Square - Big.width) // 2, (Square - Big.height) // 2))
		return Canvas
	return Big


def WriteUiIcons(Directory, Weapons):
	os.makedirs(Directory, exist_ok=True)
	for Name, Img in Weapons.items():
		# 아이콘은 45도로 눕혀 정사각형 칸에 (무기 손잡이 왼쪽 아래 → 끝 오른쪽 위)
		Rot = Img.rotate(35, resample=Image.NEAREST, expand=True)
		Upscale(Rot, 4, 128).save(os.path.join(Directory, f"{Name}.png"))
	Upscale(DrawCoin(0), 4, 48).save(os.path.join(Directory, "Coin.png"))
	Upscale(DrawHeart(), 4, 48).save(os.path.join(Directory, "Heart.png"))
	Upscale(DrawCrosshair(), 4).save(os.path.join(Directory, "Crosshair.png"))


# ---------------------------------------------------------------- 효과음 (22050Hz 모노 16비트, 결정적 잡음)
Rate = 22050


class FNoise:
	def __init__(self, Seed):
		self.State = Seed & 0xFFFFFFFF or 1

	def Next(self):
		X = self.State
		X ^= (X << 13) & 0xFFFFFFFF
		X ^= X >> 17
		X ^= (X << 5) & 0xFFFFFFFF
		self.State = X
		return (X / 0xFFFFFFFF) * 2.0 - 1.0


def WriteWav(Path, Samples):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	Peak = max(1e-6, max(abs(S) for S in Samples))
	Gain = 0.8 / Peak
	with wave.open(Path, "wb") as File:
		File.setnchannels(1)
		File.setsampwidth(2)
		File.setframerate(Rate)
		File.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, S * Gain)) * 32767)) for S in Samples))


def SynthJump():
	Out = []
	for I in range(int(Rate * 0.16)):
		T = I / Rate
		F = 260 + 900 * T / 0.16
		Out.append(math.sin(2 * math.pi * F * T) * (1 - T / 0.16) ** 1.5 * 0.6)
	return Out


def SynthShoot():
	Noise = FNoise(7)
	Out = []
	Low = 0.0
	for I in range(int(Rate * 0.18)):
		T = I / Rate
		Twang = math.sin(2 * math.pi * (180 - 300 * T) * T) * math.exp(-T * 22)
		Low += (Noise.Next() - Low) * 0.25
		Out.append(Twang * 0.8 + Low * math.exp(-T * 40) * 0.6)
	return Out


def SynthFireball():
	Noise = FNoise(11)
	Out = []
	Low = 0.0
	for I in range(int(Rate * 0.35)):
		T = I / Rate
		Cut = 0.05 + 0.25 * (T / 0.35)
		Low += (Noise.Next() - Low) * Cut
		Env = min(1.0, T / 0.05) * (1 - T / 0.35)
		Out.append(Low * Env + math.sin(2 * math.pi * 90 * T) * Env * 0.3)
	return Out


def SynthExplode():
	Noise = FNoise(23)
	Out = []
	Low = 0.0
	for I in range(int(Rate * 0.6)):
		T = I / Rate
		Low += (Noise.Next() - Low) * 0.12
		Out.append(Low * math.exp(-T * 6) + math.sin(2 * math.pi * (70 - 40 * T) * T) * math.exp(-T * 8) * 0.8)
	return Out


def SynthPortal():
	Out = []
	for I in range(int(Rate * 0.9)):
		T = I / Rate
		Env = math.sin(math.pi * T / 0.9)
		F = 300 + 250 * math.sin(2 * math.pi * 3 * T)
		Out.append((math.sin(2 * math.pi * F * T) + 0.5 * math.sin(2 * math.pi * F * 1.5 * T)) * Env * 0.5)
	return Out


def SynthRoar():
	Noise = FNoise(31)
	Out = []
	Low = 0.0
	for I in range(int(Rate * 1.1)):
		T = I / Rate
		Low += (Noise.Next() - Low) * 0.08
		Env = min(1.0, T / 0.15) * max(0.0, 1 - T / 1.1)
		Growl = math.sin(2 * math.pi * (110 + 20 * math.sin(2 * math.pi * 7 * T)) * T)
		Out.append((Growl * 0.7 + Low * 0.8) * Env)
	return Out


def SynthDoor():
	Noise = FNoise(41)
	Out = []
	Low = 0.0
	for I in range(int(Rate * 0.45)):
		T = I / Rate
		Low += (Noise.Next() - Low) * 0.06
		Grind = 0.6 + 0.4 * math.sin(2 * math.pi * 18 * T)
		Out.append(Low * Grind * min(1.0, T / 0.03) * (1 - T / 0.45) + math.sin(2 * math.pi * 55 * T) * math.exp(-T * 9) * 0.5)
	return Out


def WriteSynthSounds(Directory):
	for Name, Fn in (("Jump", SynthJump), ("Shoot", SynthShoot), ("Fireball", SynthFireball), ("Explode", SynthExplode),
	                 ("Portal", SynthPortal), ("BossRoar", SynthRoar), ("Door", SynthDoor)):
		WriteWav(os.path.join(Directory, f"{Name}.wav"), Fn())
