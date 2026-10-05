# HD-2D 데모 맵 게임플레이 콘텐츠 (BuildHD2D.py가 부른다 — 배경·조명·카메라는 BuildHD2D.py, 여기는 게임에 쓰이는 것만):
#   데이터 표(Data/Demo/HD2D — 무기·아이템(장비 포함)·적·마을 사람·퀘스트·서브 퀘스트·밸런스), 게임 UI(UI/Demo/HD2D/HUD.eui + 창틀·아이콘·초상화),
#   프리팹(Prefabs/Demo/HD2D — 플레이어·적 5종 + 동굴 변형 2종·보스 2종(골렘·수정 거미 여왕)·효과 조각·보물상자·마을 사람·소품), 씬 배치(게임 관리자 속성 + HUD + 내비메시 + 플레이어),
#   자동 검증 시나리오 변형(--views), 내비메시 굽기용 씬(WriteNavBakeScene).
#   전투 깊이(약점·실드·브레이크·스킬·상태 이상·정예·동료 엘라)의 표 필드·HUD 위젯·도트 아트·프리팹은 HD2DCombatGen.py — 여기서는 그 함수를 부르기만 한다.
#   규약: 좌표·카메라는 BuildHD2D.py 머리 주석과 같다 (화면 오른쪽 = +X, 화면 안쪽 = -Y, 스프라이트는 XZ 평면 + 앞면 +Y, 1 도트 = 6cm).
#         적·보물상자·마을 사람·소품은 게임 관리자(HD2DGame.lua)가 시작할 때 프리팹으로 만든다 — 자리는 여기서 지면 높이까지 계산해 속성 문자열로 넘긴다.
#         수치(무기 피해·적 체력·가격·대사)는 데이터 표에 있고 스크립트는 HD2DData.lua로만 읽는다.
#   다른 맵(동굴 등)의 생성기도 쓴다: FMapLayout(맵 id + 적/마을 사람/상자/소품/보스 목록) → AddGame(S, Height, Path, AutoPlay, Layout=..., NavMesh=...),
#         AddPlayer(S, Start, Height). 맵 이동 = HD2DTravel.lua 엔티티(트리거 + TargetScene/SpawnName) → 도착 씬의 "Spawn_<SpawnName>" 엔티티(위치 = 발 자리).
#   내비메시(적 길찾기): 지형은 굽기 입력에 들어가지 않으므로 BuildHD2D.py가 굽기 전용 씬 Scenes/Demo/_HD2DNavBake.escene(지형 높이 바닥 판 + 모델 + 콜라이더 상자)을
#         쓰고, 다음 순서로 다시 굽는다 (배경·게임 자리를 바꾸면):
#           python Tools/DemoMap/BuildHD2D.py --views
#           .\Scripts\Verify.ps1 -Target Editor -Config Release -Frames 30 -ExtraArgs "--scene Scenes/Demo/_HD2DNavBake.escene --bake-navmesh"
#           python Tools/DemoMap/HD2DGameplay.py --install-nav     (_HD2DNavBake.enav → Scenes/Demo/HD2D.enav — 이것만 커밋)
import json
import math
import os
import sys

import HD2DArt
import HD2DCombatGen

PREFABS = "Prefabs/Demo/HD2D"
DATA    = "Data/Demo/HD2D"
UI_DIR  = "UI/Demo/HD2D"

# ---- 배치 (마을 = 서쪽, 들판 = 동쪽) — 지면 높이는 BuildHD2D의 높이 함수로 ------------------------------------------------------------
NPCS = [  # (Id, X, Y) — Id = Npcs.etable 행
	("Elder", -2150.0, 160.0),
	("Merchant", -1380.0, 330.0),
	("Guard", -430.0, -170.0),
	("Girl", 1100.0, 520.0),  # 야영지 천막(870, 730) 오른쪽 위 — 천막 콜라이더에 막히지 않게
	("Smith", -3420.0, 180.0),
	("Farmer", 4300.0, -130.0),
]
CHESTS = [  # (X, Y, 내용 "아이템*개수+...", 골드는 Gold*n)
	(-3250.0, -450.0, "Spear*1+Potion*1"),
	(1450.0, -800.0, "Ether*2+Gold*50"),
	(1300.0, 1130.0, "Staff*1+HiPotion*1"),  # 야영지 모닥불 남쪽 빈 땅 (배경 합친 뒤 옛 자리는 야영지 나무 상자와 붙어 막혔다)
]
PROPS = [  # (Id, X, Y) — 소품: SavePoint = 게시판(저장), Cat = 서브 퀘스트 "고양이"(받은 뒤에만 보임)
	("SavePoint", -2650.0, 420.0),
	("Cat", 3350.0, -850.0),
]
SCARECROW = (4980.0, -900.0)  # 밀밭 허수아비 (BuildHD2D.py 배경) — 서브 퀘스트 "허수아비" 사냥 구역 가운데
ENEMY_SLOTS = [  # (종류, X, Y) — 죽으면 RespawnTime 뒤 같은 자리에 다시 (플레이어가 가까우면 미룸)
	("Slime", 1000.0, -700.0), ("Slime", 1700.0, -250.0), ("Slime", 2300.0, 450.0), ("Slime", 900.0, 650.0), ("Slime", 3150.0, -250.0),
	("Bat", 2400.0, -300.0), ("Bat", 3400.0, -1350.0), ("Bat", 1900.0, -1500.0),
	("Goblin", 3400.0, 750.0), ("Goblin", 2800.0, 950.0), ("EliteGoblin", 3900.0, 350.0),
	("Archer", 1250.0, -1350.0), ("Archer", 3300.0, -1450.0),
	("Mushroom", 2200.0, -500.0), ("Mushroom", 600.0, -950.0), ("Mushroom", 3700.0, -300.0),
	# 밀밭 (허수아비 둘레 — 서브 퀘스트)
	("Mushroom", 4650.0, -800.0), ("Slime", 5050.0, -600.0), ("Goblin", 4800.0, -1100.0),
]
BOSS = (4950.0, 500.0)   # 길 동쪽 끝 너른 풀밭 (밀밭 허수아비와 1300cm 떨어져 사냥 중에 깨지 않는다)
BOSS_ARENA_RADIUS = 450.0
# 자동 검증 시작 자리 / 스크린샷 시나리오 (이름: (시작 자리, 시나리오)) — BuildHD2D.py --views가 _HD2D<이름>.escene으로 쓴다
AUTOPLAY_START = (-2350.0, 300.0)
TRAVEL_TEST = ((-2950.0, 820.0), (-2200.0, 560.0))  # 자동 검증 전용 맵 이동 트리거 자리 / 도착 Spawn_Test 자리 (같은 씬을 다시 연다)
SHOT_SCENES = {
	"Shot_Title":     ((-2350.0, 300.0), "TitleShot"),
	"Shot_Inventory": ((-2350.0, 300.0), "Inventory"),
	"Shot_Equip":     ((-2350.0, 300.0), "Equip"),
	"Shot_Shop":      ((-1450.0, 520.0), "Shop"),
	"Shot_Dialog":    ((-2250.0, 330.0), "Dialog"),
	"Shot_Combat":    ((3000.0, 650.0), "Combat"),
	"Shot_Boost":     ((2500.0, 500.0), "Boost"),
	"Shot_Boss":      ((4250.0, 520.0), "Boss"),
}
SHOT_SCENES.update(HD2DCombatGen.SHOT_SCENES)  # 전투 깊이 (브레이크·스킬·상태 이상)


class FMapLayout:
	# 맵 하나의 게임 배치 (다른 맵 생성기도 만들어 AddGame에 넘긴다). 목록 형식은 위 상수와 같다
	#   BossKind = 보스 프리팹/적 표 행, BossReward = 보스 보상 상자 내용, Respawn = 쓰러진 적이 다시 나타나는가(던전은 끔),
	#   Extra = 관리자 속성 추가 (동굴: Traps / Gate / Arena — HD2DDungeon.lua)
	def __init__(self, MapId, Npcs=(), Chests=(), Props=(), Enemies=(), Boss=None, Scarecrow=None, Title=False, BossKind="Golem",
				 BossReward="HiPotion*2+Gold*100", Respawn=True, Extra=None):
		self.MapId, self.Npcs, self.Chests, self.Props, self.Enemies, self.Boss, self.Scarecrow, self.Title = \
			MapId, list(Npcs), list(Chests), list(Props), list(Enemies), Boss, Scarecrow, Title
		self.BossKind, self.BossReward, self.Respawn, self.Extra = BossKind, BossReward, Respawn, dict(Extra or {})


VILLAGE = FMapLayout("Village", NPCS, CHESTS, PROPS, ENEMY_SLOTS, BOSS, SCARECROW, Title=True)

PLAYER_RADIUS, PLAYER_HALF = 32.0, 53.0  # 캡슐 바닥 = 중심 - 85
ENEMY_CAPSULE = {  # 종류: (반지름, 반높이, 몸 스프라이트 높이(발 위, 박쥐만 공중), 체력바 높이(발 위), 질량)
	"Slime":    (38.0, 4.0, 0.0, 95.0, 30.0),
	"Bat":      (30.0, 10.0, 105.0, 185.0, 15.0),
	"Goblin":   (34.0, 30.0, 0.0, 160.0, 45.0),
	"Archer":   (32.0, 40.0, 0.0, 205.0, 40.0),
	"Mushroom": (40.0, 12.0, 0.0, 165.0, 40.0),
	"Golem":    (95.0, 80.0, 0.0, 450.0, 600.0),
	# 동굴 (HD2DCave): 변형은 바탕 종류(표의 Look)와 같은 캡슐, 보스 "수정 거미 여왕"
	"CaveBat":      (30.0, 10.0, 105.0, 185.0, 15.0),
	"CrystalSlime": (38.0, 4.0, 0.0, 95.0, 30.0),
	"SpiderQueen":  (90.0, 45.0, 0.0, 380.0, 500.0),
}  # 정예 변형 캡슐은 HD2DCombatGen.ExtendEnemies가 더한다
BOSS_SCRIPTS = {"Golem": "Scripts/Demo/HD2D/HD2DBoss.lua", "SpiderQueen": "Scripts/Demo/HD2D/HD2DSpiderQueen.lua"}


def _WriteJson(Path, Doc):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")


# ================================================================ 데이터 표
def _Field(Name, Type, Default, Description, **Extra):
	F = {"Name": Name, "Type": Type, "Default": Default, "Description": Description}
	F.update(Extra)
	return F


def _Table(Content, Name, Struct, Rows):
	_WriteJson(os.path.join(Content, *DATA.split("/"), f"{Name}.etable"),
			   {"Version": 1, "Struct": f"{DATA}/{Struct}.estruct", "Rows": [{"Name": N, "Values": V} for N, V in Rows]})


def _Struct(Content, Name, Description, Fields):
	_WriteJson(os.path.join(Content, *DATA.split("/"), f"{Name}.estruct"), {"Version": 1, "Name": Name, "Description": Description, "Fields": Fields})


def Icon(Name):
	return f"{UI_DIR}/Icons/{Name}.png"


