# FarmBie 데이터 표 생성 (Data/FarmBie/*.estruct·*.etable·*.edata) — BuildFarmBie.py가 부른다.
#   수치는 전부 여기에 두고 스크립트는 Scripts/FarmBie/FarmData.lua로만 읽는다 (스크립트에 수치 상수를 두지 않는다).
#   표를 고치려면 이 파일을 고치고 BuildFarmBie.py를 다시 실행한다.
import json
import os

DATA = "Data/FarmBie"


def _WriteJson(Path, Doc):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Doc, File, indent=2, ensure_ascii=False)
		File.write("\n")


def Field(Name, Type, Default, Description, **Extra):
	F = {"Name": Name, "Type": Type, "Default": Default, "Description": Description}
	F.update(Extra)
	return F


def Struct(Content, Name, Description, Fields):
	_WriteJson(os.path.join(Content, *DATA.split("/"), f"{Name}.estruct"), {"Version": 1, "Name": Name, "Description": Description, "Fields": Fields})


def Table(Content, Name, StructName, Rows):
	_WriteJson(os.path.join(Content, *DATA.split("/"), f"{Name}.etable"),
			   {"Version": 1, "Struct": f"{DATA}/{StructName}.estruct", "Rows": [{"Name": N, "Values": V} for N, V in Rows]})


def Values(Content, Name, StructName, Vals):
	_WriteJson(os.path.join(Content, *DATA.split("/"), f"{Name}.edata"), {"Version": 1, "Struct": f"{DATA}/{StructName}.estruct", "Values": Vals})


# ---- 시간·달력 ------------------------------------------------------------------------------------------------------------
# 하루 = 낮(DayStartHour → NightStartHour, DayRealMinutes 실제 분) + 밤(NightStartHour → NightEndHour(다음 날 새벽), NightRealMinutes 실제 분 = 디펜스 제한 시간)
CALENDAR = {
	"DayStartHour": 6.0, "NightStartHour": 20.0, "NightEndHour": 2.0, "DayRealMinutes": 8.0, "NightRealMinutes": 3.5,
	"SeasonDays": 30, "SeasonWarnDays": 3, "BossEveryDays": 10,
	"SeasonNames": ["봄", "여름", "가을", "겨울"], "WeekdayNames": ["월", "화", "수", "목", "금", "토", "일"],
	"MerchantDays": [2, 5],  # 0 = 월 → 수·토
	# 게임 시각 → 하늘 시각 (하늘 모델의 해 지는 시각이 게임 밤 시작보다 이르다): 낮 DayStart~NightStart → SkyDayStart~SkyNightStart,
	# 밤 NightStart~NightEnd(+24) → SkyNightStart~SkyNightEnd(+24)
	"SkyDayStart": 6.3, "SkyNightStart": 19.0, "SkyNightEnd": 4.0,
	"SaveSlots": 3,
}

CALENDAR_FIELDS = [
	Field("DayStartHour", "Float", 6.0, "아침 기상 시각 (시)"),
	Field("NightStartHour", "Float", 20.0, "밤(좀비 습격) 시작 시각"),
	Field("NightEndHour", "Float", 2.0, "밤 끝 시각 (다음 날 새벽) — 여기서 하루가 끝나고 잠든다"),
	Field("DayRealMinutes", "Float", 8.0, "낮 길이 (실제 분)"),
	Field("NightRealMinutes", "Float", 3.5, "밤 길이 (실제 분) = 디펜스 제한 시간"),
	Field("SeasonDays", "Int", 30, "계절 길이 (일)"),
	Field("SeasonWarnDays", "Int", 3, "계절 끝 며칠 전부터 작물 사멸 경고"),
	Field("BossEveryDays", "Int", 10, "중간 보스 주기 (일차가 이 배수인 밤)"),
	Field("SeasonNames", "Array", ["봄", "여름", "가을", "겨울"], "계절 이름", Element="String"),
	Field("WeekdayNames", "Array", ["월", "화", "수", "목", "금", "토", "일"], "요일 이름 (1일 = 첫 요일)", Element="String"),
	Field("MerchantDays", "Array", [2, 5], "보부상 방문 요일 (0 = 첫 요일)", Element="Int"),
	Field("SaveSlots", "Int", 3, "저장 슬롯 수"),
	Field("SkyDayStart", "Float", 6.3, "아침 기상 때의 하늘 시각"),
	Field("SkyNightStart", "Float", 19.0, "밤 시작 때의 하늘 시각 (해 진 직후)"),
	Field("SkyNightEnd", "Float", 4.0, "밤 끝 때의 하늘 시각 (아직 어두움)"),
]

