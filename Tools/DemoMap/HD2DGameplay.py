# HD-2D 데모 맵 게임플레이 콘텐츠 (BuildHD2D.py가 부른다 — 배경·조명·카메라는 BuildHD2D.py, 여기는 게임에 쓰이는 것만):
#   데이터 표(Data/Demo/HD2D — 무기·아이템·적·마을 사람·퀘스트·밸런스), 게임 UI(UI/Demo/HD2D/HUD.eui + 창틀·아이콘 텍스처),
#   프리팹(Prefabs/Demo/HD2D — 플레이어·적 5종·보스·효과 조각·보물상자·마을 사람), 씬 배치(게임 관리자 속성 — 적 자리·보물상자·마을 사람·보스·길,
#   HUD 엔티티, 플레이어), 자동 검증 시나리오 변형(--views).
#   규약: 좌표·카메라는 BuildHD2D.py 머리 주석과 같다 (화면 오른쪽 = +X, 화면 안쪽 = -Y, 스프라이트는 XZ 평면 + 앞면 +Y, 1 도트 = 6cm).
#         적·보물상자·마을 사람은 게임 관리자(HD2DGame.lua)가 시작할 때 프리팹으로 만든다 — 자리는 여기서 지면 높이까지 계산해 속성 문자열로 넘긴다.
#         수치(무기 피해·적 체력·가격·대사)는 데이터 표에 있고 스크립트는 HD2DData.lua로만 읽는다.
import json
import math
import os

import HD2DArt

PREFABS = "Prefabs/Demo/HD2D"
DATA    = "Data/Demo/HD2D"
UI_DIR  = "UI/Demo/HD2D"

# ---- 배치 (마을 = 서쪽, 들판 = 동쪽) — 지면 높이는 BuildHD2D의 높이 함수로 ------------------------------------------------------------
NPCS = [  # (Id, X, Y) — Id = Npcs.etable 행
	("Elder", -2150.0, 160.0),
	("Merchant", -1380.0, 330.0),
	("Guard", -430.0, -170.0),
	("Girl", 1000.0, 560.0),
]
CHESTS = [  # (X, Y, 내용 "아이템*개수+...", 골드는 Gold*n)
	(-3250.0, -450.0, "Spear*1+Potion*1"),
	(1450.0, -800.0, "Ether*2+Gold*50"),
	(950.0, 1080.0, "Staff*1+HiPotion*1"),
]
ENEMY_SLOTS = [  # (종류, X, Y) — 죽으면 RespawnTime 뒤 같은 자리에 다시 (플레이어가 가까우면 미룸)
	("Slime", 1000.0, -700.0), ("Slime", 1700.0, -250.0), ("Slime", 2300.0, 450.0), ("Slime", 900.0, 650.0), ("Slime", 3150.0, -250.0),
	("Bat", 2400.0, -300.0), ("Bat", 3400.0, -1350.0), ("Bat", 1900.0, -1500.0),
	("Goblin", 3400.0, 750.0), ("Goblin", 2800.0, 950.0), ("Goblin", 3900.0, 350.0),
	("Archer", 1250.0, -1350.0), ("Archer", 3300.0, -1450.0),
	("Mushroom", 2200.0, -500.0), ("Mushroom", 600.0, -950.0), ("Mushroom", 3700.0, -300.0),
]
BOSS = (4650.0, -250.0)
BOSS_ARENA_RADIUS = 450.0
# 자동 검증 시작 자리 / 스크린샷 시나리오 (이름: (시작 자리, 시나리오)) — BuildHD2D.py --views가 _HD2D<이름>.escene으로 쓴다
AUTOPLAY_START = (-2350.0, 300.0)
SHOT_SCENES = {
	"Shot_Inventory": ((-2350.0, 300.0), "Inventory"),
	"Shot_Shop":      ((-1450.0, 520.0), "Shop"),
	"Shot_Dialog":    ((-2250.0, 330.0), "Dialog"),
	"Shot_Combat":    ((3000.0, 650.0), "Combat"),
	"Shot_Boss":      ((3900.0, -100.0), "Boss"),
}