WEAPONS = [
	("Sword", {"DisplayName": "여행자의 검", "Kind": "Slash", "Icon": Icon("Sword"), "Description": "손에 익은 한손검. 빠르게 세 번 이어 베고, 마지막 일격은 크게 휘두른다.",
			   "Damage": 14, "Cooldown": 0.22, "Range": 155, "Arc": 170, "Knockback": 720, "ProjectileSpeed": 0, "ManaCost": 0, "Splash": 0,
			   "HitStop": 0.05, "AttackTime": 0.29, "HitDelay": 0.09, "Flipbook": "Hero_Attack", "Sound": "Audio/RPG/Swing1.wav"}),
	("Spear", {"DisplayName": "기사의 창", "Kind": "Thrust", "Icon": Icon("Spear"), "Description": "멀리 닿는 긴 창. 앞으로 곧게 찔러 줄지은 적을 한꺼번에 꿰뚫는다.",
			   "Damage": 22, "Cooldown": 0.4, "Range": 290, "Arc": 36, "Knockback": 950, "ProjectileSpeed": 0, "ManaCost": 0, "Splash": 0,
			   "HitStop": 0.07, "AttackTime": 0.39, "HitDelay": 0.08, "Flipbook": "Hero_AttackSpear", "Sound": "Audio/RPG/Swing3.wav"}),
	("Bow", {"DisplayName": "사냥꾼의 활", "Kind": "Arrow", "Icon": Icon("Bow"), "Description": "가벼운 짧은 활. 가까운 적을 겨냥해 빠른 화살을 쏜다.",
			 "Damage": 13, "Cooldown": 0.34, "Range": 1300, "Arc": 50, "Knockback": 300, "ProjectileSpeed": 2400, "ManaCost": 0, "Splash": 0,
			 "HitStop": 0.03, "AttackTime": 0.4, "HitDelay": 0.14, "Flipbook": "Hero_AttackBow", "Sound": "Audio/RPG/Swing2.wav"}),
	("Staff", {"DisplayName": "현자의 지팡이", "Kind": "Bolt", "Icon": Icon("Staff"), "Description": "마나를 담아 빛의 탄을 쏘는 지팡이. 탄은 부딪히면 터져 주변을 휩쓴다.",
			   "Damage": 28, "Cooldown": 0.6, "Range": 1100, "Arc": 60, "Knockback": 600, "ProjectileSpeed": 1300, "ManaCost": 8, "Splash": 170,
			   "HitStop": 0.06, "AttackTime": 0.42, "HitDelay": 0.13, "Flipbook": "Hero_AttackStaff", "Sound": "Audio/RPG/Spin.wav"}),
	# 동굴 유적 숨은 보물 (갈림길 뒤 보물 단 상자) — 검과 같은 베기 동작
	("CrystalSword", {"DisplayName": "수정 검", "Kind": "Slash", "Icon": Icon("CrystalSword"),
					  "Description": "동굴 유적 깊은 곳에 잠들어 있던 푸른 수정 검. 가볍고 날카로워 여행자의 검보다 훨씬 깊게 벤다.",
					  "Damage": 21, "Cooldown": 0.2, "Range": 165, "Arc": 175, "Knockback": 760, "ProjectileSpeed": 0, "ManaCost": 0, "Splash": 0,
					  "HitStop": 0.05, "AttackTime": 0.28, "HitDelay": 0.09, "Flipbook": "Hero_Attack", "Sound": "Audio/RPG/Swing1.wav"}),
]

def _Gear(Id, Name, Kind, Icon_, Desc, Price, Defense=0, Health=0, Speed=0.0, Crit=0.0):
	return (Id, {"DisplayName": Name, "Kind": Kind, "Amount": 0, "Weapon": "", "Icon": Icon(Icon_), "Description": Desc, "Price": Price,
				 "Defense": Defense, "HealthBonus": Health, "SpeedBonus": Speed, "CritBonus": Crit})


def _Item(Id, Values):
	V = {"Defense": 0, "HealthBonus": 0, "SpeedBonus": 0.0, "CritBonus": 0.0}
	V.update(Values)
	return (Id, V)


ITEMS = [
	_Item("Sword", {"DisplayName": "여행자의 검", "Kind": "Weapon", "Amount": 0, "Weapon": "Sword", "Icon": Icon("Sword"), "Description": "", "Price": 0}),
	_Item("Spear", {"DisplayName": "기사의 창", "Kind": "Weapon", "Amount": 0, "Weapon": "Spear", "Icon": Icon("Spear"), "Description": "", "Price": 160}),
	_Item("Bow", {"DisplayName": "사냥꾼의 활", "Kind": "Weapon", "Amount": 0, "Weapon": "Bow", "Icon": Icon("Bow"), "Description": "", "Price": 90}),
	_Item("Staff", {"DisplayName": "현자의 지팡이", "Kind": "Weapon", "Amount": 0, "Weapon": "Staff", "Icon": Icon("Staff"), "Description": "", "Price": 140}),
	_Item("Potion", {"DisplayName": "회복약", "Kind": "Heal", "Amount": 50, "Weapon": "", "Icon": Icon("Potion"),
					 "Description": "약초를 달인 붉은 물약. 체력을 50 회복한다. (단축키 1)", "Price": 15}),
	_Item("HiPotion", {"DisplayName": "고급 회복약", "Kind": "Heal", "Amount": 130, "Weapon": "", "Icon": Icon("HiPotion"),
					   "Description": "진하게 우려낸 귀한 물약. 체력을 130 회복한다.", "Price": 45}),
	_Item("Ether", {"DisplayName": "마나 물약", "Kind": "Mana", "Amount": 40, "Weapon": "", "Icon": Icon("Ether"),
					"Description": "푸른 빛이 도는 물약. 마나를 40 회복한다. (단축키 2)", "Price": 25}),
	_Item("Elixir", {"DisplayName": "엘릭서", "Kind": "Elixir", "Amount": 0, "Weapon": "", "Icon": Icon("Elixir"),
					 "Description": "전설의 영약. 체력과 마나를 모두 회복한다.", "Price": 300}),
	# 방어구·장신구 (장비 칸 — 인벤토리 "장비" 탭에서 비교·장착)
	_Gear("LeatherVest", "가죽 조끼", "Armor", "Vest", "길을 떠날 때 걸친 낡은 조끼. 없는 것보다는 낫다.", 0, Defense=3),
	_Gear("ChainMail", "사슬 갑옷", "Armor", "Mail", "촘촘히 엮은 쇠사슬 갑옷. 몸이 조금 무거워진다.", 160, Defense=8, Health=15, Speed=-0.03),
	_Gear("KnightPlate", "기사의 판금 갑옷", "Armor", "Plate", "대장장이 브론이 슬라임 젤리로 담금질한 판금 갑옷.", 0, Defense=14, Health=30, Speed=-0.05),
	_Gear("LuckyRing", "행운의 반지", "Accessory", "Ring", "붉은 보석이 박힌 반지. 급소를 노리는 눈이 밝아진다.", 110, Crit=0.12),
	_Gear("SwiftCharm", "질풍의 부적", "Accessory", "Charm", "바람을 담은 푸른 부적. 발걸음이 가벼워진다.", 0, Speed=0.15),
	_Gear("LifeAmulet", "생명의 목걸이", "Accessory", "Amulet", "리나가 건넨 엄마의 목걸이. 따뜻한 힘이 깃들어 있다.", 0, Defense=2, Health=40),
	# 동굴 유적 (HD2DCave 보물상자)
	_Item("CrystalSword", {"DisplayName": "수정 검", "Kind": "Weapon", "Amount": 0, "Weapon": "CrystalSword", "Icon": Icon("CrystalSword"),
						   "Description": "", "Price": 0}),
	_Gear("CrystalCharm", "수정 부적", "Accessory", "CrystalCharm", "수정 거미 여왕의 둥지 곁에서 찾은 부적. 차가운 빛이 몸을 감싸 공격을 막아 준다.", 0,
		  Defense=6, Health=20, Crit=0.06),
	# 재료·귀중품 (쓰지 않는다 — 서브 퀘스트)
	_Item("Jelly", {"DisplayName": "슬라임 젤리", "Kind": "Material", "Amount": 0, "Weapon": "", "Icon": Icon("Jelly"),
					"Description": "슬라임에게서 얻은 탱탱한 젤리. 대장장이가 담금질에 쓴다고 한다.", "Price": 0}),
	_Item("LostCat", {"DisplayName": "길 잃은 고양이 미미", "Kind": "Material", "Amount": 0, "Weapon": "", "Icon": Icon("CatIcon"),
					  "Description": "리나의 고양이. 품에 안겨 얌전히 가르랑거린다.", "Price": 0}),
]
HD2DCombatGen.ExtendItems(ITEMS, _Item, Icon)  # 해독제

ENEMIES = [
	("Slime", {"DisplayName": "슬라임", "Behavior": "Hopper", "MaxHealth": 30, "ContactDamage": 8, "AttackDamage": 8, "MoveSpeed": 300, "AggroRange": 700,
			   "AttackRange": 0, "AttackCooldown": 1.1, "WindupTime": 0.0, "ProjectileSpeed": 0, "GoldMin": 2, "GoldMax": 4, "Exp": 6,
			   "DropItem": "Potion", "DropChance": 0.15, "Radius": 48, "RespawnTime": 12}),
	("Bat", {"DisplayName": "흡혈 박쥐", "Behavior": "Flyer", "MaxHealth": 30, "ContactDamage": 6, "AttackDamage": 11, "MoveSpeed": 380, "AggroRange": 950,
			 "AttackRange": 430, "AttackCooldown": 2.2, "WindupTime": 0.45, "ProjectileSpeed": 1150, "GoldMin": 2, "GoldMax": 5, "Exp": 8,
			 "DropItem": "Ether", "DropChance": 0.15, "Radius": 50, "RespawnTime": 14}),
	("Goblin", {"DisplayName": "고블린 도적", "Behavior": "Charger", "MaxHealth": 80, "ContactDamage": 6, "AttackDamage": 14, "MoveSpeed": 330, "AggroRange": 950,
				"AttackRange": 380, "AttackCooldown": 2.0, "WindupTime": 0.5, "ProjectileSpeed": 1250, "GoldMin": 4, "GoldMax": 8, "Exp": 12,
				"DropItem": "Potion", "DropChance": 0.2, "Radius": 48, "RespawnTime": 16}),
	("Archer", {"DisplayName": "해골 궁수", "Behavior": "Archer", "MaxHealth": 60, "ContactDamage": 5, "AttackDamage": 12, "MoveSpeed": 220, "AggroRange": 1300,
				"AttackRange": 950, "AttackCooldown": 2.4, "WindupTime": 0.75, "ProjectileSpeed": 1100, "GoldMin": 4, "GoldMax": 8, "Exp": 14,
				"DropItem": "HiPotion", "DropChance": 0.1, "Radius": 46, "RespawnTime": 18}),
	("Mushroom", {"DisplayName": "독버섯", "Behavior": "Spore", "MaxHealth": 70, "ContactDamage": 6, "AttackDamage": 5, "MoveSpeed": 160, "AggroRange": 750,
				  "AttackRange": 280, "AttackCooldown": 3.2, "WindupTime": 0.6, "ProjectileSpeed": 0, "GoldMin": 3, "GoldMax": 6, "Exp": 10,
				  "DropItem": "Ether", "DropChance": 0.15, "Radius": 50, "RespawnTime": 16}),
	("Golem", {"DisplayName": "고대의 바위 골렘", "Behavior": "Boss", "MaxHealth": 4000, "ContactDamage": 14, "AttackDamage": 24, "MoveSpeed": 170, "AggroRange": 850,
			   "AttackRange": 340, "AttackCooldown": 1.5, "WindupTime": 0.8, "ProjectileSpeed": 900, "GoldMin": 150, "GoldMax": 150, "Exp": 150,
			   "DropItem": "Elixir", "DropChance": 1.0, "Radius": 115, "RespawnTime": 0}),
	# 동굴 유적 (HD2DCave): 변형은 바탕 종류의 그림(Look)에 색(Tint)만 다르다
	("CaveBat", {"DisplayName": "동굴 흡혈 박쥐", "Behavior": "Flyer", "MaxHealth": 46, "ContactDamage": 8, "AttackDamage": 14, "MoveSpeed": 430, "AggroRange": 1000,
				 "AttackRange": 460, "AttackCooldown": 1.9, "WindupTime": 0.4, "ProjectileSpeed": 1250, "GoldMin": 3, "GoldMax": 6, "Exp": 11,
				 "DropItem": "Ether", "DropChance": 0.2, "Radius": 50, "RespawnTime": 14, "Look": "Bat", "Tint": [0.62, 0.92, 1.45, 1.0]}),
	("CrystalSlime", {"DisplayName": "수정 슬라임", "Behavior": "Hopper", "MaxHealth": 90, "ContactDamage": 13, "AttackDamage": 13, "MoveSpeed": 280, "AggroRange": 750,
					  "AttackRange": 0, "AttackCooldown": 1.0, "WindupTime": 0.0, "ProjectileSpeed": 0, "GoldMin": 4, "GoldMax": 7, "Exp": 14,
					  "DropItem": "HiPotion", "DropChance": 0.15, "Radius": 50, "RespawnTime": 14, "Look": "Slime", "Tint": [0.55, 1.05, 1.7, 1.0]}),
	("SpiderQueen", {"DisplayName": "수정 거미 여왕", "Behavior": "Boss", "MaxHealth": 5200, "ContactDamage": 14, "AttackDamage": 22, "MoveSpeed": 240,
					 "AggroRange": 780, "AttackRange": 520, "AttackCooldown": 1.35, "WindupTime": 0.7, "ProjectileSpeed": 950, "GoldMin": 220, "GoldMax": 220,
					 "Exp": 220, "DropItem": "Elixir", "DropChance": 1.0, "Radius": 125, "RespawnTime": 0}),
]
for _, _Row in ENEMIES:
	_Row.setdefault("Look", "")
	_Row.setdefault("Tint", [1, 1, 1, 1])