# ---- 낮밤 화면 열쇠 (Hour = 하늘 시각 — 게임 시각을 하늘 시각으로 바꾼 값, FarmTime:GetSkyHour. 하늘 모델은 18시 무렵 해가 진다) ---------------------------------------------
NIGHT = {"SkyScale": 2.4, "SunScale": 1.0, "Temperature": -0.32, "Tint": 0.0, "Saturation": 0.92, "Contrast": 1.12, "Lift": [0.0, 0.012, 0.042],
		 "Gamma": [0.95, 1.0, 1.08], "Gain": [0.94, 1.04, 1.22], "Vignette": 0.5, "Fog": [0.03, 0.05, 0.11], "Inscatter": [0.25, 0.32, 0.55],
		 "Exposure": 1.2, "Moon": 1.2, "LampScale": 1.6}
DAY_NIGHT_KEYS = [
	dict(NIGHT, Hour=0.0),
	dict(NIGHT, Hour=4.8),
	{"Hour": 6.0, "SkyScale": 1.6, "SunScale": 1.0, "Temperature": 0.06, "Tint": 0.07, "Saturation": 1.06, "Contrast": 1.06, "Lift": [0.02, 0.01, 0.04],
	 "Gamma": [1.0, 1.0, 1.02], "Gain": [1.06, 0.98, 1.02], "Vignette": 0.45, "Fog": [0.55, 0.46, 0.5], "Inscatter": [1.0, 0.62, 0.5], "Exposure": 1.1,
	 "Moon": 0.4, "LampScale": 1.0},
	{"Hour": 8.0, "SkyScale": 1.0, "SunScale": 1.0, "Temperature": 0.0, "Tint": 0.0, "Saturation": 1.1, "Contrast": 1.06, "Lift": [0.0, 0.006, 0.02],
	 "Gamma": [1.0, 1.0, 1.0], "Gain": [1.02, 1.01, 1.0], "Vignette": 0.35, "Fog": [0.5, 0.56, 0.62], "Inscatter": [0.9, 0.8, 0.65], "Exposure": 1.0,
	 "Moon": 0.3, "LampScale": 0.0},
	{"Hour": 12.0, "SkyScale": 1.0, "SunScale": 1.0, "Temperature": 0.04, "Tint": 0.0, "Saturation": 1.1, "Contrast": 1.06, "Lift": [0.0, 0.006, 0.02],
	 "Gamma": [1.0, 1.0, 1.0], "Gain": [1.02, 1.01, 1.0], "Vignette": 0.35, "Fog": [0.52, 0.58, 0.64], "Inscatter": [0.92, 0.84, 0.7], "Exposure": 1.0,
	 "Moon": 0.3, "LampScale": 0.0},
	{"Hour": 15.5, "SkyScale": 1.0, "SunScale": 1.0, "Temperature": 0.12, "Tint": 0.02, "Saturation": 1.12, "Contrast": 1.08, "Lift": [0.0, 0.007, 0.03],
	 "Gamma": [1.0, 1.0, 1.01], "Gain": [1.03, 1.0, 0.98], "Vignette": 0.4, "Fog": [0.48, 0.47, 0.46], "Inscatter": [0.9, 0.7, 0.45], "Exposure": 1.0,
	 "Moon": 0.3, "LampScale": 0.0},
	{"Hour": 16.9, "SkyScale": 1.0, "SunScale": 1.0, "Temperature": 0.22, "Tint": 0.04, "Saturation": 1.12, "Contrast": 1.1, "Lift": [0.0, 0.008, 0.035],
	 "Gamma": [1.0, 1.0, 1.02], "Gain": [1.04, 1.0, 0.96], "Vignette": 0.48, "Fog": [0.45, 0.42, 0.4], "Inscatter": [0.9, 0.6, 0.35], "Exposure": 1.0,
	 "Moon": 0.3, "LampScale": 0.0},
	{"Hour": 18.2, "SkyScale": 1.5, "SunScale": 1.0, "Temperature": 0.08, "Tint": 0.08, "Saturation": 1.1, "Contrast": 1.08, "Lift": [0.01, 0.0, 0.05],
	 "Gamma": [1.0, 0.98, 1.04], "Gain": [1.04, 0.97, 1.06], "Vignette": 0.5, "Fog": [0.3, 0.24, 0.34], "Inscatter": [0.8, 0.45, 0.45], "Exposure": 1.12,
	 "Moon": 0.5, "LampScale": 1.4},
	dict(NIGHT, Hour=19.6),
]

