# HD-2D 데모 4차(세계 확장 — 항구 "갈매기 항구" + 낮밤 + 지역 이름) 데이터. HD2DGameplay.py가 표 정의 뒤에 ExtendTables(모듈)를 불러
#   기존 행 뒤에 덧붙인다(기존 행 순서는 그대로 — 저장 파일·다른 트랙 호환). 새 표(Regions·DayNight)와 HUD 위젯도 여기서.
#   스크립트 쪽: Scripts/Demo/HD2D/HD2DWorld.lua (낮밤·지역 이름·밤 적/주민·여관·항구 소품), HD2DPirateCaptain.lua (보스)
#   좌표·자리는 HD2DHarborLayout.py, 맵 생성은 BuildHD2DHarbor.py

WEAPON_ROWS = [
	("Harpoon", {"DisplayName": "어부의 작살", "Kind": "Thrust", "Description": "어부 토마가 쓰던 묵직한 작살. 창보다 조금 더 멀리 닿고 맞은 적을 크게 밀어낸다.",
				 "Damage": 26, "Cooldown": 0.38, "Range": 310, "Arc": 34, "Knockback": 1050, "ProjectileSpeed": 0, "ManaCost": 0, "Splash": 0,
				 "HitStop": 0.07, "AttackTime": 0.39, "HitDelay": 0.08, "Flipbook": "Hero_AttackSpear", "Sound": "Audio/RPG/Swing3.wav"}),
]

NPC_FIELD_DEFAULTS = {"ShopStock": [], "ShopTitle": "", "ShopLines": [], "NightLines": [], "NightOnly": False}


def _Npc(G, Id, Name, Role, Lines=(), SubQuest="", **Extra):
	Row = {"DisplayName": Name, "Flipbook": f"Sprites/HD2D/Npc_{Id}.eflipbook", "Role": Role, "Lines": list(Lines), "Portrait": G.Portrait(Id), "SubQuest": SubQuest}
	Row.update(NPC_FIELD_DEFAULTS)
	Row.update(Extra)
	return (Id, Row)