HD2DCombatGen.ExtendEnemies(ENEMIES, ENEMY_CAPSULE)  # 정예 행·캡슐 + 약점·실드·브레이크 필드
HD2DCombatGen.ExtendWeapons(WEAPONS)                 # 기본 공격 속성 + 무기 기술

def Portrait(Name):
	return f"{UI_DIR}/Portraits/{Name}.png"


NPC_ROWS = [
	("Elder", {"DisplayName": "촌장 바르톨로", "Flipbook": "Sprites/HD2D/Npc_Elder.eflipbook", "Role": "Elder", "Lines": [],
			   "Portrait": Portrait("Elder"), "SubQuest": ""}),
	("Merchant", {"DisplayName": "상인 미라", "Flipbook": "Sprites/HD2D/Npc_Merchant.eflipbook", "Role": "Shop",
				  "Lines": ["어서 오세요, 여행자님! 들판으로 나가신다면 준비는 단단히 하셔야죠.", "회복약부터 활과 갑옷까지, 필요한 건 다 있답니다."],
				  "Portrait": Portrait("Merchant"), "SubQuest": ""}),
	("Guard", {"DisplayName": "경비병 오웬", "Flipbook": "Sprites/HD2D/Npc_Guard.eflipbook", "Role": "Talk",
			   "Lines": ["문 밖은 마물 천지야. 조심하라고.", "R 키로 무기를 바꿀 수 있지. 창은 멀리, 활과 지팡이는 더 멀리 닿는다네.",
						 "Q로 힘을 모아(BP) 공격하면 몇 번이고 연달아 몰아친다더군. 위험할 때를 위해 아껴 두게."],
			   "Portrait": Portrait("Guard"), "SubQuest": ""}),
	("Girl", {"DisplayName": "꼬마 리나", "Flipbook": "Sprites/HD2D/Npc_Girl.eflipbook", "Role": "Quest", "Lines": [],
			  "Portrait": Portrait("Girl"), "SubQuest": "Cat"}),
	("Smith", {"DisplayName": "대장장이 브론", "Flipbook": "Sprites/HD2D/Npc_Smith.eflipbook", "Role": "Quest", "Lines": [],
			   "Portrait": Portrait("Smith"), "SubQuest": "Smith"}),
	("Farmer", {"DisplayName": "농부 한스", "Flipbook": "Sprites/HD2D/Npc_Farmer.eflipbook", "Role": "Quest", "Lines": [],
				"Portrait": Portrait("Farmer"), "SubQuest": "Scarecrow"}),
]

SUB_QUESTS = [
	("Cat", {"Title": "길 잃은 고양이", "Giver": "Girl", "Kind": "Find", "Target": "LostCat", "Count": 1, "AreaX": 0, "AreaY": 0, "AreaRadius": 0,
			 "RewardGold": 60, "RewardItem": "LifeAmulet", "RewardExp": 25,
			 "Summary": "리나의 고양이 미미가 연못 동쪽 바위 근처로 사라졌다. 찾아서 데려다주자.",
			 "AcceptLines": ["흑… 우리 미미가 없어졌어.", "연못 쪽으로 나비를 쫓아가는 걸 봤는데… 언니 오빠가 찾아 줄래?"],
			 "ProgressLines": ["미미는 연못 동쪽 바위 근처에 있을 거야. 부탁해!"],
			 "DoneLines": ["미미! 정말 찾아 줬구나!", "고마워… 이건 엄마가 준 목걸이야. 언니 오빠가 가져가!"],
			 "AfterLines": ["미미가 이제 내 옆에서만 잔대. 헤헤."]}),
	("Smith", {"Title": "대장간의 담금질", "Giver": "Smith", "Kind": "Collect", "Target": "Jelly", "Count": 3, "AreaX": 0, "AreaY": 0, "AreaRadius": 0,
			   "RewardGold": 0, "RewardItem": "KnightPlate", "RewardExp": 30,
			   "Summary": "대장장이 브론이 갑옷 담금질에 쓸 슬라임 젤리 3개를 찾는다. 들판의 슬라임이 떨어뜨린다.",
			   "AcceptLines": ["어이, 젊은이. 쓸 만한 갑옷이 필요해 보이는군.", "슬라임 젤리 세 개만 가져오면 기사의 판금 갑옷을 담금질해 주지."],
			   "ProgressLines": ["젤리는 들판의 슬라임이 떨어뜨린다네. 세 개면 충분해."],
			   "DoneLines": ["오, 탱탱한 젤리로군! 잠깐 기다리게…", "자, 받게. 이 마을에서 제일 단단한 갑옷일세."],
			   "AfterLines": ["갑옷이 손에 익거든 또 들르게. 쇠는 쓸수록 길이 드는 법이지."]}),
	("Scarecrow", {"Title": "허수아비를 지켜라", "Giver": "Farmer", "Kind": "Hunt", "Target": "", "Count": 3, "AreaX": SCARECROW[0], "AreaY": SCARECROW[1],
				   "AreaRadius": 750, "RewardGold": 80, "RewardItem": "SwiftCharm", "RewardExp": 30,
				   "Summary": "밀밭 허수아비 근처의 마물 3마리를 쓰러뜨려 농부 한스의 밀밭을 지키자.",
				   "AcceptLines": ["이거 큰일이구먼. 밀밭 허수아비 근처에 마물이 눌러앉았지 뭐야.", "세 마리만 쫓아내 주면 사례하겠네!"],
				   "ProgressLines": ["허수아비 근처의 마물을 부탁하네. 밀밭 울타리 틈으로 들어가면 돼."],
				   "DoneLines": ["밀밭이 조용해졌구먼! 정말 고맙네.", "바람의 부적일세. 발이 빨라지면 마물한테 덜 맞을 걸세."],
				   "AfterLines": ["올해 밀은 풍년이겠어. 다 자네 덕일세."]}),
]

def _Quest(Title, Objective, Lines=(), WaitLines=(), KillGoal=0, AdvanceOnTalk=False, BossGoal="", NewGoal=False, RewardGold=0, RewardItem="",
		   Ending=False):
	return {"Title": Title, "Objective": Objective, "KillGoal": KillGoal, "Lines": list(Lines), "WaitLines": list(WaitLines), "AdvanceOnTalk": AdvanceOnTalk,
			"BossGoal": BossGoal, "NewGoal": NewGoal, "RewardGold": RewardGold, "RewardItem": RewardItem, "Ending": Ending}


# 메인 퀘스트: 마을(촌장) → 들판 마물 → 골렘(들판 보스) → 보고 → 동굴 유적 탐사(동굴 보스 — HD2DCave) → 귀환 보고 → 엔딩·크레딧
QUESTS = [
	("Stage0", _Quest("촌장의 부탁", "광장의 촌장 바르톨로에게 말을 걸자", AdvanceOnTalk=True)),
	("Stage1", _Quest("촌장의 부탁", "들판의 마물을 쓰러뜨리자", KillGoal=6,
					  Lines=["오오, 여행자여. 마침 잘 왔네.", "요즘 동쪽 들판에 마물이 들끓어 마을 사람들이 밭에 나가질 못하고 있다네.",
							 "부디 들판의 마물을 몰아내 주게. 대장간 옆 상자에 오래된 창이 있으니 가져가도 좋네."],
					  WaitLines=["들판의 마물을 부탁하네. 대장간 옆 상자도 잊지 말게나.", "야영지 쪽에도 누가 두고 간 상자가 있다더군."])),
	("Stage2", _Quest("고대의 수호자", "들판 동쪽 끝의 바위 골렘을 쓰러뜨리자", BossGoal="Village", NewGoal=True,
					  Lines=["대단하군! 하지만 마물들이 날뛰는 까닭은 따로 있다네.", "들판 동쪽 끝에서 고대의 바위 골렘이 깨어났다는 소문이야.",
							 "그 골렘을 쓰러뜨리면 들판도 다시 평화로워질 걸세."],
					  WaitLines=["골렘은 들판 동쪽 끝에 있다네. 바위를 던지니 바닥의 붉은 원을 조심하게."])),
	("Stage3", _Quest("고대의 수호자", "촌장 바르톨로에게 돌아가 보고하자", AdvanceOnTalk=True,
					  Lines=["골렘은 쓰러뜨렸는가? 어서 돌아와 이야기를 들려주게."])),
	("Stage4", _Quest("동굴 유적의 그림자", "폭포 옆 벼랑의 동굴 유적 깊은 곳을 살펴보자", BossGoal="Cave", NewGoal=True, RewardGold=300, RewardItem="Elixir",
					  Lines=["정말로 해냈구먼! 골렘을 쓰러뜨리다니, 자네는 이 마을의 은인일세.", "약소하지만 사례를 받아 주게. 마을 사람 모두의 마음이라네.",
							 "헌데… 골렘이 깨어난 그날 밤부터 폭포 옆 벼랑의 오래된 동굴 유적에서 푸른 빛이 새어 나온다네.",
							 "유적 깊은 곳에 무언가가 둥지를 틀었다는 소문일세. 부디 살펴봐 주게."],
					  WaitLines=["동굴 유적은 폭포 옆 벼랑에 입구가 있다네.", "안쪽 복도엔 가시 함정이 있다더군. 바닥이 붉게 깜빡이면 물러서게."])),
	("Stage5", _Quest("동굴 유적의 그림자", "마을로 돌아가 촌장 바르톨로에게 보고하자", AdvanceOnTalk=True,
					  Lines=["돌아왔구먼! 유적 깊은 곳에서 무엇을 보았나?"])),
	("Stage6", _Quest("마을의 영웅", "모든 의뢰 완료! 들판과 유적을 자유롭게 탐험하자", RewardGold=500, Ending=True,
					  Lines=["수정 거미 여왕이라니…! 그런 것이 유적에 숨어 있었구먼.", "자네 덕에 하르트 마을에 다시 아침이 찾아왔네. 정말로 이 마을의 영웅일세.",
							 "마을 사람 모두가 모은 사례일세. 부디 받아 주게."],
					  WaitLines=["덕분에 마을이 평화롭구먼. 고맙네, 젊은이."])),
]

BALANCE = {
	"PlayerName": "아르펜", "MaxHealth": 120, "MaxMana": 50, "ManaRegen": 3.0, "HealthPerLevel": 18, "ManaPerLevel": 8, "DamagePerLevel": 0.12,
	"ExpTable": [0, 30, 75, 140, 230, 350, 500, 700, 950, 1250], "StartWeapon": "Sword", "StartGold": 120, "StartItems": ["Potion", "Potion", "Potion"],
	"ShopStock": ["Potion", "HiPotion", "Ether", "Bow", "ChainMail", "LuckyRing", "Elixir"], "InvulnTime": 0.8, "DashSpeed": 1700, "DashTime": 0.2,
	"DashCooldown": 0.5, "CritChance": 0.1, "CritMultiplier": 1.6,
	"StartArmor": "LeatherVest", "BoostMax": 5, "BoostStart": 1, "BoostRegenTime": 10.0, "BoostHitsPerPoint": 8, "BoostMaxLevel": 3,
	"BoostDamagePerLevel": 0.35,
	"TitleName": "황혼의 들판", "TitleSub": "― 여명을 찾는 아르펜의 여행 ―",
	"IntroLines": ["||해 질 녘, 낡은 지도 한 장을 손에 쥔 여행자가 작은 마을 하르트에 닿았다.",
				   "||들판 너머에서는 오래전 잠든 무언가가 다시 눈을 뜨려 하고 있었다…",
				   "아르펜|Hero|여기가 하르트 마을인가. 지도에 그려진 빛의 들판은 동쪽이라고 했지.",
				   "아르펜|Hero|우선 광장의 촌장님께 인사부터 드려야겠다."],
	"ShopLines": ["무엇을 찾으세요? 천천히 둘러보세요!", "감사합니다! 좋은 물건이에요.", "어머, 골드가 조금 모자라네요…", "그건 이미 가지고 계시잖아요?",
				  "또 오세요! 들판에선 몸조심하시고요."],
	"SaveSlot": "HD2D",
	# 엔딩·크레딧 쪽 (제목|본문 — 본문 줄바꿈은 \n): 메인 퀘스트 마지막 단계(Ending)에서 차례로 보인다
	"EndingPages": ["황혼의 들판|하르트 마을에 다시 아침이 찾아왔다.\n들판의 골렘도, 유적의 여왕도 이제 깊은 잠에 들었다.",
					"그 뒤의 이야기|아르펜은 낡은 지도를 다시 펼쳤다.\n빛의 들판 너머, 아직 아무도 가 보지 않은 길이 이어져 있었다.",
					"만든 것들|ProjectE — 자체 C++ / DirectX 12 엔진\nHD-2D 디오라마 · 도트 스프라이트 · 틸트시프트 흐림 · 볼류메트릭 안개",
					"도트 아트 · 지도|절차 생성 (Tools/DemoMap — HD2DArt · BuildHD2D · BuildHD2DCave)\n3D 키트 KayKit · 질감 Poly Haven (CC0)",
					"소리|효과음 Kenney (CC0)\n\n플레이해 주셔서 고맙습니다!"],
}
HD2DCombatGen.ExtendBalance(BALANCE)  # 해독제 진열 + 마법 칸 + 브레이크/약점 배율


