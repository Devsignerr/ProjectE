# FarmBie 작물 정의 + 도트 아트 (CC0, 절차 생성). 표(FarmBieData.py)와 그림(이 파일)이 같은 CROPS 목록을 쓴다.
#   작물 그림: 다 자란 모습은 글자 틀(16 도트 폭)로 종마다 그리고, 싹·자라는 중은 종의 잎 색으로 공통 모양을 그린다.
#   희귀도 4단계(일반·레어·유니크·레전더리)는 다 자란 그림의 외곽선 색 + 반짝임(유니크·레전더리) + 레전더리 금빛 테두리로 구분한다.
#   타일 규약: 1 도트 = TILE_UPP cm (16 도트 = 격자 한 칸 100cm), 피벗 = 아래 가운데(땅). 바닥에 눕히는 흙 칸도 같은 크기.
import os

import numpy as np

from HD2DArt import FAtlas, FCanvas  # noqa: E402 — BuildFarmBie.py가 Tools/DemoMap을 경로에 넣는다

TILE_UPP = 100.0 / 16.0

SEASONS = ["Spring", "Summer", "Autumn", "Winter"]
RARITIES = ["Common", "Rare", "Unique", "Legendary"]
RARITY_NAMES = ["일반", "레어", "유니크", "레전더리"]
RARITY_OUTLINE = [(40, 28, 26), (40, 92, 200), (150, 52, 196), (232, 168, 30)]

# 작물: (Id, 이름, 계절, 자라는 날, 다시 열림(0 = 한 번), 기본 판매가, 씨앗 값, 전용 희귀종, 정신력 회복(0 = 음식 아님), 잎 색, 설명, 틀, 팔레트)
#   틀 글자: '.' 투명, 그 밖은 팔레트 키. 아래 줄이 땅. 폭 16
CROPS = []


def Crop(Id, Name, Season, Days, Regrow, Price, SeedPrice, Exclusive, Sanity, Leaf, Desc, Art, Palette):
	CROPS.append({"Id": Id, "Name": Name, "Season": Season, "Days": Days, "Regrow": Regrow, "Price": Price, "SeedPrice": SeedPrice,
				  "Exclusive": Exclusive, "Sanity": Sanity, "Leaf": Leaf, "Desc": Desc, "Art": Art, "Palette": Palette})


G, GL, GD = (86, 156, 70), (130, 196, 92), (52, 106, 50)

# ---- 봄 -------------------------------------------------------------------------------------------------------------------------
Crop("EyeRadish", "눈알무", "Spring", 4, 0, 35, 15, False, 0, G, "흙 속에서 이쪽을 올려다보는 무. 뽑을 때 눈을 마주치지 말 것.", [
	"......g..g......",
	".....gLg.Lg.....",
	"....gLLggLLg....",
	".....gLgLLg.....",
	"......gLLg......",
	".....wwWWww.....",
	"....wWWWWWWw....",
	"...wWWrrrrWWw...",
	"...wWrRppRrWw...",
	"...wWrRppRrWw...",
	"...wWWrrrrWWw...",
	"....wWWvWWWw....",
	".....wWWvWw.....",
	"......wWWw......",
	".......ww.......",
], {"g": GD, "L": GL, "w": (196, 188, 176), "W": (244, 238, 228), "r": (200, 70, 66), "R": (232, 116, 96), "p": (24, 16, 22), "v": (214, 120, 120)})

Crop("BrainCabbage", "뇌양배추", "Spring", 6, 0, 55, 25, False, 0, G, "주름진 속잎이 뇌처럼 꿈틀거린다. 따뜻할 때 수확하면 맛이 좋다.", [
	"................",
	".....gg..gg.....",
	"...gGGg..gGGg...",
	"..gGGppPPppGGg..",
	".gGgpPPpPPppGgg.",
	".gGpPpPPpPPppGg.",
	"gGgpPPpPpPPpPgGg",
	"gGgPpPPpPPpPpgGg",
	"gGgpPpPpPpPPpgGg",
	".gGgppPPpPPpgGg.",
	".gGGgpppppppgGg.",
	"..gGGgggggggGg..",
	"...gGGGGGGGGg...",
	".....gggggg.....",
], {"g": GD, "G": G, "p": (196, 110, 128), "P": (238, 160, 172)})

Crop("TentacleLeek", "촉수부추", "Spring", 5, 3, 30, 12, False, 0, (120, 84, 150), "물을 주면 촉수가 기지개를 켠다. 베어도 사흘이면 다시 자란다.", [
	"..t.........t...",
	"..tT......tTt...",
	"...tT.t..tT.....",
	"...tTtT..tT..t..",
	"....tTt.tTt.tT..",
	"..t.tT..tT.tT...",
	"..tTtT.tTtTt....",
	"...tTTttTTt.....",
	"....tTsTsTt.....",
	"....tTsTsTt.....",
	"....tTTTTTt.....",
	".....tTsTt......",
	".....tTTTt......",
	"......ttt.......",
], {"t": (84, 46, 110), "T": (156, 100, 196), "s": (236, 196, 220)})