def ExtendTables(G):
	# G = HD2DGameplay 모듈 (표 정의가 끝난 뒤 — 기존 행 뒤에 덧붙임)
	Icon = G.Icon
	for Id, Row in WEAPON_ROWS:
		G.WEAPONS.append((Id, dict(Row, Icon=Icon(Id))))
	G.ITEMS.extend([
		G._Item("Harpoon", {"DisplayName": "어부의 작살", "Kind": "Weapon", "Amount": 0, "Weapon": "Harpoon", "Icon": Icon("Harpoon"), "Description": "", "Price": 0}),
		G._Item("GrilledFish", {"DisplayName": "구운 생선", "Kind": "Heal", "Amount": 80, "Weapon": "", "Icon": Icon("GrilledFish"),
								"Description": "베라네 노점의 소금 구이 고등어. 체력을 80 회복한다.", "Price": 22}),
		G._Gear("SailorCoat", "선원의 외투", "Armor", "Coat", "바닷바람을 막는 두꺼운 푸른 외투. 가벼워서 움직이기 편하다.", 190, Defense=9, Health=10, Speed=0.04),
		G._Gear("PearlRing", "진주 반지", "Accessory", "Pearl", "갈매기 항구 앞바다의 흰 진주를 박은 반지. 마음이 차분해져 급소가 잘 보인다.", 150, Defense=3, Crit=0.08),
		G._Gear("CompassCharm", "선장의 나침반", "Accessory", "Compass", "해적 선장 바렌의 선장실에서 나온 놋쇠 나침반. 길을 잃지 않게 발걸음을 이끈다.", 0,
				Defense=5, Health=25, Speed=0.08, Crit=0.05),
		G._Item("LighthouseLens", {"DisplayName": "등대 렌즈", "Kind": "Material", "Amount": 0, "Weapon": "", "Icon": Icon("Lens"),
								   "Description": "해적들이 떼어 간 등대의 큰 유리 렌즈. 등대지기 노아에게 돌려주자.", "Price": 0}),
	])
	G.ENEMIES.extend([
		("Crab", {"DisplayName": "바위 게", "Behavior": "Charger", "MaxHealth": 52, "ContactDamage": 9, "AttackDamage": 14, "MoveSpeed": 250, "AggroRange": 800,
				  "AttackRange": 360, "AttackCooldown": 2.1, "WindupTime": 0.55, "ProjectileSpeed": 1100, "GoldMin": 4, "GoldMax": 7, "Exp": 12,
				  "DropItem": "GrilledFish", "DropChance": 0.25, "Radius": 52, "RespawnTime": 16, "Look": "", "Tint": [1, 1, 1, 1]}),
		("Pirate", {"DisplayName": "해적 졸개", "Behavior": "Charger", "MaxHealth": 64, "ContactDamage": 7, "AttackDamage": 16, "MoveSpeed": 320, "AggroRange": 1000,
					"AttackRange": 400, "AttackCooldown": 1.9, "WindupTime": 0.45, "ProjectileSpeed": 1350, "GoldMin": 6, "GoldMax": 11, "Exp": 16,
					"DropItem": "HiPotion", "DropChance": 0.12, "Radius": 48, "RespawnTime": 20, "Look": "", "Tint": [1, 1, 1, 1]}),
		("Seagull", {"DisplayName": "사나운 갈매기", "Behavior": "Flyer", "MaxHealth": 26, "ContactDamage": 6, "AttackDamage": 12, "MoveSpeed": 420, "AggroRange": 1050,
					 "AttackRange": 470, "AttackCooldown": 2.0, "WindupTime": 0.4, "ProjectileSpeed": 1300, "GoldMin": 2, "GoldMax": 5, "Exp": 9,
					 "DropItem": "Ether", "DropChance": 0.12, "Radius": 50, "RespawnTime": 14, "Look": "", "Tint": [1, 1, 1, 1]}),
		("Ghost", {"DisplayName": "떠도는 망령", "Behavior": "Flyer", "MaxHealth": 70, "ContactDamage": 10, "AttackDamage": 16, "MoveSpeed": 300, "AggroRange": 1100,
				   "AttackRange": 520, "AttackCooldown": 2.2, "WindupTime": 0.6, "ProjectileSpeed": 1100, "GoldMin": 8, "GoldMax": 14, "Exp": 22,
				   "DropItem": "Ether", "DropChance": 0.3, "Radius": 52, "RespawnTime": 0, "Look": "", "Tint": [1.15, 1.25, 1.5, 1]}),
		("PirateCaptain", {"DisplayName": "해적 선장 바렌", "Behavior": "Boss", "MaxHealth": 1300, "ContactDamage": 14, "AttackDamage": 20, "MoveSpeed": 270,
						   "AggroRange": 820, "AttackRange": 480, "AttackCooldown": 1.4, "WindupTime": 0.65, "ProjectileSpeed": 1400, "GoldMin": 200,
						   "GoldMax": 200, "Exp": 200, "DropItem": "Elixir", "DropChance": 1.0, "Radius": 105, "RespawnTime": 0, "Look": "", "Tint": [1, 1, 1, 1]}),
	])
	G.ENEMY_CAPSULE.update({
		"Crab":          (40.0, 6.0, 0.0, 115.0, 35.0),
		"Pirate":        (34.0, 36.0, 0.0, 195.0, 50.0),
		"Seagull":       (30.0, 10.0, 105.0, 185.0, 12.0),
		"Ghost":         (32.0, 14.0, 80.0, 190.0, 10.0),
		"PirateCaptain": (62.0, 58.0, 0.0, 330.0, 500.0),
	})
	G.BOSS_SCRIPTS["PirateCaptain"] = "Scripts/Demo/HD2D/HD2DPirateCaptain.lua"
	G.ENEMY_SPRITES.update({Kind: ("Sprites/HD2D/HarborEnemies.esprite", "", f"Sprites/HD2D/{Kind}_Idle.eflipbook") for Kind in ("Crab", "Pirate", "Seagull", "Ghost")})
	G.ENEMY_SPRITES["PirateCaptain"] = ("Sprites/HD2D/PirateCaptain.esprite", "Dormant0", "Sprites/HD2D/PirateCaptain_Dormant.eflipbook")
	G.ENEMY_SHADOWS.update({"PirateCaptain": 2.2, "Seagull": 0.6, "Ghost": 0.7, "Crab": 0.9})

	# 기존 마을 사람 행에 새 필드 기본값 (상점 진열·밤 대사 — 비면 예전 동작)
	for _, Row in G.NPC_ROWS:
		for Key, Value in NPC_FIELD_DEFAULTS.items():
			Row.setdefault(Key, list(Value) if isinstance(Value, list) else Value)
	G.NPC_ROWS.extend([
		_Npc(G, "Keeper", "등대지기 노아", "Quest", SubQuest="Lighthouse"),
		_Npc(G, "HarborMaster", "항만장 마르타", "Quest", SubQuest="Captain"),
		_Npc(G, "Fisher", "어부 토마", "Quest", SubQuest="Pirates"),
		_Npc(G, "Fishmonger", "생선 장수 베라", "Shop",
			 ["어서 와요! 오늘 새벽에 잡은 고등어가 아주 싱싱해요.", "구운 생선 하나면 기운이 번쩍 난답니다. 바다 사람 옷이랑 진주 반지도 있어요!"],
			 ShopStock=["GrilledFish", "Potion", "Ether", "HiPotion", "SailorCoat", "PearlRing"], ShopTitle="베라의 어시장",
			 ShopLines=["뭘 드릴까요? 전부 오늘 들어온 거예요!", "고마워요! 바다의 축복이 함께하길!", "어머, 골드가 조금 모자라네요…", "그건 이미 가지고 있잖아요?",
						"또 와요! 절벽 길에선 몸조심하고요."],
			 NightLines=["오늘 장사는 끝났어요. 내일 아침 일찍 와요!", "밤바다엔 이상한 게 떠다닌다니까, 절벽 길엔 가지 말아요."]),
		_Npc(G, "Innkeeper", "여관 주인 한나", "Inn", ["갈매기 여관에 어서 오세요.", "하룻밤 묵으면 20골드예요. 푹 자고 나면 아침이랍니다."]),
		_Npc(G, "Sailor", "선원 잭", "Talk",
			 ["저 배? 내가 타는 '바다제비호'야. 해적 놈들 때문에 벌써 보름째 출항을 못 하고 있지.", "동쪽 문 밖 절벽 길엔 해적 졸개랑 사나운 갈매기가 들끓어. 바위 게 집게도 조심하고."],
			 NightLines=["(딸꾹) 이 시간엔 노래나 한 곡… ♪ 바다 건너 저 멀리~ 갈매기 우는 항구로~", "밤엔 절벽 길에 망령이 나온다던데… 난 절대 안 가."]),
		_Npc(G, "GhostSailor", "유령 선원", "Talk",
			 ["…추워. 백 년 전 그 폭풍이 치던 밤처럼.", "등대 불이 다시 켜진다면… 나도 집으로 돌아가는 길을 찾을 수 있을까."], NightOnly=True),
		_Npc(G, "VillageInnkeeper", "여관 주인 로렌", "Inn", ["어서 오게. 하르트 선술집 겸 여관일세.", "하룻밤 묵으면 20골드. 침대는 푹신하고 아침밥도 따뜻하다네."]),
	])
	Area = (2700.0, -150.0, 1100.0)
	G.SUB_QUESTS.extend([
		("Lighthouse", {"Title": "꺼진 등대", "Giver": "Keeper", "Kind": "Find", "Target": "LighthouseLens", "Count": 1, "AreaX": 0, "AreaY": 0, "AreaRadius": 0,
						"RewardGold": 150, "RewardItem": "Elixir", "RewardExp": 35,
						"Summary": "해적들이 등대 렌즈를 떼어 가 등대 불이 꺼졌다. 동쪽 절벽 길 해적 야영지에서 렌즈를 되찾아 등대지기 노아에게 가져가자.",
						"AcceptLines": ["이보게, 여행자. 이 등대가 꺼진 지 벌써 열흘째라네.", "해적 놈들이 등실의 렌즈를 떼어 가 버렸지. 동쪽 절벽 위 야영지 어딘가에 있을 걸세.",
										"렌즈만 찾아 주면 밤바다를 다시 비출 수 있다네."],
						"ProgressLines": ["렌즈는 동쪽 절벽 위 해적 야영지에 있을 걸세. 바위 게 집게를 조심하게."],
						"DoneLines": ["오오, 바로 이 렌즈일세! 흠집 하나 없구먼.", "오늘 밤부터 다시 등대 불을 켤 수 있겠어. 해가 지면 꼭 바다를 보게나."],
						"AfterLines": ["등대 불빛이 닿는 데까지는 배들이 안심하고 다닌다네."]}),
		("Pirates", {"Title": "해적 소탕", "Giver": "Fisher", "Kind": "Hunt", "Target": "", "Count": 4, "AreaX": Area[0], "AreaY": Area[1], "AreaRadius": Area[2],
					 "RewardGold": 120, "RewardItem": "Harpoon", "RewardExp": 40,
					 "Summary": "동쪽 절벽 길에 진을 친 해적과 마물 4마리를 쓰러뜨려 어부 토마가 다시 그물을 걷으러 나갈 수 있게 하자.",
					 "AcceptLines": ["후우… 물고기는 많은데 나갈 수가 없어.", "절벽 길에 해적 졸개들이 진을 치고 지나가는 배에 돌을 던진다니까.",
									 "해적이든 마물이든 넷만 쫓아내 주면 내 작살을 주지!"],
					 "ProgressLines": ["절벽 길의 해적·마물 넷만 부탁해. 동쪽 문 밖이야."],
					 "DoneLines": ["정말 해냈구나! 이제 아침 그물을 걷으러 나갈 수 있겠어.", "자, 내 작살이야. 창보다 멀리 닿고 묵직하지."],
					 "AfterLines": ["오늘은 고등어가 잘 잡혔어. 베라네 노점에 넘겼으니 가서 맛봐."]}),
		("Captain", {"Title": "해적 선장", "Giver": "HarborMaster", "Kind": "Boss", "Target": "Harbor", "Count": 1, "AreaX": 0, "AreaY": 0, "AreaRadius": 0,
					 "RewardGold": 300, "RewardItem": "CompassCharm", "RewardExp": 80,
					 "Summary": "동쪽 후미에 배를 댄 해적 선장 바렌을 쓰러뜨려 항구를 지키자. 화약통과 권총, 칼 돌진을 조심하자.",
					 "AcceptLines": ["당신이 소문의 여행자군요. 항만장 마르타예요.", "동쪽 후미에 해적 선장 바렌이 배를 대고 항구를 노리고 있어요.",
									 "그자를 쓰러뜨려 준다면 항구가 크게 사례하겠어요. 화약통과 권총을 조심해요!"],
					 "ProgressLines": ["바렌은 동쪽 후미 모래 해변에 있어요. 절벽 길 끝에서 내려가면 돼요."],
					 "DoneLines": ["바렌을 쓰러뜨렸다고요?! 항구 사람 모두 당신에게 빚을 졌네요.", "선장실에서 찾은 나침반이에요. 길을 잃지 않게 지켜 줄 거예요."],
					 "AfterLines": ["바다제비호도 곧 출항할 수 있겠어요. 언제든 다시 들러 줘요."]}),
	])