def WriteData(Content):
	_Struct(Content, "Weapon", "HD2D 무기 (Weapons.etable, 행 이름 = 무기 id = Items.etable의 무기 행)", [
		_Field("DisplayName", "String", "무기", "표시 이름"),
		_Field("Kind", "Enum", "Slash", "공격 방식: 베기(부채꼴, 3연타) / 찌르기(좁고 긴 직선, 관통) / 화살(투사체) / 마법 탄(투사체 + 폭발)",
			   Values=["Slash", "Thrust", "Arrow", "Bolt"]),
		_Field("Icon", "String", Icon("Sword"), "UI 아이콘 (Content 기준)"),
		_Field("Description", "String", "", "인벤토리 설명"),
		_Field("Damage", "Float", 10, "한 대 피해 (레벨 배율 전)"),
		_Field("Cooldown", "Float", 0.3, "공격 사이 최소 간격 (초, 공격 동작 시간과 별개)"),
		_Field("Range", "Float", 150, "근접 = 판정 거리 (cm), 원거리 = 사거리"),
		_Field("Arc", "Float", 120, "근접 = 판정 각 (도), 원거리 = 자동 조준 각"),
		_Field("Knockback", "Float", 600, "넉백 속도 (cm/s)"),
		_Field("ProjectileSpeed", "Float", 0, "투사체 속도 (cm/s)"),
		_Field("ManaCost", "Float", 0, "한 번 쏠 때 마나"),
		_Field("Splash", "Float", 0, "> 0이면 명중 시 폭발 반경 (cm)"),
		_Field("HitStop", "Float", 0.05, "명중 시 멈춤 (초)"),
		_Field("AttackTime", "Float", 0.3, "공격 동작 시간 (초 — 플립북 길이)"),
		_Field("HitDelay", "Float", 0.1, "공격 시작 뒤 판정·발사 시점 (초)"),
		_Field("Flipbook", "String", "Hero_Attack", "용사 공격 플립북 접두사 (+ Down/Up/Side)"),
		_Field("Sound", "String", "Audio/RPG/Swing1.wav", "공격 소리"),
	] + HD2DCombatGen.WeaponFields(_Field))
	_Table(Content, "Weapons", "Weapon", WEAPONS)

	_Struct(Content, "Item", "HD2D 아이템 (Items.etable, 행 이름 = 아이템 id — 인벤토리·상점·보물상자·전리품)", [
		_Field("DisplayName", "String", "아이템", "표시 이름"),
		_Field("Kind", "Enum", "Heal", "무기 / 체력 회복 / 마나 회복 / 둘 다 전부 / 방어구 / 장신구 / 재료·귀중품(쓰지 않음) / 상태 이상 회복(해독제)",
			   Values=["Weapon", "Heal", "Mana", "Elixir", "Armor", "Accessory", "Material", "Cure"]),
		_Field("Amount", "Float", 0, "회복량"),
		_Field("Weapon", "String", "", "무기 id (Kind = Weapon)"),
		_Field("Icon", "String", Icon("Potion"), "UI 아이콘 (Content 기준)"),
		_Field("Description", "String", "", "설명 (무기는 비우면 무기 표 설명)"),
		_Field("Price", "Int", 10, "상점 가격 (골드)"),
		_Field("Defense", "Float", 0, "방어력 (방어구·장신구) — 받는 피해 × 60 / (60 + 방어력 × 3)"),
		_Field("HealthBonus", "Float", 0, "최대 체력 +"),
		_Field("SpeedBonus", "Float", 0, "이동 속도 배율 + (0.1 = 10%)"),
		_Field("CritBonus", "Float", 0, "치명타 확률 +"),
	])
	_Table(Content, "Items", "Item", ITEMS)

	_Struct(Content, "Enemy", "HD2D 적 (Enemies.etable, 행 이름 = 적 종류 = 프리팹 이름)", [
		_Field("DisplayName", "String", "마물", "표시 이름"),
		_Field("Behavior", "Enum", "Hopper", "행동: 깡충 추적 / 날며 급강하 / 예비 동작 후 돌진 / 거리 두고 활 / 독 웅덩이 / 보스",
			   Values=["Hopper", "Flyer", "Charger", "Archer", "Spore", "Boss"]),
		_Field("MaxHealth", "Float", 30, "체력"),
		_Field("ContactDamage", "Float", 6, "몸에 닿으면"),
		_Field("AttackDamage", "Float", 10, "공격 한 대 (독 웅덩이는 0.5초마다)"),
		_Field("MoveSpeed", "Float", 250, "이동 속도 (cm/s)"),
		_Field("AggroRange", "Float", 800, "발견 거리 (cm)"),
		_Field("AttackRange", "Float", 300, "공격 시작 거리 (cm)"),
		_Field("AttackCooldown", "Float", 2.0, "공격 간격 (초)"),
		_Field("WindupTime", "Float", 0.5, "예비 동작 (초 — 피할 틈)"),
		_Field("ProjectileSpeed", "Float", 0, "투사체·돌진 속도 (cm/s)"),
		_Field("GoldMin", "Int", 1, "떨어뜨리는 골드 최소"),
		_Field("GoldMax", "Int", 3, "최대"),
		_Field("Exp", "Int", 5, "경험치"),
		_Field("DropItem", "String", "", "떨어뜨리는 아이템 id"),
		_Field("DropChance", "Float", 0, "확률 0~1"),
		_Field("Radius", "Float", 45, "피격 판정 반지름 (cm)"),
		_Field("RespawnTime", "Float", 15, "다시 나타나는 시간 (초, 0 = 안 나타남)"),
		_Field("Look", "String", "", "그림·몸 모양을 빌려 올 종류 (비면 자기 행 이름 — 동굴 변형)"),
		_Field("Tint", "Array", [1.0, 1.0, 1.0, 1.0], "몸 스프라이트 색 배율 RGBA (변형 색)", Element="Float"),
	] + HD2DCombatGen.EnemyFields(_Field))
	_Table(Content, "Enemies", "Enemy", ENEMIES)

	_Struct(Content, "Npc", "HD2D 마을 사람 (Npcs.etable, 행 이름 = 마을 사람 id)", [
		_Field("DisplayName", "String", "마을 사람", "대화 창 이름"),
		_Field("Flipbook", "String", "", "대기 플립북"),
		_Field("Role", "Enum", "Talk", "대화만 / 대화 뒤 상점 / 촌장(퀘스트 단계 대사) / 서브 퀘스트 의뢰인(SubQuest)",
			   Values=["Talk", "Shop", "Elder", "Quest"]),
		_Field("Lines", "Array", [], "대사 (촌장은 Quests.etable 단계 대사, 의뢰인은 SubQuests.etable 대사)", Element="String"),
		_Field("Portrait", "String", "", "대화 창 초상화 (UI 텍스처)"),
		_Field("SubQuest", "String", "", "의뢰하는 서브 퀘스트 (SubQuests.etable 행)"),
	])
	_Table(Content, "Npcs", "Npc", NPC_ROWS)

	_Struct(Content, "Quest", "HD2D 퀘스트 단계 (Quests.etable, 행 이름 = Stage<n>, 촌장에게 말하면 다음 단계로)", [
		_Field("Title", "String", "", "퀘스트 이름"),
		_Field("Objective", "String", "", "목표 표시"),
		_Field("KillGoal", "Int", 0, "> 0이면 마물 처치 수가 이만큼 되면 다음 단계"),
		_Field("Lines", "Array", [], "이 단계로 넘어갈 때 촌장 대사", Element="String"),
		_Field("WaitLines", "Array", [], "이 단계에서 아직 넘어갈 수 없을 때 촌장 대사", Element="String"),
		_Field("AdvanceOnTalk", "Bool", False, "촌장에게 말하면 다음 단계로 (보고 단계)"),
		_Field("BossGoal", "String", "", "이 맵 id의 보스를 쓰러뜨리면 다음 단계로 (Village / Cave)"),
		_Field("NewGoal", "Bool", False, "이 단계로 넘어갈 때 \"새 목표\" 알림"),
		_Field("RewardGold", "Int", 0, "이 단계로 넘어갈 때 보상 골드 (앞 단계 완료 보상 — \"퀘스트 완료!\" 알림)"),
		_Field("RewardItem", "String", "", "보상 아이템"),
		_Field("Ending", "Bool", False, "이 단계로 넘어가면 엔딩·크레딧 (Balance.EndingPages)"),
	])
	_Table(Content, "Quests", "Quest", QUESTS)
	_Struct(Content, "SubQuest", "HD2D 서브 퀘스트 (SubQuests.etable, 행 이름 = 서브 퀘스트 id — 의뢰인 = Npcs.etable SubQuest)", [
		_Field("Title", "String", "", "퀘스트 이름 (퀘스트 탭)"),
		_Field("Giver", "String", "", "의뢰인 (Npcs.etable 행)"),
		_Field("Kind", "Enum", "Find", "찾기(Target 아이템 1개 — 맵의 소품을 조사) / 모으기(Target 아이템 Count개) / 사냥(구역 안 처치 Count번)",
			   Values=["Find", "Collect", "Hunt"]),
		_Field("Target", "String", "", "아이템 id (찾기·모으기)"),
		_Field("Count", "Int", 1, "필요 수"),
		_Field("AreaX", "Float", 0, "사냥 구역 가운데 X (cm)"),
		_Field("AreaY", "Float", 0, "사냥 구역 가운데 Y (cm)"),
		_Field("AreaRadius", "Float", 0, "사냥 구역 반지름 (cm)"),
		_Field("RewardGold", "Int", 0, "보상 골드"),
		_Field("RewardItem", "String", "", "보상 아이템"),
		_Field("RewardExp", "Int", 0, "보상 경험치"),
		_Field("Summary", "String", "", "퀘스트 탭 설명"),
		_Field("AcceptLines", "Array", [], "받을 때 대사", Element="String"),
		_Field("ProgressLines", "Array", [], "진행 중 대사", Element="String"),
		_Field("DoneLines", "Array", [], "보고할 때 대사 (보상)", Element="String"),
		_Field("AfterLines", "Array", [], "끝난 뒤 대사", Element="String"),
	])
	_Table(Content, "SubQuests", "SubQuest", SUB_QUESTS)


	_Struct(Content, "Balance", "HD2D 플레이어·진행 밸런스 (Balance.edata 하나)", [
		_Field("PlayerName", "String", "용사", "이름"),
		_Field("MaxHealth", "Float", 100, "1레벨 최대 체력"),
		_Field("MaxMana", "Float", 40, "1레벨 최대 마나"),
		_Field("ManaRegen", "Float", 2, "초당 마나 회복"),
		_Field("HealthPerLevel", "Float", 15, "레벨마다 최대 체력 +"),
		_Field("ManaPerLevel", "Float", 5, "레벨마다 최대 마나 +"),
		_Field("DamagePerLevel", "Float", 0.1, "레벨마다 피해 배율 +"),
		_Field("ExpTable", "Array", [0, 30], "레벨 n이 되는 누적 경험치 (1레벨 = 0)", Element="Int"),
		_Field("StartWeapon", "String", "Sword", "시작 무기"),
		_Field("StartGold", "Int", 0, "시작 골드"),
		_Field("StartItems", "Array", [], "시작 소지품", Element="String"),
		_Field("ShopStock", "Array", [], "상점 진열 (아이템 id)", Element="String"),
		_Field("InvulnTime", "Float", 0.8, "피격 뒤 무적 (초)"),
		_Field("DashSpeed", "Float", 1700, "대시 속도 (cm/s)"),
		_Field("DashTime", "Float", 0.2, "대시 시간 (초)"),
		_Field("DashCooldown", "Float", 0.5, "대시 간격 (초)"),
		_Field("CritChance", "Float", 0.1, "치명타 확률"),
		_Field("CritMultiplier", "Float", 1.5, "치명타 배율"),
		_Field("StartArmor", "String", "", "시작 방어구"),
		_Field("BoostMax", "Int", 5, "BP 최대"),
		_Field("BoostStart", "Int", 1, "시작 BP"),
		_Field("BoostRegenTime", "Float", 7.0, "BP 1 자연 충전 (초)"),
		_Field("BoostHitsPerPoint", "Int", 5, "명중 이만큼마다 BP +1"),
		_Field("BoostMaxLevel", "Int", 3, "한 번에 올릴 수 있는 부스트 단계"),
		_Field("BoostDamagePerLevel", "Float", 0.35, "부스트 단계마다 피해 배율 +"),
		_Field("TitleName", "String", "", "타이틀 로고"),
		_Field("TitleSub", "String", "", "타이틀 부제"),
		_Field("IntroLines", "Array", [], "새 게임 시작 연출 대사 (이름|초상화|대사 — 이름 비면 해설)", Element="String"),
		_Field("ShopLines", "Array", [], "상점 주인 말: 들어옴 / 구입 / 골드 부족 / 이미 가짐 / 나감", Element="String"),
		_Field("SaveSlot", "String", "HD2D", "저장 슬롯 (SaveGame)"),
		_Field("EndingPages", "Array", [], "엔딩·크레딧 쪽 (제목|본문, 본문 줄바꿈 \\n)", Element="String"),
	] + HD2DCombatGen.BalanceFields(_Field))
	_WriteJson(os.path.join(Content, *DATA.split("/"), "Balance.edata"), {"Version": 1, "Struct": f"{DATA}/Balance.estruct", "Values": BALANCE})
	HD2DCombatGen.WriteSkills(Content, _Struct, _Table, _Field)