Crop("WhisperPotato", "속삭이는 감자", "Spring", 6, 0, 60, 25, False, 18, G, "밤마다 흙 속에서 위로의 말을 속삭인다. 먹으면 마음이 차분해진다.", [
	".....g.gg.g.....",
	"....gLgLLgLg....",
	".....gLLLLg.....",
	"......gLLg......",
	"....bbbbbbbb....",
	"...bBBBBBBBBb...",
	"..bBBBeBBeBBBb..",
	"..bBBBBBBBBBBb..",
	"..bBBmmmmmmBBb..",
	"..bBBmtmtmtBBb..",
	"..bBBBmmmmBBBb..",
	"...bBBBBBBBBb...",
	"....bbbbbbbb....",
], {"g": GD, "L": GL, "b": (120, 84, 52), "B": (186, 142, 92), "e": (34, 22, 20), "m": (70, 30, 30), "t": (240, 232, 214)})

Crop("FingerBean", "손가락콩", "Spring", 7, 3, 40, 18, False, 0, G, "꼬투리 끝에 손톱이 자란다. 수확할 때 가끔 손을 잡아 온다.", [
	"......gg........",
	".....gLLg..gg...",
	"....gLgLLggLLg..",
	"...gLg.gLLLgLg..",
	"...fF..gLLg.....",
	"..fFFf.gLg..fF..",
	"..fFFf.gLg.fFFf.",
	"..fFFf.gLg.fFFf.",
	"..fFFf.gLg.fFFf.",
	"..fnnf.gLg.fFFf.",
	"...nn..gLg.fnnf.",
	".......gLg..nn..",
	"......gLLLg.....",
	".......ggg......",
], {"g": GD, "L": GL, "f": (190, 140, 116), "F": (240, 196, 168), "n": (250, 236, 230)})

Crop("Mandrake", "달빛 만드라고라", "Spring", 8, 0, 160, 0, True, 30, (140, 190, 120), "보름달 아래에서만 싹을 틔운다는 사람 모양 뿌리. 울음소리를 들으면 정신이 맑아진다.", [
	"....g..gg..g....",
	"...gLg.LL.gLg...",
	"....gLgLLgLg....",
	".....gLLLLg.....",
	"......mmmm......",
	".....mMMMMm.....",
	"....mMeMMeMm....",
	"....mMMooMMm....",
	"..mmmMMooMMmmm..",
	".mMMmMMMMMMmMMm.",
	"..m..mMMMMm..m..",
	".....mMm.mMm....",
	"....mMm...mMm...",
	"....mm.....mm...",
], {"g": GD, "L": (180, 230, 150), "m": (130, 96, 70), "M": (206, 170, 128), "e": (30, 40, 70), "o": (90, 40, 40)})

# ---- 여름 ------------------------------------------------------------------------------------------------------------------------
Crop("FangCorn", "이빨옥수수", "Summer", 9, 4, 50, 20, False, 0, (120, 170, 60), "알갱이 대신 송곳니가 촘촘히 박혔다. 씹는 쪽이 누구인지 헷갈린다.", [
	"......gLg.......",
	".....gLg........",
	"....hgLg.h......",
	"...hHtTtTHh.....",
	"...hHTtTtHh.....",
	"...hHtTtTHh.....",
	"...hHTtTtHh.....",
	"...hHtTtTHh.....",
	"....hHTtHh......",
	".....hHHh.......",
	"......gLg..g....",
	".....gLLggLg....",
	"....gLg.gLg.....",
	"......gLg.......",
	"......ggg.......",
], {"g": GD, "L": GL, "h": (118, 140, 60), "H": (176, 196, 96), "t": (250, 244, 220), "T": (214, 204, 170)})

Crop("HeartTomato", "심장토마토", "Summer", 7, 3, 45, 18, False, 0, G, "줄기에 매달린 채 쿵쿵 뛴다. 따면 박동이 천천히 멎는다.", [
	"......gg.gg.....",
	".....gLgLLg.....",
	"....gLg.gLLg....",
	"..aa.g...gaa....",
	".aRRa.g.aRRRa...",
	"aRrRRa..aRrRRa..",
	"aRRRRa.aRRRRRa..",
	".aRRa..aRrRRa...",
	"..vv...gaRRa....",
	"......gLgvv.....",
	".....gLLg.......",
	"......gLg.......",
	".....gLLLg......",
	"......ggg.......",
], {"g": GD, "L": GL, "a": (120, 20, 34), "R": (210, 48, 60), "r": (250, 140, 140), "v": (70, 90, 170)})

Crop("VeinPepper", "혈관고추", "Summer", 5, 3, 35, 14, False, 0, G, "껍질 아래로 파란 핏줄이 비친다. 매운맛이 오래 맥박친다.", [
	".......gg.......",
	"......gLLg......",
	"....g.gLLg.g....",
	"...gLggLLggLg...",
	"....pPp..pPp....",
	"....pPvp.pvPp...",
	"....pPPp.pPPp...",
	"....pvPp.pPvp...",
	"....pPPp.pPPp...",
	".....pp...pp....",
	"......gLLg......",
	"......gLLg......",
	".....gLLLLg.....",
	"......gggg......",
], {"g": GD, "L": GL, "p": (150, 24, 30), "P": (226, 54, 50), "v": (70, 90, 200)})

