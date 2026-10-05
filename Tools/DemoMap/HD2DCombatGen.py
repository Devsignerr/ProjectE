# HD-2D 데모 전투 깊이 (HD2DGameplay.py가 부른다 — 표·UI·프리팹 생성 중 전투 몫만 여기에 모았다):
#   공격 속성(무기 = 베기/찌르기/활/빛, 마법 = 불/얼음/번개/빛)·적 약점과 실드·브레이크(옥토패스식), 무기 기술 2개씩 + 마법 3개(Skills.etable — MP·쿨다운·해금 레벨),
#   상태 이상(독·화상·빙결·기절)과 해독제, 정예 적(바탕 종류의 강화 변형 — 금빛 윤곽 + "정예" 이름 + 실드 많음 + 보상 많음),
#   동료 "견습 마법사 엘라"(마을에서 영입 — 따라오며 적 약점 속성 마법·치유, 쓰러지면 잠시 뒤 일어남, HUD 체력, 저장·맵 이동 유지 — HD2DCompanion.lua).
#   데이터는 Data/Demo/HD2D(무기·적 표의 전투 필드, Skills.etable/Skill.estruct, 아이템 Antidote), UI는 HUD.eui에 위젯 묶음(적 머리 위 실드·약점 태그 템플릿,
#   브레이크 글자, 스킬 칸 5개, 스킬 이름 띠), 도트 아트는 HD2DCombatArt.py.
#   스크립트: Scripts/Demo/HD2D/HD2DCombat.lua(속성·실드·브레이크·상태 이상 — 관리자에 붙음), HD2DSkills.lua(스킬 — 플레이어에 붙음), HD2DCombatHud.lua(태그·스킬 칸 — HUD에 붙음).
#   규약: 약점 공개 기록은 일행 저장(Party → Combat.Revealed, 키 = 적 표 행 이름, 보스 2단계 = "<행>#2")에 들어간다.
import os

import HD2DCombatArt
import HD2DCompanionArt

PHYSICAL = ("Slash", "Pierce", "Bow")
MAGIC = ("Fire", "Ice", "Thunder", "Light")
ELEMENTS = PHYSICAL + MAGIC
ELEMENT_NAMES = {"Slash": "베기", "Pierce": "찌르기", "Bow": "활", "Fire": "불", "Ice": "얼음", "Thunder": "번개", "Light": "빛"}
STATUSES = ("Poison", "Burn", "Freeze", "Stun")

# ---- 무기: 기본 공격 속성 + 무기 기술 두 개 (K / L)
WEAPON_COMBAT = {
	"Sword":        {"Element": "Slash", "SkillK": "Whirl", "SkillL": "FlameSlash"},
	"Spear":        {"Element": "Pierce", "SkillK": "PierceRush", "SkillL": "ThunderThrust"},
	"Bow":          {"Element": "Bow", "SkillK": "ArrowRain", "SkillL": "FrostArrow"},
	"Staff":        {"Element": "Light", "SkillK": "HolyPillar", "SkillL": "ChainLightning"},
	"CrystalSword": {"Element": "Slash", "SkillK": "Whirl", "SkillL": "FrostSlash"},
}


def _Skill(Name, Element, Kind, Damage, Mana, Cooldown, Level, Desc, Hits=1, Range=0, Radius=0, Status="", Chance=0.0, Cast=0.3, Delay=0.1,
		   Slot="Weapon", Icon=None, Speed=0):
	return {"DisplayName": Name, "Slot": Slot, "Element": Element, "Kind": Kind, "Damage": Damage, "Hits": Hits, "ManaCost": Mana, "Cooldown": Cooldown,
			"UnlockLevel": Level, "Range": Range, "Radius": Radius, "Speed": Speed, "Status": Status, "StatusChance": Chance, "CastTime": Cast,
			"HitDelay": Delay, "Icon": f"UI/Demo/HD2D/Combat/Skill{Icon}.png", "Description": Desc}