# ================================================================ 메타 시스템(lane-c HD2DMetaGen) 표에 항구 행 덧붙이기 — 기존 행 순서 뒤 (여러 번 불러도 한 번만)
def ExtendMeta():
	import HD2DMetaArt
	import HD2DMetaGen
	import HD2DWorldArt as WA
	Known = {Row[0] for Row in HD2DMetaGen.BESTIARY}
	for Kind, Habitat, Desc, Draw in (
			("Crab", "해안 절벽 길", "바위틈에 사는 붉은 게. 집게를 치켜들었다가 옆걸음으로 돌진해 꼬집는다. 구워 먹으면 맛있다는 소문.", lambda: WA.DrawCrab("Idle", 0)),
			("Seagull", "해안 절벽 길", "생선 냄새에 사나워진 갈매기. 머리 위를 맴돌다 부리로 내리꽂는다.", lambda: WA.DrawSeagull("Idle", 0)),
			("Pirate", "해적 야영지", "바렌 선장을 따르는 해적 졸개. 칼을 치켜들었다가 단숨에 베어 들어온다.", lambda: WA.DrawPirate("Idle", 0)),
			("Ghost", "밤의 절벽 길", "밤바다에서 길을 잃은 혼. 해가 지면 절벽 길을 떠돌다 해가 뜨면 사라진다.", lambda: WA.DrawGhost("Idle", 0)),
			("PirateCaptain", "해적 후미", "갈매기 항구를 노리는 해적 선장 바렌. 칼 돌진·권총·화약통을 쓰고, 화나면 앞바다 배에서 포격을 부른다.",
			 lambda: WA.DrawCaptain("Idle", 0))):
		HD2DMetaArt.PICTURES.setdefault(Kind, Draw)
		if Kind not in Known:
			HD2DMetaGen.BESTIARY.append((Kind, Habitat, Desc))
	Journal = {Row[0] for Row in HD2DMetaGen.QUEST_JOURNAL}
	for Key, Row in (
			("Sub_Lighthouse", {"Title": "꺼진 등대", "Summary": "", "Objective": "해적 야영지에서 등대 렌즈를 찾자", "Report": "등대지기 노아에게 렌즈를 돌려주자"}),
			("Sub_Pirates", {"Title": "해적 소탕", "Summary": "", "Objective": "동쪽 절벽 길의 해적·마물을 쓰러뜨리자", "Report": "어부 토마에게 보고하자"}),
			("Sub_Captain", {"Title": "해적 선장", "Summary": "", "Objective": "동쪽 후미의 해적 선장 바렌을 쓰러뜨리자", "Report": "항만장 마르타에게 보고하자"})):
		if Key not in Journal:
			HD2DMetaGen.QUEST_JOURNAL.append((Key, Row))
	# 재료 드랍(MaterialDrops)은 넣지 않는다 — 항구 적은 골드·회복약만 (행이 없으면 드랍 없음)


