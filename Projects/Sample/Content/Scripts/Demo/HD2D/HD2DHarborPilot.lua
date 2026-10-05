-- HD-2D 데모 자동 조종 ⑤ 항구·낮밤 시나리오 (HD2DAutoPilot.lua가 끝에서 이 표의 함수를 AutoPilot에 붙인다 — 상태는 AutoPilot 객체에).
--   Harbor: 갈매기 항구(_HD2DHarborAutoPlay.escene)에서 시작해 메인 맵까지 두 씬 — 단계 Persistent "HD2D_AutoPhase", 시나리오 "HD2D_AutoScenario"
--     ① Run    : 도착(지역 이름·타이틀 없음) → 의뢰 셋(등대지기·어부·항만장) + 선원 → 어시장 상점(구운 생선·진주 반지) → 동쪽 절벽 길 적 3종
--                (바위 게·갈매기·해적 — 해적 소탕 사냥) → 해적 야영지 렌즈 → 동쪽 후미 해적 선장(패턴 3종 이상·2단계) → 보상 상자
--                → 마을로 보고 셋(작살·엘릭서·나침반) → 낮 → 밤(등불·창·등대 불·밤 적·유령 선원·밤 대사·상점 닫힘) → 망령 처치
--                → 여관 잠 → 아침(회복·밤 적 사라짐) → 언덕길 트리거로 메인 맵
--     ② Arrive : 메인 맵 Spawn_Harbor 도착 (세션 상태·항구 퀘스트·게임 시각 유지, 지역 이름) → 결과
--   HarborDay / HarborNight / HarborLighthouse / HarborCombat / HarborBoss / VillageNight : 스크린샷·측정용 (그 장면에서 머문다)
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local Pilot = {}

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function Pilot:HarborLoadout()
	local GM, P = self.GM, self.Player
	for _, Id in ipairs({ "Spear", "Bow", "Staff", "KnightPlate", "SwiftCharm" }) do GM:AddItem(Id, 1, false) end
	GM:AddItem("Potion", 5, false)
	GM:AddItem("HiPotion", 3, false)
	GM:AddItem("Ether", 3, false)
	GM.Armor, GM.Accessory = "KnightPlate", "SwiftCharm"
	GM.Gold = 420
	P:AddExp(D.Balance().ExpTable[5] or 0)
	P:RecalcStats()
	P.Health, P.Mana, P.BP = P.MaxHealth, P.MaxMana, 3
	GM:RefreshQuest()
	self:Note(string.format("항구 시작 상태: Lv %d, HP %d, 골드 %d", P.Level, P.MaxHealth, GM.Gold))
end

function Pilot:AliveOf(Kind)
	local N = 0
	for _, S in ipairs(self.GM:AliveEnemies()) do
		if S.Kind == Kind then N = N + 1 end
	end
	return N
end

-- 보스전: 공격하며 패턴을 본다 (2단계 뒤 3종을 다 못 봤으면 거리를 두고 기다림 — 최대 15초)
function Pilot:FightCaptain(Timeout)
	local GM = self.GM
	local Until = self.Time + Timeout
	local HoldUntil = nil
	while not GM.bBossDead and self.Time < Until do
		local Boss = GM:NearestEnemy(self:Pos(), 3000, function(S) return S.bBoss end)
		if not GM:IsMenuOpen() then
			local Seen = self:PatternsSeen()
			if Boss and Boss.Phase == 2 and Seen < 3 and HoldUntil == nil then
				HoldUntil = self.Time + 15
				self:Note(string.format("보스 패턴 관찰 대기 (본 패턴 %d)", Seen))
			end
			if Boss and HoldUntil and Seen < 3 and self.Time < HoldUntil then
				local L = Flat(self:Pos() - Boss.entity:GetWorldPosition()):Length()
				if L < 650 then self:MoveToward(GM.BossPos + Vector3(-600, 300, 0)) end
				if Boss.State == "ChargeWindup" and self.Player.DashCooldown <= 0 then self.In.Dash = true end
			elseif Boss then
				self:Engage(Boss, true)
			else
				self:MoveToward(GM.BossPos)
			end
			self:Survive()
		end
		self:Yield()
	end