# 스킬 (행 이름 = 스킬 id). 수치 근거: 1레벨 검 기본 공격 = 14 × 약 3회/초 ≈ 40 DPS, MP 50(+3/초) — 스킬은 한 번에 기본 공격 2~3대 값,
#   MP로 연속 사용을 막는다(평균 10~18 → 처음엔 3~4번). 다단 기술은 한 대가 실드 1칸이라 약점이면 브레이크를 앞당긴다.
SKILLS = [
	("Whirl", _Skill("회오리 베기", "Slash", "Whirl", 12, 10, 4.0, 1, "몸을 한 바퀴 돌려 둘레의 적을 세 번 벤다. 둘러싸였을 때.", Hits=3, Radius=200,
					 Cast=0.42, Delay=0.08, Icon="Whirl")),
	("FlameSlash", _Skill("불꽃 베기", "Fire", "Cone", 30, 14, 6.0, 2, "칼날에 불을 실어 앞을 넓게 벤다. 화상을 입힐 때가 있다.", Range=210, Radius=150,
						  Status="Burn", Chance=0.6, Cast=0.36, Delay=0.12, Icon="FlameSlash")),
	("FrostSlash", _Skill("빙결 베기", "Ice", "Cone", 32, 14, 6.0, 2, "수정 칼날로 차갑게 벤다. 적을 얼려 느리게 만든다.", Range=220, Radius=150,
						  Status="Freeze", Chance=0.65, Cast=0.36, Delay=0.12, Icon="FrostSlash")),
	("PierceRush", _Skill("관통 돌진", "Pierce", "Rush", 24, 12, 5.0, 1, "창을 앞세워 돌진하며 길 위의 적을 모두 꿰뚫는다. 돌진하는 동안은 맞지 않는다.",
						  Range=430, Radius=55, Cast=0.34, Delay=0.04, Icon="PierceRush", Speed=1500)),
	("ThunderThrust", _Skill("번개 찌르기", "Thunder", "Line", 18, 14, 6.0, 3, "번개를 두른 창으로 멀리 두 번 찌른다. 적을 기절시킬 때가 있다.", Hits=2,
							 Range=380, Radius=45, Status="Stun", Chance=0.35, Cast=0.44, Delay=0.1, Icon="ThunderThrust")),
	("ArrowRain", _Skill("화살비", "Bow", "Volley", 11, 10, 4.5, 1, "화살 다섯 발을 부채꼴로 흩뿌린다.", Hits=5, Range=1200, Radius=44,
						 Cast=0.4, Delay=0.14, Icon="ArrowRain", Speed=2300)),
	("FrostArrow", _Skill("얼음 화살", "Ice", "Pierce", 26, 12, 5.0, 2, "적을 꿰뚫고 날아가는 얼음 화살. 맞은 적을 얼린다.", Range=1400,
						  Status="Freeze", Chance=0.7, Cast=0.4, Delay=0.14, Icon="FrostArrow", Speed=2000)),
	("HolyPillar", _Skill("빛의 기둥", "Light", "Pillar", 14, 16, 6.0, 1, "가장 가까운 적의 자리에 빛의 기둥을 내려 세 번 태운다.", Hits=3, Range=950,
						  Radius=150, Cast=0.45, Delay=0.35, Icon="HolyPillar")),
	("ChainLightning", _Skill("연쇄 번개", "Thunder", "Chain", 24, 18, 7.0, 3, "번개가 가까운 적에서 적으로 네 번까지 옮겨 붙는다. 기절시킬 때가 있다.", Hits=4,
							  Range=750, Radius=480, Status="Stun", Chance=0.25, Cast=0.45, Delay=0.15, Icon="ChainLightning")),
	# 마법 (무기와 상관없이 3/4/5)
	("Fireball", _Skill("화염구", "Fire", "Bolt", 26, 12, 3.0, 1, "불덩이를 던진다. 부딪히면 터져 둘레까지 태운다. 화상을 입힐 때가 있다.", Range=1100,
						Radius=120, Status="Burn", Chance=0.5, Cast=0.38, Delay=0.16, Slot="Magic", Icon="Fireball", Speed=1450)),
	("IceLance", _Skill("얼음 창", "Ice", "Pierce", 22, 12, 3.5, 2, "얼음 창을 쏘아 줄지은 적을 꿰뚫는다. 맞은 적을 얼린다.", Range=1200,
						Status="Freeze", Chance=0.6, Cast=0.38, Delay=0.16, Slot="Magic", Icon="IceLance", Speed=1900)),
	("Heal", _Skill("치유", "Light", "Heal", 0, 20, 12.0, 3, "최대 HP의 35%를 회복하고 독·화상·빙결을 씻어 낸다.", Cast=0.5, Delay=0.25,
					Slot="Magic", Icon="Heal")),
]
MAGIC_SKILLS = ["Fireball", "IceLance", "Heal"]

