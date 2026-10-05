# HD-2D 데모 메타 시스템 생성 (HD2DGameplay.WriteAll / AddGame이 부른다 — 게임 콘텐츠 생성기와 같은 규약·도우미를 쓴다):
#   데이터 표(Data/Demo/HD2D): 재료 아이템 행(Items.etable 끝에 이어 붙임), 적별 재료 드랍 MaterialDrops, 무기 강화 Upgrades(+1~+3), 물약 조합 Recipes,
#                              퀘스트 일지 QuestJournal(메인 장 요약·서브 한 줄 목표),
#                              도감 설명 Bestiary(서식지·설명·그림 — 약점 칸은 게임 쪽 훅 HD2DGame:BestiaryWeakness)
#   UI(UI/Demo/HD2D/Meta.eui — 씬의 MetaUI 엔티티, HUD 위): 일시정지 메뉴(왼쪽 메뉴 10줄 + 오른쪽 쪽: 상태/퀘스트 일지/지도/도감/기록/설정),
#        저장 슬롯 창(3칸 카드), 확인 창(예/아니요), 대장간 창(HD2DGameplay.MenuWindow 재사용 — 강화/조합 탭), 오른쪽 위 미니맵(스크롤 상자로 잘라 보임)
#   그림: 메뉴·지도 아이콘·재료 아이콘·도감 그림(HD2DMetaArt), 지도 PNG는 맵 생성기가 HD2DMapArt.WriteMinimap으로 쓴다
#   씬: AddMeta(S, Layout, Minimap) — MetaUI 엔티티(UI + HD2DMetaHud.lua, 지도 정보 속성). 스크립트는 Scripts/Demo/HD2D/HD2DMeta*.lua·HD2DPause.lua 등
#   스크린샷 시나리오(--views): SHOT_SCENES (HD2DGameplay.SHOT_SCENES에 합쳐진다 → _HD2DShot_<이름>.escene)
import json
import os

import HD2DGameplay as G
import HD2DMetaArt

UI_DIR = G.UI_DIR
DATA = G.DATA


def MetaIcon(Name):
	return f"{UI_DIR}/Meta/{Name}.png"


# ================================================================ 데이터
def _Material(Id, Name, Icon, Desc):
	return (Id, {"DisplayName": Name, "Kind": "Material", "Amount": 0, "Weapon": "", "Icon": G.Icon(Icon), "Description": Desc, "Price": 0,
				 "Defense": 0, "HealthBonus": 0, "SpeedBonus": 0.0, "CritBonus": 0.0})


# 재료 (Items.etable 끝에 이어 붙는다 — 기존 행 순서는 그대로)
EXTRA_ITEMS = [
	_Material("BatWing", "박쥐 날개", "BatWing", "흡혈 박쥐의 얇은 날개막. 가볍고 질겨 물약을 달이거나 활을 손볼 때 쓴다."),
	_Material("GoblinFang", "고블린 송곳니", "Fang", "고블린 도적의 날카로운 송곳니. 대장간에서 창끝을 벼릴 때 쓴다."),
	_Material("OldBone", "낡은 뼛조각", "Bone", "해골 궁수가 남긴 뼛조각. 오래 묵어 쇠처럼 단단하다."),
	_Material("Spore", "독버섯 포자", "Spore", "독버섯이 뿜는 보랏빛 포자. 잘 달이면 약이 된다."),
	_Material("CrystalShard", "수정 조각", "Shard", "동굴 유적의 푸른 수정 조각. 마력을 머금어 은은히 빛난다."),
	_Material("GolemCore", "골렘의 핵", "Core", "바위 골렘의 가슴에서 빛나던 핵. 아직도 따뜻하다."),
]
MATERIAL_ORDER = ["Jelly", "BatWing", "GoblinFang", "OldBone", "Spore", "CrystalShard", "GolemCore"]

# 적 종류 → 떨어뜨리는 재료 (확률 0~1, 개수 범위)
MATERIAL_DROPS = [
	("Slime", {"Material": "Jelly", "Chance": 0.5, "Min": 1, "Max": 1}),
	("Bat", {"Material": "BatWing", "Chance": 0.45, "Min": 1, "Max": 1}),
	("Goblin", {"Material": "GoblinFang", "Chance": 0.45, "Min": 1, "Max": 1}),
	("Archer", {"Material": "OldBone", "Chance": 0.5, "Min": 1, "Max": 1}),
	("Mushroom", {"Material": "Spore", "Chance": 0.55, "Min": 1, "Max": 2}),
	("Golem", {"Material": "GolemCore", "Chance": 1.0, "Min": 1, "Max": 1}),
	("CaveBat", {"Material": "BatWing", "Chance": 0.55, "Min": 1, "Max": 2}),
	("CrystalSlime", {"Material": "CrystalShard", "Chance": 0.6, "Min": 1, "Max": 2}),
	("SpiderQueen", {"Material": "CrystalShard", "Chance": 1.0, "Min": 3, "Max": 4}),
]

# 무기 강화: 무기 → [(+1 재료, 골드), (+2 …), (+3 …)] — 공격력 보너스는 기본 피해의 20% / 45% / 75% (반올림, 최소 2/4/6)
UPGRADE_COSTS = {
	"Sword": [("Jelly*1", 30), ("GoblinFang*2+OldBone*1", 80), ("CrystalShard*2+GolemCore*1", 160)],
	"Spear": [("GoblinFang*1+Jelly*1", 40), ("GoblinFang*2+OldBone*2", 90), ("CrystalShard*2+GolemCore*1", 180)],
	"Bow": [("BatWing*2", 35), ("BatWing*2+OldBone*2", 85), ("CrystalShard*3", 170)],
	"Staff": [("Spore*2", 40), ("Spore*2+BatWing*2", 90), ("CrystalShard*3+GolemCore*1", 200)],
	"CrystalSword": [("CrystalShard*2", 80), ("CrystalShard*3+OldBone*2", 150), ("CrystalShard*4+GolemCore*1", 260)],
}
UPGRADE_RATIO = [(0.2, 2), (0.45, 4), (0.75, 6)]

RECIPES = [
	("Potion", {"Result": "Potion", "Count": 1, "Gold": 5, "Materials": ["Spore*1"]}),
	("Ether", {"Result": "Ether", "Count": 1, "Gold": 8, "Materials": ["BatWing*1", "Spore*1"]}),
	("HiPotion", {"Result": "HiPotion", "Count": 1, "Gold": 15, "Materials": ["Jelly*2", "Spore*2"]}),
	("Elixir", {"Result": "Elixir", "Count": 1, "Gold": 60, "Materials": ["CrystalShard*2", "HiPotion*1", "Ether*1"]}),
]