DAY_NIGHT_FIELDS = [
	Field("Hour", "Float", 0.0, "시각 (오름차순)"),
	Field("SkyScale", "Float", 1.0, "하늘빛 배율"),
	Field("SunScale", "Float", 1.0, "해/달빛 배율"),
	Field("Temperature", "Float", 0.0, "색온도"),
	Field("Tint", "Float", 0.0, "색조"),
	Field("Saturation", "Float", 1.0, "채도"),
	Field("Contrast", "Float", 1.0, "대비"),
	Field("Lift", "Array", [0.0, 0.0, 0.0], "어두운 영역 색", Element="Float"),
	Field("Gamma", "Array", [1.0, 1.0, 1.0], "중간 영역 색", Element="Float"),
	Field("Gain", "Array", [1.0, 1.0, 1.0], "밝은 영역 색", Element="Float"),
	Field("Vignette", "Float", 0.4, "비네트 세기"),
	Field("Fog", "Array", [0.5, 0.5, 0.5], "안개 색", Element="Float"),
	Field("Inscatter", "Array", [0.9, 0.8, 0.6], "해 쪽 안개 산란 색", Element="Float"),
	Field("Exposure", "Float", 1.0, "밝기 배율 (Gain에 곱함)"),
	Field("Moon", "Float", 0.3, "달 밝기"),
	Field("LampScale", "Float", 0.0, "등불 세기 배율 (0 = 꺼짐)"),
]


# ---- 농사 ---------------------------------------------------------------------------------------------------------------------
# 희귀도: 판매가 배율, 수확 때 한 단계 위 씨앗이 나올 확률(비료 배율을 곱함)
RARITY_ROWS = [
	("Common",    {"DisplayName": "일반",     "PriceMul": 1.0,  "UpChance": 0.10,  "Color": [0.92, 0.9, 0.86, 1.0]}),
	("Rare",      {"DisplayName": "레어",     "PriceMul": 3.0,  "UpChance": 0.06,  "Color": [0.45, 0.7, 1.0, 1.0]}),
	("Unique",    {"DisplayName": "유니크",   "PriceMul": 10.0, "UpChance": 0.03,  "Color": [0.8, 0.5, 1.0, 1.0]}),
	("Legendary", {"DisplayName": "레전더리", "PriceMul": 40.0, "UpChance": 0.0,   "Color": [1.0, 0.82, 0.3, 1.0]}),
]

FARMING = {
	"SeedReturnChance": 0.35,       # 한 번 열리는 작물을 수확할 때 같은 단계 씨앗 1개가 돌아올 확률
	"ExclusiveChance": 0.012,       # 일반 작물 수확 때 그 계절 전용 희귀종(레어) 씨앗이 나올 확률 (비료 배율을 곱함)
	"FertBasicMul": 1.6, "FertPremiumMul": 2.6,  # 비료를 준 칸의 희귀 확률 배율
	"CanCapacity": 40,              # 물뿌리개 물 (칸 하나에 1)
	"ToolTime": 0.42, "ToolHitTime": 0.2,  # 도구 동작 길이 / 효과가 나는 때 (초)
	"ReachDistance": 75.0,          # 발 앞 대상 칸 거리 (cm)
	"StartItems": ["Hoe*1", "Can*1", "Seed:EyeRadish:0*10", "Seed:TentacleLeek:0*5", "FertBasic*5"],
}

FARMING_FIELDS = [
	Field("SeedReturnChance", "Float", 0.35, "한 번 열리는 작물 수확 때 같은 단계 씨앗 1개가 돌아올 확률"),
	Field("ExclusiveChance", "Float", 0.012, "일반 작물 수확 때 계절 전용 희귀종 씨앗 확률"),
	Field("FertBasicMul", "Float", 1.6, "기본 비료 칸 희귀 확률 배율"),
	Field("FertPremiumMul", "Float", 2.6, "고급 비료 칸 희귀 확률 배율"),
	Field("CanCapacity", "Int", 40, "물뿌리개 물 양"),
	Field("ToolTime", "Float", 0.42, "도구 동작 길이 (초)"),
	Field("ToolHitTime", "Float", 0.2, "도구 효과가 나는 때 (초)"),
	Field("ReachDistance", "Float", 75.0, "발 앞 대상 칸 거리 (cm)"),
	Field("StartItems", "Array", [], "처음 소지품 \"아이템*개수\"", Element="String"),
]

ITEM_ROWS = [
	("Hoe",         {"DisplayName": "괭이",       "Kind": "Tool", "Price": 0,   "Description": "풀밭을 갈아 밭을 만든다. 시든 작물도 걷어낸다."}),
	("Can",         {"DisplayName": "물뿌리개",   "Kind": "Tool", "Price": 0,   "Description": "작물에 물을 준다. 물이 떨어지면 우물에서 채운다."}),
	("FertBasic",   {"DisplayName": "기본 비료",  "Kind": "Fertilizer", "Price": 20, "Description": "간 칸에 뿌리면 그 칸 작물의 희귀 씨앗 확률이 오른다."}),
	("FertPremium", {"DisplayName": "고급 비료",  "Kind": "Fertilizer", "Price": 60, "Description": "희귀 씨앗 확률이 크게 오른다. 보부상이 가끔 들고 온다."}),
]