# ---- 적 전투 필드: 약점·실드·브레이크 시간 (보스는 2단계 약점·실드), 이 적이 플레이어에게 거는 상태 이상
#   실드 수 근거: 약점 한 대 ≈ 1레벨 무기 14~22 × 1.25 = 18~28 (4레벨이면 24~37). 약한 적(슬라임·박쥐, 체력 30)은 실드 1 = 첫 약점에 브레이크,
#   보통 적(체력 46~90)은 실드 2~3 = 약점 두세 대로 체력이 1/4쯤 남았을 때 브레이크 → 브레이크 배율(×1.5)로 마무리 (평타만 치면 4~6대).
#   정예(체력 240~260, 실드 5)·보스(900/1500, 실드 8~12)는 기본 공격만으로는 깨기 전에 지치므로 약점 기술·부스트(한 번에 여러 칸)를 섞게 된다.
#   일반 적 체력은 HD2DGameplay.ENEMIES (2026-10-05 전투 깊이로 1.3~1.7배)
# 보스 수치 근거 (2026-10-05 — master에서 거미 여왕이 입장 18초 만에 쓰러졌다): 6~7레벨 창·수정 검 연타 ≈ 60~70 DPS(부스트 포함).
#   보스는 브레이크가 아닐 때 받는 피해 × Guard(0.5) — 실드를 깨지 않고 때리면 35 DPS 남짓, 브레이크 5초 동안은 × 1.5(약점이면 × 1.25 더).
#   한 주기 ≈ 약점 12~16대(6~9초) + 브레이크 5초 ≈ 피해 850~950. 자동 조종(패턴을 거의 다 피하며 쉬지 않고 때림)이 약 50초,
#   사람은 피하느라 절반쯤만 때리므로 골렘 4000 / 여왕 5200 ≈ 1분 반~3분. (실드 8·체력 2600은 자동 조종 26초 — 너무 쉬웠다)
#   부스트 BP는 10초에 1, 명중 8번에 1 (예전 7초·5번 — 일반 적을 부스트 연타로 녹였다)
ENEMY_COMBAT = {  # 종류: (약점, 실드, 2단계 약점, 2단계 실드, 브레이크 초, 거는 상태, 확률)
	"Slime":        (["Slash", "Fire"], 1, [], 0, 3.5, "", 0.0),
	"Bat":          (["Bow", "Thunder"], 1, [], 0, 3.5, "", 0.0),
	"Goblin":       (["Pierce", "Ice"], 2, [], 0, 3.5, "", 0.0),
	"Archer":       (["Light", "Fire"], 2, [], 0, 3.5, "", 0.0),
	"Mushroom":     (["Fire", "Slash"], 2, [], 0, 3.5, "", 0.0),  # 독은 독 웅덩이가 건다
	"Golem":        (["Pierce", "Ice"], 12, ["Thunder", "Light"], 14, 5.0, "Stun", 1.0),  # 내리치기에 맞으면 기절
	"CaveBat":      (["Bow", "Fire"], 2, [], 0, 3.5, "Poison", 0.35),
	"CrystalSlime": (["Thunder", "Pierce"], 3, [], 0, 3.5, "Freeze", 0.4),
	"SpiderQueen":  (["Fire", "Bow"], 14, ["Ice", "Light"], 16, 5.0, "Stun", 1.0),  # 덮치기 착지에 맞으면 기절 (거미줄·수정 가시는 빙결 — 스크립트)
	"EliteGoblin":  (["Pierce", "Ice", "Light"], 5, [], 0, 4.0, "", 0.0),
	"EliteArcher":  (["Light", "Fire", "Slash"], 5, [], 0, 4.0, "", 0.0),
}

# ---- 정예 적 (바탕 종류의 강화 변형): 표 행 = 바탕 행 + 덮어쓰기, 캡슐은 몸 배율만큼 크게
ELITES = {
	"EliteGoblin": ("Goblin", 1.2, {"DisplayName": "정예 고블린 도적", "MaxHealth": 260, "ContactDamage": 9, "AttackDamage": 19, "MoveSpeed": 360,
									 "AggroRange": 1000, "AttackCooldown": 1.7, "WindupTime": 0.45, "ProjectileSpeed": 1400, "GoldMin": 30, "GoldMax": 40,
									 "Exp": 45, "DropItem": "HiPotion", "DropChance": 1.0, "Radius": 56, "RespawnTime": 90, "Look": "Goblin",
									 "Tint": [1.12, 0.86, 0.8, 1.0]}),
	"EliteArcher": ("Archer", 1.2, {"DisplayName": "정예 해골 궁수", "MaxHealth": 240, "ContactDamage": 8, "AttackDamage": 17, "AttackCooldown": 1.9,
									 "WindupTime": 0.6, "ProjectileSpeed": 1300, "GoldMin": 35, "GoldMax": 45, "Exp": 55, "DropItem": "HiPotion",
									 "DropChance": 1.0, "Radius": 54, "RespawnTime": 0, "Look": "Archer", "Tint": [1.08, 0.95, 1.3, 1.0]}),
}