Crop("SkullMelon", "해골수박", "Summer", 11, 0, 180, 60, False, 0, G, "줄무늬가 웃는 해골을 그린다. 잘 익을수록 이가 하얗다.", [
	"................",
	"......g.........",
	".....gLg........",
	"....kkkkkkkk....",
	"...kMmMmMmMmk...",
	"..kMmWWMmWWmMk..",
	"..kmMWdWMWdWmk..",
	"..kMmWWMmWWmMk..",
	"..kmMmMWWMmMmk..",
	"..kMmWtWtWtWMk..",
	"..kmMmMmMmMmMk..",
	"...kMmMmMmMmk...",
	"....kkkkkkkk....",
], {"g": GD, "L": GL, "k": (30, 70, 34), "M": (70, 150, 70), "m": (40, 106, 48), "W": (236, 232, 214), "d": (30, 24, 24), "t": (250, 250, 240)})

Crop("VoidSunflower", "공허 해바라기", "Summer", 8, 0, 90, 35, False, 22, (100, 150, 70), "해를 따라 도는 대신 밤하늘의 빈 곳을 바라본다. 씨앗을 먹으면 마음이 고요해진다.", [
	"....y.yyy.y.....",
	"...yYyYYYyYy....",
	"..yYYkkkkkYYy...",
	"..yYkVVVVVkYy...",
	".yYkVVsVVVVkYy..",
	".yYkVVVeeVVkYy..",
	".yYkVVVeeVsVkYy.",
	"..yYkVVVVVkYy...",
	"..yYYkkkkkYYy...",
	"...yYyYYYyYy....",
	"....y.ygy.y.....",
	".....gLgLg......",
	"......gLg.......",
	"......gLg.......",
	".....gLLLg......",
	"......ggg.......",
], {"g": GD, "L": GL, "y": (190, 140, 30), "Y": (250, 206, 60), "k": (30, 20, 40), "V": (16, 10, 30), "s": (200, 210, 255), "e": (170, 60, 220)})

Crop("SunEyeLotus", "태양눈 연꽃", "Summer", 10, 0, 220, 0, True, 0, (90, 170, 120), "한낮에만 눈을 뜨는 연꽃. 그 시선을 받은 땅은 기름져진다고 한다.", [
	"................",
	".......pp.......",
	"......pPPp......",
	"..pp.pPyyPp.pp..",
	".pPPppyEEyppPPp.",
	".pPPPpyEeyPPPPp.",
	"..pPPPpyyPPPPp..",
	"...ppPPPPPPpp...",
	".....pppppp.....",
	"......gLg.......",
	"......gLg.......",
	"....ggLLLgg.....",
	"...gLLgggLLg....",
	"....ggg.ggg.....",
], {"g": GD, "L": GL, "p": (200, 90, 140), "P": (250, 170, 200), "y": (250, 200, 70), "E": (250, 240, 200), "e": (200, 80, 20)})

# ---- 가을 ------------------------------------------------------------------------------------------------------------------------
Crop("ScreamPumpkin", "비명호박", "Autumn", 12, 0, 200, 70, False, 0, (110, 140, 60), "수확하는 순간 비명을 지른다. 귀마개는 따로 팔지 않는다.", [
	".......gg.......",
	"......gLg.......",
	"...oooogooooo...",
	"..oOOoOOOoOOOo..",
	".oOOoOOOOoOOOOo.",
	".oOkkOOOOOkkOOo.",
	".oOkkkOoOkkkOOo.",
	".oOOoOOOOOoOOOo.",
	".oOOokkkkkoOOOo.",
	".oOOkrrrrrkOOOo.",
	".oOOokkkkkoOOOo.",
	"..oOOoOOOoOOOo..",
	"...ooooooooo....",
], {"g": GD, "L": GL, "o": (176, 86, 20), "O": (236, 136, 40), "k": (40, 20, 10), "r": (200, 40, 30)})

Crop("ShadowEggplant", "그림자가지", "Autumn", 6, 4, 50, 20, False, 0, G, "빛을 먹고 자라 그림자를 남긴다. 어둠 속에서 두 눈이 빛난다.", [
	"......gg.gg.....",
	".....gLggLLg....",
	"......gLLg......",
	".....dDDDDd.....",
	"....dDDDDDDd....",
	"...dDDyDDyDDd...",
	"...dDDDDDDDDd...",
	"...dDDDhDDDDd...",
	"...dDDDDDDDDd...",
	"....dDDDDDhd....",
	".....dDDDDd.....",
	"......dddd......",
	".......gg.......",
	"......gLLg......",
	"......ggg.......",
], {"g": GD, "L": GL, "d": (36, 16, 50), "D": (88, 42, 120), "y": (250, 230, 90), "h": (150, 100, 190)})