PLAYER_RADIUS, PLAYER_HALF = 32.0, 53.0  # 캡슐 바닥 = 중심 - 85
ENEMY_CAPSULE = {  # 종류: (반지름, 반높이, 몸 스프라이트 높이(발 위, 박쥐만 공중), 체력바 높이(발 위), 질량)
	"Slime":    (38.0, 4.0, 0.0, 95.0, 30.0),
	"Bat":      (30.0, 10.0, 105.0, 185.0, 15.0),
	"Goblin":   (34.0, 30.0, 0.0, 160.0, 45.0),
	"Archer":   (32.0, 40.0, 0.0, 205.0, 40.0),
	"Mushroom": (40.0, 12.0, 0.0, 165.0, 40.0),
	"Golem":    (95.0, 80.0, 0.0, 450.0, 600.0),
}


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
]

ITEMS = [
	("Sword", {"DisplayName": "여행자의 검", "Kind": "Weapon", "Amount": 0, "Weapon": "Sword", "Icon": Icon("Sword"), "Description": "", "Price": 0}),
	("Spear", {"DisplayName": "기사의 창", "Kind": "Weapon", "Amount": 0, "Weapon": "Spear", "Icon": Icon("Spear"), "Description": "", "Price": 160}),
	("Bow", {"DisplayName": "사냥꾼의 활", "Kind": "Weapon", "Amount": 0, "Weapon": "Bow", "Icon": Icon("Bow"), "Description": "", "Price": 90}),
	("Staff", {"DisplayName": "현자의 지팡이", "Kind": "Weapon", "Amount": 0, "Weapon": "Staff", "Icon": Icon("Staff"), "Description": "", "Price": 140}),
	("Potion", {"DisplayName": "회복약", "Kind": "Heal", "Amount": 50, "Weapon": "", "Icon": Icon("Potion"),
				"Description": "약초를 달인 붉은 물약. 체력을 50 회복한다. (단축키 1)", "Price": 15}),
	("HiPotion", {"DisplayName": "고급 회복약", "Kind": "Heal", "Amount": 130, "Weapon": "", "Icon": Icon("HiPotion"),
				  "Description": "진하게 우려낸 귀한 물약. 체력을 130 회복한다.", "Price": 45}),
	("Ether", {"DisplayName": "마나 물약", "Kind": "Mana", "Amount": 40, "Weapon": "", "Icon": Icon("Ether"),
			   "Description": "푸른 빛이 도는 물약. 마나를 40 회복한다. (단축키 2)", "Price": 25}),
	("Elixir", {"DisplayName": "엘릭서", "Kind": "Elixir", "Amount": 0, "Weapon": "", "Icon": Icon("Elixir"),
				"Description": "전설의 영약. 체력과 마나를 모두 회복한다.", "Price": 300}),
]

