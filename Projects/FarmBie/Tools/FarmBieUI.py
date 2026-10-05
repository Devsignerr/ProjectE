# FarmBie UI (.eui + UI 텍스처) 생성 — BuildFarmBie.py가 부른다.
#   UI 텍스처는 도트 아트를 4배 최근접 확대한 PNG(UI 샘플러가 선형), 창틀은 24 도트 9-슬라이스(가장자리 8 도트 = Margin 1/3).
#   위젯 이름은 Scripts/FarmBie/FarmHud.lua와 약속이다 — 이름을 바꾸면 둘 다 고친다.
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "Tools", "DemoMap"))
from HD2DArt import FCanvas, UpscaleSave  # noqa: E402

UI_DIR = "UI/FarmBie"

TEXT_LIGHT = (0.98, 0.95, 0.88, 1)
TEXT_GOLD  = (1.0, 0.86, 0.45, 1)
TEXT_DIM   = (0.78, 0.74, 0.68, 1)
TEXT_RED   = (1.0, 0.45, 0.4, 1)
TEXT_GREEN = (0.6, 0.95, 0.55, 1)
OUTLINE    = (0.06, 0.03, 0.02, 1)

WOOD   = (122, 78, 44)
WOOD_L = (164, 110, 62)
WOOD_D = (78, 48, 28)
PAPER  = (64, 44, 34)


def DrawWoodFrame9():
	# 나무 창틀: 짙은 갈색 반투명 안 + 나무 테(밝은 윗줄·어두운 아랫줄) + 네 귀퉁이 못
	C = FCanvas(24, 24)
	C.Rect(2, 2, 21, 21, PAPER, 242)
	for (X0, Y0, X1, Y1, Col) in ((1, 1, 22, 2, WOOD), (1, 21, 22, 22, WOOD), (1, 1, 2, 22, WOOD), (21, 1, 22, 22, WOOD),
								  (2, 1, 21, 1, WOOD_L), (1, 2, 1, 21, WOOD_L), (2, 22, 21, 22, WOOD_D), (22, 2, 22, 21, WOOD_D)):
		C.Rect(X0, Y0, X1, Y1, Col)
	for CX, CY in ((2, 2), (21, 2), (2, 21), (21, 21)):
		C.Px(CX, CY, (210, 200, 180))
	C.Outline((24, 14, 8))
	return C


def DrawSun():
	C = FCanvas(14, 14)
	C.Ellipse(7, 7, 3.6, 3.6, (255, 206, 72))
	C.Ellipse(6.4, 6.4, 1.8, 1.8, (255, 240, 170))
	for X, Y in ((7, 0), (7, 1), (7, 13), (7, 12), (0, 7), (1, 7), (13, 7), (12, 7), (2, 2), (11, 2), (2, 11), (11, 11)):
		C.Px(X, Y, (255, 190, 60))
	C.Outline((60, 32, 10))
	return C


def DrawMoon():
	C = FCanvas(14, 14)
	C.Ellipse(7, 7, 5.2, 5.2, (226, 232, 255))
	# 초승달: 오른쪽 위를 파낸다
	for Y in range(14):
		for X in range(14):
			if (X + 0.5 - 9.4) ** 2 + (Y + 0.5 - 5.2) ** 2 < 4.2 ** 2:
				C.P[Y, X] = (0, 0, 0, 0)
	C.Px(4, 8, (180, 190, 230))
	C.Px(5, 10, (180, 190, 230))
	C.Outline((20, 24, 48))
	return C


def DrawSlot(bSelected):
	# 핫바 칸 20x20 9-슬라이스 (선택 = 금빛 굵은 테)
	C = FCanvas(24, 24)
	C.Rect(2, 2, 21, 21, (52, 36, 28), 248)
	Edge = (250, 210, 90) if bSelected else WOOD
	Light = (255, 240, 170) if bSelected else WOOD_L
	for (X0, Y0, X1, Y1, Col) in ((1, 1, 22, 2, Edge), (1, 21, 22, 22, Edge), (1, 1, 2, 22, Edge), (21, 1, 22, 22, Edge), (2, 1, 21, 1, Light), (1, 2, 1, 21, Light)):
		C.Rect(X0, Y0, X1, Y1, Col)
	C.Outline((24, 14, 8))
	return C