# ================================================================ 게임 UI (.eui)
TEXT_LIGHT = (0.96, 0.93, 0.86, 1)
TEXT_GOLD  = (1.0, 0.85, 0.42, 1)
TEXT_DIM   = (0.72, 0.7, 0.78, 1)
TEXT_CYAN  = (0.6, 0.86, 1.0, 1)
OUTLINE    = (0.03, 0.02, 0.06, 1)
FRAME      = f"{UI_DIR}/Frame.png"
FRAME_L    = f"{UI_DIR}/FrameLight.png"


def Brush(Color=(1, 1, 1, 1), Corner=0, BorderWidth=0, BorderColor=(0, 0, 0, 1), Texture=None, NineSlice=False, TextureSize=48):
	B = {"Color": list(Color), "CornerRadius": Corner, "BorderWidth": BorderWidth, "BorderColor": list(BorderColor)}
	if Texture:
		B["Texture"] = Texture
	if NineSlice:
		# 창틀 텍스처 24 도트 중 가장자리 8 도트 = 1/3, 화면 두께 = Margin × TextureSize
		B["DrawAs"] = "NineSlice"
		B["Margin"] = [1.0 / 3.0] * 4
		B["TextureSize"] = [TextureSize, TextureSize]
	return B


def FrameBrush(Light=False, Size=48):
	return Brush(Texture=FRAME_L if Light else FRAME, NineSlice=True, TextureSize=Size)


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
				  OutlineWidth=Outline, OutlineColor=list(OUTLINE), ShadowOffset=[0, 2], ShadowColor=[0, 0, 0, 0.55])


def Img(Name, Texture, Size, Slot=None, Visibility="HitTestInvisible", Color=(1, 1, 1, 1)):
	return Widget("Image", Name, Slot, Visibility, Brush=Brush(Color, Texture=Texture), ImageSize=[Size, Size])


def Bar(Name, W, H, Fill, Percent=1.0):
	return Widget("ProgressBar", Name, BoxSlot(), MinSize=[W, H], Brush=Brush((0.05, 0.04, 0.08, 0.9), 2, 1, (0.0, 0.0, 0.0, 1)),
				  FillBrush=Brush(Fill, 1), Percent=Percent, FillDirection="LeftToRight")


ROWS = 10


def MenuWindow(Prefix, Title, Hint, Z, Tabs=None, Say=False):
	# 옥토패스풍 메뉴 창: 9-슬라이스 금테 창 + 제목·상태 줄 (+ 탭 / 상점 주인 말) + 왼쪽 목록(ROWS줄 단추 — 아이콘·이름·개수/가격·선택 띠·커서)
	#   + 오른쪽 설명 창(아이콘·이름·종류·설명·능력치 + 장비 비교 4줄 ↑↓ 색) + 아래 안내
	Rows = []
	for I in range(ROWS):
		Rows.append(Widget("Button", f"{Prefix}Row{I}", BoxSlot((0, 0, 0, 2)), "Visible", [
			Widget("Overlay", f"{Prefix}RowBox{I}", BoxSlot(), "SelfHitTestInvisible", [
				Widget("Image", f"{Prefix}Sel{I}", BoxSlot(), "Collapsed", Brush=Brush((1, 1, 1, 1), Texture=f"{UI_DIR}/Select.png"), ImageSize=[0, 0]),
				Widget("HorizontalBox", f"{Prefix}RowItems{I}", BoxSlot((6, 1, 10, 1)), "HitTestInvisible", [
					Widget("Image", f"{Prefix}Cursor{I}", BoxSlot((0, 0, 6, 0), VAlign="Center"), "Hidden",
						   Brush=Brush(Texture=f"{UI_DIR}/Cursor.png"), ImageSize=[18, 18]),
					Img(f"{Prefix}Icon{I}", Icon("Potion"), 32, BoxSlot((0, 0, 10, 0), VAlign="Center")),
					Text(f"{Prefix}Name{I}", "", 21, BoxSlot(VAlign="Center", Size="Fill")),
					Text(f"{Prefix}Count{I}", "", 19, BoxSlot((8, 0, 0, 0), VAlign="Center"), TEXT_GOLD, "Right"),
				]),
			]),
		], Brush=Brush((0, 0, 0, 0)), HoveredBrush=Brush((1.0, 0.85, 0.5, 0.08)), PressedBrush=Brush((1.0, 0.85, 0.5, 0.16)),
			DisabledBrush=Brush((0, 0, 0, 0)), ContentPadding=[0, 0, 0, 0], MinSize=[430, 38]))
	ListY = 122 if (Tabs or Say) else 84
	Children = [
		Widget("Border", f"{Prefix}Bg", StretchSlot(), "Visible", Brush=FrameBrush(), ContentPadding=[0, 0, 0, 0]),
		Text(f"{Prefix}Title", Title, 32, CanvasSlot((0, 0), 34, 20, 0, 0, AutoSize=True, Z=1), TEXT_GOLD),
		Text(f"{Prefix}Status", "", 18, CanvasSlot((0, 0), 200, 31, 0, 0, AutoSize=True, Z=1), TEXT_DIM),
		Widget("HorizontalBox", f"{Prefix}GoldBox", CanvasSlot((1, 0), -34, 24, 0, 0, (1, 0), True, 1), "HitTestInvisible", [
			Img(f"{Prefix}GoldIcon", Icon("Coin"), 26, BoxSlot((0, 0, 8, 0), VAlign="Center")),
			Text(f"{Prefix}Gold", "0 G", 22, BoxSlot(VAlign="Center"), TEXT_GOLD),
		]),
		Widget("Border", f"{Prefix}Rule", CanvasSlot((0, 0), 30, 70, 860, 2, Z=1), Brush=Brush((0.96, 0.77, 0.3, 0.7)), ContentPadding=[0, 0, 0, 0]),
		Widget("VerticalBox", f"{Prefix}List", CanvasSlot((0, 0), 30, ListY, 440, 400, Z=1), "SelfHitTestInvisible", Rows),
		Widget("Border", f"{Prefix}Detail", CanvasSlot((0, 0), 492, ListY, 398, 576 - ListY - 60, Z=1), "HitTestInvisible", [
			Widget("VerticalBox", f"{Prefix}DetailBox", BoxSlot(), "HitTestInvisible", [
				Widget("HorizontalBox", f"{Prefix}DetailHead", BoxSlot((0, 0, 0, 12)), "HitTestInvisible", [
					Img(f"{Prefix}DetailIcon", Icon("Potion"), 64, BoxSlot((0, 0, 14, 0), VAlign="Center")),
					Widget("VerticalBox", f"{Prefix}DetailNames", BoxSlot(VAlign="Center"), "HitTestInvisible", [
						Text(f"{Prefix}DetailName", "", 25, BoxSlot(), TEXT_GOLD),
						Text(f"{Prefix}DetailType", "", 17, BoxSlot((0, 4, 0, 0)), TEXT_DIM),
					]),
				]),
				Text(f"{Prefix}DetailDesc", "", 19, BoxSlot((0, 0, 0, 12)), TEXT_LIGHT, Wrap=True),
				Text(f"{Prefix}DetailStats", "", 18, BoxSlot((0, 0, 0, 8)), TEXT_CYAN, Wrap=True),
			] + [Text(f"{Prefix}Cmp{K}", "", 18, BoxSlot((0, 2, 0, 0)), TEXT_LIGHT, "Left", "Collapsed") for K in range(4)]),
		], Brush=FrameBrush(True, 36), ContentPadding=[22, 18, 22, 16]),
		Text(f"{Prefix}Hint", Hint, 17, CanvasSlot((0.5, 1), 0, -16, 0, 0, (0.5, 1), True, 1), TEXT_DIM, "Center"),
	]
	if Tabs:
		for K, Name in enumerate(Tabs):
			Children.append(Widget("Border", f"{Prefix}Tab{K}", CanvasSlot((0, 0), 30 + K * 132, 80, 124, 34, Z=1), "HitTestInvisible",
								   [Text(f"{Prefix}TabText{K}", Name, 20, BoxSlot(HAlign="Center", VAlign="Center"), TEXT_DIM, "Center")],
								   Brush=Brush((0.1, 0.09, 0.18, 0.7), 3, 1, (0.96, 0.77, 0.3, 0.5)), ContentPadding=[0, 0, 0, 0]))
	if Say:
		Children.append(Img(f"{Prefix}SayPortrait", f"{UI_DIR}/Portraits/Merchant.png", 40, CanvasSlot((0, 0), 30, 78, 40, 40, Z=1)))
		Children.append(Text(f"{Prefix}Say", "", 19, CanvasSlot((0, 0), 80, 88, 0, 0, AutoSize=True, Z=1), (1.0, 0.92, 0.78, 1)))
	return Widget("Canvas", f"{Prefix}Window", CanvasSlot((0.5, 0.5), 0, -6, 920, 576, (0.5, 0.5), Z=Z), "Collapsed", Children)