ENEMIES = [
	("Slime", {"DisplayName": "슬라임", "Behavior": "Hopper", "MaxHealth": 30, "ContactDamage": 8, "AttackDamage": 8, "MoveSpeed": 300, "AggroRange": 700,
			   "AttackRange": 0, "AttackCooldown": 1.1, "WindupTime": 0.0, "ProjectileSpeed": 0, "GoldMin": 2, "GoldMax": 4, "Exp": 6,
			   "DropItem": "Potion", "DropChance": 0.15, "Radius": 48, "RespawnTime": 12}),
	("Bat", {"DisplayName": "흡혈 박쥐", "Behavior": "Flyer", "MaxHealth": 22, "ContactDamage": 6, "AttackDamage": 11, "MoveSpeed": 380, "AggroRange": 950,
			 "AttackRange": 430, "AttackCooldown": 2.2, "WindupTime": 0.45, "ProjectileSpeed": 1150, "GoldMin": 2, "GoldMax": 5, "Exp": 8,
			 "DropItem": "Ether", "DropChance": 0.15, "Radius": 50, "RespawnTime": 14}),
	("Goblin", {"DisplayName": "고블린 도적", "Behavior": "Charger", "MaxHealth": 48, "ContactDamage": 6, "AttackDamage": 14, "MoveSpeed": 330, "AggroRange": 950,
				"AttackRange": 380, "AttackCooldown": 2.0, "WindupTime": 0.5, "ProjectileSpeed": 1250, "GoldMin": 4, "GoldMax": 8, "Exp": 12,
				"DropItem": "Potion", "DropChance": 0.2, "Radius": 48, "RespawnTime": 16}),
	("Archer", {"DisplayName": "해골 궁수", "Behavior": "Archer", "MaxHealth": 36, "ContactDamage": 5, "AttackDamage": 12, "MoveSpeed": 220, "AggroRange": 1300,
				"AttackRange": 950, "AttackCooldown": 2.4, "WindupTime": 0.75, "ProjectileSpeed": 1100, "GoldMin": 4, "GoldMax": 8, "Exp": 14,
				"DropItem": "HiPotion", "DropChance": 0.1, "Radius": 46, "RespawnTime": 18}),
	("Mushroom", {"DisplayName": "독버섯", "Behavior": "Spore", "MaxHealth": 44, "ContactDamage": 6, "AttackDamage": 5, "MoveSpeed": 160, "AggroRange": 750,
				  "AttackRange": 280, "AttackCooldown": 3.2, "WindupTime": 0.6, "ProjectileSpeed": 0, "GoldMin": 3, "GoldMax": 6, "Exp": 10,
				  "DropItem": "Ether", "DropChance": 0.15, "Radius": 50, "RespawnTime": 16}),
	("Golem", {"DisplayName": "고대의 바위 골렘", "Behavior": "Boss", "MaxHealth": 900, "ContactDamage": 14, "AttackDamage": 24, "MoveSpeed": 170, "AggroRange": 850,
			   "AttackRange": 340, "AttackCooldown": 1.5, "WindupTime": 0.8, "ProjectileSpeed": 900, "GoldMin": 150, "GoldMax": 150, "Exp": 150,
			   "DropItem": "Elixir", "DropChance": 1.0, "Radius": 115, "RespawnTime": 0}),
]

NPC_ROWS = [
	("Elder", {"DisplayName": "촌장 바르톨로", "Flipbook": "Sprites/HD2D/Npc_Elder.eflipbook", "Role": "Elder", "Lines": []}),
	("Merchant", {"DisplayName": "상인 미라", "Flipbook": "Sprites/HD2D/Npc_Merchant.eflipbook", "Role": "Shop",
				  "Lines": ["어서 오세요, 여행자님! 들판으로 나가신다면 준비는 단단히 하셔야죠.", "회복약부터 활과 지팡이까지, 필요한 건 다 있답니다."]}),
	("Guard", {"DisplayName": "경비병 오웬", "Flipbook": "Sprites/HD2D/Npc_Guard.eflipbook", "Role": "Talk",
			   "Lines": ["문 밖은 마물 천지야. 조심하라고.", "R 키로 무기를 바꿀 수 있지. 창은 멀리, 활과 지팡이는 더 멀리 닿는다네.",
						 "Space로 몸을 날리면 잠깐 동안은 아무것도 맞지 않아."]}),
	("Girl", {"DisplayName": "꼬마 리나", "Flipbook": "Sprites/HD2D/Npc_Girl.eflipbook", "Role": "Talk",
			  "Lines": ["언니 오빠들이 그러는데, 연못 근처에서 보물상자를 봤대!", "보라색 버섯은 만지면 안 돼. 독 웅덩이를 뿌린다니까!"]}),
]

QUESTS = [
	("Stage0", {"Title": "촌장의 부탁", "Objective": "광장의 촌장 바르톨로에게 말을 걸자", "KillGoal": 0, "Lines": [], "WaitLines": []}),
	("Stage1", {"Title": "촌장의 부탁", "Objective": "들판의 마물을 쓰러뜨리자", "KillGoal": 6,
				"Lines": ["오오, 여행자여. 마침 잘 왔네.", "요즘 동쪽 들판에 마물이 들끓어 마을 사람들이 밭에 나가질 못하고 있다네.",
						  "부디 들판의 마물을 몰아내 주게. 대장간 옆 상자에 오래된 창이 있으니 가져가도 좋네."],
				"WaitLines": ["들판의 마물을 부탁하네. 대장간 옆 상자도 잊지 말게나.", "야영지 쪽에도 누가 두고 간 상자가 있다더군."]}),
	("Stage2", {"Title": "고대의 수호자", "Objective": "들판 동쪽 끝의 바위 골렘을 쓰러뜨리자", "KillGoal": 0,
				"Lines": ["대단하군! 하지만 마물들이 날뛰는 까닭은 따로 있다네.", "들판 동쪽 끝에서 고대의 바위 골렘이 깨어났다는 소문이야.",
						  "그 골렘을 쓰러뜨리면 들판도 다시 평화로워질 걸세."],
				"WaitLines": ["골렘은 들판 동쪽 끝에 있다네. 바위를 던지니 바닥의 붉은 원을 조심하게."]}),
	("Stage3", {"Title": "고대의 수호자", "Objective": "촌장 바르톨로에게 돌아가 보고하자", "KillGoal": 0,
				"Lines": ["골렘은 쓰러뜨렸는가? 어서 돌아와 이야기를 들려주게."], "WaitLines": []}),
	("Stage4", {"Title": "마을의 영웅", "Objective": "모든 의뢰 완료! 들판을 자유롭게 탐험하자", "KillGoal": 0,
				"Lines": ["정말로 해냈구먼! 자네는 이 마을의 영웅일세.", "약소하지만 사례를 받아 주게. 마을 사람 모두의 마음이라네."],
				"WaitLines": ["덕분에 마을이 평화롭구먼. 고맙네, 젊은이."]}),
]