# ================================================================ 새 표 (지역·낮밤)
REGIONS = [
	("Village", {"DisplayName": "하르트 마을", "Subtitle": "황혼의 들판 어귀", "DayNight": True,
				 "NightLights": ["LanternPost_#_Light", "WindowPool_#", "Tavern_DoorLight", "Shop_DoorLight", "Chapel_Glow", "Well_Lantern", "Mill_Lantern",
								 "Castle_GateLight-1", "Castle_GateLight1", "Night_#"],
				 "NightWindows": "NightWin_#", "DayLights": []}),
	("Cave", {"DisplayName": "고대 동굴 유적", "Subtitle": "푸른 수정이 잠든 곳", "DayNight": False, "NightLights": [], "NightWindows": "",
			  "DayLights": ["Entrance_Shaft", "Entrance_Glow"]}),
	("Harbor", {"DisplayName": "갈매기 항구", "Subtitle": "바닷바람이 머무는 항구 마을", "DayNight": True, "NightLights": ["Night_#"],
				"NightWindows": "NightWin_#", "DayLights": []}),
]

DAY_NIGHT = {"DayMinutes": 10.0, "StartHour": 9.0, "SleepHour": 7.0, "InnPrice": 20, "NightStart": 19.2, "NightEnd": 5.4,
			 "LampOn": 17.4, "LampFull": 18.6, "LampDim": 5.6, "LampOff": 6.8}