end

function Pilot:RunHarborRun()
	local GM, P = self.GM, self.Player
	self:Wait(0.8)
	self:Expect(GM.Properties.Map == "Harbor" and GM.Menu == nil and GM.Mode == "Play" and Game.GetTimeScale() == 1, "항구 직접 시작: 타이틀 없이 바로 플레이")
	self:Expect(GM.Report.Regions >= 1 and GM:Hud():W("RegionBanner").Visible, "지역 이름 표시 (" .. tostring(GM.Region and GM.Region.DisplayName) .. ")")
	self:Expect(#GM.Slots == 8 and #GM.Npcs == 7 and #GM.Chests == 2 and GM.BossPos ~= nil and #GM.NightSlots == 3,
		string.format("항구 배치 (적 %d, 주민 %d, 상자 %d, 밤 적 자리 %d)", #GM.Slots, #GM.Npcs, #GM.Chests, #GM.NightSlots))
	self:HarborLoadout()
	GM:SetGameHour(9.0)
	self:Wait(0.3)
	self:Expect(not GM:IsNight() and GM:LampLevel() < 0.05, "아침 9시: 낮 (등불 꺼짐)")
	local Ghost = self:FindNpc("GhostSailor")
	self:Expect(Ghost ~= nil and Ghost.bHidden == true, "유령 선원은 낮에 숨어 있다")

	-- 의뢰 셋 + 선원 대화
	self:TalkToNpc("HarborMaster")
	self:TalkThrough()
	self:Expect(GM:SubState("Captain") == "Active", "서브 퀘스트 받음: 해적 선장")
	self:TalkToNpc("Sailor")
	self:TalkThrough()
	self:TalkToNpc("Fisher")
	self:TalkThrough()
	self:Expect(GM:SubState("Pirates") == "Active", "서브 퀘스트 받음: 해적 소탕")

	-- 어시장 상점 (낮): 구운 생선 + 진주 반지
	local Gold0 = GM.Gold
	self:TalkToNpc("Fishmonger")
	self:TalkThrough()
	self:Expect(GM.Menu == "Shop" and GM:Hud():W("ShopTitle").Text == "베라의 어시장", "어시장 상점 열림 (주민별 진열·제목)")
	self:SelectRow("GrilledFish")
	self:Press("Confirm")
	self:Wait(0.25)
	self:SelectRow("PearlRing")
	self:Press("Confirm")
	self:Wait(0.25)
	self:Expect(GM:Count("GrilledFish") == 1 and GM:Count("PearlRing") == 1 and GM.Gold == Gold0 - D.Item("GrilledFish").Price - D.Item("PearlRing").Price,
		"어시장 구입 (구운 생선 + 진주 반지, 골드 차감)")
	self:Expect(GM.ShopSay == D.Npc("Fishmonger").ShopLines[2], "상점 주인 말 (베라)")
	self:Press("Cancel")
	self:Wait(0.3)

	-- 등대지기 (방파제 위)
	self:TalkToNpc("Keeper")
	self:TalkThrough()
	self:Expect(GM:SubState("Lighthouse") == "Active", "서브 퀘스트 받음: 꺼진 등대")
	self:Expect(not GM.Lighthouse.bOn, "등대 불 꺼져 있음 (퀘스트 전)")

	-- 동쪽 절벽 길: 적 3종 (사냥 퀘스트도 함께)
	self:Fight("Crab", "Spear", 1, 70, true)
	self:Fight("Seagull", "Bow", 1, 70, true)
	self:Fight("Pirate", "Spear", 2, 90, true)
	self:Fight("*", "Spear", 0, 90, true, Vector3(2800, -250, 0), function() return GM:SubReady("Pirates") end)
	self:Expect(GM:SubReady("Pirates"), "해적 소탕 사냥 (구역 안 4마리)")
	self:Expect((GM.Report.Kills.Crab or 0) >= 1 and (GM.Report.Kills.Seagull or 0) >= 1 and (GM.Report.Kills.Pirate or 0) >= 1, "항구 적 3종 처치 (게·갈매기·해적)")

	-- 해적 야영지 렌즈
	local Lens = self:FindProp("Lens")
	self:Expect(Lens ~= nil and GM:IsPropActive(Lens), "렌즈 소품 보임 (퀘스트 받은 뒤)")
	self:InteractAt(Lens.Pos, "등대 렌즈")
	self:TalkThrough()
	self:Expect(GM:Count("LighthouseLens") == 1 and GM:SubReady("Lighthouse"), "등대 렌즈 주움")

	-- 동쪽 후미: 해적 선장
	self:EquipBySwitch("Spear")
	self:GoTo(Vector3(4000, 120, 0), 150, 60, "후미 입구")
	self:FightCaptain(160)
	self:Expect(GM.bBossDead and GM:IsBossDefeated("Harbor"), "해적 선장 처치")
	self:Expect(self:PatternsSeen() >= 3, "보스 패턴 3종 이상 (" .. self:PatternsSeen() .. ")")
	self:Expect((GM.Report.BossPatterns.Summon or 0) >= 1, "보스 2단계 (격노·졸개 소환)")
	self:Expect(GM:SubReady("Captain"), "해적 선장 의뢰 보고 가능")
	self:Wait(1.2)
	self:ClearArea(GM.BossPos, 900, 40, "Spear", "후미")
	self:OpenChestAt(#GM.Chests)
	self:Expect(GM.Opened["Harbor:Boss"] == true, "보스 보상 상자")

	-- 마을로 보고 셋
	self:TalkToNpc("Fisher")
	self:TalkThrough()
	self:Expect(GM:SubState("Pirates") == "Done" and GM:Count("Harpoon") == 1, "서브 퀘스트 완료: 해적 소탕 (작살)")
	self:EquipBySwitch("Harpoon")
	self:TalkToNpc("Keeper")
	self:TalkThrough()
	self:Expect(GM:SubState("Lighthouse") == "Done", "서브 퀘스트 완료: 꺼진 등대")
	self:TalkToNpc("HarborMaster")
	self:TalkThrough()
	self:Expect(GM:SubState("Captain") == "Done" and GM:Count("CompassCharm") == 1, "서브 퀘스트 완료: 해적 선장 (나침반)")
	self:EquipFromMenu("CompassCharm")

	-- 낮 → 밤: 등불·창·등대·밤 적·유령 선원·밤 대사
	local LampBefore = GM.NightLights[1] and GM.NightLights[1].Light.Intensity or -1
	GM:SetGameHour(18.7)
	self:WaitUntil(function() return GM:IsNight() end, 25)
	self:Wait(0.5)
	local LampAfter = GM.NightLights[1] and GM.NightLights[1].Light.Intensity or -1
	self:Expect(GM:IsNight() and GM:LampLevel() > 0.95 and LampAfter > LampBefore + 1, string.format("해 짐 → 밤 (%s, 등불 %.1f → %.1f)", GM:HourText(), LampBefore, LampAfter))
	self:Expect(GM.bWindowsLit == true, "창 불빛 켜짐")
	self:Expect(GM.Lighthouse.bOn and GM.Report.LighthouseOn >= 1, "등대 불 켜짐 (퀘스트 뒤 밤)")
	self:Expect(GM.Report.NightSpawns >= 3, "밤 적(망령) 나타남 (" .. GM.Report.NightSpawns .. ")")
	self:Expect(not Ghost.bHidden, "유령 선원이 밤에 나타남")
	self:TalkToNpc("GhostSailor")
	self:TalkThrough()
	local Talks0 = GM.Report.NightTalks
	self:TalkToNpc("Sailor")
	self:TalkThrough()
	self:Expect(GM.Report.NightTalks > Talks0, "선원 밤 대사")
	self:TalkToNpc("Fishmonger")
	self:Expect(GM.Menu == "Dialog", "밤: 어시장 닫힘 (대화만)")
	self:TalkThrough()
	self:Expect(GM.Menu == nil, "밤 상점 열리지 않음")
	self:GoTo(Vector3(2400, -150, 0), 200, 70, "밤 절벽 길")
	self:Fight("Ghost", "Harpoon", 1, 100, true)
	self:Expect((GM.Report.Kills.Ghost or 0) >= 1, "밤 적 망령 처치")

	-- 여관 잠 → 아침
	P.Health = math.floor(P.MaxHealth * 0.5)
	self:TalkToNpc("Innkeeper")
	self:TalkThrough()
	self:WaitUntil(function() return GM.Menu == nil and not GM:IsNight() end, 8)
	self:Wait(0.6)
	self:Expect(GM.Report.Sleeps == 1 and math.abs(GM.GameHour - D.DayNight().SleepHour) < 0.3 and P.Health >= P.MaxHealth and GM.Report.InnPaid == D.DayNight().InnPrice,
		string.format("여관 잠 → 아침 %s, 체력 %d/%d, 숙박비 %d", GM:HourText(), P.Health, P.MaxHealth, GM.Report.InnPaid or 0))
	self:Expect(self:AliveOf("Ghost") == 0 and Ghost.bHidden, "아침: 망령·유령 선원 사라짐")
	self:Expect(not GM.Lighthouse.bOn and GM.bWindowsLit == false, "아침: 등대·창 불 꺼짐")

	-- 언덕길 트리거 → 메인 맵
	self:HandOff("Arrive")
	Game.SetPersistent("HD2D_AutoScenario", "Harbor")
	local Trigger = Scene.Find("HartRoad_Travel")
	local Target = Trigger:GetWorldPosition()
	local Route = self:Route(self:Pos(), Target)
	local Index, Until = 1, self.Time + 80
	while GM.Mode ~= "Travel" and self.Time < Until do
		local WP = Route[math.min(Index, #Route)]
		if Index < #Route and Flat(WP - self:Pos()):Length() < 90 then Index = Index + 1 end
		if not GM:IsMenuOpen() then self:MoveToward(WP) end
		self:Survive()
		self:Yield()
	end
	self:Expect(GM.Mode == "Travel", "언덕길 → 맵 이동 시작")
	Game.SetPersistent("HD2D_AutoExpect", GM:StateSignature())
	Game.SetPersistent("HD2D_AutoHour", GM.GameHour)
	Game.SetPersistent("HD2D_AutoFailures", table.concat(self.Failures, "|"))
	Game.SetPersistent("HD2D_AutoChecks", self.Checks)
	Game.SetPersistent("HD2D_AutoTime", self.Time)
	self:Idle()
end

function Pilot:RunHarborArrive()
	local GM = self.GM
	self:Wait(0.8)
	self:Expect(GM.Properties.Map == "Village" and GM.Menu == nil and GM.Mode == "Play", "메인 맵 도착: 타이틀 없음")
	local Spawn = Scene.Find("Spawn_Harbor")
	self:Expect(Spawn ~= nil and Flat(self:Pos() - Spawn:GetWorldPosition()):Length() < 150, "도착 자리 = Spawn_Harbor (남쪽 시냇가 길)")
	local Expect = Game.GetPersistent("HD2D_AutoExpect", "")
	local Now = GM:StateSignature()
	if not self:Expect(Now == Expect, "세션 상태 유지 (항구 → 마을)") then
		self:Note("기대 " .. Expect)
		self:Note("실제 " .. Now)
	end
	self:Expect(GM:SubState("Lighthouse") == "Done" and GM:SubState("Pirates") == "Done" and GM:SubState("Captain") == "Done" and GM:IsBossDefeated("Harbor"),
		"항구 퀘스트·보스 진행 유지")
	local Hour = Game.GetPersistent("HD2D_AutoHour", -1)
	self:Expect(math.abs(GM.GameHour - Hour) < 0.25, string.format("게임 시각 유지 (%.2f → %.2f)", Hour, GM.GameHour))
	self:Expect(GM.Report.Regions >= 1 and GM.Region and GM.Region.DisplayName == "하르트 마을", "지역 이름 표시 (하르트 마을)")
	self:Expect(#GM.NightWindows > 0 and #GM.NightLights > 0, string.format("메인 맵 낮밤 대상 (등불 %d, 창 %d)", #GM.NightLights, #GM.NightWindows))
	self:Finish()
	self:Idle()
end

-- ================================================================ 스크린샷·측정용
function Pilot:RunHarborDay()
	self:Wait(0.3)
	self:HarborLoadout()
	self.GM:SetGameHour(10.5)
	self:Note("항구 한낮에서 대기")
	self:Idle()
end

function Pilot:RunHarborNight()
	self:Wait(0.3)
	self:HarborLoadout()
	self.GM.Sub.Lighthouse = { State = "Done", Count = 0 }
	self.GM:SetGameHour(21.5)
	self:Note("항구 밤에서 대기")
	self:Idle()
end

function Pilot:RunHarborLighthouse()
	self:Wait(0.3)
	self:HarborLoadout()
	self.GM.Sub.Lighthouse = { State = "Done", Count = 0 }
	self.GM:SetGameHour(20.5)
	self:Note("등대 밤에서 대기")
	self:Idle()
end

function Pilot:RunHarborCombat()
	-- 절벽 길 전투가 이어지게 (측정용): 둘레 적이 셋보다 적으면 항구 적을 돌아가며 더 부른다
	self:Wait(0.3)
	self:HarborLoadout()
	self.GM:SetGameHour(11.0)
	self:EquipBySwitch("Spear")
	local Kinds, Next, SpawnTimer = { "Crab", "Pirate", "Seagull" }, 1, 0
	local Home = self:Pos()
	while true do
		local GM = self.GM
		SpawnTimer = SpawnTimer - (self.GameDt or 0)
		local Near = 0
		for _, S in ipairs(GM:AliveEnemies()) do
			if not S.bBoss and Flat(S.entity:GetWorldPosition() - Home):Length() < 1300 then Near = Near + 1 end
		end
		if Near < 3 and SpawnTimer <= 0 then
			local A = Next * 2.1
			GM:SpawnEnemy(Kinds[(Next - 1) % #Kinds + 1], Vector3(Home.X + math.cos(A) * 450, Home.Y - 250 + math.sin(A) * 200, Home.Z + 20))
			Next, SpawnTimer = Next + 1, 0.6
		end
		local Target = GM:NearestEnemy(self:Pos(), 1500, function(S) return not S.bBoss end)
		if Target and not GM:IsMenuOpen() then self:Engage(Target, true) end
		if self.Player.Health < self.Player.MaxHealth * 0.5 then self.Player.Health = self.Player.MaxHealth end
		self:Yield()
	end
end

function Pilot:RunHarborBoss()
	self:Wait(0.3)
	self:HarborLoadout()
	self.GM:SetGameHour(15.0)
	self:EquipBySwitch("Spear")
	while true do
		local Boss = self.GM:NearestEnemy(self:Pos(), 3000, function(S) return S.bBoss end)
		if not self.GM:IsMenuOpen() then
			if Boss then
				if Boss.Health < Boss.Row.MaxHealth * 0.3 then Boss.Health = Boss.Row.MaxHealth * 0.45 end
				self:Engage(Boss, true)
			else
				self:MoveToward(self.GM.BossPos)
			end
			if self.Player.Health < self.Player.MaxHealth * 0.5 then self.Player.Health = self.Player.MaxHealth end
		end
		self:Yield()
	end
end

function Pilot:RunVillageNight()
	self:Wait(0.3)
	self.GM:SetGameHour(21.0)
	self:Note("메인 맵 밤에서 대기")
	self:Idle()
end

function Pilot:RunVillageDay()
	self:Wait(0.3)
	self.GM:SetGameHour(11.0)
	self:Note("메인 맵 낮에서 대기")
	self:Idle()
end

return Pilot
