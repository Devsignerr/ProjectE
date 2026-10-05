# FarmBie 밸런스 표 — 데이터(FarmBieData.py·FarmBieCrops.py)에서 기대값을 계산해 Projects/FarmBie/Balance.md에 쓴다.
#   python Projects/FarmBie/Tools/FarmBieBalance.py   (데이터를 바꾸면 다시 돌려 표를 갱신한다 — 게임은 이 파일을 읽지 않는다)
#   작물: 계절 30일 동안 칸 하나 기대 수익(일반 단계, 매일 물, 1일차 심기, 한 번 열리는 작물은 다시 심음 — 씨앗 반환 확률 반영)
#   희귀: 수확 한 번에 위 단계 씨앗이 나올 확률(비료 배율), 단계별 값
#   밤: 계절 일차별 좀비 수·체력 배율·총 체력(종류 가중 평균), 1·2·3년차
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "Tools", "DemoMap"))
import FarmBieCrops as FC  # noqa: E402
import FarmBieData as D  # noqa: E402

SEASONS = ["Spring", "Summer", "Autumn", "Winter"]
SEASON_KO = {"Spring": "봄", "Summer": "여름", "Autumn": "가을", "Winter": "겨울"}


def CropSeason(C, Days=30):
	# → (수확 수, 씨앗 비용, 수입)
	if C["Regrow"] > 0:
		Harvests = 0 if C["Days"] > Days else 1 + (Days - C["Days"]) // C["Regrow"]
		return Harvests, C["SeedPrice"], Harvests * C["Price"]
	Harvests = Days // C["Days"]
	Seeds = 1 + (Harvests - 1) * (1.0 - D.FARMING["SeedReturnChance"]) if Harvests > 0 else 1
	return Harvests, Seeds * C["SeedPrice"], Harvests * C["Price"]


def ZombieAverage(Season, Day):
	Rows = [R for _, R in D.ZOMBIE_ROWS if Day >= R["MinDay"] and R["Season"] in ("Any", Season)]
	Total = sum(R["Weight"] for R in Rows)
	Hp = sum(R["Hp"] * R["Weight"] for R in Rows) / Total
	Kinds = ", ".join(Name for Name, R in D.ZOMBIE_ROWS if Day >= R["MinDay"] and R["Season"] in ("Any", Season))
	return Hp, Kinds


def BossHp(Season, Day):
	# 그 밤 보스 기본 체력 합 (10·20일 = 중간 보스 평균, 30일 = 중간 평균 + 그 계절 보스)
	if Day % D.CALENDAR["BossEveryDays"] != 0:
		return 0.0
	Mids = [B["Hp"] for _, B in D.BOSS_ROWS if B["Kind"] == "Mid"]
	Hp = sum(Mids) / len(Mids)
	if Day == D.CALENDAR["SeasonDays"]:
		Hp += sum(B["Hp"] for _, B in D.BOSS_ROWS if B["Kind"] == "Season" and B["Season"] == Season)
	return Hp


def Night(SeasonIndex, Day, Year):
	N = D.NIGHT
	YearMul = 1 + N["CountPerYear"] * (Year - 1)
	Count = int((N["BaseCount"] + N["PerDay"] * Day + N["PerSeason"] * SeasonIndex) * YearMul + 0.5)
	HpMul = (1 + N["HpPerDay"] * Day + N["HpPerSeason"] * SeasonIndex) * (1 + N["HpPerYear"] * (Year - 1))
	return Count, HpMul