# 시각별 화면 열쇠 (사이는 선형 보간, 24시 = 0시). 색 보정 값은 절대값, SkyScale/SunScale/MoonScale은 씬 기본값에 곱한다.
#   Vignette = 비네트 세기, Fog = 높이 안개 색, Inscatter = 방향광 산란 색, Exposure = 색 보정 이득 전체 배율
_K = ("Hour", "SkyScale", "SunScale", "Temperature", "Tint", "Saturation", "Contrast", "Lift", "Gamma", "Gain", "Vignette", "Fog", "Inscatter", "Exposure",
	  "Moon", "LampScale", "FogLocal")
# 밤: 환경광은 낮추고(등불 웅덩이가 살게) 달빛을 푸르고 세게 + 푸른 기 색 보정·대비를 올려 탁하지 않게
_NIGHT = (2.4, 1.0, -0.32, 0.0, 1.16, 1.16, [0.0, 0.012, 0.042], [0.95, 1.0, 1.08], [0.94, 1.04, 1.22], 0.5, [0.03, 0.05, 0.11], [0.25, 0.32, 0.55], 1.5,
		  1.1, 2.0, 1.8)
DAY_NIGHT_KEYS = [
	(0.0,) + _NIGHT,
	(4.8,) + _NIGHT,
	(6.0,  1.6, 1.0, 0.06, 0.07, 1.06, 1.06, [0.02, 0.01, 0.04], [1.0, 1.0, 1.02], [1.06, 0.98, 1.02], 0.5, [0.55, 0.46, 0.5], [1.0, 0.62, 0.5], 1.1, 0.4, 1.2, 0.8),
	(8.0,  1.0, 1.0, 0.0, 0.0, 1.1, 1.08, [0.0, 0.006, 0.02], [1.0, 1.0, 1.0], [1.02, 1.01, 1.0], 0.45, [0.5, 0.56, 0.62], [0.9, 0.8, 0.65], 1.0, 0.3, 1.0, 0.45),
	(12.0, 1.0, 1.0, 0.02, 0.0, 1.12, 1.08, [0.0, 0.006, 0.02], [1.0, 1.0, 1.0], [1.02, 1.01, 1.0], 0.42, [0.52, 0.58, 0.64], [0.92, 0.84, 0.7], 1.0, 0.3, 1.0, 0.45),
	(15.5, 1.0, 1.0, 0.1, 0.02, 1.12, 1.09, [0.0, 0.007, 0.03], [1.0, 1.0, 1.01], [1.03, 1.0, 0.98], 0.46, [0.48, 0.47, 0.46], [0.9, 0.7, 0.45], 1.0, 0.3, 1.0, 0.45),
	(16.9, 1.0, 1.0, 0.2, 0.04, 1.12, 1.1, [0.0, 0.008, 0.035], [1.0, 1.0, 1.02], [1.04, 1.0, 0.96], 0.5, [0.45, 0.42, 0.4], [0.9, 0.6, 0.35], 1.0, 0.3, 1.0, 0.5),
	(18.2, 1.5, 1.0, 0.08, 0.08, 1.1, 1.08, [0.01, 0.0, 0.05], [1.0, 0.98, 1.04], [1.04, 0.97, 1.06], 0.52, [0.3, 0.24, 0.34], [0.8, 0.45, 0.45], 1.12, 0.5, 1.4, 1.0),
	(19.6,) + _NIGHT,
]