Crop("CandleMushroom", "촛농버섯", "Autumn", 5, 0, 40, 15, False, 20, (180, 160, 120), "갓 위에서 작은 불꽃이 탄다. 따뜻한 빛을 쬐면 불안이 녹는다.", [
	"....f......f....",
	"...fFf....fFf...",
	"....w......w....",
	"..cCCCc..cCCCc..",
	".cCWCCCccCCWCCc.",
	".cCCCCCccCCCCCc.",
	"..ccccc..ccccc..",
	"...sSs....sSs...",
	"...sSs.f..sSs...",
	"...sSs.w..sSs...",
	"...sSscCcsSSs...",
	"..sSSsCWCsSSs...",
	"..ssssssssssss..",
], {"f": (250, 160, 40), "F": (255, 240, 150), "w": (60, 40, 30), "c": (170, 120, 70), "C": (226, 196, 150), "W": (250, 240, 220),
	"s": (196, 180, 160), "S": (240, 230, 214)})

Crop("CobwebGrape", "거미줄포도", "Autumn", 9, 3, 55, 22, False, 0, G, "송이 사이로 끈적한 거미줄이 엉겨 있다. 알갱이 몇 개는 눈을 깜빡인다.", [
	"......gg........",
	".....gLLg.......",
	"..w...gLg...w...",
	"...w.vvVvv.w....",
	"....wvVvVvVw....",
	"...vVvwVvVvv....",
	"...vVvVwVeVv....",
	"....vVevVvwV....",
	"....vVvVvVvw....",
	".....vVvVvv.....",
	"......vVvv......",
	".......vv.......",
	"......gLg.......",
	".....gLLLg......",
	"......ggg.......",
], {"g": GD, "L": GL, "w": (230, 230, 240), "v": (70, 30, 90), "V": (130, 70, 160), "e": (240, 220, 80)})

Crop("BloodBeet", "피뿌리 비트", "Autumn", 6, 0, 70, 28, False, 0, (120, 90, 80), "뽑으면 붉은 즙이 뚝뚝 떨어진다. 피는 아니라고 믿고 싶다.", [
	"....g.gg.g......",
	"...gLgLLgLg.....",
	"....rLrLLr......",
	".....rLLr.......",
	"......rr........",
	"....bbbbbb......",
	"...bBBBBBBb.....",
	"..bBBhBBBBBb....",
	"..bBBBBBBBBb....",
	"..bBBBBBBBBb....",
	"...bBBBBBBb.....",
	"....bbBBbb.d....",
	"......bb...d....",
	"......b....D....",
], {"g": GD, "L": (110, 150, 80), "r": (150, 30, 60), "b": (90, 10, 40), "B": (170, 30, 70), "h": (230, 110, 140), "d": (160, 20, 30), "D": (220, 40, 40)})

Crop("WraithCorn", "망령 옥수수", "Autumn", 11, 0, 240, 0, True, 0, (160, 170, 150), "죽은 이의 숨결로 여문다는 창백한 옥수수. 알갱이마다 작은 얼굴이 떠 있다.", [
	"......gLg.......",
	".....gLg........",
	"....hgLg.h......",
	"...hHwWwWHh.....",
	"...hHWfWfHh.....",
	"...hHwWwWHh.....",
	"...hHWfWfHh.....",
	"...hHwWwWHh.....",
	"....hHWwHh......",
	".....hHHh.......",
	"......gLg..g....",
	".....gLLggLg....",
	"....gLg.gLg.....",
	"......gLg.......",
	"......ggg.......",
], {"g": (90, 100, 90), "L": (150, 170, 150), "h": (120, 130, 130), "H": (180, 190, 190), "w": (200, 220, 240), "W": (240, 250, 255), "f": (90, 110, 160)})

# ---- 겨울 ------------------------------------------------------------------------------------------------------------------------
Crop("FrostEyeFlower", "서리눈꽃", "Winter", 6, 0, 70, 28, False, 0, (110, 160, 170), "눈 덮인 땅에서 피어나 차가운 눈동자로 주위를 살핀다.", [
	"................",
	"......iIIi......",
	"....iIIiiIIi....",
	"...iIiCCCCiIi...",
	"..iIiCbbbbCiIi..",
	"..iIiCbkkbCiIi..",
	"..iIiCbkkbCiIi..",
	"...iIiCCCCiIi...",
	"....iIIiiIIi....",
	"......iIIi......",
	".......gL.......",
	".....g.gL.g.....",
	"......gLLg......",
	"......ggg.......",
], {"g": (60, 100, 110), "L": (120, 170, 180), "i": (120, 170, 220), "I": (210, 236, 255), "C": (250, 255, 255), "b": (60, 140, 220), "k": (10, 20, 40)})

Crop("BoneCarrot", "뼈당근", "Winter", 5, 0, 55, 22, False, 0, (100, 140, 90), "단단하고 하얀 뿌리가 뼈를 꼭 닮았다. 국물을 우리면 진하다.", [
	".....g.gg.g.....",
	"....gLgLLgLg....",
	".....gLLLLg.....",
	"......gLLg......",
	".....WWWWWW.....",
	"....WWbWWbWW....",
	".....WWWWWW.....",
	"......WbWW......",
	"......WWbW......",
	"......WbWW......",
	".....WWWWWW.....",
	"....WWbWWbWW....",
	".....WW..WW.....",
], {"g": (60, 96, 60), "L": (120, 170, 110), "W": (240, 234, 216), "b": (190, 180, 160)})