GUARD = {"Golem": 0.5, "SpiderQueen": 0.5, "EliteGoblin": 0.8, "EliteArcher": 0.8}  # 브레이크가 아닐 때 받는 피해 배율 (없으면 1)


def ExtendEnemies(Enemies, Capsules):
	# HD2DGameplay.ENEMIES(목록)·ENEMY_CAPSULE(사전)에 정예 행·캡슐을 더하고 모든 행에 전투 필드를 채운다
	Rows = dict(Enemies)
	for Kind, (Base, Scale, Over) in ELITES.items():
		if Kind in Rows:
			continue
		Row = dict(Rows[Base])
		Row.update(Over)
		Enemies.append((Kind, Row))
		R, Half, Lift, BarZ, Mass = Capsules[Base]
		Capsules[Kind] = (R * Scale, Half * Scale, Lift * Scale, BarZ * Scale + 10.0, Mass * 1.6)
	for Kind, Row in Enemies:
		Weak, Shield, Weak2, Shield2, BreakTime, Inflict, Chance = ENEMY_COMBAT.get(Kind, ([], 0, [], 0, 3.5, "", 0.0))
		Row.update({"Weakness": list(Weak), "Shield": Shield, "Weakness2": list(Weak2), "Shield2": Shield2, "BreakTime": BreakTime,
					"Elite": Kind in ELITES, "Inflict": Inflict, "InflictChance": Chance, "Guard": GUARD.get(Kind, 1.0)})


def BodyScale(Kind):
	S = ELITES[Kind][1] if Kind in ELITES else 1.0
	return (S, S, S)


def EnemyExtraChildren(Kind, Link, Transform, Sprite, SpriteAsset, Slice, BodyZ):
	# 정예: 몸 뒤(-Y = 카메라에서 먼 쪽)에 같은 그림을 조금 크게 금빛으로 덮어 윤곽처럼 빛나게 (슬라이스·반전은 HD2DCombat.lua가 몸을 따라 맞춘다)
	if Kind not in ELITES:
		return []
	S = ELITES[Kind][1] * 1.13
	Aura = Sprite(SpriteAsset, Slice, Lit=False, Shadows=False, Blend=0, Billboard=2, Color=(1.0, 0.85, 0.35, 0.6))
	Aura["FlashColor"] = [1.0, 0.82, 0.3, 1.0]
	return [{"Name": "Aura", "Parent": 1, "Components": {"SpriteComponent": Aura, "PrefabLinkComponent": Link(8),
																   "TransformComponent": Transform((0, -4, BodyZ - 3.0), None, (S, S, S))}}]


# ---- 동료 (Balance 필드 — 수치 근거: 마법 한 대 15 ≈ 1레벨 검 한 대, 2.8초 간격 ≈ 플레이어 화력의 1/8 — 주인공을 대신하지 않고
#      약점을 찾아 주는 보조. 체력 90 = 일반 적 접촉 8~13을 7~10번 버틴다, 쓰러져도 8초 뒤 절반 체력으로)
COMPANION = {
	"CompanionName": "엘라", "CompanionHealth": 90, "CompanionHealthPerLevel": 12, "CompanionDamage": 15, "CompanionCastInterval": 2.8,
	"CompanionHealInterval": 14.0, "CompanionHealRatio": 0.25, "CompanionReviveTime": 8.0,
	"CompanionLines": ["엘라|Mage|저기, 여행자님! 들판으로 나가신다고 들었어요. 저도 데려가 주세요!",
					   "엘라|Mage|마법 학교에서 배운 속성 마법이라면 마물마다 다른 약점을 찾아낼 수 있어요.",
					   "아르펜|Hero|약점이라… 그거 든든한걸. 잘 부탁해, 엘라.",
					   "엘라|Mage|네! 다치시면 제가 치유 마법으로 돌봐 드릴게요."],
	"CompanionWaitLines": ["엘라|Mage|어머, 처음 뵙는 분이네요. 촌장님께 먼저 인사드리고 오세요.",
						   "엘라|Mage|들판에 나가실 거라면… 그때 다시 이야기해요!"],
	"CompanionTalkLines": ["엘라|Mage|마물의 머리 위 \"?\" 칸이 약점이에요. 제가 마법으로 하나씩 찾아 볼게요!"],
}
COMPANION_SPOT = (-2600.0, 0.0)  # 마을 광장 서쪽 (소품 300cm 안 없음 — 2026-10-05 씬에서 확인)