def WriteUi(Content):
	HD2DArt.WriteUiTextures(os.path.join(Content, *UI_DIR.split("/")))
	C = []
	# ---- 왼쪽 위: 이름·레벨 + 체력/마나/경험치
	C.append(Widget("Border", "StatusPanel", CanvasSlot((0, 0), 16, 14, 372, 0, AutoSize=True), "HitTestInvisible", [
		Widget("VerticalBox", "StatusBox", BoxSlot(), "HitTestInvisible", [
			Widget("HorizontalBox", "NameRow", BoxSlot((0, 0, 0, 6)), "HitTestInvisible", [
				Text("PlayerName", "아르펜", 24, BoxSlot(VAlign="Center", Size="Fill"), TEXT_GOLD),
				Text("LevelText", "Lv 1", 22, BoxSlot(VAlign="Center"), TEXT_LIGHT, "Right"),
			]),
			Widget("HorizontalBox", "HpRow", BoxSlot((0, 0, 0, 5)), "HitTestInvisible", [
				Text("HpLabel", "HP", 16, BoxSlot((0, 0, 8, 0), VAlign="Center"), (1.0, 0.62, 0.55, 1)),
				Widget("Overlay", "HpBox", BoxSlot(VAlign="Center", Size="Fill"), "HitTestInvisible", [
					Bar("HpBar", 280, 18, (0.86, 0.24, 0.24, 1)),
					Text("HpText", "120 / 120", 15, BoxSlot(HAlign="Center", VAlign="Center"), Justify="Center"),
				]),
			]),
			Widget("HorizontalBox", "MpRow", BoxSlot((0, 0, 0, 5)), "HitTestInvisible", [
				Text("MpLabel", "MP", 16, BoxSlot((0, 0, 8, 0), VAlign="Center"), (0.6, 0.78, 1.0, 1)),
				Widget("Overlay", "MpBox", BoxSlot(VAlign="Center", Size="Fill"), "HitTestInvisible", [
					Bar("MpBar", 280, 14, (0.3, 0.52, 0.98, 1)),
					Text("MpText", "50 / 50", 13, BoxSlot(HAlign="Center", VAlign="Center"), Justify="Center"),
				]),
			]),
			Widget("HorizontalBox", "ExpRow", BoxSlot(), "HitTestInvisible", [
				Text("ExpLabel", "EXP", 13, BoxSlot((0, 0, 8, 0), VAlign="Center"), TEXT_GOLD),
				Widget("ProgressBar", "ExpBar", BoxSlot(VAlign="Center", Size="Fill"), MinSize=[280, 5], Brush=Brush((0.05, 0.04, 0.08, 0.9)),
					   FillBrush=Brush((1.0, 0.82, 0.35, 1)), Percent=0.0, FillDirection="LeftToRight"),
			]),
		]),
	], Brush=FrameBrush(), ContentPadding=[22, 14, 22, 16]))
	C.append(Widget("HorizontalBox", "GoldRow", CanvasSlot((0, 0), 30, 150, 0, 0, AutoSize=True), "HitTestInvisible", [
		Img("GoldIcon", Icon("Coin"), 26, BoxSlot((0, 0, 8, 0), VAlign="Center")),
		Text("GoldText", "0", 22, BoxSlot(VAlign="Center"), TEXT_GOLD),
	]))
	# ---- 왼쪽 아래: 장비 무기
	C.append(Widget("Border", "WeaponPanel", CanvasSlot((0, 1), 16, -14, 0, 0, (0, 1), True), "HitTestInvisible", [
		Widget("HorizontalBox", "WeaponRow", BoxSlot(), "HitTestInvisible", [
			Img("WeaponIcon", Icon("Sword"), 52, BoxSlot((0, 0, 14, 0), VAlign="Center")),
			Widget("VerticalBox", "WeaponTexts", BoxSlot(VAlign="Center"), "HitTestInvisible", [
				Text("WeaponName", "여행자의 검", 22, BoxSlot(), TEXT_GOLD),
				Text("WeaponHint", "J 공격 · R 무기 교체 · Space 회피 · I 소지품", 15, BoxSlot((0, 4, 0, 0)), TEXT_DIM),
			]),
		]),
	], Brush=FrameBrush(), ContentPadding=[20, 14, 24, 14]))
	# ---- 오른쪽 위: 퀘스트
	C.append(Widget("Border", "QuestPanel", CanvasSlot((1, 0), -16, 14, 400, 0, (1, 0), True), "HitTestInvisible", [
		Widget("VerticalBox", "QuestBox", BoxSlot(), "HitTestInvisible", [
			Text("QuestTitle", "◆ 촌장의 부탁", 20, BoxSlot((0, 0, 0, 6)), TEXT_GOLD),
			Text("QuestText", "", 18, BoxSlot(), TEXT_LIGHT, Wrap=True),
		]),
	], Brush=FrameBrush(), ContentPadding=[22, 14, 22, 16], MinSize=[400, 0]))
	# ---- 오른쪽 아래: 획득 알림 (최근 것이 아래)
	Toasts = []
	for I in range(4):
		Toasts.append(Widget("Border", f"Toast{I}", BoxSlot((0, 4, 0, 0), HAlign="Right"), "Collapsed", [
			Widget("HorizontalBox", f"ToastRow{I}", BoxSlot(), "HitTestInvisible", [
				Img(f"ToastIcon{I}", Icon("Coin"), 26, BoxSlot((0, 0, 10, 0), VAlign="Center")),
				Text(f"ToastText{I}", "", 19, BoxSlot(VAlign="Center")),
			]),
		], Brush=Brush((0.04, 0.04, 0.1, 0.72), 3, 1, (0.96, 0.77, 0.3, 0.6)), ContentPadding=[12, 6, 16, 6]))
	C.append(Widget("VerticalBox", "ToastBox", CanvasSlot((1, 1), -18, -18, 0, 0, (1, 1), True), "HitTestInvisible", Toasts))
	# ---- 가운데 위: 큰 알림 (레벨 업·보스 등장·퀘스트 완료)
	C.append(Text("Announce", "", 46, CanvasSlot((0.5, 0), 0, 170, 0, 0, (0.5, 0), True, 5), TEXT_GOLD, "Center", "Collapsed", Outline=3))
	C.append(Text("AnnounceSub", "", 22, CanvasSlot((0.5, 0), 0, 230, 0, 0, (0.5, 0), True, 5), TEXT_LIGHT, "Center", "Collapsed"))
	# ---- 아래 가운데: 보스 체력
	C.append(Widget("Border", "BossPanel", CanvasSlot((0.5, 1), 0, -22, 0, 0, (0.5, 1), True, 2), "Collapsed", [
		Widget("VerticalBox", "BossBox", BoxSlot(), "HitTestInvisible", [
			Text("BossName", "고대의 바위 골렘", 22, BoxSlot((0, 0, 0, 6), HAlign="Center"), (1.0, 0.72, 0.62, 1), "Center"),
			Bar("BossBar", 520, 16, (0.9, 0.3, 0.22, 1)),
		]),
	], Brush=FrameBrush(), ContentPadding=[24, 10, 24, 16]))
	# ---- 상호작용 안내
	C.append(Widget("Border", "Prompt", CanvasSlot((0.5, 1), 0, -150, 0, 0, (0.5, 1), True, 3), "Collapsed",
					[Text("PromptText", "E  말하기", 21, BoxSlot(HAlign="Center", VAlign="Center"), TEXT_LIGHT, "Center")],
					Brush=FrameBrush(False, 30), ContentPadding=[18, 9, 18, 9]))
	# ---- 데미지 숫자 템플릿 (복제해 쓴다)
	C.append(Text("DmgTemplate", "0", 28, CanvasSlot((0, 0), 0, 0, 0, 0, (0.5, 1), True, 4), (1, 1, 1, 1), "Center", "Collapsed", Outline=3))
	# ---- 대화 창 (아래 가운데) — 이름표 + 본문(타자기) + 다음 표시
	C.append(Widget("Canvas", "DialogWindow", CanvasSlot((0.5, 1), 0, -24, 940, 170, (0.5, 1), Z=20), "Collapsed", [
		Widget("Border", "DialogBg", StretchSlot(), "Visible", Brush=FrameBrush(), ContentPadding=[0, 0, 0, 0]),
		Widget("Border", "DialogPortraitFrame", CanvasSlot((0, 0), 26, 22, 124, 124, Z=1), "HitTestInvisible",
			   [Img("DialogPortrait", f"{UI_DIR}/Portraits/Hero.png", 108, BoxSlot(HAlign="Center", VAlign="Center"))],
			   Brush=FrameBrush(True, 30), ContentPadding=[8, 8, 8, 8]),
		Widget("Border", "DialogNamePlate", CanvasSlot((0, 0), 168, -22, 0, 0, AutoSize=True, Z=2), "HitTestInvisible",
			   [Text("DialogName", "", 22, BoxSlot(), TEXT_GOLD)], Brush=FrameBrush(True, 30), ContentPadding=[18, 7, 18, 7]),
		Text("DialogText", "", 24, CanvasSlot((0, 0), 176, 42, 720, 104, Z=1), TEXT_LIGHT, Wrap=True),
		Text("DialogNext", "▼", 20, CanvasSlot((1, 1), -34, -18, 0, 0, (1, 1), True, 2), TEXT_GOLD),
	]))
	# ---- BP (부스트) 구슬 — 골드 줄 아래. 채운 구슬 = 쓸 수 있는 BP, 푸른 구슬 = 이번 공격에 올린 단계
	C.append(Widget("HorizontalBox", "BoostRow", CanvasSlot((0, 0), 26, 186, 0, 0, AutoSize=True), "HitTestInvisible", [
		Text("BoostLabel", "BP", 18, BoxSlot((0, 0, 8, 0), VAlign="Center"), (1.0, 0.75, 0.4, 1)),
	] + [Img(f"Orb{K}", f"{UI_DIR}/OrbEmpty.png", 26, BoxSlot((0, 0, 4, 0), VAlign="Center")) for K in range(5)] + [
		Text("BoostLevel", "", 20, BoxSlot((10, 0, 0, 0), VAlign="Center"), (0.6, 0.92, 1.0, 1)),
	]))
	# ---- 부스트 발동 글자 (가운데 아래) + 화면 번쩍임 + 장면 전환 페이드(맨 위)
	C.append(Text("BoostBurst", "", 40, CanvasSlot((0.5, 0.5), 0, 120, 0, 0, (0.5, 0.5), True, 6), (0.65, 0.95, 1.0, 1), "Center", "Collapsed", Outline=3))
	C.append(Widget("Border", "ScreenFlash", StretchSlot(60), "Collapsed", Brush=Brush((1.0, 0.97, 0.88, 1)), ContentPadding=[0, 0, 0, 0]))
	C.append(Widget("Border", "Fade", StretchSlot(100), "Collapsed", Brush=Brush((0.0, 0.0, 0.02, 1)), ContentPadding=[0, 0, 0, 0]))
	# ---- 메뉴 (인벤토리·상점): 화면 어둡게 + 창
	C.append(Widget("Border", "MenuShade", StretchSlot(29), "Collapsed", Brush=Brush((0.01, 0.0, 0.04, 0.55)), ContentPadding=[0, 0, 0, 0]))
	C.append(MenuWindow("Inv", "메뉴", "A/D 탭     W/S 선택     E · J 사용 / 장비     I 닫기", 30, Tabs=["도구", "장비", "퀘스트"]))
	C.append(MenuWindow("Shop", "미라의 잡화점", "W/S 선택     E · J 구입     I · Space 나가기", 31, Say=True))
	# ---- 타이틀 화면 (마을이 흐리게 보이는 위에 어둡게 + 로고 + 처음부터/이어하기)
	def TitleButton(K, Label):
		return Widget("Button", f"TitleRow{K}", BoxSlot((0, 4, 0, 4), HAlign="Center"), "Visible", [
			Widget("HorizontalBox", f"TitleRowBox{K}", BoxSlot(HAlign="Center"), "HitTestInvisible", [
				Widget("Image", f"TitleCursor{K}", BoxSlot((0, 0, 12, 0), VAlign="Center"), "Hidden", Brush=Brush(Texture=f"{UI_DIR}/Cursor.png"),
					   ImageSize=[22, 22]),
				Text(f"TitleText{K}", Label, 30, BoxSlot(VAlign="Center"), TEXT_LIGHT, "Center"),
			]),
		], Brush=Brush((0, 0, 0, 0)), HoveredBrush=Brush((1.0, 0.85, 0.5, 0.08)), PressedBrush=Brush((1.0, 0.85, 0.5, 0.16)),
			DisabledBrush=Brush((0, 0, 0, 0)), ContentPadding=[24, 6, 24, 6], MinSize=[300, 50])

	C.append(Widget("Canvas", "TitleScreen", StretchSlot(80), "Collapsed", [
		Widget("Border", "TitleShade", StretchSlot(), "Visible", Brush=Brush((0.02, 0.01, 0.05, 0.45)), ContentPadding=[0, 0, 0, 0]),
		Widget("Border", "TitleBand", CanvasSlot((0.5, 0), 0, 112, 1280, 210, (0.5, 0), Z=1), "HitTestInvisible",
			   Brush=Brush((0.01, 0.0, 0.04, 0.5)), ContentPadding=[0, 0, 0, 0]),
		Text("TitleLogo", "황혼의 들판", 76, CanvasSlot((0.5, 0), 0, 128, 0, 0, (0.5, 0), True, 2), TEXT_GOLD, "Center", Outline=4),
		Widget("Image", "TitleOrnament", CanvasSlot((0.5, 0), 0, 236, 384, 48, (0.5, 0), Z=2), "HitTestInvisible",
			   Brush=Brush(Texture=f"{UI_DIR}/TitleOrnament.png"), ImageSize=[384, 48]),
		Text("TitleSub", "", 24, CanvasSlot((0.5, 0), 0, 282, 0, 0, (0.5, 0), True, 2), (0.92, 0.86, 1.0, 1), "Center"),
		Widget("VerticalBox", "TitleMenu", CanvasSlot((0.5, 0), 0, 420, 0, 0, (0.5, 0), True, 2), "SelfHitTestInvisible",
			   [TitleButton(0, "처음부터"), TitleButton(1, "이어하기")]),
		Text("TitleInfo", "", 18, CanvasSlot((0.5, 0), 0, 540, 0, 0, (0.5, 0), True, 2), TEXT_DIM, "Center"),
		Text("TitleHint", "W/S 선택    E · J 결정", 17, CanvasSlot((0.5, 1), 0, -40, 0, 0, (0.5, 1), True, 2), TEXT_DIM, "Center"),
		Text("TitleCredit", "ProjectE HD-2D 데모 · 도트 아트 절차 생성 · 효과음 Kenney (CC0)", 14,
			 CanvasSlot((0.5, 1), 0, -16, 0, 0, (0.5, 1), True, 2), (0.6, 0.58, 0.66, 1), "Center"),
	]))

	# ---- 엔딩·크레딧 (메인 퀘스트 마지막 — 검은 화면 위 금빛 제목 + 본문 쪽이 차례로 페이드)
	C.append(Widget("Canvas", "EndingScreen", StretchSlot(90), "Collapsed", [
		Widget("Border", "EndingShade", StretchSlot(), "Visible", Brush=Brush((0.01, 0.0, 0.03, 0.94)), ContentPadding=[0, 0, 0, 0]),
		Widget("Image", "EndingOrnamentTop", CanvasSlot((0.5, 0.5), 0, -150, 384, 48, (0.5, 0.5), Z=1), "HitTestInvisible",
			   Brush=Brush(Texture=f"{UI_DIR}/TitleOrnament.png"), ImageSize=[384, 48]),
		Text("EndingTitle", "", 50, CanvasSlot((0.5, 0.5), 0, -84, 0, 0, (0.5, 0.5), True, 2), TEXT_GOLD, "Center", Outline=3),
		Text("EndingBody", "", 25, CanvasSlot((0.5, 0.5), 0, 10, 980, 220, (0.5, 0), False, 2), TEXT_LIGHT, "Center", Wrap=True),
		Text("EndingHint", "E · J 넘기기", 17, CanvasSlot((0.5, 1), 0, -30, 0, 0, (0.5, 1), True, 2), TEXT_DIM, "Center"),
	]))

	C += HD2DCombatGen.HudWidgets(sys.modules[__name__])  # 전투: 적 실드·약점 태그, 브레이크 글자, 스킬 칸·이름 띠
	Root = Widget("Canvas", "Root", None, "SelfHitTestInvisible", C)
	_WriteJson(os.path.join(Content, *UI_DIR.split("/"), "HUD.eui"),
			   {"Version": 2, "DesignSize": [1280, 720], "ScaleMode": "MatchHeight", "Root": Root, "Animations": []})