BALANCE = {
	"PlayerName": "아르펜", "MaxHealth": 120, "MaxMana": 50, "ManaRegen": 3.0, "HealthPerLevel": 18, "ManaPerLevel": 8, "DamagePerLevel": 0.12,
	"ExpTable": [0, 30, 75, 140, 230, 350, 500, 700], "StartWeapon": "Sword", "StartGold": 120, "StartItems": ["Potion", "Potion", "Potion"],
	"ShopStock": ["Potion", "HiPotion", "Ether", "Bow", "Elixir"], "InvulnTime": 0.8, "DashSpeed": 1700, "DashTime": 0.2, "DashCooldown": 0.5,
	"CritChance": 0.1, "CritMultiplier": 1.6, "QuestRewardGold": 300, "QuestRewardItem": "Elixir",
}


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
	])
	_Table(Content, "Weapons", "Weapon", WEAPONS)

	_Struct(Content, "Item", "HD2D 아이템 (Items.etable, 행 이름 = 아이템 id — 인벤토리·상점·보물상자·전리품)", [
		_Field("DisplayName", "String", "아이템", "표시 이름"),
		_Field("Kind", "Enum", "Heal", "무기(장비) / 체력 회복 / 마나 회복 / 둘 다 전부", Values=["Weapon", "Heal", "Mana", "Elixir"]),
		_Field("Amount", "Float", 0, "회복량"),
		_Field("Weapon", "String", "", "무기 id (Kind = Weapon)"),
		_Field("Icon", "String", Icon("Potion"), "UI 아이콘 (Content 기준)"),
		_Field("Description", "String", "", "설명 (무기는 비우면 무기 표 설명)"),
		_Field("Price", "Int", 10, "상점 가격 (골드)"),
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
	])
	_Table(Content, "Enemies", "Enemy", ENEMIES)

	_Struct(Content, "Npc", "HD2D 마을 사람 (Npcs.etable, 행 이름 = 마을 사람 id)", [
		_Field("DisplayName", "String", "마을 사람", "대화 창 이름"),
		_Field("Flipbook", "String", "", "대기 플립북"),
		_Field("Role", "Enum", "Talk", "대화만 / 대화 뒤 상점 / 촌장(퀘스트 단계 대사)", Values=["Talk", "Shop", "Elder"]),
		_Field("Lines", "Array", [], "대사 (촌장은 Quests.etable 단계 대사)", Element="String"),
	])
	_Table(Content, "Npcs", "Npc", NPC_ROWS)

	_Struct(Content, "Quest", "HD2D 퀘스트 단계 (Quests.etable, 행 이름 = Stage<n>, 촌장에게 말하면 다음 단계로)", [
		_Field("Title", "String", "", "퀘스트 이름"),
		_Field("Objective", "String", "", "목표 표시"),
		_Field("KillGoal", "Int", 0, "> 0이면 마물 처치 수가 이만큼 되면 다음 단계"),
		_Field("Lines", "Array", [], "이 단계로 넘어갈 때 촌장 대사", Element="String"),
		_Field("WaitLines", "Array", [], "이 단계에서 아직 넘어갈 수 없을 때 촌장 대사", Element="String"),
	])
	_Table(Content, "Quests", "Quest", QUESTS)

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
		_Field("QuestRewardGold", "Int", 100, "퀘스트 완료 골드"),
		_Field("QuestRewardItem", "String", "", "퀘스트 완료 아이템"),
	])
	_WriteJson(os.path.join(Content, *DATA.split("/"), "Balance.edata"), {"Version": 1, "Struct": f"{DATA}/Balance.estruct", "Values": BALANCE})


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