def WriteData(Content, Struct, Table, Field, WriteJson, Path):
	# HD2DGameplay.WriteData 끝에서 부른다 (같은 도우미를 넘겨받는다)
	Struct(Content, "Region", "HD2D 지역 (Regions.etable, 행 이름 = 맵 id — 관리자 Map 속성): 도착 이름 표시 + 낮밤 적용", [
		Field("DisplayName", "String", "", "지역 이름 (맵 이동 도착 때 화면 위 가운데)"),
		Field("Subtitle", "String", "", "지역 부제"),
		Field("DayNight", "Bool", True, "하늘·색 보정·등불이 게임 시각을 따르는가 (동굴은 끔 — 입구 빛만 DayLights)"),
		Field("NightLights", "Array", [], "밤에 켜는 점광원 엔티티 이름 (# = 0부터 번호, 없으면 멈춤)", Element="String"),
		Field("NightWindows", "String", "", "밤에 불 켜는 창 유리 엔티티 이름 (# = 번호) — 머티리얼을 바꾼다"),
		Field("DayLights", "Array", [], "낮 밝기에 비례하는 빛 엔티티 이름 (동굴 입구 달빛·햇빛)", Element="String"),
	])
	Table(Content, "Regions", "Region", REGIONS)
	Struct(Content, "DayNight", "HD2D 낮밤 설정 (DayNight.edata 하나 — 세 맵 공통 게임 시각, 저장·세션에 유지)", [
		Field("DayMinutes", "Float", 10.0, "실제 몇 분에 하루 (게임 시간 — 메뉴로 멈춘다)"),
		Field("StartHour", "Float", 9.0, "새 게임 시작 시각 (첫인상은 낮)"),
		Field("SleepHour", "Float", 7.0, "여관에서 자고 일어나는 시각"),
		Field("InnPrice", "Int", 20, "여관 하룻밤 골드"),
		Field("NightStart", "Float", 19.2, "밤 시작 (밤 적·밤 주민·밤 대사)"),
		Field("NightEnd", "Float", 5.4, "밤 끝"),
		Field("LampOn", "Float", 17.4, "등불이 켜지기 시작"),
		Field("LampFull", "Float", 18.6, "등불 다 켜짐"),
		Field("LampDim", "Float", 5.6, "새벽 등불이 줄기 시작"),
		Field("LampOff", "Float", 6.8, "등불 다 꺼짐"),
	])
	WriteJson(Path("DayNight.edata"), {"Version": 1, "Struct": "Data/Demo/HD2D/DayNight.estruct", "Values": DAY_NIGHT})
	Struct(Content, "DayNightKey", "HD2D 낮밤 화면 열쇠 (DayNightKeys.etable — 시각 순, 사이는 선형 보간)", [
		Field("Hour", "Float", 0.0, "시각"),
		Field("SkyScale", "Float", 1.0, "하늘빛(환경광) 배율 — 씬 값에 곱함"),
		Field("SunScale", "Float", 1.0, "방향광(해·달) 세기 배율"),
		Field("Temperature", "Float", 0.0, "색 보정 색온도"),
		Field("Tint", "Float", 0.0, "색 보정 틴트"),
		Field("Saturation", "Float", 1.0, "채도"),
		Field("Contrast", "Float", 1.0, "대비"),
		Field("Lift", "Array", [0.0, 0.0, 0.0], "어두운 부분 올림 RGB", Element="Float"),
		Field("Gamma", "Array", [1.0, 1.0, 1.0], "중간 톤 RGB", Element="Float"),
		Field("Gain", "Array", [1.0, 1.0, 1.0], "밝은 부분 이득 RGB", Element="Float"),
		Field("Vignette", "Float", 0.45, "비네트 세기"),
		Field("Fog", "Array", [0.5, 0.5, 0.5], "높이 안개 색", Element="Float"),
		Field("Inscatter", "Array", [0.9, 0.8, 0.6], "방향광 안개 산란 색", Element="Float"),
		Field("Exposure", "Float", 1.0, "이득 전체 배율 (밤을 너무 어둡지 않게)"),
		Field("Moon", "Float", 0.3, "달빛 세기 (대기 MoonIntensity — 해가 지면 방향광이 달로 바뀐다)"),
		Field("LampScale", "Float", 1.0, "등불 세기 배율 (주변이 어두운 밤에 조금 더 세게 — 반경도 함께 넓힌다)"),
		Field("FogLocal", "Float", 0.45, "볼류메트릭 안개의 점광원·스포트 산란 배율 (밤 등불 무리·등대 빛줄기)"),
	])
	Table(Content, "DayNightKeys", "DayNightKey", [(f"K{I:02d}", dict(zip(_K, Key))) for I, Key in enumerate(DAY_NIGHT_KEYS)])