# ================================================================ 프리팹
def Link(Id):
	return {"Id": str(Id), "Root": -1}


def Transform(Position=(0.0, 0.0, 0.0), Rotation=None, Scale=(1.0, 1.0, 1.0)):
	return {"Position": [float(V) for V in Position], "Rotation": Rotation or [0.0, 0.0, 0.0, 1.0], "Scale": [float(V) for V in Scale]}


def Sprite(Asset, Slice="", Lit=True, Shadows=True, Blend=3, Visible=True, Cutoff=0.5, Color=(1, 1, 1, 1), Billboard=0):
	# Blend: 0 알파, 1 프리멀티플라이드, 2 가산, 3 마스크 (ESpriteBlendMode 번호 — 씬 JSON은 번호)
	# Billboard: 0 없음, 1 전체, 2 세로축(서서 카메라를 향함 — 캐릭터·세운 소품. 바닥에 눕힌 그림자 원은 0)
	return {"Sprite": Asset, "Slice": Slice, "Color": list(Color), "FlipX": False, "FlipY": False, "SortingLayer": "", "OrderInLayer": 0,
			"Lit": Lit, "CastShadows": Shadows, "Size": [0.0, 0.0], "Visible": Visible, "Blend": Blend, "AlphaCutoff": Cutoff, "SliceMode": 0,
			"Billboard": Billboard}


def Flipbook(Path):
	return {"Flipbook": Path, "Speed": 1.0, "Playing": True, "StartTime": 0.0}


def _QuatRoll(Degrees):
	R = math.radians(Degrees) * 0.5
	return [math.sin(R), 0.0, 0.0, math.cos(R)]


FLAT = _QuatRoll(-90.0)  # 스프라이트 평면(XZ, 앞 +Y)을 바닥(XY, 앞 +Z)에 눕힌다 — BuildHD2D.FLAT과 같은 값


def WritePrefab(Content, Name, Entities):
	_WriteJson(os.path.join(Content, *PREFABS.split("/"), f"{Name}.eprefab"), {"Entities": Entities, "NextId": len(Entities) + 1, "Version": 1})


def Mover(Radius, Half, Speed, Mass, **Extra):
	M = {"AirControl": 1.0, "CapsuleHalfHeight": Half, "CapsuleRadius": Radius, "FaceControlYaw": False, "GravityScale": 1.0, "JumpZVelocity": 0.0,
		 "Mass": Mass, "MaxSlopeAngle": 50.0, "MaxStepHeight": 25.0, "MaxWalkSpeed": Speed, "PushForce": 500.0, "KnockbackDeceleration": 2400.0,
		 "ClientPrediction": False}
	M.update(Extra)
	return M


def WritePrefabs(Content, CameraDistance, PlayMin, PlayMax):
	Foot = -(PLAYER_RADIUS + PLAYER_HALF)
	WritePrefab(Content, "Player", [
		{"Name": "Player", "Parent": -1, "Components": {
			"CharacterMovementComponent": Mover(PLAYER_RADIUS, PLAYER_HALF, 430.0, 70.0, AirControl=0.35, MaxStepHeight=35.0, PushForce=2000.0,
												KnockbackDeceleration=2600.0, ClientPrediction=True),
			"ScriptComponent": {"ExecutionLocation": 2, "ScriptAsset": "Scripts/Demo/HD2D/HD2DPlayer.lua", "PropertyOverrides": json.dumps({
				"CameraDistance": CameraDistance, "MinX": PlayMin[0] + 900.0, "MaxX": PlayMax[0] - 900.0,
				"MinY": PlayMin[1] + 500.0, "MaxY": PlayMax[1] - 650.0}, ensure_ascii=False)},
			"PrefabLinkComponent": Link(1), "TransformComponent": Transform((0, 0, 100))}},
		{"Name": "Visual", "Parent": 0, "Components": {"PrefabLinkComponent": Link(2), "TransformComponent": Transform()}},
		{"Name": "Body", "Parent": 1, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Hero.esprite", "IdleDown0", Billboard=2), "FlipbookComponent": Flipbook("Sprites/HD2D/Hero_IdleDown.eflipbook"),
			"PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, Foot))}},
		{"Name": "Shadow", "Parent": 1, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
			"PrefabLinkComponent": Link(4), "TransformComponent": Transform((0, 0, Foot + 1.5), FLAT, (0.75, 1.0, 0.75))}},
	])

	# 적: 캡슐 이동기 + Visual > Body(몸)·Shadow·HpBack·HpFill(머리 위 체력바 — 맞으면 잠깐 보임)
	Sprites = {"Slime": ("Sprites/HD2D/Slime.esprite", "Idle0", "Sprites/HD2D/Slime_Idle.eflipbook"),
			   "Golem": ("Sprites/HD2D/Golem.esprite", "Dormant0", "Sprites/HD2D/Golem_Dormant.eflipbook"),
			   "SpiderQueen": ("Sprites/HD2D/SpiderQueen.esprite", "Dormant0", "Sprites/HD2D/SpiderQueen_Dormant.eflipbook")}
	Rows = dict(ENEMIES)
	for Kind, (Radius, Half, Lift, BarZ, Mass) in ENEMY_CAPSULE.items():
		Look = Rows[Kind]["Look"] or Kind  # 동굴 변형은 바탕 종류의 그림을 색만 바꿔 쓴다
		SpriteAsset, Slice, Book = Sprites.get(Look, ("Sprites/HD2D/Enemies.esprite", "", f"Sprites/HD2D/{Look}_Idle.eflipbook"))
		EFoot = -(Radius + Half)
		Speed = Rows[Kind]["MoveSpeed"]
		Script = BOSS_SCRIPTS.get(Kind, "Scripts/Demo/HD2D/HD2DEnemy.lua")
		ShadowScale = {"Golem": 3.4, "SpiderQueen": 3.8, "Bat": 0.6}.get(Look, 0.8)
		Extra = []
		if Kind == "SpiderQueen":
			# 등의 수정빛 (어두운 동굴에서 보스가 묻히지 않게 — 몸과 둘레 바닥을 푸르게 비춘다)
			Extra.append({"Name": "Glow", "Parent": 1, "Components": {
				"PointLightComponent": {"Color": [0.62, 0.8, 1.0], "Intensity": 4.5, "Radius": 750.0, "CastShadows": False},
				"PrefabLinkComponent": Link(7), "TransformComponent": Transform((0, 120, EFoot + 260))}})
		Extra += HD2DCombatGen.EnemyExtraChildren(Kind, Link, Transform, Sprite, SpriteAsset, Slice, EFoot + Lift)  # 정예 금빛 윤곽
		WritePrefab(Content, Kind, [
			{"Name": Kind, "Parent": -1, "Components": {
				"CharacterMovementComponent": Mover(Radius, Half, Speed, Mass, PushForce=4000.0 if Kind in BOSS_SCRIPTS else 500.0),
				"ScriptComponent": {"ExecutionLocation": 0, "ScriptAsset": Script, "PropertyOverrides": json.dumps({"Kind": Kind}, ensure_ascii=False)},
				"PrefabLinkComponent": Link(1), "TransformComponent": Transform()}},
			{"Name": "Visual", "Parent": 0, "Components": {"PrefabLinkComponent": Link(2), "TransformComponent": Transform()}},
			{"Name": "Body", "Parent": 1, "Components": {
				"SpriteComponent": Sprite(SpriteAsset, Slice, Billboard=2, Color=Rows[Kind]["Tint"]), "FlipbookComponent": Flipbook(Book),
				"PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, EFoot + Lift), None, HD2DCombatGen.BodyScale(Kind))}},
			{"Name": "Shadow", "Parent": 1, "Components": {
				"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
				"PrefabLinkComponent": Link(4), "TransformComponent": Transform((0, 0, EFoot + 1.5), FLAT, (ShadowScale, 1.0, ShadowScale))}},
			{"Name": "HpBack", "Parent": 1, "Components": {
				"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "HpBack", Lit=False, Shadows=False, Blend=0, Visible=False, Billboard=2),
				"PrefabLinkComponent": Link(5), "TransformComponent": Transform((0, 6, EFoot + BarZ))}},
			{"Name": "HpFill", "Parent": 1, "Components": {
				"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "HpFill", Lit=False, Shadows=False, Blend=0, Visible=False, Color=(0.95, 0.3, 0.3, 1), Billboard=2),
				"PrefabLinkComponent": Link(6), "TransformComponent": Transform((-60, 8, EFoot + BarZ))}},
		] + Extra)

	# 효과 조각: 스크립트 없음 — HD2DGame.lua가 만든 직후 콜백에서 모양을 정하고 수명이 끝나면 지운다
	WritePrefab(Content, "FxSprite", [
		{"Name": "FxSprite", "Parent": -1, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Spark0", Lit=False, Shadows=False, Blend=0, Visible=False),
			"FlipbookComponent": Flipbook(""), "PrefabLinkComponent": Link(1), "TransformComponent": Transform()}},
	])
	# 보물상자: 정적 상자 콜라이더(루트) + 몸 스프라이트 (스크립트 없음 — 관리자가 열기·내용을 맡는다)
	WritePrefab(Content, "Chest", [
		{"Name": "Chest", "Parent": -1, "Components": {
			"BoxColliderComponent": {"HalfExtents": [62.0, 36.0, 45.0]}, "PrefabLinkComponent": Link(1), "TransformComponent": Transform()}},
		{"Name": "Body", "Parent": 0, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Props.esprite", "Chest0", Billboard=2), "FlipbookComponent": Flipbook(""),
			"PrefabLinkComponent": Link(2), "TransformComponent": Transform((0, 0, -45))}},
	])
	# 소품 (게시판 = 저장 지점, 고양이 = 서브 퀘스트): 정적 상자 콜라이더 + 몸(슬라이스·플립북은 관리자가) + 그림자. 고양이는 콜라이더 없는 판
	for Name, Half in (("Prop", (50.0, 26.0, 45.0)), ("PropSmall", None)):
		Root = {"PrefabLinkComponent": Link(1), "TransformComponent": Transform()}
		if Half:
			Root["BoxColliderComponent"] = {"HalfExtents": list(Half)}
		WritePrefab(Content, Name, [
			{"Name": Name, "Parent": -1, "Components": Root},
			{"Name": "Body", "Parent": 0, "Components": {
				"SpriteComponent": Sprite("Sprites/HD2D/Props.esprite", "Board", Billboard=2), "FlipbookComponent": Flipbook(""),
				"PrefabLinkComponent": Link(2), "TransformComponent": Transform((0, 0, -45))}},
			{"Name": "Shadow", "Parent": 0, "Components": {
				"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
				"PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, -43.5), FLAT, (0.7, 1.0, 0.7))}},
			{"Name": "Marker", "Parent": 0, "Components": {
				"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Exclaim", Lit=False, Shadows=False, Blend=0, Visible=False, Billboard=2),
				"PrefabLinkComponent": Link(4), "TransformComponent": Transform((0, 6, 130))}},
		])
	# 마을 사람: 정적 상자 콜라이더 + 몸(대기 플립북은 관리자가 표 값으로) + 그림자 + 머리 위 "!" 표시
	WritePrefab(Content, "Npc", [
		{"Name": "Npc", "Parent": -1, "Components": {
			"BoxColliderComponent": {"HalfExtents": [36.0, 30.0, 85.0]}, "PrefabLinkComponent": Link(1), "TransformComponent": Transform()}},
		{"Name": "Body", "Parent": 0, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Npcs.esprite", "Elder0", Billboard=2), "FlipbookComponent": Flipbook(""),
			"PrefabLinkComponent": Link(2), "TransformComponent": Transform((0, 0, -85))}},
		{"Name": "Shadow", "Parent": 0, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
			"PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, -83.5), FLAT, (0.75, 1.0, 0.75))}},
		{"Name": "Marker", "Parent": 0, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Exclaim", Lit=False, Shadows=False, Blend=0, Visible=False, Billboard=2),
			"PrefabLinkComponent": Link(4), "TransformComponent": Transform((0, 6, 135))}},
	])