def DrawSelectBar():
	# 목록 선택 줄: 왼쪽에서 옅어지는 금빛 띠 (가로로 늘림)
	C = FCanvas(24, 12)
	for X in range(24):
		C.Rect(X, 0, X, 11, (240, 196, 90), int(170 * (1.0 - X / 30.0)))
	C.Rect(0, 0, 23, 0, (255, 226, 140), 220)
	C.Rect(0, 11, 23, 11, (170, 120, 50), 200)
	return C


def DrawCoin():
	C = FCanvas(12, 12)
	C.Ellipse(6, 6, 5.2, 5.2, (226, 168, 40))
	C.Ellipse(6, 6, 4, 4, (252, 214, 90))
	C.Rect(5, 3, 6, 8, (226, 168, 40))
	C.Px(4, 4, (255, 246, 190))
	C.Outline((70, 40, 10))
	return C


def WriteTextures(Content):
	Folder = os.path.join(Content, *UI_DIR.split("/"))
	UpscaleSave(DrawSelectBar(), os.path.join(Folder, "Select.png"))
	UpscaleSave(DrawCoin(), os.path.join(Folder, "Coin.png"))
	UpscaleSave(DrawSlot(False), os.path.join(Folder, "Slot.png"))
	UpscaleSave(DrawSlot(True), os.path.join(Folder, "SlotSel.png"))
	import FarmBieCrops
	FarmBieCrops.WriteIcons(os.path.join(Folder, "Icons"), UpscaleSave)
	UpscaleSave(DrawWoodFrame9(), os.path.join(Folder, "Frame.png"))
	UpscaleSave(DrawSun(), os.path.join(Folder, "Sun.png"))
	UpscaleSave(DrawMoon(), os.path.join(Folder, "Moon.png"))
	from FarmBieArt import DrawPeddler
	UpscaleSave(DrawPeddler(0), os.path.join(Folder, "Peddler.png"), 3)


# ---- 위젯 도우미 (HD2DGameplay.py와 같은 .eui 형식) -------------------------------------------------------------------------
def Brush(Color=(1, 1, 1, 1), Corner=0, BorderWidth=0, BorderColor=(0, 0, 0, 1), Texture=None, NineSlice=False, TextureSize=48):
	B = {"Color": list(Color), "CornerRadius": Corner, "BorderWidth": BorderWidth, "BorderColor": list(BorderColor)}
	if Texture:
		B["Texture"] = Texture
	if NineSlice:
		B["DrawAs"] = "NineSlice"
		B["Margin"] = [1.0 / 3.0] * 4
		B["TextureSize"] = [TextureSize, TextureSize]
	return B


def FrameBrush(Size=48):
	return Brush(Texture=f"{UI_DIR}/Frame.png", NineSlice=True, TextureSize=Size)


def CanvasSlot(Anchor, X, Y, W, H, Align=(0, 0), AutoSize=False, Z=0):
	return {"AnchorMin": list(Anchor), "AnchorMax": list(Anchor), "Offsets": [X, Y, W, H], "Alignment": list(Align), "AutoSize": AutoSize, "ZOrder": Z}


def StretchSlot(Z=0):
	return {"AnchorMin": [0, 0], "AnchorMax": [1, 1], "Offsets": [0, 0, 0, 0], "Alignment": [0, 0], "AutoSize": False, "ZOrder": Z}


def BoxSlot(Pad=(0, 0, 0, 0), HAlign="Fill", VAlign="Fill", Size="Auto"):
	return {"Padding": list(Pad), "HAlign": HAlign, "VAlign": VAlign, "SizeRule": Size, "FillWeight": 1}