BESTIARY = [  # 적 종류 → (서식지, 설명) — 순서 = 도감 순서
	("Slime", "하르트 들판", "들판 어디서나 볼 수 있는 말랑한 마물. 깡충 뛰어 몸으로 부딪친다. 떨어뜨리는 젤리는 대장장이가 반긴다."),
	("Bat", "들판 숲 그늘", "해 질 녘이면 날아오르는 박쥐. 높이 떠 있다가 급강하하며 침을 뱉는다."),
	("Goblin", "들판 동쪽", "떠돌이 도적 고블린. 몸을 웅크렸다가 단숨에 돌진한다. 웅크리면 옆으로 비켜서자."),
	("Archer", "옛 성채 터", "성채 터를 지키던 병사의 해골. 거리를 두고 활을 당긴다. 시위를 당기는 틈을 노리자."),
	("Mushroom", "연못가·밀밭", "느릿느릿 걷는 독버섯. 보랏빛 포자를 뿜어 땅에 독 웅덩이를 남긴다."),
	("Golem", "들판 동쪽 끝", "들판을 지키던 고대의 바위 골렘. 땅을 내리치고 바위를 던진다. 바닥의 붉은 원을 조심하라."),
	("CaveBat", "동굴 유적", "동굴 깊은 곳에 사는 푸른 박쥐. 들판의 박쥐보다 빠르고 사납다."),
	("CrystalSlime", "동굴 유적", "수정을 머금어 단단해진 슬라임. 몸이 무거워 부딪히면 꽤 아프다."),
	("SpiderQueen", "유적 깊은 곳", "유적 깊은 곳에 둥지를 튼 거대한 거미. 거미줄을 쏘고 수정 가시를 솟게 하며 덮쳐 온다."),
]


# 퀘스트 일지 (Main_<장> = 메인 퀘스트 장(Quests.etable Title이 같은 단계 묶음) 요약, Sub_<id> = 서브 퀘스트 한 줄 목표·보고 안내)
QUEST_JOURNAL = [
	("Main_0", {"Title": "촌장의 부탁", "Summary": "하르트 마을의 촌장 바르톨로가 들판에 들끓는 마물로 골머리를 앓고 있다. 마물을 몰아내 마을 사람들이 다시 밭에 나갈 수 있게 하자.",
				"Objective": "", "Report": ""}),
	("Main_1", {"Title": "고대의 수호자", "Summary": "마물이 날뛰는 까닭은 들판 동쪽 끝에서 깨어난 고대의 바위 골렘이라고 한다. 골렘을 잠재워 들판에 평화를 되찾자.",
				"Objective": "", "Report": ""}),
	("Main_2", {"Title": "동굴 유적의 그림자", "Summary": "골렘이 깨어난 밤부터 폭포 옆 벼랑의 동굴 유적에서 푸른 빛이 새어 나온다. 유적 깊은 곳에 둥지를 튼 것의 정체를 밝혀내자.",
				"Objective": "", "Report": ""}),
	("Main_3", {"Title": "마을의 영웅", "Summary": "들판과 유적의 위협이 사라지고 하르트 마을에 다시 아침이 찾아왔다. 이제 아르펜은 마을의 영웅이다.",
				"Objective": "", "Report": ""}),
	("Sub_Cat", {"Title": "길 잃은 고양이", "Summary": "", "Objective": "연못 동쪽 바위 근처에서 고양이 미미를 찾자", "Report": "리나에게 미미를 데려다주자"}),
	("Sub_Smith", {"Title": "대장간의 담금질", "Summary": "", "Objective": "슬라임에게서 슬라임 젤리를 모으자", "Report": "대장장이 브론에게 젤리를 건네자"}),
	("Sub_Scarecrow", {"Title": "허수아비를 지켜라", "Summary": "", "Objective": "허수아비 근처의 마물을 쓰러뜨리자", "Report": "농부 한스에게 돌아가 보고하자"}),
]


# 메타 설정값 하나 (Meta.edata): 대장장이 말, 타이틀 씬, 기록 가능 거리
META_BALANCE = {
	"ForgeLines": ["어서 오게. 쇠는 두드릴수록 단단해지는 법이지.", "좋아, 한층 날카로워졌군! 손에 쥐어 보게.", "자, 다 됐네. 들판에서 요긴하게 쓰게나.",
				   "재료가 모자라는군. 들판의 마물이 떨어뜨리는 걸 모아 오게.", "골드가 조금 모자라는데… 외상은 안 되네.", "이 이상은 내 솜씨로도 무리일세. 훌륭한 무기야.",
				   "또 오게. 무기는 쓸수록 길이 드는 법이지."],
	"TitleScene": "Scenes/Demo/HD2D.escene", "SaveRadius": 320.0,
}


def UpgradeRows():
	Weapons = dict(G.WEAPONS)
	Rows = []
	for Weapon, Costs in UPGRADE_COSTS.items():
		Base = Weapons[Weapon]["Damage"]
		for Level, ((Mats, Gold), (Ratio, Min)) in enumerate(zip(Costs, UPGRADE_RATIO), start=1):
			Rows.append((f"{Weapon}_{Level}", {"Weapon": Weapon, "Level": Level, "Damage": float(max(Min, round(Base * Ratio))), "Gold": Gold,
											   "Materials": Mats.split("+")}))
	return Rows