Crop("GhostGarlic", "유령마늘", "Winter", 7, 0, 80, 30, False, 24, (180, 200, 190), "반투명한 마늘이 가끔 흙 위로 떠오른다. 먹으면 악몽이 물러간다.", [
	".......gg.......",
	"......gLLg......",
	".......gg.......",
	"......wWWw......",
	".....wWWWWw.....",
	"....wWWWWWWw....",
	"...wWWkWWkWWw...",
	"...wWWWWWWWWw...",
	"...wWWWooWWWw...",
	"...wWWWooWWWw...",
	"....wWWWWWWw....",
	".....wwWWww.....",
	"......w.w.w.....",
], {"g": (100, 130, 120), "L": (170, 210, 190), "w": (170, 190, 210), "W": (226, 236, 248), "k": (50, 60, 90), "o": (90, 100, 140)})

Crop("AbyssTurnip", "심연 순무", "Winter", 8, 0, 110, 40, False, 0, (70, 120, 120), "속을 들여다보면 별이 박힌 밤하늘이 보인다. 밑동에서 가는 촉수가 꿈틀댄다.", [
	".....g.gg.g.....",
	"....gLgLLgLg....",
	".....gLLLLg.....",
	"......gLLg......",
	".....aaaaaa.....",
	"....aAAAAAAa....",
	"...aAAsAAAAAa...",
	"...aAAAAAsAAa...",
	"...aAsAAAAAAa...",
	"....aAAAsAAa....",
	".....aaAAaa.....",
	"....t..aa..t....",
	"...t..t..t..t...",
], {"g": (50, 90, 80), "L": (90, 150, 140), "a": (20, 40, 60), "A": (40, 80, 110), "s": (230, 240, 255), "t": (90, 60, 120)})

Crop("FrozenStar", "얼어붙은 별", "Winter", 9, 0, 260, 0, True, 0, (130, 170, 210), "겨울 밤하늘에서 떨어진 별이 얼음 꽃으로 뿌리내렸다. 손을 대면 맑은 소리가 난다.", [
	".......II.......",
	".......II.......",
	"..I...iSSi...I..",
	"...I.iSSSSi.I...",
	"....iSSyySSi....",
	"IIiSSSyYYySSSiII",
	"....iSSyySSi....",
	"...I.iSSSSi.I...",
	"..I...iSSi...I..",
	".......II.......",
	".......gL.......",
	"......gLLg......",
	"......ggg.......",
], {"g": (60, 96, 120), "L": (120, 170, 200), "i": (120, 170, 230), "I": (200, 230, 255), "S": (220, 240, 255), "y": (250, 230, 140), "Y": (255, 255, 220)})


def CropById(Id):
	for C in CROPS:
		if C["Id"] == Id:
			return C
	raise KeyError(Id)


# ---- 그리기 --------------------------------------------------------------------------------------------------------------------
def _Shade(C, F):
	return tuple(max(0, min(255, int(V * F))) for V in C)


def _Blend(A, B, T):
	return tuple(int(A[I] + (B[I] - A[I]) * T) for I in range(3))


def DrawMature(Crop, Rarity):
	Art, Pal = Crop["Art"], Crop["Palette"]
	H = len(Art)
	C = FCanvas(16, H + 2)
	Gold = (255, 214, 90)
	for Y, Row in enumerate(Art):
		for X, Ch in enumerate(Row):
			if Ch != "." and Ch in Pal:
				Col = Pal[Ch]
				if Rarity == 3:
					Col = _Blend(Col, Gold, 0.18)  # 레전더리: 금빛이 감돈다
				C.Px(X, Y + 1, Col)
	# 외곽선 = 희귀도 색 (안쪽 그림 둘레 4-이웃)
	C.Outline(RARITY_OUTLINE[Rarity])
	if Rarity >= 2:
		# 반짝임 (유니크 = 흰 별 2, 레전더리 = 금 별 3)
		Spark = (255, 255, 255) if Rarity == 2 else (255, 236, 140)
		Points = [(1, 2), (14, 5)] if Rarity == 2 else [(1, 1), (14, 3), (13, H - 1)]
		for X, Y in Points:
			for DX, DY in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
				if C.P[Y + DY, X + DX, 3] == 0 or (DX, DY) == (0, 0):
					C.Px(X + DX, Y + DY, Spark)
	return C


def DrawSeedMound():
	C = FCanvas(16, 6)
	C.Ellipse(8, 4, 4.5, 2.2, (92, 64, 44))
	C.Ellipse(8, 3.5, 3.2, 1.6, (120, 86, 58))
	C.Px(7, 2, (200, 180, 110))
	C.Px(9, 3, (200, 180, 110))
	return C