def Widget(Type, Name, Slot=None, Visibility="HitTestInvisible", Children=None, **Fields):
	W = {"Type": Type, "Name": Name, "Enabled": True, "Opacity": 1, "Visibility": Visibility}
	if Slot is not None:
		W["Slot"] = Slot
	W.update(Fields)
	if Children:
		W["Children"] = Children
	return W


def Text(Name, Value, Size, Slot=None, Color=TEXT_LIGHT, Justify="Left", Visibility="HitTestInvisible", Wrap=False, Outline=2):
	return Widget("Text", Name, Slot, Visibility, Text=Value, FontSize=Size, TextColor=list(Color), Justify=Justify, Wrap=Wrap,
				  OutlineWidth=Outline, OutlineColor=list(OUTLINE), ShadowOffset=[0, 2], ShadowColor=[0, 0, 0, 0.5])


def Img(Name, Texture, Size, Slot=None, Visibility="HitTestInvisible", Color=(1, 1, 1, 1)):
	return Widget("Image", Name, Slot, Visibility, Brush=Brush(Color, Texture=Texture), ImageSize=[Size, Size])


# ---- HUD ---------------------------------------------------------------------------------------------------------------------
def HudWidgets():
	C = []
	# 오른쪽 위: 날짜·시각 (계절 일차 (요일) / 시계 + 해·달)
	C.append(Widget("Border", "ClockPanel", CanvasSlot((1, 0), -16, 14, 250, 0, (1, 0), AutoSize=True), "HitTestInvisible", [
		Widget("VerticalBox", "ClockBox", BoxSlot(), "HitTestInvisible", [
			Text("ClockDate", "봄 1일 (월)", 22, BoxSlot((0, 0, 0, 4), HAlign="Center"), TEXT_GOLD, "Center"),
			Widget("HorizontalBox", "ClockRow", BoxSlot(HAlign="Center"), "HitTestInvisible", [
				Img("ClockIcon", f"{UI_DIR}/Sun.png", 30, BoxSlot((0, 0, 10, 0), VAlign="Center")),
				Text("ClockTime", "06:00", 26, BoxSlot(VAlign="Center"), TEXT_LIGHT),
			]),
			Text("ClockYear", "1년차", 15, BoxSlot((0, 4, 0, 0), HAlign="Center"), TEXT_DIM, "Center"),
		]),
	], Brush=FrameBrush(), ContentPadding=[24, 14, 24, 14]))
	# 화면 위 가운데: 알림 띠 (밤 시작·새 날·계절)
	C.append(Widget("VerticalBox", "Banner", CanvasSlot((0.5, 0), 0, 96, 0, 0, (0.5, 0), AutoSize=True, Z=3), "Collapsed", [
		Text("BannerTitle", "", 40, BoxSlot(HAlign="Center"), TEXT_GOLD, "Center", Outline=3),
		Text("BannerSub", "", 21, BoxSlot((0, 6, 0, 0), HAlign="Center"), TEXT_LIGHT, "Center"),
	]))
	# 아래 가운데: 상호작용 안내
	C.append(Widget("Border", "Prompt", CanvasSlot((0.5, 1), 0, -40, 0, 0, (0.5, 1), AutoSize=True, Z=2), "Collapsed", [
		Text("PromptText", "", 21, BoxSlot(HAlign="Center"), TEXT_LIGHT, "Center"),
	], Brush=FrameBrush(36), ContentPadding=[20, 8, 20, 10]))
	# 아래 가운데: 핫바 9칸 (아이콘·개수·번호) + 위에 고른 물건 이름, 물뿌리개 물 막대
	Slots = []
	for I in range(9):
		Slots.append(Widget("Overlay", f"Slot{I}", BoxSlot((2, 0, 2, 0)), "HitTestInvisible", [
			Widget("Image", f"SlotBg{I}", BoxSlot(), "HitTestInvisible", Brush=Brush(Texture=f"{UI_DIR}/Slot.png", NineSlice=True, TextureSize=48), ImageSize=[64, 64]),
			Widget("Image", f"SlotIcon{I}", BoxSlot((8, 8, 8, 8), "Center", "Center"), "Collapsed", Brush=Brush(), ImageSize=[48, 48]),
			Text(f"SlotNum{I}", str(I + 1), 13, BoxSlot((7, 3, 0, 0), "Left", "Top"), TEXT_DIM),
			Text(f"SlotCount{I}", "", 16, BoxSlot((0, 0, 7, 4), "Right", "Bottom"), TEXT_LIGHT),
		]))
	C.append(Widget("VerticalBox", "HotbarBox", CanvasSlot((0.5, 1), 0, -14, 0, 0, (0.5, 1), AutoSize=True, Z=1), "HitTestInvisible", [
		Text("BuildTitle", "건설 모드  (B 나가기 · R 돌리기 · E 수리/크리스탈 · 도구 = 설치/철거)", 16, BoxSlot((0, 0, 0, 4), HAlign="Center"), (0.7, 0.95, 1.0, 1),
			 "Center", "Collapsed"),
		Text("ItemName", "", 20, BoxSlot((0, 0, 0, 6), HAlign="Center"), TEXT_GOLD, "Center"),
		Widget("HorizontalBox", "Hotbar", BoxSlot(HAlign="Center"), "HitTestInvisible", Slots),
		Widget("HorizontalBox", "WaterRow", BoxSlot((0, 6, 0, 0), HAlign="Center"), "Collapsed", [
			Text("WaterLabel", "물", 14, BoxSlot((0, 0, 8, 0), VAlign="Center"), (0.6, 0.85, 1.0, 1)),
			Widget("ProgressBar", "WaterBar", BoxSlot(VAlign="Center"), MinSize=[200, 10], Brush=Brush((0.05, 0.04, 0.08, 0.9), 2, 1, (0, 0, 0, 1)),
				   FillBrush=Brush((0.4, 0.75, 1.0, 1), 1), Percent=1.0, FillDirection="LeftToRight"),
		]),
	]))
	# 왼쪽 아래: 획득 알림 3줄 (아이콘 + 글)
	Toasts = []
	for I in range(3):
		Toasts.append(Widget("HorizontalBox", f"Toast{I}", BoxSlot((0, 0, 0, 4)), "Collapsed", [
			Widget("Image", f"ToastIcon{I}", BoxSlot((0, 0, 8, 0), VAlign="Center"), "HitTestInvisible", Brush=Brush(), ImageSize=[32, 32]),
			Text(f"ToastText{I}", "", 18, BoxSlot(VAlign="Center"), TEXT_LIGHT),
		]))
	C.append(Widget("VerticalBox", "Toasts", CanvasSlot((0, 1), 20, -20, 0, 0, (0, 1), AutoSize=True, Z=1), "HitTestInvisible", Toasts))
	# 왼쪽 위: 돈
	C.append(Widget("Border", "GoldPanel", CanvasSlot((0, 0), 16, 14, 0, 0, AutoSize=True), "HitTestInvisible", [
		Widget("HorizontalBox", "GoldRow", BoxSlot(), "HitTestInvisible", [
			Img("GoldIcon", f"{UI_DIR}/Coin.png", 30, BoxSlot((0, 0, 10, 0), VAlign="Center")),
			Text("GoldText", "0", 26, BoxSlot(VAlign="Center"), TEXT_GOLD),
		]),
	], Brush=FrameBrush(), ContentPadding=[20, 10, 24, 12]))
	# 왼쪽 위 돈 아래: 체력·정신력 막대 + 버프
	def VitalRow(Name, Label, Color, Fill):
		return Widget("HorizontalBox", f"{Name}Row", BoxSlot((0, 0, 0, 6)), "HitTestInvisible", [
			Text(f"{Name}Label", Label, 16, BoxSlot((0, 0, 8, 0), VAlign="Center"), Color),
			Widget("Overlay", f"{Name}Box", BoxSlot(VAlign="Center"), "HitTestInvisible", [
				Widget("ProgressBar", f"{Name}Bar", BoxSlot(), MinSize=[200, 16], Brush=Brush((0.05, 0.04, 0.06, 0.92), 2, 1, (0, 0, 0, 1)),
					   FillBrush=Brush(Fill, 1), Percent=1.0, FillDirection="LeftToRight"),
				Text(f"{Name}Text", "100", 13, BoxSlot(HAlign="Center", VAlign="Center"), TEXT_LIGHT, "Center"),
			]),
		])
	C.append(Widget("Border", "VitalPanel", CanvasSlot((0, 0), 16, 82, 0, 0, AutoSize=True), "HitTestInvisible", [
		Widget("VerticalBox", "VitalBox", BoxSlot(), "HitTestInvisible", [
			VitalRow("Health", "체력", (1.0, 0.62, 0.55, 1), (0.86, 0.26, 0.24, 1)),
			VitalRow("Sanity", "정신", (0.78, 0.66, 1.0, 1), (0.56, 0.38, 0.9, 1)),
			Text("BuffText", "", 15, BoxSlot((0, 2, 0, 0)), (0.75, 1.0, 0.7, 1)),
		]),
	], Brush=FrameBrush(36), ContentPadding=[16, 10, 18, 8]))
	# 위 가운데: 밤 디펜스 (남은 시간·좀비·처치) + 크리스탈 내구도
	C.append(Widget("Border", "DefensePanel", CanvasSlot((0.5, 0), 0, 14, 0, 0, (0.5, 0), AutoSize=True, Z=1), "Collapsed", [
		Widget("VerticalBox", "DefenseBox", BoxSlot(), "HitTestInvisible", [
			Text("DefenseText", "", 20, BoxSlot((0, 0, 0, 6), HAlign="Center"), TEXT_LIGHT, "Center"),
			Widget("HorizontalBox", "CrystalRow", BoxSlot(HAlign="Center"), "HitTestInvisible", [
				Text("CrystalLabel", "크리스탈", 15, BoxSlot((0, 0, 8, 0), VAlign="Center"), (0.85, 0.7, 1.0, 1)),
				Widget("ProgressBar", "CrystalBar", BoxSlot(VAlign="Center"), MinSize=[220, 12], Brush=Brush((0.05, 0.04, 0.08, 0.92), 2, 1, (0, 0, 0, 1)),
					   FillBrush=Brush((0.7, 0.45, 1.0, 1), 1), Percent=1.0, FillDirection="LeftToRight"),
			]),
		]),
	], Brush=FrameBrush(36), ContentPadding=[18, 8, 18, 10]))
	C.append(Text("RespawnText", "", 30, CanvasSlot((0.5, 0.5), 0, 0, 0, 0, (0.5, 0.5), True, 4), TEXT_RED, "Center", "Collapsed", Outline=3))
	C.append(Widget("Border", "GameOverWindow", StretchSlot(Z=12), "Collapsed", [
		Widget("Canvas", "GameOverCanvas", BoxSlot(), "HitTestInvisible", [
			Text("GameOverTitle", "", 48, CanvasSlot((0.5, 0.5), 0, -90, 0, 0, (0.5, 0.5), True), (0.85, 0.6, 1.0, 1), "Center", Outline=3),
			Text("GameOverBody", "", 22, CanvasSlot((0.5, 0.5), 0, 0, 900, 120, (0.5, 0)), TEXT_LIGHT, "Center", Wrap=True),
			Text("GameOverHint", "E  처음부터 다시", 20, CanvasSlot((0.5, 1), 0, -60, 0, 0, (0.5, 1), True), TEXT_GOLD, "Center"),
		]),
	], Brush=Brush((0.02, 0.0, 0.04, 0.9)), ContentPadding=[0, 0, 0, 0]))
	C.append(ShopWindow())
	C.append(BagWindow())
	# 화면 전체 어둡게 (잠들기·새 날 전환)
	C.append(Widget("Border", "Fade", StretchSlot(Z=10), "Collapsed", Brush=Brush((0.0, 0.0, 0.0, 1.0)), ContentPadding=[0, 0, 0, 0]))
	return C