def GameOverrides(Layout, P3):
	# HD2DGameplay.AddGame이 관리자 속성에 더한다: 동료가 처음 서 있는 자리 (마을만)
	if Layout.MapId == "Village":
		return {"Companion": P3(COMPANION_SPOT[0], COMPANION_SPOT[1], 89.0)}
	return {}


def WritePrefabs(Content, G):
	# 동료: 플레이어와 같은 캡슐 이동기(조금 빠르게 — 따라잡도록) + Visual > Body(엘라)·Shadow·Marker("!")
	Foot = -(G.PLAYER_RADIUS + G.PLAYER_HALF)
	G.WritePrefab(Content, "Companion", [
		{"Name": "Companion", "Parent": -1, "Components": {
			"CharacterMovementComponent": G.Mover(G.PLAYER_RADIUS, G.PLAYER_HALF, 520.0, 60.0, PushForce=600.0, KnockbackDeceleration=2600.0),
			"ScriptComponent": {"ExecutionLocation": 0, "ScriptAsset": "Scripts/Demo/HD2D/HD2DCompanion.lua", "PropertyOverrides": ""},
			"PrefabLinkComponent": G.Link(1), "TransformComponent": G.Transform()}},
		{"Name": "Visual", "Parent": 0, "Components": {"PrefabLinkComponent": G.Link(2), "TransformComponent": G.Transform()}},
		{"Name": "Body", "Parent": 1, "Components": {
			"SpriteComponent": G.Sprite("Sprites/HD2D/Mage.esprite", "IdleDown0", Billboard=2),
			"FlipbookComponent": G.Flipbook("Sprites/HD2D/Mage_IdleDown.eflipbook"),
			"PrefabLinkComponent": G.Link(3), "TransformComponent": G.Transform((0, 0, Foot))}},
		{"Name": "Shadow", "Parent": 1, "Components": {
			"SpriteComponent": G.Sprite("Sprites/HD2D/Fx.esprite", "Shadow", Lit=False, Shadows=False, Blend=0),
			"PrefabLinkComponent": G.Link(4), "TransformComponent": G.Transform((0, 0, Foot + 1.5), G.FLAT, (0.75, 1.0, 0.75))}},
		{"Name": "Marker", "Parent": 1, "Components": {
			"SpriteComponent": G.Sprite("Sprites/HD2D/Fx.esprite", "Exclaim", Lit=False, Shadows=False, Blend=0, Visible=False, Billboard=2),
			"PrefabLinkComponent": G.Link(5), "TransformComponent": G.Transform((0, 6, Foot + 220))}},
	])


# ---- 무기·아이템·밸런스 확장
def ExtendWeapons(Weapons):
	for Id, Row in Weapons:
		Row.update(WEAPON_COMBAT.get(Id, {"Element": "Slash", "SkillK": "", "SkillL": ""}))


def ExtendItems(Items, ItemFn, Icon):
	if not any(Id == "Antidote" for Id, _ in Items):
		Items.append(ItemFn("Antidote", {"DisplayName": "해독제", "Kind": "Cure", "Amount": 0, "Weapon": "", "Icon": Icon("Antidote"),
										 "Description": "쓴 약초 즙. 독·화상·빙결을 씻어 낸다.", "Price": 20}))


def ExtendBalance(Balance):
	if "Antidote" not in Balance["ShopStock"]:
		Balance["ShopStock"].insert(3, "Antidote")
	if "Antidote" not in Balance["StartItems"]:
		Balance["StartItems"].append("Antidote")  # 첫 독에 대비한 한 병
	Balance["MagicSkills"] = list(MAGIC_SKILLS)
	Balance.update(COMPANION)
	Balance["BreakDamage"] = 1.5
	Balance["WeakDamage"] = 1.25