def WriteData(Content):
	F = G._Field
	G._Struct(Content, "MaterialDrop", "HD2D 적 재료 드랍 (MaterialDrops.etable, 행 이름 = 적 종류)", [
		F("Material", "String", "", "떨어뜨리는 재료 (Items.etable 행)"),
		F("Chance", "Float", 0.5, "확률 0~1"),
		F("Min", "Int", 1, "개수 최소"),
		F("Max", "Int", 1, "개수 최대"),
	])
	G._Table(Content, "MaterialDrops", "MaterialDrop", MATERIAL_DROPS)
	G._Struct(Content, "Upgrade", "HD2D 무기 강화 단계 (Upgrades.etable, 행 이름 = <무기>_<단계>)", [
		F("Weapon", "String", "", "무기 id (Weapons.etable)"),
		F("Level", "Int", 1, "강화 단계 (+1~+3)"),
		F("Damage", "Float", 0, "이 단계의 공격력 보너스 (기본 피해에 더함 — 누적 값)"),
		F("Gold", "Int", 0, "비용 골드"),
		F("Materials", "Array", [], "재료 (아이템*개수)", Element="String"),
	])
	G._Table(Content, "Upgrades", "Upgrade", UpgradeRows())
	G._Struct(Content, "Recipe", "HD2D 물약 조합 (Recipes.etable, 행 이름 = 조합 id)", [
		F("Result", "String", "", "만들어지는 아이템"),
		F("Count", "Int", 1, "만들어지는 개수"),
		F("Gold", "Int", 0, "비용 골드"),
		F("Materials", "Array", [], "재료 (아이템*개수 — 소모품도 된다)", Element="String"),
	])
	G._Table(Content, "Recipes", "Recipe", RECIPES)
	G._Struct(Content, "QuestJournal", "HD2D 퀘스트 일지 (QuestJournal.etable, 행 이름 = Main_<장 번호> / Sub_<서브 퀘스트 id>)", [
		F("Title", "String", "", "퀘스트 이름 (메인 장은 Quests.etable Title과 같아야 이어진다)"),
		F("Summary", "String", "", "일지 설명 (메인 장)"),
		F("Objective", "String", "", "한 줄 목표 (서브 퀘스트 — HUD 추적 칸)"),
		F("Report", "String", "", "보고할 수 있을 때 목표 (서브 퀘스트)"),
	])
	G._Table(Content, "QuestJournal", "QuestJournal", QUEST_JOURNAL)
	G._Struct(Content, "MetaBalance", "HD2D 메타 시스템 값 (Meta.edata 하나)", [
		F("ForgeLines", "Array", [], "대장장이 말: 들어옴 / 강화 / 조합 / 재료 부족 / 골드 부족 / 최대 단계 / 나감", Element="String"),
		F("TitleScene", "String", "Scenes/Demo/HD2D.escene", "일시정지 메뉴 \"타이틀로\"가 여는 씬 (타이틀이 있는 맵)"),
		F("SaveRadius", "Float", 320.0, "일시정지 메뉴에서 기록할 수 있는 게시판까지 거리 (cm)"),
	])
	G._WriteJson(os.path.join(Content, *DATA.split("/"), "Meta.edata"), {"Version": 1, "Struct": f"{DATA}/MetaBalance.estruct", "Values": META_BALANCE})
	Rows = dict(G.ENEMIES)
	Pictures = []
	for Kind, Habitat, Desc in BESTIARY:
		Look = Rows[Kind]["Look"] or Kind
		W, H = HD2DMetaArt.WritePicture(os.path.join(Content, *UI_DIR.split("/"), "Bestiary", f"{Kind}.png"), Look, Rows[Kind]["Tint"])
		Pictures.append((Kind, {"Habitat": Habitat, "Description": Desc, "Picture": f"{UI_DIR}/Bestiary/{Kind}.png", "PictureW": W, "PictureH": H}))
	G._Struct(Content, "BestiaryEntry", "HD2D 도감 (Bestiary.etable, 행 이름 = 적 종류 — 순서 = 도감 순서)", [
		F("Habitat", "String", "", "서식지"),
		F("Description", "String", "", "설명"),
		F("Picture", "String", "", "도감 그림 (UI 텍스처)"),
		F("PictureW", "Int", 128, "그림 너비 (UI 단위)"),
		F("PictureH", "Int", 128, "그림 높이"),
	])
	G._Table(Content, "Bestiary", "BestiaryEntry", Pictures)


# ================================================================ UI
TL, TG, TD, TC = G.TEXT_LIGHT, G.TEXT_GOLD, G.TEXT_DIM, G.TEXT_CYAN
GOLD_LINE = (0.96, 0.77, 0.3, 0.7)
CLEAR = (0, 0, 0, 0)


def W(*Args, **Kw):
	return G.Widget(*Args, **Kw)


def T(Name, Value, Size, Slot=None, Color=TL, Justify="Left", Visibility="HitTestInvisible", Wrap=False, Outline=2):
	return G.Text(Name, Value, Size, Slot, Color, Justify, Visibility, Wrap, Outline)


def Img(Name, Texture, Size, Slot=None, Visibility="HitTestInvisible"):
	return G.Img(Name, Texture, Size, Slot, Visibility)


def At(X, Y, Wd=0, Hd=0, Anchor=(0, 0), Align=(0, 0), Auto=None, Z=1):
	return G.CanvasSlot(Anchor, X, Y, Wd, Hd, Align, (Wd == 0 and Hd == 0) if Auto is None else Auto, Z)


def Underlay(Name, Alpha=0.42):
	# 창틀(반투명 남색) 아래 한 겹 더 — 메뉴 뒤 장면·타이틀 글자가 비쳐 읽기 어렵지 않게 (창틀 가장자리 8px 안쪽)
	return W("Border", Name, G.CanvasSlot((0, 0), 0, 0, 0, 0, (0, 0), False, 0) | {"AnchorMax": [1, 1], "Offsets": [8, 8, 8, 8]}, "HitTestInvisible",
			 Brush=G.Brush((0.015, 0.012, 0.05, Alpha), 6), ContentPadding=[0, 0, 0, 0])


def Rule(Name, X, Y, Wd):
	return W("Border", Name, At(X, Y, Wd, 2), Brush=G.Brush(GOLD_LINE), ContentPadding=[0, 0, 0, 0])


def RowButton(Name, Children, Wd, Hd, Pad=(0, 0, 0, 2)):
	# 선택 줄 (옥토패스풍): 선택 띠(Sel) + 커서 + 내용 — 마우스 올리기/누르기 밝기
	return W("Button", Name, G.BoxSlot(Pad), "Visible", [
		W("Overlay", f"{Name}Box", G.BoxSlot(), "SelfHitTestInvisible", [
			W("Image", f"{Name}Sel", G.BoxSlot(), "Collapsed", Brush=G.Brush(Texture=f"{UI_DIR}/Select.png"), ImageSize=[0, 0]),
			W("HorizontalBox", f"{Name}Items", G.BoxSlot((8, 0, 12, 0)), "HitTestInvisible", [
				W("Image", f"{Name}Cursor", G.BoxSlot((0, 0, 8, 0), VAlign="Center"), "Hidden", Brush=G.Brush(Texture=f"{UI_DIR}/Cursor.png"),
				  ImageSize=[18, 18]),
			] + Children),
		]),
	], Brush=G.Brush(CLEAR), HoveredBrush=G.Brush((1.0, 0.85, 0.5, 0.08)), PressedBrush=G.Brush((1.0, 0.85, 0.5, 0.16)),
		DisabledBrush=G.Brush(CLEAR), ContentPadding=[0, 0, 0, 0], MinSize=[Wd, Hd])


def Tabs(Prefix, Names, X, Y, Wd=150):
	return [W("Border", f"{Prefix}{K}", At(X + K * (Wd + 8), Y, Wd, 36), "HitTestInvisible",
			  [T(f"{Prefix}Text{K}", Name, 20, G.BoxSlot(HAlign="Center", VAlign="Center"), TD, "Center")],
			  Brush=G.Brush((0.1, 0.09, 0.18, 0.7), 3, 1, (0.96, 0.77, 0.3, 0.5)), ContentPadding=[0, 0, 0, 0]) for K, Name in enumerate(Names)]