ROWS = 8


def MenuWindow(Prefix, Title, Hint, Z):
	# 옥토패스풍 메뉴 창: 9-슬라이스 금테 창 + 제목·상태 줄 + 왼쪽 목록(ROWS줄 단추 — 아이콘·이름·개수/가격·선택 띠·커서) + 오른쪽 설명 창 + 아래 안내
	Rows = []
	for I in range(ROWS):
		Rows.append(Widget("Button", f"{Prefix}Row{I}", BoxSlot((0, 0, 0, 4)), "Visible", [
			Widget("Overlay", f"{Prefix}RowBox{I}", BoxSlot(), "SelfHitTestInvisible", [
				Widget("Image", f"{Prefix}Sel{I}", BoxSlot(), "Collapsed", Brush=Brush((1, 1, 1, 1), Texture=f"{UI_DIR}/Select.png"), ImageSize=[0, 0]),
				Widget("HorizontalBox", f"{Prefix}RowItems{I}", BoxSlot((6, 2, 10, 2)), "HitTestInvisible", [
					Widget("Image", f"{Prefix}Cursor{I}", BoxSlot((0, 0, 6, 0), VAlign="Center"), "Hidden",
						   Brush=Brush(Texture=f"{UI_DIR}/Cursor.png"), ImageSize=[18, 18]),
					Img(f"{Prefix}Icon{I}", Icon("Potion"), 36, BoxSlot((0, 0, 10, 0), VAlign="Center")),
					Text(f"{Prefix}Name{I}", "", 22, BoxSlot(VAlign="Center", Size="Fill")),
					Text(f"{Prefix}Count{I}", "", 20, BoxSlot((8, 0, 0, 0), VAlign="Center"), TEXT_GOLD, "Right"),
				]),
			]),
		], Brush=Brush((0, 0, 0, 0)), HoveredBrush=Brush((1.0, 0.85, 0.5, 0.08)), PressedBrush=Brush((1.0, 0.85, 0.5, 0.16)),
			DisabledBrush=Brush((0, 0, 0, 0)), ContentPadding=[0, 0, 0, 0], MinSize=[430, 44]))
	return Widget("Canvas", f"{Prefix}Window", CanvasSlot((0.5, 0.5), 0, -10, 920, 520, (0.5, 0.5), Z=Z), "Collapsed", [
		Widget("Border", f"{Prefix}Bg", StretchSlot(), "Visible", Brush=FrameBrush(), ContentPadding=[0, 0, 0, 0]),
		Text(f"{Prefix}Title", Title, 32, CanvasSlot((0, 0), 34, 20, 0, 0, AutoSize=True, Z=1), TEXT_GOLD),
		Text(f"{Prefix}Status", "", 18, CanvasSlot((0, 0), 230, 31, 0, 0, AutoSize=True, Z=1), TEXT_DIM),
		Widget("HorizontalBox", f"{Prefix}GoldBox", CanvasSlot((1, 0), -34, 24, 0, 0, (1, 0), True, 1), "HitTestInvisible", [
			Img(f"{Prefix}GoldIcon", Icon("Coin"), 26, BoxSlot((0, 0, 8, 0), VAlign="Center")),
			Text(f"{Prefix}Gold", "0 G", 22, BoxSlot(VAlign="Center"), TEXT_GOLD),
		]),
		Widget("Border", f"{Prefix}Rule", CanvasSlot((0, 0), 30, 70, 860, 2, Z=1), Brush=Brush((0.96, 0.77, 0.3, 0.7)), ContentPadding=[0, 0, 0, 0]),
		Widget("VerticalBox", f"{Prefix}List", CanvasSlot((0, 0), 30, 84, 440, 380, Z=1), "SelfHitTestInvisible", Rows),
		Widget("Border", f"{Prefix}Detail", CanvasSlot((0, 0), 492, 84, 398, 370, Z=1), "HitTestInvisible", [
			Widget("VerticalBox", f"{Prefix}DetailBox", BoxSlot(), "HitTestInvisible", [
				Widget("HorizontalBox", f"{Prefix}DetailHead", BoxSlot((0, 0, 0, 12)), "HitTestInvisible", [
					Img(f"{Prefix}DetailIcon", Icon("Potion"), 64, BoxSlot((0, 0, 14, 0), VAlign="Center")),
					Widget("VerticalBox", f"{Prefix}DetailNames", BoxSlot(VAlign="Center"), "HitTestInvisible", [
						Text(f"{Prefix}DetailName", "", 26, BoxSlot(), TEXT_GOLD),
						Text(f"{Prefix}DetailType", "", 17, BoxSlot((0, 4, 0, 0)), TEXT_DIM),
					]),
				]),
				Text(f"{Prefix}DetailDesc", "", 19, BoxSlot((0, 0, 0, 14)), TEXT_LIGHT, Wrap=True),
				Text(f"{Prefix}DetailStats", "", 18, BoxSlot(), TEXT_CYAN, Wrap=True),
			]),
		], Brush=FrameBrush(True, 36), ContentPadding=[22, 20, 22, 18]),
		Text(f"{Prefix}Hint", Hint, 17, CanvasSlot((0.5, 1), 0, -16, 0, 0, (0.5, 1), True, 1), TEXT_DIM, "Center"),
	])


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
	C.append(Widget("Canvas", "DialogWindow", CanvasSlot((0.5, 1), 0, -24, 900, 160, (0.5, 1), Z=20), "Collapsed", [
		Widget("Border", "DialogBg", StretchSlot(), "Visible", Brush=FrameBrush(), ContentPadding=[0, 0, 0, 0]),
		Widget("Border", "DialogNamePlate", CanvasSlot((0, 0), 30, -22, 0, 0, AutoSize=True, Z=2), "HitTestInvisible",
			   [Text("DialogName", "", 22, BoxSlot(), TEXT_GOLD)], Brush=FrameBrush(True, 30), ContentPadding=[18, 7, 18, 7]),
		Text("DialogText", "", 24, CanvasSlot((0, 0), 42, 40, 816, 96, Z=1), TEXT_LIGHT, Wrap=True),
		Text("DialogNext", "▼", 20, CanvasSlot((1, 1), -34, -18, 0, 0, (1, 1), True, 2), TEXT_GOLD),
	]))
	# ---- 메뉴 (인벤토리·상점): 화면 어둡게 + 창
	C.append(Widget("Border", "MenuShade", StretchSlot(29), "Collapsed", Brush=Brush((0.01, 0.0, 0.04, 0.55)), ContentPadding=[0, 0, 0, 0]))
	C.append(MenuWindow("Inv", "소지품", "W/S 선택     E · J 사용 / 장비     I 닫기", 30))
	C.append(MenuWindow("Shop", "미라의 잡화점", "W/S 선택     E · J 구입     I · Space 나가기", 31))
	Root = Widget("Canvas", "Root", None, "SelfHitTestInvisible", C)
	_WriteJson(os.path.join(Content, *UI_DIR.split("/"), "HUD.eui"),
			   {"Version": 2, "DesignSize": [1280, 720], "ScaleMode": "MatchHeight", "Root": Root, "Animations": []})