SHOP_ROWS = 9


def ListRow(Prefix, I):
	# 목록 한 줄: 선택 띠 + 아이콘 + 이름(늘어남) + 오른쪽 값(가격·수량)
	return Widget("Overlay", f"{Prefix}Row{I}", BoxSlot((0, 0, 0, 3)), "HitTestInvisible", [
		Widget("Image", f"{Prefix}Sel{I}", BoxSlot(), "Collapsed", Brush=Brush(Texture=f"{UI_DIR}/Select.png"), ImageSize=[0, 0]),
		Widget("HorizontalBox", f"{Prefix}RowBox{I}", BoxSlot((10, 4, 14, 4)), "HitTestInvisible", [
			Widget("Image", f"{Prefix}Icon{I}", BoxSlot((0, 0, 10, 0), VAlign="Center"), "HitTestInvisible", Brush=Brush(), ImageSize=[36, 36]),
			Text(f"{Prefix}Name{I}", "", 20, BoxSlot(VAlign="Center", Size="Fill")),
			Text(f"{Prefix}Price{I}", "", 19, BoxSlot((12, 0, 0, 0), VAlign="Center"), TEXT_GOLD, "Right"),
			Text(f"{Prefix}Stock{I}", "", 17, BoxSlot((14, 0, 0, 0), VAlign="Center"), TEXT_DIM, "Right"),
		]),
	])