def WeaponFields(Field):
	Elements = list(ELEMENTS)
	return [Field("Element", "Enum", "Slash", "기본 공격 속성 (적 약점 — HD2DCombatGen.py)", Values=Elements),
			Field("SkillK", "String", "", "무기 기술 K (Skills.etable)"),
			Field("SkillL", "String", "", "무기 기술 L (Skills.etable)")]


def EnemyFields(Field):
	return [Field("Weakness", "Array", [], "약점 속성 (맞히면 실드 -1)", Element="String"),
			Field("Shield", "Int", 0, "실드 수 (0이 되면 브레이크)"),
			Field("Weakness2", "Array", [], "보스 2단계 약점 (비면 그대로)", Element="String"),
			Field("Shield2", "Int", 0, "보스 2단계 실드 (0이면 그대로)"),
			Field("BreakTime", "Float", 3.5, "브레이크 기절 시간 (초)"),
			Field("Guard", "Float", 1.0, "브레이크가 아닐 때 받는 피해 배율 (보스 0.6 — 실드를 깨야 잡힌다)"),
			Field("Elite", "Bool", False, "정예 (금빛 윤곽 · 이름표)"),
			Field("Inflict", "String", "", "몸·공격에 맞으면 거는 상태 이상 (Poison/Burn/Freeze/Stun)"),
			Field("InflictChance", "Float", 0.0, "확률 0~1")]


def BalanceFields(Field):
	return [Field("MagicSkills", "Array", [], "마법 칸 3/4/5 (Skills.etable)", Element="String"),
			Field("CompanionName", "String", "엘라", "동료 이름"),
			Field("CompanionHealth", "Float", 90, "동료 1레벨 체력 (플레이어 레벨을 따른다)"),
			Field("CompanionHealthPerLevel", "Float", 12, "레벨마다 동료 체력 +"),
			Field("CompanionDamage", "Float", 15, "동료 마법 한 대 (플레이어 레벨 배율)"),
			Field("CompanionCastInterval", "Float", 2.8, "동료 마법 간격 (초)"),
			Field("CompanionHealInterval", "Float", 14.0, "동료 치유 간격 (초)"),
			Field("CompanionHealRatio", "Float", 0.25, "동료 치유량 (플레이어 최대 HP 비율)"),
			Field("CompanionReviveTime", "Float", 8.0, "쓰러진 뒤 일어나기까지 (초)"),
			Field("CompanionLines", "Array", [], "영입 대사 (이름|초상화|대사)", Element="String"),
			Field("CompanionWaitLines", "Array", [], "영입 전 대사 (촌장 퀘스트 전)", Element="String"),
			Field("CompanionTalkLines", "Array", [], "영입 뒤 말 걸면", Element="String"),
			Field("BreakDamage", "Float", 1.5, "브레이크 중 받는 피해 배율"),
			Field("WeakDamage", "Float", 1.25, "약점 공격 피해 배율")]


def WriteSkills(Content, Struct, Table, Field):
	Struct(Content, "Skill", "HD2D 스킬 (Skills.etable, 행 이름 = 스킬 id — 무기 SkillK/SkillL, Balance.MagicSkills)", [
		Field("DisplayName", "String", "스킬", "표시 이름 (쓸 때 화면 위 띠)"),
		Field("Slot", "Enum", "Weapon", "무기 기술(K/L) / 마법(3/4/5)", Values=["Weapon", "Magic"]),
		Field("Element", "Enum", "Slash", "공격 속성", Values=list(ELEMENTS)),
		Field("Kind", "Enum", "Cone", "동작: 회전 베기 / 부채꼴 / 돌진 / 직선 / 화살 부채 / 관통 탄 / 빛기둥 / 연쇄 / 폭발 탄 / 회복",
			  Values=["Whirl", "Cone", "Rush", "Line", "Volley", "Pierce", "Pillar", "Chain", "Bolt", "Heal"]),
		Field("Damage", "Float", 10, "한 대 피해 (레벨 배율 전)"),
		Field("Hits", "Int", 1, "대 수 (회전·기둥·연쇄 = 판정 수, 화살비 = 화살 수)"),
		Field("ManaCost", "Float", 10, "MP"),
		Field("Cooldown", "Float", 5, "다시 쓸 때까지 (초)"),
		Field("UnlockLevel", "Int", 1, "배우는 레벨"),
		Field("Range", "Float", 0, "사거리·돌진 거리 (cm)"),
		Field("Radius", "Float", 0, "반경·폭 (cm)"),
		Field("Speed", "Float", 0, "투사체·돌진 속도 (cm/s)"),
		Field("Status", "String", "", "거는 상태 이상"),
		Field("StatusChance", "Float", 0, "확률 0~1"),
		Field("CastTime", "Float", 0.3, "동작 시간 (초 — 그동안 움직이지 않음)"),
		Field("HitDelay", "Float", 0.1, "시작 뒤 첫 판정 (초)"),
		Field("Icon", "String", "", "UI 아이콘"),
		Field("Description", "String", "", "설명"),
	])
	Table(Content, "Skills", "Skill", SKILLS)