def Main():
	Lines = ["# FarmBie 밸런스 표", "", "`Tools/FarmBieBalance.py`가 데이터에서 계산한 기대값이다 (손으로 고치지 않는다). 시간: 낮 "
	         f"{D.CALENDAR['DayRealMinutes']:.0f}분 · 밤 {D.CALENDAR['NightRealMinutes']}분 · 계절 {D.CALENDAR['SeasonDays']}일 · 처음 돈 {D.ECONOMY['StartGold']}.", ""]
	# ---- 작물
	Lines += ["## 작물 (칸 하나, 일반 단계, 계절 30일)", "",
	          "| 계절 | 작물 | 자람/다시 | 값 | 씨앗 | 수확 수 | 씨앗 비용 | 수입 | 순이익 | 하루 순이익 |", "|---|---|---|---|---|---|---|---|---|---|"]
	Best = {}
	for S in SEASONS:
		for C in FC.CROPS:
			if C["Season"] != S:
				continue
			Harvests, Cost, Income = CropSeason(C)
			Net = Income - Cost
			Tag = " (전용)" if C["Exclusive"] else ""
			Lines.append(f"| {SEASON_KO[S]} | {C['Name']}{Tag} | {C['Days']}/{C['Regrow'] or '-'} | {C['Price']} | {C['SeedPrice']} | {Harvests} | "
			             f"{Cost:.0f} | {Income} | {Net:.0f} | {Net / 30:.1f} |")
			if not C["Exclusive"]:
				Best[S] = max(Best.get(S, 0), Net / 30)
	Can = D.FARMING["CanCapacity"]
	Lines += ["", f"물뿌리개 한 통 = {Can}칸. 계절마다 가장 좋은 일반 작물로 {Can}칸을 채우면 하루 순이익 ≈ " +
	          ", ".join(f"{SEASON_KO[S]} {Best[S] * Can:.0f}" for S in SEASONS) + "골드.", ""]
	# ---- 희귀
	Rar = [R for _, R in D.RARITY_ROWS]
	Lines += ["## 희귀도", "", "| 단계 | 값 배율 | 위 단계 씨앗 확률 (비료 없음 / 기본 / 고급) | 먹을 때 버프 크기 |", "|---|---|---|---|"]
	F = D.FARMING
	for Name, R in D.RARITY_ROWS:
		Up = R["UpChance"]
		Lines.append(f"| {R['DisplayName']} | ×{R['PriceMul']:g} | {Up:.1%} / {min(1, Up * F['FertBasicMul']):.1%} / {min(1, Up * F['FertPremiumMul']):.1%} | {R['BuffSize']:g} |")
	Ex = F["ExclusiveChance"]
	Lines += ["", f"전용 희귀종 씨앗: 일반 작물 일반 단계 수확마다 {Ex:.1%} (기본 비료 {Ex * F['FertBasicMul']:.1%}, 고급 {Ex * F['FertPremiumMul']:.1%}). "
	          f"{Can}칸을 한 계절 내내 고급 비료로 키우면 기대 수확 약 {Can * 6}번 → 전용 씨앗 기대 {Can * 6 * Ex * F['FertPremiumMul']:.1f}개.",
	          "레어 이상 씨앗을 칸 하나에 다시 심으면 그 칸 수입이 ×" + "/×".join(f"{R['PriceMul']:g}" for R in Rar[1:]) + "가 된다.", ""]
	# ---- 밤
	# ---- 무기
	Lines += ["## 무기 (초당 피해, 정신력·버프 배율 1)", "", "| 무기 | 피해 | 재사용 | 발 수 | 초당 피해 |", "|---|---|---|---|---|"]
	for Name, W in D.WEAPON_ROWS:
		Shots = W.get("Shots", 1) or 1
		Lines.append(f"| {Name} | {W['Damage']} | {W['Cooldown']}초 | {Shots} | {W['Damage'] * Shots / W['Cooldown']:.0f} |")
	Turrets = [(N, B) for N, B in D.BUILD_ROWS if B.get("Kind") == "Turret"]
	for Name, B in Turrets:
		Lines.append(f"| {Name} (설치물) | {B['Damage']} | {B['Cooldown']}초 | 1 | {B['Damage'] / B['Cooldown']:.0f} |")
	NightSec = D.CALENDAR["NightRealMinutes"] * 60
	Lines += ["", f"밤 길이 {NightSec:.0f}초 안에 다 잡아야 이긴다 → 아래 표의 '필요 초당 피해' = 총 체력 ÷ {NightSec:.0f}초 (보스 밤은 보스 체력 더함, 이동·놓침 여유 없음).", ""]
	# ---- 밤
	Lines += ["## 밤 (좀비 수 · 체력 배율 · 총 체력 · 필요 초당 피해)", "", "| 계절 | 일차 | 1년차 | 2년차 | 3년차 | 나오는 종류 |", "|---|---|---|---|---|---|"]
	for SI, S in enumerate(SEASONS):
		for Day in (1, 10, 20, 30):
			AvgHp, Kinds = ZombieAverage(S, Day)
			Cells = []
			for Year in (1, 2, 3):
				Count, HpMul = Night(SI, Day, Year)
				Total = Count * AvgHp * HpMul + BossHp(S, Day) * HpMul
				Cells.append(f"{Count}마리 ×{HpMul:.2f} = {Total:.0f} ({Total / NightSec:.0f}/초)")
			Lines.append(f"| {SEASON_KO[S]} | {Day} | " + " | ".join(Cells) + f" | {Kinds} |")
	Lines += ["", f"보스 밤: {D.CALENDAR['BossEveryDays']}일마다 중간 보스, 30일차 = 중간 보스 + 계절 보스 (체력에도 그 밤 체력 배율을 곱함).", "",
	          "| 보스 | 종류 | 계절 | 체력 | 1년차 30일 체력 | 보상 돈 |", "|---|---|---|---|---|---|"]
	for Name, B in D.BOSS_ROWS:
		SI = SEASONS.index(B["Season"]) if B["Season"] in SEASONS else 0
		_, HpMul = Night(SI, 30, 1)
		Lines.append(f"| {B['DisplayName']} | {'계절' if B['Kind'] == 'Season' else '중간'} | {SEASON_KO.get(B['Season'], '아무')} | {B['Hp']} | "
		             f"{B['Hp'] * HpMul:.0f} | {B['RewardGold']} |")
	# ---- 탑
	T = D.TOWER
	Lines += ["", "## 탑", "", "| 층 | 적 수 | 체력 배율 | 공격 배율 | 보물 돈 |", "|---|---|---|---|---|"]
	for Floor in (1, 5, 10, 15, 20):
		Count = int(T["BaseEnemies"] + T["EnemiesPerFloor"] * Floor + 0.5) - (2 if Floor % T["GuardianEvery"] == 0 else 0)
		Guard = " + 수호자" if Floor % T["GuardianEvery"] == 0 else ""
		Lines.append(f"| {Floor} | {Count}{Guard} | ×{1 + T['HpPerFloor'] * Floor:.2f} | ×{1 + T['DamagePerFloor'] * Floor:.2f} | {Floor * T['GoldPerFloor']} |")
	Path = os.path.join(os.path.dirname(__file__), "..", "Balance.md")
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		File.write("\n".join(Lines) + "\n")
	print("밸런스 표:", os.path.normpath(Path))


if __name__ == "__main__":
	Main()