# ================================================================ 씬 배치
def ReserveSpots(Reserve, Layout=None):
	# 배경 무작위 배치(바위·덤불·풀)가 게임 자리를 피하게 — 맵 생성기가 무작위 배치 전에 부른다
	L = Layout or VILLAGE
	for _, X, Y in L.Enemies:
		Reserve(X, Y, 150.0)
	for _, X, Y in L.Npcs:
		Reserve(X, Y, 110.0)
	for X, Y, _ in L.Chests:
		Reserve(X, Y, 120.0)
	for _, X, Y in L.Props:
		Reserve(X, Y, 100.0)
	if L.Boss:
		Reserve(L.Boss[0], L.Boss[1], BOSS_ARENA_RADIUS)


NAV_SETTINGS = {"AgentRadius": 35.0, "AgentHeight": 150.0, "AgentMaxClimb": 40.0, "AgentMaxSlopeDegrees": 40.0, "CellSize": 20.0, "CellHeight": 10.0}


def AddGame(S, Height, Path, AutoPlay=False, Layout=None, Title=None, NavMesh="Scenes/Demo/HD2D.enav",
			TravelTestScene="Scenes/Demo/_HD2DAutoPlay.escene"):
	# 게임 관리자(맵 id + 적·보물상자·마을 사람·소품·보스 자리 + 길) + HUD + 내비메시 엔티티.
	#   AutoPlay: False | True(= "Full") | 시나리오 이름 (HD2DAutoPilot.lua). Title: 타이틀 화면을 띄우는가 (None = 맵 설정 && 사람/Full 실행)
	#   NavMesh: 구운 .enav (Content 기준, None = 없음 — 적은 곧장 걷는다)
	L = Layout or VILLAGE

	def P3(X, Y, Lift):
		return f"{X:.0f},{Y:.0f},{Height(X, Y) + Lift:.0f}"

	Enemies = ";".join(f"{Kind},{P3(X, Y, ENEMY_CAPSULE[Kind][0] + ENEMY_CAPSULE[Kind][1] + 4.0)}" for Kind, X, Y in L.Enemies)
	Npcs = ";".join(f"{Id},{P3(X, Y, 85.0)}" for Id, X, Y in L.Npcs)
	Chests = ";".join(f"{P3(X, Y, 45.0)},{Contents}" for X, Y, Contents in L.Chests)
	Props = ";".join(f"{Id},{P3(X, Y, 45.0)}" for Id, X, Y in L.Props)
	BossCap = ENEMY_CAPSULE[L.BossKind]
	BossLift = BossCap[0] + BossCap[1] + 4.0
	Scenario = ("Full" if AutoPlay is True else (AutoPlay or ""))
	if Title is None:
		Title = L.Title and Scenario in ("", "Full", "TitleShot")
	Overrides = {
		"Map": L.MapId, "Enemies": Enemies, "Npcs": Npcs, "Chests": Chests, "Props": Props,
		"Boss": P3(L.Boss[0], L.Boss[1], BossLift) if L.Boss else "",
		"Path": ";".join(f"{X:.0f},{Y:.0f}" for X, Y in Path), "AutoPlay": Scenario, "Title": bool(Title)}
	if L.BossKind != "Golem":  # 기본값과 다른 것만 (마을 씬 파일이 그대로이게)
		Overrides.update({"BossKind": L.BossKind, "BossLift": BossLift})
	if L.BossReward != "HiPotion*2+Gold*100":
		Overrides["BossReward"] = L.BossReward
	if not L.Respawn:
		Overrides["Respawn"] = False
	Overrides.update(L.Extra)
	Overrides.update(HD2DCombatGen.GameOverrides(L, P3))  # 동료 자리 (마을)
	S.Add("HD2DGame", {"ScriptComponent": {"ScriptAsset": "Scripts/Demo/HD2D/HD2DGame.lua", "ExecutionLocation": 0,
		"PropertyOverrides": json.dumps(Overrides, ensure_ascii=False)}})
	S.Add("HUD", {"UIComponent": {"Asset": f"{UI_DIR}/HUD.eui", "ZOrder": 0, "Visible": True, "ReceiveInput": True, "KeyboardFocus": False},
				  "ScriptComponent": {"ScriptAsset": "Scripts/Demo/HD2D/HD2DHud.lua", "ExecutionLocation": 0, "PropertyOverrides": ""}})
	if NavMesh:
		S.Add("NavMesh", {"NavMeshComponent": dict(NAV_SETTINGS, NavMeshAsset=NavMesh)})
	if Scenario == "Full":
		# 자동 검증 전용: 같은 씬으로 가는 맵 이동 트리거 + 도착 자리 (상태 유지·세션 슬롯 확인)
		(TX, TY), (SX, SY) = TRAVEL_TEST
		AddTravel(S, Height, "Travel_Test", TX, TY, TravelTestScene, "Test", (110.0, 90.0, 120.0))
		S.Add("Spawn_Test", {}, (SX, SY, Height(SX, SY)))


def AddTravel(S, Height, Name, X, Y, TargetScene, SpawnName, Half=(150.0, 100.0, 150.0)):
	# 맵 이동 트리거 (HD2DTravel.lua) — 다른 맵 생성기도 쓴다. 도착 씬에는 "Spawn_<SpawnName>" 엔티티(위치 = 발 자리)를 둔다
	return S.Add(Name, {"BoxColliderComponent": {"HalfExtents": [float(V) for V in Half], "IsTrigger": True},
						"ScriptComponent": {"ScriptAsset": "Scripts/Demo/HD2D/HD2DTravel.lua", "ExecutionLocation": 0, "PropertyOverrides": json.dumps(
							{"TargetScene": TargetScene, "SpawnName": SpawnName}, ensure_ascii=False)}},
				 (X, Y, Height(X, Y) + Half[2] * 0.5))


def AddPlayer(S, Start, Height):
	Z = Height(*Start) + PLAYER_RADIUS + PLAYER_HALF + 4.0
	Index = len(S.Entities)
	S.Add("Player", {
		"PrefabInstanceComponent": {"Asset": f"{PREFABS}/Player.eprefab", "Overrides": ""},
		"PrefabLinkComponent": {"Id": "1", "Root": Index}},
		(Start[0], Start[1], Z))
	return Z


# ================================================================ 내비메시 굽기용 씬
def WriteNavBakeScene(Scene, Height, PlayMin, PlayMax, Path, Cell=100.0, WaterBelow=-15.0):
	# 굽기 입력은 보이는 정적 메시뿐(지형 제외)이므로: 원래 씬의 모델·정적 메시는 그대로, 막는 상자 콜라이더(트리거 아님)는 같은 크기 큐브로,
	#   지형은 놀이 영역을 Cell 칸 판(위 = 지면 높이)으로 깐다 (물 — 지면이 WaterBelow 아래인 칸 — 은 비운다).
	#   스크립트·스프라이트·조명·지형·폴리지 등은 뺀다 (엔티티 순서·부모는 그대로 — 스크립트 하위 메시는 굽기가 어차피 뺀다)
	Keep = ("TransformComponent", "ModelComponent", "StaticMeshComponent")
	Entities = []
	for E in Scene.Entities:
		Comps = {K: V for K, V in E["Components"].items() if K in Keep}
		Box = E["Components"].get("BoxColliderComponent")
		if Box and not Box.get("IsTrigger") and "ModelComponent" not in Comps and "StaticMeshComponent" not in Comps:
			T = dict(Comps["TransformComponent"])
			T["Scale"] = [V / 50.0 for V in Box["HalfExtents"]]  # 내장 큐브 = 100cm
			Comps["TransformComponent"] = T
			Comps["StaticMeshComponent"] = {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Demo/HD2D/WoodPost.emat"}
		Entities.append({"Components": Comps, "Name": E["Name"], "Parent": E["Parent"]})
	Count = 0
	X = PlayMin[0] + Cell * 0.5
	while X < PlayMax[0]:
		Y = PlayMin[1] + Cell * 0.5
		while Y < PlayMax[1]:
			H = Height(X, Y)
			if H >= WaterBelow:
				Entities.append({"Name": "NavGround", "Parent": -1, "Components": {
					"TransformComponent": Transform((X, Y, H - 10.0), None, (Cell / 100.0 + 0.02, Cell / 100.0 + 0.02, 0.2)),
					"StaticMeshComponent": {"MeshAsset": "primitive:cube", "MaterialAsset": "Materials/Demo/HD2D/WoodPost.emat"}}})
				Count += 1
			Y += Cell
		X += Cell
	Entities.append({"Name": "NavMesh", "Parent": -1, "Components": {"TransformComponent": Transform(), "NavMeshComponent": dict(NAV_SETTINGS, NavMeshAsset="")}})
	return {"Entities": Entities, "Version": 1}, Count


def WriteAll(Content, CameraDistance, PlayMin, PlayMax):
	WriteData(Content)
	WriteUi(Content)
	WritePrefabs(Content, CameraDistance, PlayMin, PlayMax)
	HD2DCombatGen.WriteAll(Content, sys.modules[__name__])  # 전투 효과·아이콘·동료 도트 아트 + 동료 프리팹


def WriteNavBake(Content, Scene, Height, PlayMin, PlayMax, Path, Name="_HD2DNavBake", Cell=100.0):
	Doc, Count = WriteNavBakeScene(Scene, Height, PlayMin, PlayMax, Path, Cell)
	_WriteJson(os.path.join(Content, "Scenes", "Demo", f"{Name}.escene"), Doc)
	print(f"내비메시 굽기용 씬: Scenes/Demo/{Name}.escene (바닥 판 {Count}개) - 머리 주석의 순서로 굽는다")


if __name__ == "__main__":
	import shutil
	import sys
	if "--install-nav" in sys.argv:
		Root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "Projects", "Sample", "Content", "Scenes", "Demo"))
		shutil.copyfile(os.path.join(Root, "_HD2DNavBake.enav"), os.path.join(Root, "HD2D.enav"))
		print("Scenes/Demo/HD2D.enav 설치")