# ================================================================ UI (HUD.eui에 더할 위젯 묶음)
def HudWidgets(G):
	# G = HD2DGameplay 모듈 (위젯 도우미). 반환 목록을 HUD 루트 캔버스 자식으로 붙인다
	W, Text, Img, Brush, BoxSlot, CanvasSlot = G.Widget, G.Text, G.Img, G.Brush, G.BoxSlot, G.CanvasSlot
	Dir = f"{G.UI_DIR}/Combat"
	Out = []
	# ---- 적 머리 위 태그 템플릿 (복제해 쓴다 — HD2DCombatHud.lua): 이름(정예) / 상태 이상 / 실드(숫자) + 약점 칸
	# Z = -1: 월드에 붙은 표시라 HUD 창(상태·퀘스트·미니맵·스킬 이름 띠) 밑으로 지나간다
	Out.append(W("VerticalBox", "TagTemplate", CanvasSlot((0, 0), 0, 0, 0, 0, (0.5, 1), True, -1), "Collapsed", [
		Text("TagName", "", 16, BoxSlot((0, 0, 0, 2), HAlign="Center"), G.TEXT_GOLD, "Center", "Collapsed"),
		W("HorizontalBox", "TagStatus", BoxSlot((0, 0, 0, 2), HAlign="Center"), "Collapsed",
		  [Img(f"TagSt{K}", f"{Dir}/StatusPoison.png", 26, BoxSlot((1, 0, 1, 0)), "Collapsed") for K in range(4)]),
		W("HorizontalBox", "TagRow", BoxSlot(HAlign="Center"), "HitTestInvisible", [
			W("Overlay", "TagShieldBox", BoxSlot((0, 0, 3, 0), VAlign="Center"), "HitTestInvisible", [
				Img("TagShield", f"{Dir}/Shield.png", 42, BoxSlot(HAlign="Center", VAlign="Center")),
				Text("TagShieldNum", "0", 19, BoxSlot((0, 0, 0, 3), HAlign="Center", VAlign="Center"), (1, 1, 1, 1), "Center", Outline=2),
			]),
		] + [Img(f"TagWeak{K}", f"{Dir}/ElemUnknown.png", 30, BoxSlot((1, 0, 1, 0), VAlign="Center"), "Collapsed") for K in range(5)]),
	]))
	# ---- 브레이크 글자 (적 자리에서 커졌다 줄며 사라짐)
	Out.append(Text("BreakBanner", "BREAK!", 46, CanvasSlot((0, 0), 0, 0, 0, 0, (0.5, 0.5), True, 7), (1.0, 0.86, 0.32, 1), "Center", "Collapsed",
					Outline=4))
	# ---- 스킬 이름 띠 (화면 위 가운데)
	Out.append(W("Border", "SkillNamePanel", CanvasSlot((0.5, 0), 0, 92, 0, 0, (0.5, 0), True, 6), "Collapsed", [
		W("HorizontalBox", "SkillNameRow", BoxSlot(), "HitTestInvisible", [
			Img("SkillNameIcon", f"{Dir}/SkillWhirl.png", 30, BoxSlot((0, 0, 10, 0), VAlign="Center")),
			Text("SkillNameText", "", 24, BoxSlot(VAlign="Center"), G.TEXT_GOLD),
		]),
	], Brush=G.FrameBrush(False, 30), ContentPadding=[18, 8, 22, 8]))
	# ---- 스킬 칸 (왼쪽 아래 무기 창 위): K L | 3 4 5
	Slots = []
	for K in range(5):
		Slots.append(W("Overlay", f"Skill{K}", BoxSlot((0, 0, 14 if K == 1 else 6, 0)), "HitTestInvisible", [
			Img(f"SkillBg{K}", f"{Dir}/SkillFrame.png", 60, BoxSlot(HAlign="Center", VAlign="Center")),
			Img(f"SkillIcon{K}", f"{Dir}/SkillWhirl.png", 40, BoxSlot(HAlign="Center", VAlign="Center")),
			W("ProgressBar", f"SkillCd{K}", BoxSlot(HAlign="Center", VAlign="Center"), "Collapsed", MinSize=[46, 46],
			  Brush=Brush((0, 0, 0, 0)), FillBrush=Brush((0.02, 0.01, 0.06, 0.72)), Percent=0.0, FillDirection="BottomToTop"),
			Text(f"SkillTime{K}", "", 19, BoxSlot(HAlign="Center", VAlign="Center"), (1, 1, 1, 1), "Center", "Collapsed"),
			Text(f"SkillLock{K}", "", 15, BoxSlot(HAlign="Center", VAlign="Center"), G.TEXT_DIM, "Center", "Collapsed"),
			Text(f"SkillKey{K}", "KL345"[K], 15, BoxSlot((5, 2, 0, 0), HAlign="Left", VAlign="Top"), G.TEXT_GOLD),
			Text(f"SkillMp{K}", "", 13, BoxSlot((0, 0, 5, 3), HAlign="Right", VAlign="Bottom"), G.TEXT_CYAN, "Right"),
		]))
	# ---- 동료 창 (BP 줄 아래): 초상화 + 이름 + 체력 막대 (+ 쓰러짐 글자)
	Out.append(W("Border", "CompanionPanel", CanvasSlot((0, 0), 16, 222, 0, 0, AutoSize=True), "Collapsed", [
		W("HorizontalBox", "CompanionRow", BoxSlot(), "HitTestInvisible", [
			Img("CompanionPortrait", f"{G.UI_DIR}/Portraits/Mage.png", 40, BoxSlot((0, 0, 10, 0), VAlign="Center")),
			W("VerticalBox", "CompanionTexts", BoxSlot(VAlign="Center"), "HitTestInvisible", [
				W("HorizontalBox", "CompanionNameRow", BoxSlot((0, 0, 0, 4)), "HitTestInvisible", [
					Text("CompanionName", "엘라", 18, BoxSlot(VAlign="Center", Size="Fill"), G.TEXT_GOLD),
					Text("CompanionState", "", 15, BoxSlot((10, 0, 0, 0), VAlign="Center"), (1.0, 0.55, 0.5, 1), "Right"),
				]),
				W("Overlay", "CompanionHpBox", BoxSlot(), "HitTestInvisible", [
					G.Bar("CompanionHp", 150, 12, (0.4, 0.86, 0.52, 1)),
					Text("CompanionHpText", "", 12, BoxSlot(HAlign="Center", VAlign="Center"), Justify="Center", Outline=2),
				]),
			]),
		]),
	], Brush=G.FrameBrush(False, 30), ContentPadding=[12, 8, 16, 10]))
	Out.append(W("HorizontalBox", "SkillBar", CanvasSlot((0, 1), 18, -112, 0, 0, (0, 1), True, 1), "HitTestInvisible", Slots))
	return Out