def StatBar(Prefix, Label, LabelColor, Fill, Wd, Hd, TextSize):
	return W("HorizontalBox", f"{Prefix}Row", G.BoxSlot((0, 0, 0, 8)), "HitTestInvisible", [
		T(f"{Prefix}Label", Label, 17, G.BoxSlot((0, 0, 10, 0), VAlign="Center"), LabelColor),
		W("Overlay", f"{Prefix}Box", G.BoxSlot(VAlign="Center", Size="Fill"), "HitTestInvisible", [
			G.Bar(f"{Prefix}Bar", Wd, Hd, Fill),
			T(f"{Prefix}Text", "", TextSize, G.BoxSlot(HAlign="Center", VAlign="Center"), Justify="Center"),
		]),
	])


NAV = [  # (아이콘, 이름) — 순서 = HD2DPause.lua NAV
	("Resume", "재개"), ("Items", "소지품"), ("Equip", "장비"), ("Journal", "퀘스트 일지"), ("Map", "지도"), ("Bestiary", "도감"),
	("Records", "모험 기록"), ("Settings", "설정"), ("Save", "기록하기"), ("Title", "타이틀로"),
]
PAGE_X, PAGE_Y, PAGE_W, PAGE_H = 356, 36, 888, 648
JOURNAL_ROWS, BEST_ROWS, REC_ROWS, SET_ROWS = 8, 9, 7, 6
MAP_MARKERS, MAP_LABELS, MINI_MARKERS = 40, 12, 20
MAP_AREA = (28, 82, 832, 400)  # 지도 쪽 그림 영역 (쪽 좌표)
MINI = (252, 168, 12)          # 미니맵 창 크기 + 안쪽 여백


def PageHome():
	Stats = [W("HorizontalBox", f"HomeStat{K}", G.BoxSlot((0, 0, 0, 7)), "HitTestInvisible", [
		Img(f"HomeStatIcon{K}", MetaIcon("Star"), 22, G.BoxSlot((0, 0, 10, 0), VAlign="Center")),
		T(f"HomeStatName{K}", "", 19, G.BoxSlot(VAlign="Center", Size="Fill"), TD),
		T(f"HomeStatVal{K}", "", 20, G.BoxSlot(VAlign="Center"), TL, "Right"),
	]) for K in range(6)]
	Gear = [W("HorizontalBox", f"HomeGear{K}", G.BoxSlot((0, 0, 0, 10)), "HitTestInvisible", [
		T(f"HomeGearSlot{K}", "", 16, G.BoxSlot((0, 0, 12, 0), VAlign="Center"), TD),
		Img(f"HomeGearIcon{K}", G.Icon("Sword"), 34, G.BoxSlot((0, 0, 10, 0), VAlign="Center")),
		T(f"HomeGearName{K}", "", 20, G.BoxSlot(VAlign="Center"), TL),
		T(f"HomeGearPlus{K}", "", 20, G.BoxSlot((8, 0, 0, 0), VAlign="Center"), TG),
	]) for K in range(3)]
	return W("Canvas", "PgHome", G.StretchSlot(), "Collapsed", [
		W("Border", "HomePortraitFrame", At(32, 86, 150, 150), "HitTestInvisible",
		  [Img("HomePortrait", f"{UI_DIR}/Portraits/Hero.png", 126, G.BoxSlot(HAlign="Center", VAlign="Center"))],
		  Brush=G.FrameBrush(True, 36), ContentPadding=[10, 10, 10, 10]),
		T("HomeName", "아르펜", 30, At(206, 84), TG),
		T("HomeLevel", "Lv 1", 22, At(206, 126), TL),
		T("HomeNext", "", 16, At(290, 131), TD),
		W("VerticalBox", "HomeBars", At(206, 166, 280, 96), "HitTestInvisible", [
			StatBar("HomeHp", "HP", (1.0, 0.62, 0.55, 1), (0.86, 0.24, 0.24, 1), 240, 18, 15),
			StatBar("HomeMp", "MP", (0.6, 0.78, 1.0, 1), (0.3, 0.52, 0.98, 1), 240, 14, 13),
			StatBar("HomeExp", "EXP", TG, (1.0, 0.82, 0.35, 1), 240, 6, 1),
		]),
		T("HomeStatsHead", "능력치", 20, At(530, 84), TG),
		Rule("HomeStatsRule", 530, 114, 330),
		W("VerticalBox", "HomeStats", At(530, 126, 330, 180), "HitTestInvisible", Stats),
		T("HomeGearHead", "장비", 20, At(32, 300), TG),
		Rule("HomeGearRule", 32, 330, 450),
		W("VerticalBox", "HomeGear", At(32, 344, 450, 140), "HitTestInvisible", Gear),
		T("HomeQuestHead", "추적 중인 퀘스트", 20, At(530, 400), TG),
		Rule("HomeQuestRule", 530, 430, 330),
		W("HorizontalBox", "HomeQuestTitleRow", At(530, 442), "HitTestInvisible", [
			Img("HomeQuestIcon", MetaIcon("Crown"), 26, G.BoxSlot((0, 0, 8, 0), VAlign="Center")),
			T("HomeQuestTitle", "", 20, G.BoxSlot(VAlign="Center"), TL),
		]),
		T("HomeQuestText", "", 18, At(530, 480, 330, 90), TD, Wrap=True),
		Rule("HomeDescRule", 28, 546, 832),
		T("HomeMenuDesc", "", 18, At(32, 560, 824, 50), TC, Wrap=True),
	])


def PageJournal():
	Rows = [RowButton(f"JRow{I}", [
		Img(f"JRowIcon{I}", MetaIcon("Crown"), 26, G.BoxSlot((0, 0, 10, 0), VAlign="Center")),
		T(f"JRowName{I}", "", 19, G.BoxSlot(VAlign="Center", Size="Fill")),
		T(f"JRowTag{I}", "", 15, G.BoxSlot((8, 0, 0, 0), VAlign="Center"), TG, "Right"),
	], 372, 44) for I in range(JOURNAL_ROWS)]
	return W("Canvas", "PgJournal", G.StretchSlot(), "Collapsed", Tabs("JTab", ["메인 퀘스트", "서브 퀘스트"], 32, 80) + [
		T("JCount", "", 17, At(-32, 88, Anchor=(1, 0), Align=(1, 0)), TD, "Right"),
		W("VerticalBox", "JList", At(28, 130, 380, 380), "SelfHitTestInvisible", Rows),
		T("JEmpty", "받은 의뢰가 없다", 19, At(48, 140), TD, Visibility="Collapsed"),
		W("Border", "JDetail", At(420, 130, 440, 452), "HitTestInvisible", [
			W("VerticalBox", "JDetailBox", G.BoxSlot(), "HitTestInvisible", [
				W("HorizontalBox", "JHead", G.BoxSlot((0, 0, 0, 12)), "HitTestInvisible", [
					Img("JPortrait", f"{UI_DIR}/Portraits/Elder.png", 56, G.BoxSlot((0, 0, 14, 0), VAlign="Center")),
					W("VerticalBox", "JNames", G.BoxSlot(VAlign="Center"), "HitTestInvisible", [
						T("JTitle", "", 23, G.BoxSlot(), TG),
						T("JGiver", "", 16, G.BoxSlot((0, 4, 0, 0)), TD),
					]),
				]),
				T("JState", "", 17, G.BoxSlot((0, 0, 0, 10)), TC),
				T("JDesc", "", 18, G.BoxSlot((0, 0, 0, 14)), TL, Wrap=True),
				T("JGoalHead", "목표", 17, G.BoxSlot((0, 0, 0, 4)), TG),
				T("JGoal", "", 18, G.BoxSlot((0, 0, 0, 14)), TL, Wrap=True),
				T("JRewardHead", "보상", 17, G.BoxSlot((0, 0, 0, 4)), TG),
				T("JReward", "", 18, G.BoxSlot((0, 0, 0, 12)), TL, Wrap=True),
				T("JTrack", "", 17, G.BoxSlot(), TG),
			]),
		], Brush=G.FrameBrush(True, 36), ContentPadding=[22, 18, 22, 16]),
	])