def DrawSprout(Leaf):
	C = FCanvas(16, 10)
	Dark, Light = _Shade(Leaf, 0.7), _Shade(Leaf, 1.25)
	C.Ellipse(8, 8, 3.5, 1.6, (100, 72, 50))
	C.Rect(8, 4, 8, 8, Dark)
	for X, Y in ((6, 3), (5, 2), (6, 2), (7, 3)):
		C.Px(X, Y, Light)
	for X, Y in ((9, 3), (10, 2), (11, 2), (10, 3)):
		C.Px(X, Y, Leaf)
	C.Outline((30, 40, 24))
	return C


def DrawGrowing(Leaf):
	C = FCanvas(16, 14)
	Dark, Light = _Shade(Leaf, 0.7), _Shade(Leaf, 1.25)
	C.Ellipse(8, 12, 4, 1.6, (100, 72, 50))
	C.Rect(7, 5, 8, 12, Dark)
	for (CX, CY, RX, RY, Col) in ((5, 7, 2.6, 1.6, Leaf), (11, 6, 2.6, 1.6, Leaf), (6, 3, 2.2, 1.4, Light), (10, 2.5, 2.2, 1.4, Light), (8, 9, 2.4, 1.3, Leaf)):
		C.Ellipse(CX, CY, RX, RY, Col)
	C.Outline((30, 40, 24))
	return C


def DrawWithered():
	C = FCanvas(16, 12)
	Brown, Dark = (130, 100, 60), (90, 66, 40)
	C.Ellipse(8, 10, 4, 1.6, (90, 66, 46))
	C.Line(8, 10, 8, 4, Dark)
	C.Line(8, 6, 4, 3, Brown)
	C.Line(8, 5, 12, 2, Brown)
	C.Line(8, 8, 11, 7, Brown)
	C.Outline((40, 28, 20))
	return C


def DrawSoil(bWet, Seed):
	# 바닥에 눕히는 간 흙 칸 16x16: 고랑 3줄 + 흙덩이 (젖으면 짙게)
	Rng = np.random.default_rng(Seed)
	Base = (96, 66, 44) if not bWet else (66, 44, 32)
	Light = (128, 92, 62) if not bWet else (90, 62, 44)
	Dark = (76, 52, 36) if not bWet else (50, 32, 24)
	C = FCanvas(16, 16)
	C.Rect(0, 0, 15, 15, Base)
	for Row in (2, 7, 12):
		C.Rect(1, Row, 14, Row, Dark)
		C.Rect(1, Row + 1, 14, Row + 1, Light)
	for _ in range(10):
		X, Y = int(Rng.integers(0, 16)), int(Rng.integers(0, 16))
		C.Px(X, Y, Light if Rng.random() < 0.5 else Dark)
	# 가장자리 살짝 어둡게 (풀과 경계)
	C.Rect(0, 0, 15, 0, Dark)
	C.Rect(0, 15, 15, 15, Dark)
	C.Rect(0, 0, 0, 15, Dark)
	C.Rect(15, 0, 15, 15, Dark)
	return C


def DrawFertilizer(Premium):
	# 흙 위 비료 알갱이 (바닥에 눕힘, 16x16 투명 바탕)
	Rng = np.random.default_rng(7 if Premium else 3)
	C = FCanvas(16, 16)
	Col = (120, 210, 120) if not Premium else (210, 140, 250)
	for _ in range(14):
		C.Px(int(Rng.integers(2, 14)), int(Rng.integers(2, 14)), Col)
	return C


def DrawCursor():
	# 대상 칸 표시 (바닥에 눕힘): 흰 모서리 괄호
	C = FCanvas(16, 16)
	W = (255, 255, 255)
	for X0, Y0, DX, DY in ((0, 0, 1, 1), (15, 0, -1, 1), (0, 15, 1, -1), (15, 15, -1, -1)):
		for I in range(4):
			C.Px(X0 + DX * I, Y0, W)
			C.Px(X0, Y0 + DY * I, W)
	return C


def WriteSprites(Folder):
	# Crops.esprite: 종마다 Seed/Sprout/Grow/Mature0~3 + 공통 Withered, Field.esprite: 흙 칸·비료·커서 (피벗 가운데 — 바닥에 눕힘)
	Atlas = FAtlas(1024)
	Atlas.Add("Seed", DrawSeedMound())
	Atlas.Add("Withered", DrawWithered())
	for Crop in CROPS:
		Atlas.Add(f"{Crop['Id']}_Sprout", DrawSprout(Crop["Leaf"]))
		Atlas.Add(f"{Crop['Id']}_Grow", DrawGrowing(Crop["Leaf"]))
		for R in range(4):
			Atlas.Add(f"{Crop['Id']}_{R}", DrawMature(Crop, R))
	Atlas.Save(Folder, "Crops")
	_SetUnits(Folder, "Crops")
	Field = FAtlas(256)
	for I in range(3):
		Field.Add(f"Soil{I}", DrawSoil(False, 11 + I), (0.5, 0.5))
		Field.Add(f"SoilWet{I}", DrawSoil(True, 11 + I), (0.5, 0.5))
	Field.Add("FertBasic", DrawFertilizer(False), (0.5, 0.5))
	Field.Add("FertPremium", DrawFertilizer(True), (0.5, 0.5))
	Field.Add("Cursor", DrawCursor(), (0.5, 0.5))
	Field.Save(Folder, "Field")
	_SetUnits(Folder, "Field")