def DetailPanel(Prefix):
	return Widget("VerticalBox", f"{Prefix}Detail", CanvasSlot((0, 0), 560, 92, 300, 380), "HitTestInvisible", [
		Widget("Image", f"{Prefix}DetailIcon", BoxSlot((0, 0, 0, 10), HAlign="Center"), "HitTestInvisible", Brush=Brush(), ImageSize=[96, 96]),
		Text(f"{Prefix}DetailName", "", 24, BoxSlot((0, 0, 0, 8), HAlign="Center"), TEXT_GOLD, "Center"),
		Text(f"{Prefix}DetailDesc", "", 18, BoxSlot((0, 0, 0, 10)), TEXT_LIGHT, "Left", Wrap=True),
		Text(f"{Prefix}DetailInfo", "", 17, BoxSlot(), TEXT_DIM, "Left", Wrap=True),
	])


def ShopWindow():
	Rows = [ListRow("Shop", I) for I in range(SHOP_ROWS)]
	return Widget("Border", "ShopWindow", CanvasSlot((0.5, 0.5), 0, -10, 900, 560, (0.5, 0.5), Z=5), "Collapsed", [
		Widget("Canvas", "ShopCanvas", BoxSlot(), "HitTestInvisible", [
			Img("ShopPortrait", "UI/FarmBie/Peddler.png", 64, CanvasSlot((0, 0), 6, 4, 64, 64)),
			Text("ShopTitle", "떠돌이 보부상", 30, CanvasSlot((0, 0), 84, 2, 0, 0, AutoSize=True), TEXT_GOLD),
			Text("ShopSay", "", 18, CanvasSlot((0, 0), 84, 42, 0, 0, AutoSize=True), (0.95, 0.85, 0.7, 1)),
			Widget("HorizontalBox", "ShopGoldRow", CanvasSlot((1, 0), -10, 10, 0, 0, (1, 0), AutoSize=True), "HitTestInvisible", [
				Img("ShopGoldIcon", f"{UI_DIR}/Coin.png", 26, BoxSlot((0, 0, 8, 0), VAlign="Center")),
				Text("ShopGold", "0", 24, BoxSlot(VAlign="Center"), TEXT_GOLD),
			]),
			Widget("VerticalBox", "ShopList", CanvasSlot((0, 0), 0, 86, 540, 400), "HitTestInvisible", Rows),
			DetailPanel("Shop"),
			Text("ShopHint", "W/S 고르기   E 사기   Esc 닫기", 16, CanvasSlot((0.5, 1), 0, -4, 0, 0, (0.5, 1), AutoSize=True), TEXT_DIM, "Center"),
		]),
	], Brush=FrameBrush(), ContentPadding=[28, 22, 28, 22])