def PageMap():
	X, Y, Wd, Hd = MAP_AREA
	Children = [
		W("Border", "MapBack", At(X - 6, Y - 6, Wd + 12, Hd + 12), "HitTestInvisible", Brush=G.Brush((0.05, 0.04, 0.08, 0.6), 4, 2, (0.6, 0.46, 0.2, 0.9)),
		  ContentPadding=[0, 0, 0, 0]),
		W("Image", "MapImg", At(X, Y, Wd, 280, Z=2), "HitTestInvisible", Brush=G.Brush(Texture=f"{UI_DIR}/Maps/Village.png"), ImageSize=[Wd, 280]),
		W("Border", "MapFrameLine", At(X, Y, Wd, 280, Z=3), "HitTestInvisible", Brush=G.Brush(CLEAR, 0, 2, (0.24, 0.16, 0.08, 1)), ContentPadding=[0, 0, 0, 0]),
		T("MapNoMap", "이 지역의 지도는 없다", 20, At(X + Wd // 2, Y + Hd // 2, Align=(0.5, 0.5)), TD, "Center", "Collapsed"),
	]
	for I in range(MAP_LABELS):
		Children.append(T(f"MapLbl{I}", "", 16, At(0, 0, Align=(0.5, 0.5), Z=4), (1.0, 0.96, 0.84, 1), "Center", "Collapsed"))
	for I in range(MAP_MARKERS):
		Children.append(W("Image", f"MapMk{I}", At(0, 0, 22, 22, Align=(0.5, 0.5), Z=5), "Collapsed", Brush=G.Brush(Texture=MetaIcon("MkNpc")),
						  ImageSize=[22, 22]))
	Children.append(W("Image", "MapPlayer", At(0, 0, 30, 30, Align=(0.5, 0.85), Z=6), "Collapsed", Brush=G.Brush(Texture=MetaIcon("MkPlayer")),
					  ImageSize=[30, 30]))
	Legend = []
	for K, (Icon, Name) in enumerate((("MkPlayer", "현재 위치"), ("MkGoal", "목표"), ("MkNpcQuest", "의뢰"), ("MkNpc", "마을 사람"),
									   ("MkChest", "보물상자"), ("MkSave", "기록 장소"), ("MkExit", "출구"), ("MkBoss", "강적"))):
		Legend += [Img(f"MapLegIcon{K}", MetaIcon(Icon), 20, G.BoxSlot((0 if K == 0 else 18, 0, 6, 0), VAlign="Center")),
				   T(f"MapLegText{K}", Name, 16, G.BoxSlot(VAlign="Center"), TD)]
	Children += [
		W("HorizontalBox", "MapLegend", At(32, 546), "HitTestInvisible", Legend),
		W("HorizontalBox", "MapGoalRow", At(32, 500), "HitTestInvisible", [
			Img("MapGoalIcon", MetaIcon("MkGoal"), 22, G.BoxSlot((0, 0, 8, 0), VAlign="Center")),
			T("MapGoalText", "", 18, G.BoxSlot(VAlign="Center"), TL),
		]),
	]
	return W("Canvas", "PgMap", G.StretchSlot(), "Collapsed", Children)


def PageBestiary():
	Rows = [RowButton(f"BRow{I}", [
		Img(f"BRowIcon{I}", MetaIcon("Lock"), 28, G.BoxSlot((0, 0, 10, 0), VAlign="Center")),
		T(f"BRowName{I}", "", 19, G.BoxSlot(VAlign="Center", Size="Fill")),
		T(f"BRowCount{I}", "", 16, G.BoxSlot((8, 0, 0, 0), VAlign="Center"), TG, "Right"),
	], 322, 42) for I in range(BEST_ROWS)]
	Stats = [W("HorizontalBox", f"BStat{K}", At(212, 104 + K * 34), "HitTestInvisible", [
		Img(f"BStatIcon{K}", MetaIcon("Skull"), 22, G.BoxSlot((0, 0, 8, 0), VAlign="Center")),
		T(f"BStatText{K}", "", 18, G.BoxSlot(VAlign="Center"), TL),
	]) for K in range(3)]
	return W("Canvas", "PgBest", G.StretchSlot(), "Collapsed", Tabs("BTab", ["마물", "물건"], 32, 80) + [
		W("VerticalBox", "BList", At(28, 130, 330, 400), "SelfHitTestInvisible", Rows),
		T("BMore", "", 16, At(48, 534), TD),
		W("Canvas", "BDetail", At(372, 130, 488, 452), "HitTestInvisible", [
			W("Border", "BDetailBg", G.StretchSlot(), "HitTestInvisible", Brush=G.FrameBrush(True, 36), ContentPadding=[0, 0, 0, 0]),
			W("Border", "BPicFrame", At(20, 20, 176, 176), "HitTestInvisible", Brush=G.Brush((0.04, 0.03, 0.08, 0.55), 4, 1, (0.96, 0.77, 0.3, 0.45)),
			  ContentPadding=[0, 0, 0, 0]),
			W("Image", "BPic", At(108, 108, 144, 120, Align=(0.5, 0.5), Z=2), "HitTestInvisible", Brush=G.Brush(Texture=f"{UI_DIR}/Bestiary/Slime.png"),
			  ImageSize=[144, 120]),
			T("BName", "", 24, At(212, 22), TG),
			T("BKind", "", 16, At(212, 60), TD),
		] + Stats + [
			W("VerticalBox", "BLower", At(22, 212, 444, 230), "HitTestInvisible", [
				T("BDesc", "", 18, G.BoxSlot((0, 0, 0, 14)), TL, Wrap=True),
				T("BDrop", "", 17, G.BoxSlot((0, 0, 0, 8)), TC, Wrap=True),
				T("BWeak", "", 17, G.BoxSlot(), (1.0, 0.75, 0.55, 1), Wrap=True),
			]),
		]),
	])


def PageRecords():
	def Column(Prefix, X):
		Rows = [W("Border", f"{Prefix}{K}", G.BoxSlot((0, 0, 0, 6)), "HitTestInvisible", [
			W("HorizontalBox", f"{Prefix}Box{K}", G.BoxSlot(), "HitTestInvisible", [
				Img(f"{Prefix}Icon{K}", MetaIcon("Star"), 24, G.BoxSlot((0, 0, 12, 0), VAlign="Center")),
				T(f"{Prefix}Name{K}", "", 18, G.BoxSlot(VAlign="Center", Size="Fill"), TD),
				T(f"{Prefix}Val{K}", "", 20, G.BoxSlot(VAlign="Center"), TL, "Right"),
			]),
		], Brush=G.Brush((1.0, 0.9, 0.7, 0.05 if K % 2 == 0 else 0.0), 3), ContentPadding=[12, 7, 14, 7], MinSize=[390, 0]) for K in range(REC_ROWS)]
		return W("VerticalBox", f"{Prefix}Col", At(X, 84, 400, 320), "HitTestInvisible", Rows)
	return W("Canvas", "PgRec", G.StretchSlot(), "Collapsed", [
		Column("RecL", 32), Column("RecR", 456),
		T("RecKindsHead", "마물 토벌 기록", 20, At(32, 440), TG),
		Rule("RecKindsRule", 32, 470, 824),
		T("RecKinds", "", 18, At(32, 484, 824, 110), TL, Wrap=True),
	])


def PageSettings():
	Rows = [RowButton(f"SRow{I}", [
		Img(f"SRowIcon{I}", MetaIcon("Sound"), 28, G.BoxSlot((0, 0, 12, 0), VAlign="Center")),
		T(f"SRowName{I}", "", 21, G.BoxSlot(VAlign="Center", Size="Fill")),
		T(f"SRowL{I}", "◀", 18, G.BoxSlot((0, 0, 10, 0), VAlign="Center"), TG),
		W("Overlay", f"SRowValBox{I}", G.BoxSlot(VAlign="Center"), "HitTestInvisible", [
			W("ProgressBar", f"SRowBar{I}", G.BoxSlot(VAlign="Center"), "Collapsed", MinSize=[170, 14],
			  Brush=G.Brush((0.05, 0.04, 0.08, 0.9), 2, 1, (0, 0, 0, 1)), FillBrush=G.Brush((1.0, 0.8, 0.35, 1), 1), Percent=1.0, FillDirection="LeftToRight"),
			T(f"SRowVal{I}", "", 20, G.BoxSlot(HAlign="Center", VAlign="Center"), TL, "Center"),
		], MinSize=[200, 30]),
		T(f"SRowR{I}", "▶", 18, G.BoxSlot((10, 0, 0, 0), VAlign="Center"), TG),
	], 808, 56, (0, 0, 0, 6)) for I in range(SET_ROWS)]
	return W("Canvas", "PgSet", G.StretchSlot(), "Collapsed", [
		W("VerticalBox", "SList", At(40, 90, 808, 380), "SelfHitTestInvisible", Rows),
		W("Border", "SDescBox", At(40, 482, 808, 92), "HitTestInvisible", [
			T("SDesc", "", 18, G.BoxSlot(VAlign="Center"), TL, Wrap=True),
		], Brush=G.FrameBrush(True, 30), ContentPadding=[20, 12, 20, 12]),
	])


def PauseScreen():
	Nav = [RowButton(f"PNav{I}", [
		Img(f"PNavIcon{I}", MetaIcon(Icon), 28, G.BoxSlot((0, 0, 12, 0), VAlign="Center")),
		T(f"PNavText{I}", Name, 21, G.BoxSlot(VAlign="Center", Size="Fill")),
	], 268, 40) for I, (Icon, Name) in enumerate(NAV)]
	Info = [W("HorizontalBox", f"PInfo{K}", At(30, 516 + K * 30), "HitTestInvisible", [
		Img(f"PInfoIcon{K}", MetaIcon(Icon), 22, G.BoxSlot((0, 0, 10, 0), VAlign="Center")),
		T(f"PInfoText{K}", "", 18, G.BoxSlot(VAlign="Center"), Color),
	]) for K, (Icon, Color) in enumerate((("Pin", TL), ("Time", TL)))]
	Info.append(W("HorizontalBox", "PInfo2", At(30, 576), "HitTestInvisible", [
		Img("PInfoIcon2", G.Icon("Coin"), 22, G.BoxSlot((0, 0, 10, 0), VAlign="Center")),
		T("PInfoText2", "", 18, G.BoxSlot(VAlign="Center"), TG),
	]))
	return W("Canvas", "PauseScreen", G.StretchSlot(10), "Collapsed", [
		W("Border", "PauseShade", G.StretchSlot(), "Visible", Brush=G.Brush((0.01, 0.0, 0.04, 0.5)), ContentPadding=[0, 0, 0, 0]),
		W("Canvas", "PauseNav", At(36, 36, 300, 648), "SelfHitTestInvisible", [
			Underlay("PauseNavUnder"),
			W("Border", "PauseNavBg", G.StretchSlot(), "Visible", Brush=G.FrameBrush(), ContentPadding=[0, 0, 0, 0]),
			T("PauseNavTitle", "메뉴", 30, At(30, 20), TG),
			Rule("PauseNavRule", 24, 64, 252),
			W("VerticalBox", "PauseNavList", At(16, 76, 268, 424), "SelfHitTestInvisible", Nav),
			Rule("PauseNavRule2", 24, 502, 252),
		] + Info),
		W("Canvas", "PausePage", At(PAGE_X, PAGE_Y, PAGE_W, PAGE_H), "SelfHitTestInvisible", [
			Underlay("PausePageUnder"),
			W("Border", "PausePageBg", G.StretchSlot(), "Visible", Brush=G.FrameBrush(), ContentPadding=[0, 0, 0, 0]),
			W("HorizontalBox", "PageHead", At(30, 20), "HitTestInvisible", [
				Img("PageIcon", MetaIcon("Resume"), 30, G.BoxSlot((0, 0, 12, 0), VAlign="Center")),
				T("PageTitle", "", 30, G.BoxSlot(VAlign="Center"), TG),
			]),
			T("PageSub", "", 18, At(-32, 30, Anchor=(1, 0), Align=(1, 0)), TD, "Right"),
			Rule("PageRule", 28, 64, 832),
			PageHome(), PageJournal(), PageMap(), PageBestiary(), PageRecords(), PageSettings(),
			T("PageHint", "", 16, At(0, -16, Anchor=(0.5, 1), Align=(0.5, 1)), TD, "Center"),
		]),
	])


def SlotScreen():
	Cards = []
	for I in range(3):
		Cards.append(W("Button", f"SlotCard{I}", G.BoxSlot((0, 0, 0, 12)), "Visible", [
			W("Overlay", f"SlotBox{I}", G.BoxSlot(), "SelfHitTestInvisible", [
				W("Border", f"SlotBg{I}", G.BoxSlot(), "HitTestInvisible", Brush=G.FrameBrush(True, 36), ContentPadding=[0, 0, 0, 0]),
				W("Image", f"SlotSel{I}", G.BoxSlot((10, 10, 10, 10)), "Collapsed", Brush=G.Brush(Texture=f"{UI_DIR}/Select.png"), ImageSize=[0, 0]),
				W("Canvas", f"SlotContent{I}", G.BoxSlot(), "HitTestInvisible", [
					W("Image", f"SlotCursor{I}", At(14, 0, 20, 20, Anchor=(0, 0.5), Align=(0, 0.5)), "Hidden", Brush=G.Brush(Texture=f"{UI_DIR}/Cursor.png"),
					  ImageSize=[20, 20]),
					T(f"SlotNum{I}", f"기록 {I + 1}", 24, At(44, 18), TG),
					W("HorizontalBox", f"SlotMapRow{I}", At(44, 56), "HitTestInvisible", [
						Img(f"SlotMapIcon{I}", MetaIcon("Pin"), 20, G.BoxSlot((0, 0, 8, 0), VAlign="Center")),
						T(f"SlotMap{I}", "", 19, G.BoxSlot(VAlign="Center"), TL),
					]),
					W("HorizontalBox", f"SlotQuestRow{I}", At(44, 86), "HitTestInvisible", [
						Img(f"SlotQuestIcon{I}", MetaIcon("Crown"), 20, G.BoxSlot((0, 0, 8, 0), VAlign="Center")),
						T(f"SlotQuest{I}", "", 17, G.BoxSlot(VAlign="Center"), TD),
					]),
					T(f"SlotLv{I}", "", 22, At(-28, 16, Anchor=(1, 0), Align=(1, 0)), TL, "Right"),
					W("HorizontalBox", f"SlotTimeRow{I}", At(-28, 52, Anchor=(1, 0), Align=(1, 0)), "HitTestInvisible", [
						Img(f"SlotTimeIcon{I}", MetaIcon("Time"), 20, G.BoxSlot((0, 0, 8, 0), VAlign="Center")),
						T(f"SlotTime{I}", "", 18, G.BoxSlot(VAlign="Center"), TL),
					]),
					W("HorizontalBox", f"SlotGoldRow{I}", At(-28, 84, Anchor=(1, 0), Align=(1, 0)), "HitTestInvisible", [
						Img(f"SlotGoldIcon{I}", G.Icon("Coin"), 20, G.BoxSlot((0, 0, 8, 0), VAlign="Center")),
						T(f"SlotGold{I}", "", 18, G.BoxSlot(VAlign="Center"), TG),
					]),
					T(f"SlotEmpty{I}", "― 비어 있음 ―", 21, At(0, 0, Anchor=(0.5, 0.5), Align=(0.5, 0.5)), TD, "Center", "Collapsed"),
				]),
			]),
		], Brush=G.Brush(CLEAR), HoveredBrush=G.Brush((1.0, 0.85, 0.5, 0.06)), PressedBrush=G.Brush((1.0, 0.85, 0.5, 0.12)),
			DisabledBrush=G.Brush(CLEAR), ContentPadding=[0, 0, 0, 0], MinSize=[760, 124]))
	return W("Canvas", "SlotScreen", G.StretchSlot(20), "Collapsed", [
		W("Border", "SlotShade", G.StretchSlot(), "Visible", Brush=G.Brush((0.01, 0.0, 0.04, 0.5)), ContentPadding=[0, 0, 0, 0]),
		W("Canvas", "SlotWindow", At(0, 0, 820, 560, Anchor=(0.5, 0.5), Align=(0.5, 0.5)), "SelfHitTestInvisible", [
			Underlay("SlotWindowUnder", 0.9),
			W("Border", "SlotWindowBg", G.StretchSlot(), "Visible", Brush=G.FrameBrush(), ContentPadding=[0, 0, 0, 0]),
			W("HorizontalBox", "SlotHead", At(30, 20), "HitTestInvisible", [
				Img("SlotHeadIcon", MetaIcon("Save"), 30, G.BoxSlot((0, 0, 12, 0), VAlign="Center")),
				T("SlotTitle", "", 30, G.BoxSlot(VAlign="Center"), TG),
			]),
			T("SlotSub", "", 18, At(-32, 30, Anchor=(1, 0), Align=(1, 0)), TD, "Right"),
			Rule("SlotRule", 28, 64, 764),
			W("VerticalBox", "SlotList", At(30, 84, 760, 412), "SelfHitTestInvisible", Cards),
			T("SlotHint", "W/S 선택     E · J 결정     ESC · Space 돌아가기", 16, At(0, -16, Anchor=(0.5, 1), Align=(0.5, 1)), TD, "Center"),
		]),
	])


def ConfirmBox():
	def Btn(K, Label):
		return W("Button", f"ConfirmBtn{K}", G.BoxSlot((10, 0, 10, 0)), "Visible", [
			W("HorizontalBox", f"ConfirmBtnBox{K}", G.BoxSlot(HAlign="Center", VAlign="Center"), "HitTestInvisible", [
				W("Image", f"ConfirmCursor{K}", G.BoxSlot((0, 0, 10, 0), VAlign="Center"), "Hidden", Brush=G.Brush(Texture=f"{UI_DIR}/Cursor.png"),
				  ImageSize=[18, 18]),
				T(f"ConfirmText{K}", Label, 22, G.BoxSlot(VAlign="Center"), TL, "Center"),
			]),
		], Brush=G.Brush((0.1, 0.09, 0.18, 0.7), 3, 1, (0.96, 0.77, 0.3, 0.5)), HoveredBrush=G.Brush((0.3, 0.22, 0.12, 0.9), 3, 1, (0.96, 0.77, 0.3, 0.8)),
			PressedBrush=G.Brush((0.4, 0.3, 0.14, 0.95), 3, 1, (0.96, 0.77, 0.3, 1)), DisabledBrush=G.Brush(CLEAR), ContentPadding=[18, 6, 18, 6],
			MinSize=[160, 46])
	return W("Canvas", "ConfirmScreen", G.StretchSlot(30), "Collapsed", [
		W("Border", "ConfirmShade", G.StretchSlot(), "Visible", Brush=G.Brush((0.0, 0.0, 0.02, 0.45)), ContentPadding=[0, 0, 0, 0]),
		W("Canvas", "ConfirmWindow", At(0, 0, 620, 230, Anchor=(0.5, 0.5), Align=(0.5, 0.5)), "SelfHitTestInvisible", [
			Underlay("ConfirmUnder", 0.92),
			W("Border", "ConfirmBg", G.StretchSlot(), "Visible", Brush=G.FrameBrush(), ContentPadding=[0, 0, 0, 0]),
			T("ConfirmTitle", "", 24, At(0, 26, Anchor=(0.5, 0), Align=(0.5, 0)), TG, "Center"),
			T("ConfirmText", "", 19, At(40, 70, 540, 70), TL, "Center", Wrap=True),
			W("HorizontalBox", "ConfirmButtons", At(0, -26, Anchor=(0.5, 1), Align=(0.5, 1)), "SelfHitTestInvisible", [Btn(0, "예"), Btn(1, "아니요")]),
		]),
	])


def Minimap():
	BW, BH, Pad = MINI
	Content = [W("Image", "MiniImg", At(0, 0, 400, 140), "HitTestInvisible", Brush=G.Brush(Texture=f"{UI_DIR}/Maps/Village.png"), ImageSize=[400, 140])]
	for I in range(MINI_MARKERS):
		Content.append(W("Image", f"MiniMk{I}", At(0, 0, 16, 16, Align=(0.5, 0.5), Z=2), "Collapsed", Brush=G.Brush(Texture=MetaIcon("MkNpc")), ImageSize=[16, 16]))
	return W("Canvas", "Minimap", At(-16, 14, BW, BH, Anchor=(1, 0), Align=(1, 0), Z=0), "Collapsed", [
		W("Border", "MiniBack", At(Pad, Pad, BW - Pad * 2, BH - Pad * 2, Z=0), "HitTestInvisible", Brush=G.Brush((0.06, 0.05, 0.09, 1)),
		  ContentPadding=[0, 0, 0, 0]),
		W("ScrollBox", "MiniClip", At(Pad, Pad, BW - Pad * 2, BH - Pad * 2, Z=1), "HitTestInvisible", [
			W("Canvas", "MiniContent", G.BoxSlot(), "HitTestInvisible", Content, MinSize=[BW - Pad * 2, BH - Pad * 2]),
		], ScrollbarWidth=0.0, ScrollbarColor=[0, 0, 0, 0]),
		W("Image", "MiniPlayer", At(BW / 2, BH / 2, 22, 22, Align=(0.5, 0.85), Z=3), "HitTestInvisible", Brush=G.Brush(Texture=MetaIcon("MkPlayer")),
		  ImageSize=[22, 22]),
		W("Border", "MiniFrame", G.StretchSlot(4), "HitTestInvisible", Brush=G.Brush(Texture=f"{UI_DIR}/FrameOpen.png", NineSlice=True, TextureSize=36),
		  ContentPadding=[0, 0, 0, 0]),
		T("MiniName", "", 15, At(0, -2, Anchor=(0.5, 1), Align=(0.5, 1), Z=5), (1.0, 0.92, 0.7, 1), "Center"),
	])


def WriteUi(Content):
	Folder = os.path.join(Content, *UI_DIR.split("/"))
	HD2DMetaArt.WriteIcons(Folder)
	HD2DMetaArt.WriteOpenFrame(os.path.join(Folder, "FrameOpen.png"))
	Forge = G.MenuWindow("Forge", "브론의 대장간", "A/D 탭     W/S 선택     E · J 결정     ESC · Space 나가기", 16, Tabs=["무기 강화", "물약 조합"])
	Forge["Children"].insert(0, Underlay("ForgeUnder", 0.6))
	Root = W("Canvas", "MetaRoot", None, "SelfHitTestInvisible", [
		Minimap(),
		W("Border", "ForgeShade", G.StretchSlot(15), "Collapsed", Brush=G.Brush((0.01, 0.0, 0.04, 0.55)), ContentPadding=[0, 0, 0, 0]),
		Forge, PauseScreen(), SlotScreen(), ConfirmBox(),
	])
	G._WriteJson(os.path.join(Folder, "Meta.eui"), {"Version": 2, "DesignSize": [1280, 720], "ScaleMode": "MatchHeight", "Root": Root, "Animations": []})


def WriteAll(Content):
	WriteData(Content)
	WriteUi(Content)


# ================================================================ 씬
def AddMeta(S, Layout, Minimap=None):
	# 메타 UI 엔티티 (HUD 위 — ZOrder 1). Minimap = HD2DMapArt.Info(...) (없으면 지도 쪽은 "지도가 없다")
	Props = {"Map": Layout.MapId}
	if Minimap:
		MinX, MinY, MaxX, MaxY = Minimap["Bounds"]
		Props.update({"MapImage": Minimap["Image"], "MapBounds": f"{MinX:.0f},{MinY:.0f},{MaxX:.0f},{MaxY:.0f}", "MapTitle": Minimap["Title"],
					  "Landmarks": ";".join(f"{Name},{X:.0f},{Y:.0f},{Kind}" for Name, X, Y, Kind in Minimap["Landmarks"])})
	S.Add("MetaUI", {"UIComponent": {"Asset": f"{UI_DIR}/Meta.eui", "ZOrder": 1, "Visible": True, "ReceiveInput": True, "KeyboardFocus": False},
					 "ScriptComponent": {"ScriptAsset": "Scripts/Demo/HD2D/HD2DMetaHud.lua", "ExecutionLocation": 0,
										 "PropertyOverrides": json.dumps(Props, ensure_ascii=False)}})


# 스크린샷 시나리오 (HD2DGameplay.SHOT_SCENES에 합쳐짐 — 시작 자리, HD2DMetaPilot.lua 시나리오)
SHOT_SCENES = {
	"Shot_PauseHome":  ((-2350.0, 300.0), "PauseHome"),
	"Shot_Journal":    ((-2350.0, 300.0), "Journal"),
	"Shot_Map":        ((1500.0, 300.0), "MapScreen"),
	"Shot_Bestiary":   ((-2350.0, 300.0), "Bestiary"),
	"Shot_Records":    ((-2350.0, 300.0), "Records"),
	"Shot_Settings":   ((-2350.0, 300.0), "Settings"),
	"Shot_SaveSlots":  ((-2350.0, 300.0), "SaveSlots"),
	"Shot_LoadSlots":  ((-2350.0, 300.0), "LoadSlots"),
	"Shot_Forge":      ((-3300.0, 300.0), "Forge"),
	"Shot_Recipe":     ((-3300.0, 300.0), "Recipe"),
	"Shot_Minimap":    ((1500.0, 300.0), "Minimap"),
	"Shot_Confirm":    ((-2350.0, 300.0), "ConfirmTitle"),
}