# ================================================================ 프리팹
def Link(Id):
	return {"Id": str(Id), "Root": -1}


def Transform(Position=(0.0, 0.0, 0.0), Rotation=None, Scale=(1.0, 1.0, 1.0)):
	return {"Position": [float(V) for V in Position], "Rotation": Rotation or [0.0, 0.0, 0.0, 1.0], "Scale": [float(V) for V in Scale]}


def Sprite(Asset, Slice="", Lit=True, Shadows=True, Blend=3, Visible=True, Cutoff=0.5, Color=(1, 1, 1, 1)):
	# Blend: 0 알파, 1 프리멀티플라이드, 2 가산, 3 마스크 (ESpriteBlendMode 번호 — 씬 JSON은 번호)
	return {"Sprite": Asset, "Slice": Slice, "Color": list(Color), "FlipX": False, "FlipY": False, "SortingLayer": "", "OrderInLayer": 0,
			"Lit": Lit, "CastShadows": Shadows, "Size": [0.0, 0.0], "Visible": Visible, "Blend": Blend, "AlphaCutoff": Cutoff, "SliceMode": 0}


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
			"SpriteComponent": Sprite("Sprites/HD2D/Hero.esprite", "IdleDown0"), "FlipbookComponent": Flipbook("Sprites/HD2D/Hero_IdleDown.eflipbook"),
			"PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, Foot))}},
		{"Name": "Shadow", "Parent": 1, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
			"PrefabLinkComponent": Link(4), "TransformComponent": Transform((0, 0, Foot + 1.5), FLAT, (0.75, 1.0, 0.75))}},
	])

	# 적: 캡슐 이동기 + Visual > Body(몸)·Shadow·HpBack·HpFill(머리 위 체력바 — 맞으면 잠깐 보임)
	Sprites = {"Slime": ("Sprites/HD2D/Slime.esprite", "Idle0", "Sprites/HD2D/Slime_Idle.eflipbook"),
			   "Golem": ("Sprites/HD2D/Golem.esprite", "Dormant0", "Sprites/HD2D/Golem_Dormant.eflipbook")}
	for Kind, (Radius, Half, Lift, BarZ, Mass) in ENEMY_CAPSULE.items():
		SpriteAsset, Slice, Book = Sprites.get(Kind, ("Sprites/HD2D/Enemies.esprite", "", f"Sprites/HD2D/{Kind}_Idle.eflipbook"))
		EFoot = -(Radius + Half)
		Speed = next(V["MoveSpeed"] for N, V in ENEMIES if N == Kind)
		Script = "Scripts/Demo/HD2D/HD2DBoss.lua" if Kind == "Golem" else "Scripts/Demo/HD2D/HD2DEnemy.lua"
		ShadowScale = {"Golem": 3.4, "Bat": 0.6}.get(Kind, 0.8)
		WritePrefab(Content, Kind, [
			{"Name": Kind, "Parent": -1, "Components": {
				"CharacterMovementComponent": Mover(Radius, Half, Speed, Mass, PushForce=4000.0 if Kind == "Golem" else 500.0),
				"ScriptComponent": {"ExecutionLocation": 0, "ScriptAsset": Script, "PropertyOverrides": json.dumps({"Kind": Kind}, ensure_ascii=False)},
				"PrefabLinkComponent": Link(1), "TransformComponent": Transform()}},
			{"Name": "Visual", "Parent": 0, "Components": {"PrefabLinkComponent": Link(2), "TransformComponent": Transform()}},
			{"Name": "Body", "Parent": 1, "Components": {
				"SpriteComponent": Sprite(SpriteAsset, Slice), "FlipbookComponent": Flipbook(Book),
				"PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, EFoot + Lift))}},
			{"Name": "Shadow", "Parent": 1, "Components": {
				"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
				"PrefabLinkComponent": Link(4), "TransformComponent": Transform((0, 0, EFoot + 1.5), FLAT, (ShadowScale, 1.0, ShadowScale))}},
			{"Name": "HpBack", "Parent": 1, "Components": {
				"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "HpBack", Lit=False, Shadows=False, Blend=0, Visible=False),
				"PrefabLinkComponent": Link(5), "TransformComponent": Transform((0, 6, EFoot + BarZ))}},
			{"Name": "HpFill", "Parent": 1, "Components": {
				"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "HpFill", Lit=False, Shadows=False, Blend=0, Visible=False, Color=(0.95, 0.3, 0.3, 1)),
				"PrefabLinkComponent": Link(6), "TransformComponent": Transform((-60, 8, EFoot + BarZ))}},
		])

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
			"SpriteComponent": Sprite("Sprites/HD2D/Props.esprite", "Chest0"), "FlipbookComponent": Flipbook(""),
			"PrefabLinkComponent": Link(2), "TransformComponent": Transform((0, 0, -45))}},
	])
	# 마을 사람: 정적 상자 콜라이더 + 몸(대기 플립북은 관리자가 표 값으로) + 그림자 + 머리 위 "!" 표시
	WritePrefab(Content, "Npc", [
		{"Name": "Npc", "Parent": -1, "Components": {
			"BoxColliderComponent": {"HalfExtents": [36.0, 30.0, 85.0]}, "PrefabLinkComponent": Link(1), "TransformComponent": Transform()}},
		{"Name": "Body", "Parent": 0, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Npcs.esprite", "Elder0"), "FlipbookComponent": Flipbook(""),
			"PrefabLinkComponent": Link(2), "TransformComponent": Transform((0, 0, -85))}},
		{"Name": "Shadow", "Parent": 0, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
			"PrefabLinkComponent": Link(3), "TransformComponent": Transform((0, 0, -83.5), FLAT, (0.75, 1.0, 0.75))}},
		{"Name": "Marker", "Parent": 0, "Components": {
			"SpriteComponent": Sprite("Sprites/HD2D/Fx.esprite", "Exclaim", Lit=False, Shadows=False, Blend=0, Visible=False),
			"PrefabLinkComponent": Link(4), "TransformComponent": Transform((0, 6, 135))}},
	])