def HudWidgets(G):
	# 지역 이름 띠 (맵 도착 — "─ 갈매기 항구 ─" + 부제, 위 가운데) + 시계 (위 가운데 작은 해/달 + 시각)
	W, T, CS, BS = G.Widget, G.Text, G.CanvasSlot, G.BoxSlot
	return [
		W("Canvas", "RegionBanner", CS((0.5, 0), 0, 96, 760, 120, (0.5, 0), Z=6), "Collapsed", [
			W("Border", "RegionBand", CS((0.5, 0), 0, 6, 760, 104, (0.5, 0)), "HitTestInvisible", Brush=G.Brush((0.02, 0.01, 0.05, 0.42)), ContentPadding=[0, 0, 0, 0]),
			W("Image", "RegionOrnament", CS((0.5, 0), 0, 64, 384, 48, (0.5, 0), Z=1), "HitTestInvisible", Brush=G.Brush(Texture=f"{G.UI_DIR}/TitleOrnament.png"),
			  ImageSize=[384, 48]),
			T("RegionName", "", 44, CS((0.5, 0), 0, 10, 0, 0, (0.5, 0), True, 2), G.TEXT_GOLD, "Center", Outline=3),
			T("RegionSub", "", 19, CS((0.5, 0), 0, 82, 0, 0, (0.5, 0), True, 2), (0.92, 0.88, 1.0, 1), "Center"),
		]),
		W("HorizontalBox", "ClockRow", CS((0.5, 0), 0, 14, 0, 0, (0.5, 0), True, 3), "HitTestInvisible", [
			G.Img("ClockIcon", f"{G.UI_DIR}/Icons/Sun.png", 26, BS((0, 0, 8, 0), VAlign="Center")),
			T("ClockText", "16:24", 20, BS(VAlign="Center"), G.TEXT_LIGHT),
		]),
	]