def BagWindow():
	Slots = []
	for Row in range(4):
		Cells = []
		for Col in range(9):
			I = Row * 9 + Col
			Cells.append(Widget("Overlay", f"Bag{I}", BoxSlot((2, 2, 2, 2)), "HitTestInvisible", [
				Widget("Image", f"BagBg{I}", BoxSlot(), "HitTestInvisible", Brush=Brush(Texture=f"{UI_DIR}/Slot.png", NineSlice=True, TextureSize=48), ImageSize=[60, 60]),
				Widget("Image", f"BagIcon{I}", BoxSlot((8, 8, 8, 8), "Center", "Center"), "Collapsed", Brush=Brush(), ImageSize=[44, 44]),
				Text(f"BagCount{I}", "", 15, BoxSlot((0, 0, 6, 3), "Right", "Bottom"), TEXT_LIGHT),
			]))
		Slots.append(Widget("HorizontalBox", f"BagRow{Row}", BoxSlot((0, 0, 0, 6 if Row == 0 else 0)), "HitTestInvisible", Cells))
	return Widget("Border", "BagWindow", CanvasSlot((0.5, 0.5), 0, -20, 1000, 520, (0.5, 0.5), Z=5), "Collapsed", [
		Widget("Canvas", "BagCanvas", BoxSlot(), "HitTestInvisible", [
			Text("BagTitle", "소지품", 30, CanvasSlot((0, 0), 4, 0, 0, 0, AutoSize=True), TEXT_GOLD),
			Text("BagSub", "맨 윗줄 = 핫바", 16, CanvasSlot((0, 0), 130, 12, 0, 0, AutoSize=True), TEXT_DIM),
			Widget("VerticalBox", "BagGrid", CanvasSlot((0, 0), 0, 60, 600, 300), "HitTestInvisible", Slots),
			Widget("VerticalBox", "BagDetail", CanvasSlot((0, 0), 640, 56, 300, 360), "HitTestInvisible", [
				Widget("Image", "BagDetailIcon", BoxSlot((0, 0, 0, 10), HAlign="Center"), "HitTestInvisible", Brush=Brush(), ImageSize=[96, 96]),
				Text("BagDetailName", "", 24, BoxSlot((0, 0, 0, 8), HAlign="Center"), TEXT_GOLD, "Center"),
				Text("BagDetailDesc", "", 18, BoxSlot((0, 0, 0, 10)), TEXT_LIGHT, "Left", Wrap=True),
				Text("BagDetailInfo", "", 17, BoxSlot(), TEXT_DIM, "Left", Wrap=True),
			]),
			Text("BagHint", "WASD 고르기   E 집기/놓기 (자리 바꾸기)   I·Esc 닫기", 16, CanvasSlot((0.5, 1), 0, -4, 0, 0, (0.5, 1), AutoSize=True), TEXT_DIM, "Center"),
		]),
	], Brush=FrameBrush(), ContentPadding=[28, 22, 28, 22])


def WriteHud(Content):
	WriteTextures(Content)
	Root = Widget("Canvas", "Root", None, "SelfHitTestInvisible", HudWidgets())
	Path = os.path.join(Content, *UI_DIR.split("/"), "HUD.eui")
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump({"Version": 2, "DesignSize": [1280, 720], "ScaleMode": "MatchHeight", "Root": Root, "Animations": []}, File, indent=2, ensure_ascii=False)
		File.write("\n")