# ================================================================ 씬 배치
def ReserveSpots(Reserve):
	# 배경 무작위 배치(바위·덤불·풀)가 게임 자리를 피하게 — BuildHD2D.BuildScene이 무작위 배치 전에 부른다
	for _, X, Y in ENEMY_SLOTS:
		Reserve(X, Y, 150.0)
	for _, X, Y in NPCS:
		Reserve(X, Y, 110.0)
	for X, Y, _ in CHESTS:
		Reserve(X, Y, 120.0)
	Reserve(BOSS[0], BOSS[1], BOSS_ARENA_RADIUS)


def AddGame(S, Height, Path, AutoPlay=False):
	# 게임 관리자(적·보물상자·마을 사람·보스 자리 + 길) + HUD. AutoPlay: False | True(= "Full") | 시나리오 이름 (HD2DAutoPilot.lua)
	def P3(X, Y, Lift):
		return f"{X:.0f},{Y:.0f},{Height(X, Y) + Lift:.0f}"

	Enemies = ";".join(f"{Kind},{P3(X, Y, ENEMY_CAPSULE[Kind][0] + ENEMY_CAPSULE[Kind][1] + 4.0)}" for Kind, X, Y in ENEMY_SLOTS)
	Npcs = ";".join(f"{Id},{P3(X, Y, 85.0)}" for Id, X, Y in NPCS)
	Chests = ";".join(f"{P3(X, Y, 45.0)},{Contents}" for X, Y, Contents in CHESTS)
	Golem = ENEMY_CAPSULE["Golem"]
	Scenario = ("Full" if AutoPlay is True else (AutoPlay or ""))
	S.Add("HD2DGame", {"ScriptComponent": {"ScriptAsset": "Scripts/Demo/HD2D/HD2DGame.lua", "ExecutionLocation": 0,
		"PropertyOverrides": json.dumps({
			"Enemies": Enemies, "Npcs": Npcs, "Chests": Chests, "Boss": P3(BOSS[0], BOSS[1], Golem[0] + Golem[1] + 4.0),
			"Path": ";".join(f"{X:.0f},{Y:.0f}" for X, Y in Path), "AutoPlay": Scenario}, ensure_ascii=False)}})
	S.Add("HUD", {"UIComponent": {"Asset": f"{UI_DIR}/HUD.eui", "ZOrder": 0, "Visible": True, "ReceiveInput": True, "KeyboardFocus": False},
				  "ScriptComponent": {"ScriptAsset": "Scripts/Demo/HD2D/HD2DHud.lua", "ExecutionLocation": 0, "PropertyOverrides": ""}})


def AddPlayer(S, Start, Height):
	Z = Height(*Start) + PLAYER_RADIUS + PLAYER_HALF + 4.0
	Index = len(S.Entities)
	S.Add("Player", {
		"PrefabInstanceComponent": {"Asset": f"{PREFABS}/Player.eprefab", "Overrides": ""},
		"PrefabLinkComponent": {"Id": "1", "Root": Index}},
		(Start[0], Start[1], Z))
	return Z


def WriteAll(Content, CameraDistance, PlayMin, PlayMax):
	WriteData(Content)
	WriteUi(Content)
	WritePrefabs(Content, CameraDistance, PlayMin, PlayMax)