def WriteFarming(Content):
	import FarmBieCrops as FC
	Struct(Content, "Crop", "작물", [
		Field("DisplayName", "String", "", "이름"),
		Field("Season", "Enum", "Spring", "자라는 계절", Values=FC.SEASONS),
		Field("Days", "Int", 4, "다 자라는 날 수 (물 준 날만 센다)"),
		Field("Regrow", "Int", 0, "수확 뒤 다시 열리는 날 수 (0 = 한 번)"),
		Field("Price", "Int", 10, "일반 단계 판매가"),
		Field("SeedPrice", "Int", 10, "일반 단계 씨앗 값 (0 = 팔지 않음)"),
		Field("Exclusive", "Bool", False, "계절 전용 희귀종 (수확 때 드물게만 씨앗이 나온다)"),
		Field("Sanity", "Int", 0, "먹으면 회복하는 정신력 (0 = 정신력 음식 아님)"),
		Field("Description", "String", "", "설명"),
	])
	Table(Content, "Crops", "Crop", [(C["Id"], {"DisplayName": C["Name"], "Season": C["Season"], "Days": C["Days"], "Regrow": C["Regrow"],
											   "Price": C["Price"], "SeedPrice": C["SeedPrice"], "Exclusive": C["Exclusive"], "Sanity": C["Sanity"],
											   "Description": C["Desc"]}) for C in FC.CROPS])
	Struct(Content, "Rarity", "희귀도", [
		Field("DisplayName", "String", "", "이름"),
		Field("PriceMul", "Float", 1.0, "판매가 배율"),
		Field("UpChance", "Float", 0.0, "수확 때 한 단계 위 씨앗 확률"),
		Field("Color", "Array", [1.0, 1.0, 1.0, 1.0], "글자 색", Element="Float"),
	])
	Table(Content, "Rarities", "Rarity", RARITY_ROWS)
	Struct(Content, "Farming", "농사 수치", FARMING_FIELDS)
	Values(Content, "Farming", "Farming", FARMING)
	Struct(Content, "Item", "도구·비료 등 (씨앗·작물은 Crops 표에서 만든다)", [
		Field("DisplayName", "String", "", "이름"),
		Field("Kind", "Enum", "Tool", "종류", Values=["Tool", "Fertilizer", "Material", "Food", "Weapon", "Trap"]),
		Field("Price", "Int", 0, "보부상 값 (0 = 팔지 않음)"),
		Field("Description", "String", "", "설명"),
	])
	Table(Content, "Items", "Item", ITEM_ROWS)


def WriteFarmMap(Content, Grid):
	# 농장 격자 (BuildFarmBie.py 상수 — 생성기와 게임 코드가 같은 값을 쓰도록 여기로 넘긴다)
	Struct(Content, "FarmMap", "농장 격자", [
		Field("Tile", "Float", 100.0, "칸 크기 (cm)"),
		Field("Width", "Int", 52, "가로 칸 수"),
		Field("Height", "Int", 40, "세로 칸 수"),
		Field("OriginX", "Float", -2600.0, "격자 왼쪽 위 X"),
		Field("OriginY", "Float", -2000.0, "격자 왼쪽 위 Y"),
		Field("FarmMinX", "Float", 0.0, "밭을 갈 수 있는 영역 (울타리 안)"),
		Field("FarmMinY", "Float", 0.0, ""),
		Field("FarmMaxX", "Float", 0.0, ""),
		Field("FarmMaxY", "Float", 0.0, ""),
		Field("Well", "Array", [0.0, 0.0], "우물 위치 (물 채우기)", Element="Float"),
	])
	Values(Content, "FarmMap", "FarmMap", Grid)


def WriteAll(Content):
	Struct(Content, "Calendar", "FarmBie 시간·달력", CALENDAR_FIELDS)
	Values(Content, "Calendar", "Calendar", CALENDAR)
	Struct(Content, "DayNightKey", "낮밤 화면 열쇠 (시각별 하늘빛·색 보정·등불)", DAY_NIGHT_FIELDS)
	Table(Content, "DayNightKeys", "DayNightKey", [(f"K{I:02d}", Row) for I, Row in enumerate(DAY_NIGHT_KEYS)])
	WriteFarming(Content)