def _SetUnits(Folder, Name):
	# FAtlas는 HD2DArt.UNITS_PER_PIXEL(6cm)로 쓴다 → 작물·흙 칸은 16 도트 = 100cm로 바꿔 쓴다
	import json
	Path = os.path.join(Folder, f"{Name}.esprite")
	with open(Path, encoding="utf-8") as File:
		Doc = json.load(File)
	Doc["UnitsPerPixel"] = TILE_UPP
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")


# ---- 아이콘 (UI — 4배 최근접 확대) -------------------------------------------------------------------------------------------------
def DrawSeedPacket(Crop, Rarity):
	C = FCanvas(14, 16)
	Paper = [(214, 196, 150), (170, 200, 240), (210, 170, 236), (250, 220, 120)][Rarity]
	C.Rect(1, 2, 12, 15, Paper)
	C.Rect(1, 2, 12, 3, _Shade(Paper, 0.8))
	C.Rect(3, 0, 10, 2, _Shade(Paper, 0.9))
	Leaf = Crop["Leaf"]
	# 봉투 그림: 작물 다 자란 모습 축소 (가운데 8x8)
	Mini = DrawMature(Crop, 0)
	Small = Mini.P[1:-1:max(1, (Mini.H - 2) // 8), ::2][:8, :8]
	for Y in range(Small.shape[0]):
		for X in range(Small.shape[1]):
			if Small[Y, X, 3] > 0:
				C.Px(3 + X, 5 + Y, tuple(int(V) for V in Small[Y, X, :3]))
	C.Rect(2, 14, 11, 14, _Shade(Leaf, 0.8))
	C.Outline(RARITY_OUTLINE[Rarity])
	return C


def DrawCropIcon(Crop, Rarity):
	return DrawMature(Crop, Rarity)


def DrawToolIcon(Name):
	C = FCanvas(16, 16)
	if Name == "Hoe":
		C.Line(3, 14, 11, 4, (140, 96, 56))
		C.Line(4, 14, 12, 4, (110, 72, 40))
		C.Rect(9, 2, 14, 4, (170, 176, 186))
		C.Rect(12, 4, 14, 6, (130, 136, 146))
	elif Name == "Can":
		C.Ellipse(7, 10, 5, 4, (90, 140, 190))
		C.Rect(3, 7, 11, 13, (90, 140, 190))
		C.Rect(4, 8, 6, 12, (140, 186, 230))
		C.Line(11, 9, 15, 5, (70, 110, 160))
		C.Rect(5, 4, 9, 5, (70, 110, 160))
	elif Name == "Axe":
		C.Line(4, 14, 11, 3, (140, 96, 56))
		C.Line(5, 14, 12, 3, (110, 72, 40))
		C.Rect(9, 1, 13, 6, (176, 182, 192))
		C.Rect(12, 2, 14, 7, (210, 216, 226))
	elif Name == "Pick":
		C.Line(7, 14, 8, 3, (140, 96, 56))
		C.Line(8, 14, 9, 3, (110, 72, 40))
		C.Rect(2, 2, 14, 3, (176, 182, 192))
		C.Px(1, 4, (130, 136, 146))
		C.Px(15, 4, (130, 136, 146))
	elif Name == "Wood":
		for Y, X0 in ((4, 2), (8, 4), (11, 1)):
			C.Rect(X0, Y, X0 + 11, Y + 3, (150, 100, 56))
			C.Rect(X0, Y, X0 + 11, Y, (186, 132, 80))
			C.Ellipse(X0 + 11, Y + 1.5, 1.8, 1.8, (214, 170, 110))
	elif Name == "Stone":
		C.Ellipse(8, 9, 6, 4.5, (130, 132, 128))
		C.Ellipse(7, 8, 4, 3, (166, 168, 160))
		C.Px(10, 10, (100, 102, 100))
	elif Name == "Fiber":
		for X in (4, 6, 8, 10, 12):
			C.Line(X, 14, X + (2 if X % 4 == 0 else -1), 2, (200, 194, 120))
		C.Rect(3, 8, 13, 9, (130, 90, 50))
	elif Name == "Herb":
		C.Line(8, 14, 8, 5, (70, 120, 60))
		for X, Y in ((5, 6), (11, 5), (5, 10), (11, 9)):
			C.Ellipse(X, Y, 2.6, 1.6, (110, 180, 90))
		C.Ellipse(8, 3, 1.8, 1.8, (240, 236, 130))
	elif Name == "Mushroom":
		C.Rect(7, 8, 9, 14, (226, 216, 196))
		C.Ellipse(8, 7, 6.5, 4, (170, 70, 160))
		C.Ellipse(8, 6, 5, 2.6, (210, 110, 200))
		C.Px(5, 6, (250, 230, 250))
		C.Px(10, 5, (250, 230, 250))
	elif Name == "Iron":
		C.Rect(2, 7, 13, 12, (110, 116, 126))
		C.Rect(3, 6, 14, 6, (160, 166, 176))
		C.Rect(4, 8, 11, 9, (170, 176, 186))
	elif Name == "CrystalShard":
		for Y in range(14):
			W = 1 + (Y if Y < 7 else 13 - Y) // 2
			C.Rect(8 - W, Y + 1, 8 + W - 1, Y + 1, (160, 90, 230))
		C.Rect(7, 3, 8, 11, (220, 180, 255))
	elif Name == "Mine":
		C.Ellipse(8, 10, 6.5, 3.5, (70, 74, 82))
		C.Ellipse(8, 9, 5, 2.5, (110, 116, 126))
		C.Rect(7, 4, 8, 7, (130, 136, 146))
		C.Rect(7, 3, 8, 3, (240, 60, 40))
	elif Name == "WallWood":
		for X in (2, 6, 10):
			C.Rect(X, 3, X + 3, 14, (140, 92, 52))
			C.Px(X + 1, 2, (110, 72, 40))
		C.Rect(1, 6, 14, 6, (190, 160, 100))
		C.Rect(1, 11, 14, 11, (190, 160, 100))
	elif Name == "WallStone":
		C.Rect(1, 4, 14, 14, (130, 132, 128))
		for Y in (7, 10):
			C.Rect(1, Y, 14, Y, (90, 92, 90))
		for X, Y in ((5, 4), (10, 4), (3, 8), (8, 8), (12, 8), (5, 11), (10, 11)):
			C.Rect(X, Y, X, Y + 2, (90, 92, 90))
	elif Name == "WallIron":
		C.Rect(1, 3, 14, 14, (80, 86, 96))
		for X in (2, 7, 12):
			C.Rect(X, 3, X + 1, 14, (150, 156, 166))
		for X, Y in ((4, 6), (10, 6), (4, 11), (10, 11)):
			C.Px(X, Y, (200, 204, 210))
	elif Name == "Gate":
		C.Rect(1, 2, 2, 14, (90, 58, 34))
		C.Rect(13, 2, 14, 14, (90, 58, 34))
		C.Rect(3, 5, 12, 14, (150, 100, 56))
		C.Rect(3, 8, 12, 8, (90, 92, 100))
		C.Rect(3, 12, 12, 12, (90, 92, 100))
	elif Name == "Spike":
		C.Rect(1, 12, 14, 14, (140, 92, 52))
		for X in (2, 6, 10):
			C.Line(X + 1, 11, X + 2, 4, (200, 204, 214))
			C.Px(X + 2, 3, (240, 244, 250))
	elif Name == "Turret":
		C.Rect(3, 9, 12, 14, (130, 132, 128))
		C.Rect(2, 8, 13, 8, (90, 92, 90))
		C.Rect(7, 4, 8, 7, (110, 72, 40))
		C.Rect(3, 3, 13, 4, (150, 100, 56))
		C.Line(13, 1, 13, 6, (110, 72, 40))
	elif Name == "Greenhouse":
		C.Rect(1, 6, 14, 14, (180, 220, 230))
		C.Line(1, 6, 7, 1, (110, 72, 40))
		C.Line(14, 6, 8, 1, (110, 72, 40))
		C.Rect(1, 6, 1, 14, (110, 72, 40))
		C.Rect(14, 6, 14, 14, (110, 72, 40))
		C.Rect(6, 9, 9, 14, (120, 180, 90))
	elif Name == "CrystalUp":
		for Y in range(12):
			W = 1 + (Y if Y < 6 else 11 - Y) // 2
			C.Rect(7 - W, Y + 3, 7 + W, Y + 3, (160, 90, 230))
		C.Rect(12, 1, 13, 6, (120, 230, 120))
		C.Rect(10, 3, 15, 4, (120, 230, 120))
	elif Name in ("FertBasic", "FertPremium"):
		Col = (120, 200, 110) if Name == "FertBasic" else (196, 130, 240)
		C.Rect(3, 4, 12, 14, (186, 160, 120))
		C.Rect(3, 4, 12, 5, (150, 126, 90))
		C.Rect(5, 8, 10, 11, Col)
		C.Rect(6, 2, 9, 4, (150, 126, 90))
	C.Outline((30, 22, 20))
	return C


def WriteIcons(Folder, UpscaleSave):
	for Crop in CROPS:
		for R in range(4):
			UpscaleSave(DrawSeedPacket(Crop, R), os.path.join(Folder, f"Seed_{Crop['Id']}_{R}.png"))
			UpscaleSave(DrawCropIcon(Crop, R), os.path.join(Folder, f"Crop_{Crop['Id']}_{R}.png"))
	for Name in ("Hoe", "Can", "FertBasic", "FertPremium", "Axe", "Pick", "Wood", "Stone", "Fiber", "Herb", "Mushroom", "Iron", "CrystalShard", "Mine",
				 "WallWood", "WallStone", "WallIron", "Gate", "Spike", "Turret", "Greenhouse", "CrystalUp"):
		UpscaleSave(DrawToolIcon(Name), os.path.join(Folder, f"{Name}.png"))
