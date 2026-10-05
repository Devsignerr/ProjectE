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
	C.Rect(2, 2, 21, 21, PAPER, 225)
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


def WriteTextures(Content):
	Folder = os.path.join(Content, *UI_DIR.split("/"))
	UpscaleSave(DrawWoodFrame9(), os.path.join(Folder, "Frame.png"))
	UpscaleSave(DrawSun(), os.path.join(Folder, "Sun.png"))
	UpscaleSave(DrawMoon(), os.path.join(Folder, "Moon.png"))


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
	# 화면 전체 어둡게 (잠들기·새 날 전환)
	C.append(Widget("Border", "Fade", StretchSlot(Z=10), "Collapsed", Brush=Brush((0.0, 0.0, 0.0, 1.0)), ContentPadding=[0, 0, 0, 0]))
	return C


def WriteHud(Content):
	WriteTextures(Content)
	Root = Widget("Canvas", "Root", None, "SelfHitTestInvisible", HudWidgets())
	Path = os.path.join(Content, *UI_DIR.split("/"), "HUD.eui")
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump({"Version": 2, "DesignSize": [1280, 720], "ScaleMode": "MatchHeight", "Root": Root, "Animations": []}, File, indent=2, ensure_ascii=False)
		File.write("\n")