# ================================================================ 쓰기
# 스크린샷 시나리오 (BuildHD2D.py --views → _HD2DShot_<이름>.escene, HD2DAutoCombat.lua Run<시나리오>)
SHOT_SCENES = {
	"Shot_Break":  ((3450.0, 500.0), "BreakShot"),    # 정예 고블린 도적 — 창(찌르기 약점)으로 첫 브레이크 순간
	"Shot_Skill":  ((1450.0, -150.0), "SkillShot"),   # 회오리 베기 → 화염구 폭발 순간
	"Shot_Status": ((1450.0, -150.0), "StatusShot"),  # 화상·빙결에 걸린 적
	"Shot_Party":  ((3000.0, 650.0), "PartyShot"),    # 동료 엘라와 함께 고블린 무리 — 엘라가 마법을 쏘는 순간
}


def WriteAll(Content, G):
	# 도트 아트(전투 효과·UI 아이콘·동료) + 동료 프리팹 (HD2DGameplay.WriteAll이 부른다 — 표·HUD는 WriteData/WriteUi 안에서)
	Ui = os.path.join(Content, *G.UI_DIR.split("/"))
	Sprites = os.path.join(Content, "Sprites", "HD2D")
	HD2DCombatArt.WriteUi(Ui)
	HD2DCombatArt.WriteSprites(Sprites)
	HD2DCompanionArt.WriteArt(Sprites, Ui)
	WritePrefabs(Content, G)
