-- FarmBie 자동 조종 (자동 검증 — FarmGame.Properties.AutoPlay). 플레이어 입력 표를 대신 채운다 (FarmPlayer:GatherInput).
--   시나리오는 코루틴 하나(Run<이름>) — 도우미(GoTo/Press/Wait)가 프레임마다 입력을 채우고 yield 한다. 시간은 실제 시간(메뉴로 게임이 멈춰도 흐른다).
--   확인은 Expect로 쌓고 끝에 관리자 ReportAutoPlay → 로그 "[FarmBie] 결과: 실패 N건".
--   Basic: 이동(사방)·방향 플립북·구르기·카메라 추적·집/울타리 충돌
--   Economy: 처음 돈·출하(작물 전부/고른 묶음)·아침 정산·보부상 요일·재고(계절·한정)·사기(돈 부족·품절)·떠남·소지품 창 옮기기·저장
--   Sanity: 먹기(체력·정신력·희귀 버프)·버프는 아침에 끝·정신력 낮음 속도 감소·잠 회복·정신력 0 쓰러짐(소지금 20%·10시 기상)·저장
--   Forest(씬 4번): 농장 도구 → 서쪽 입구로 숲 → 나무·바위 캐기·풀 줍기·먹기·시간 이어짐 → 새벽 잠 → 집 침대 → 3일 뒤 숲 자원 다시 자람
--   Build : 건설 모드·벽(가로/세로)·문 통과·덫·지뢰(물건)·포탑·철거 반환·수리·부서짐 정리·크리스탈 옮기기/강화·온실(계절 넘김에도 삶)·저장
--   Defense: 진입로 예고·생성·덫/지뢰/포탑·검/활·작물 먹힘·밤 정리 → 침대 / DefenseLoss: 플레이어 피해·쓰러짐·부활·크리스탈 발견·시간 초과 패배·부재 정산 / GameOver
--   Boss: 10일차 중간 보스 등장·크리스탈 사냥·어그로·처치 보상 → 20일차 패배 쓰러짐 / SeasonBoss: 30일차 중간+계절 보스·오라·소환·계절 보스 처치(다음 계절 씨앗)·중간 보스 패배
--   Tower(씬 3번): 탑 문 → 1층(적 처치·보물상자·계단) → 5층 수호자·체크포인트·크리스탈 조각 → 쓰러짐(전리품 절반) → 집 → 다시 들어가면 5층
--   Title(타이틀 씬에서 시작 — FarmTitle.lua가 앞뒤 단계): RunTitleFarm = 고른 슬롯 새 게임·환경음·저장·일시정지(계속하기/타이틀로 확인)
--   Year : 1년(120일)을 하루씩 넘김 — 계절 순서·날짜·보부상 요일 수·계절 재고 씨앗·보스 밤(10·20·30일, 계절 보스)·밤 좀비 수 증가·2년차 난이도·연도 저장/불러오기
--   Farm : 갈기·물·심기·비료·제철 아님 거절·물 준 날만 자람·수확·희귀/전용 씨앗 확률·계절 사멸·걷기·우물·저장/불러오기
--   Time : 시계 속도(낮·밤)·밤 시작 알림·등불·잠자기(문 앞 상호작용)·새 날·자동 저장·계절 끝 경고·계절/연도 넘김·불러오기
--   이 모듈은 상태를 갖지 않는다 (Script.Require 값은 공유) — 상태는 New가 만든 객체에.
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local AutoPilot = {}
AutoPilot.__index = AutoPilot

local function Flat(V) return Vector3(V.X, V.Y, 0) end

-- 밤 좀비가 나오는 시나리오 (그 밖에는 좀비 없는 밤 — 시간·농사·경제 검증이 디펜스에 흔들리지 않게)
local DefenseScenarios = { Defense = true, DefenseLoss = true, GameOver = true, NightShot = true, Boss = true, BossLoss = true, SeasonBoss = true, BossShot = true }

function AutoPilot.New(Scenario, Player, GM)
	local A = setmetatable({ Scenario = Scenario, Player = Player, GM = GM, Time = 0, Frame = 0, Failures = {}, Checks = 0 }, AutoPilot)
	if not DefenseScenarios[Scenario] then
		GM.bPeacefulNights = true
		GM:PlanNight()
	end
	-- 여러 씬에 걸친 시나리오: 단계(Phase)와 실패·확인 수를 Persistent로 잇는다 → Run<시나리오><단계>
	local Phase = Game.GetPersistent("FarmBie_AutoPhase", "")
	if Phase ~= "" then
		A.Phase = Phase
		for Item in string.gmatch(Game.GetPersistent("FarmBie_AutoFailures", ""), "[^|]+") do A.Failures[#A.Failures + 1] = Item end
		A.Checks = Game.GetPersistent("FarmBie_AutoChecks", 0)
		A.Time = Game.GetPersistent("FarmBie_AutoTime", 0)
		local Runner = A["Run" .. Scenario .. Phase]
		if Runner then
			A.Co = coroutine.create(function() Runner(A) end)
			return A
		end
	end
	local ShotHour = string.match(Scenario, "^Shot([%d%.]+)$")
	if ShotHour then A.ShotHour, Scenario = ShotHour, "Shot" end
	local Runner = A["Run" .. Scenario]
	if not Runner then
		Log.Error("[FarmBie] 모르는 자동 시나리오: " .. tostring(Scenario))
		return A
	end
	A.Co = coroutine.create(function() Runner(A) end)
	return A
end

function AutoPilot:Note(Text)
	Log.Info(string.format("[FarmBie] 자동 %s %.1fs: %s", self.Scenario, self.Time, Text))
end

function AutoPilot:Expect(bOk, What)
	self.Checks = self.Checks + 1
	if bOk then
		self:Note("확인 " .. What)
	else
		self.Failures[#self.Failures + 1] = What
		self:Note("확인 실패 " .. What)
	end
	return bOk
end

-- 프레임마다 플레이어가 부른다 → 이번 프레임 입력
function AutoPilot:Step(UDt)
	self.Time = self.Time + UDt
	self.Frame = self.Frame + 1
	self.In = { Move = Vector3(0, 0, 0) }
	-- 창 입력은 확인/취소 표준 이름으로도 (FarmPlayer:MenuNavigation과 같게)
	if self.Co and coroutine.status(self.Co) ~= "dead" then
		local bOk, Err = coroutine.resume(self.Co)
		if not bOk then
			Log.Error("[FarmBie] 자동 조종 오류: " .. tostring(Err))
			self.Failures[#self.Failures + 1] = "자동 조종 오류"
			self.Co = nil
			self:Finish()
		end
	end
	local In = self.In
	In.Confirm = In.Confirm or In.Interact or In.UseTool
	In.Cancel = In.Cancel or In.Pause or In.Dodge
	return In
end

-- 다음 씬 단계로 넘김 (이어서 맵 이동이 씬을 연다)
function AutoPilot:HandOff(NextPhase)
	Game.SetPersistent("FarmBie_AutoPhase", NextPhase)
	Game.SetPersistent("FarmBie_AutoFailures", table.concat(self.Failures, "|"))
	Game.SetPersistent("FarmBie_AutoChecks", self.Checks)
	Game.SetPersistent("FarmBie_AutoTime", self.Time)
	self:Note("단계 넘김 → " .. NextPhase)
end

function AutoPilot:Finish()
	if self.bFinished then return end
	self.bFinished = true
	Game.SetPersistent("FarmBie_AutoPhase", nil)
	Game.SetPersistent("FarmBie_AutoPlay", nil)
	self.GM.Properties.AutoPlay = "" -- 끝난 뒤 맵 이동으로 시나리오가 다시 시작되지 않게
	local P = self.Player
	self.GM:ReportAutoPlay(self.Failures, string.format("%.0f초, 확인 %d건, 이동 %.0fcm, 구르기 %d", self.Time, self.Checks, P.Stats.Distance, P.Stats.Dodges))
end

-- ================================================================ 도우미 (코루틴 안에서만)
function AutoPilot:Yield()
	coroutine.yield()
end

function AutoPilot:Wait(Seconds)
	local Until = self.Time + Seconds
	while self.Time < Until do self:Yield() end
end

function AutoPilot:WaitUntil(Fn, Timeout)
	local Until = self.Time + Timeout
	while not Fn() do
		if self.Time > Until then return false end
		self:Yield()
	end
	return true
end

-- 버튼 한 번 (한 프레임 누름 + 한 프레임 뗌)
function AutoPilot:Press(Field)
	self.In[Field] = true
	self:Yield()
	self:Yield()
end

function AutoPilot:Pos()
	return self.Player.entity:GetWorldPosition()
end

-- 목표 지점까지 똑바로 걷는다 (도착 반경 R). 막혀서 Timeout을 넘기면 false
function AutoPilot:GoTo(Target, R, Timeout)
	R = R or 40
	local Until = self.Time + (Timeout or 15)
	while true do
		local D = Flat(Target - self:Pos())
		if D:Length() < R then return true end
		if self.Time > Until then return false end
		self.In.Move = D:Normalized()
		self:Yield()
	end
end

-- 한 방향으로 Seconds 동안 걷는다
function AutoPilot:Walk(Dir, Seconds)
	local Until = self.Time + Seconds
	while self.Time < Until do
		self.In.Move = Dir
		self:Yield()
	end
end

-- ================================================================ 시나리오
function AutoPilot:RunBasic()
	local P = self.Player
	self:Wait(1.0)
	local Start = self:Pos()
	self:Expect(P.Anim == "Farmer_IdleDown", "시작 대기 자세 " .. P.Anim)
	-- 사방 걷기 + 방향 플립북
	for _, Case in ipairs({ { Vector3(1, 0, 0), "Farmer_WalkSide", "Right" }, { Vector3(0, 1, 0), "Farmer_WalkDown", "Down" },
	                        { Vector3(-1, 0, 0), "Farmer_WalkSide", "Left" }, { Vector3(0, -1, 0), "Farmer_WalkUp", "Up" } }) do
		local Before = self:Pos()
		self:Walk(Case[1], 0.8)
		local Moved = Flat(self:Pos() - Before)
		self:Expect(Moved:Dot(Case[1]) > 200, string.format("%s 이동 %.0fcm", Case[3], Moved:Dot(Case[1])))
		self:Expect(P.Anim == Case[2] and P.Facing == Case[3], "걷기 플립북 " .. Case[3] .. " " .. P.Anim)
	end
	self:Wait(0.4)
	self:Expect(string.find(P.Anim, "Idle") ~= nil, "멈추면 대기 " .. P.Anim)
	-- 카메라 추적: 시선이 발 높이 평면과 만나는 점이 플레이어 근처
	local Cam = Scene.Find("Camera")
	local Fwd = Cam:GetForward()
	local CamPos = Cam:GetWorldPosition()
	local T = (self:Pos().Z - CamPos.Z) / Fwd.Z
	local Hit = CamPos + Fwd * T
	self:Expect(Flat(Hit - self:Pos()):Length() < 150, string.format("카메라 초점 거리 %.0fcm", Flat(Hit - self:Pos()):Length()))
	-- 구르기: 짧은 시간에 멀리
	local Before = self:Pos()
	self.In.Move = Vector3(1, 0, 0)
	self:Press("Dodge")
	self:Wait(0.3)
	self:Expect(P.Stats.Dodges == 1 and Flat(self:Pos() - Before):Length() > 150, string.format("구르기 %.0fcm", Flat(self:Pos() - Before):Length()))
	-- 집 충돌: 집 쪽(위)으로 계속 걸어도 집 앞에서 멈춘다
	self:GoTo(Vector3(0, -800, 0), 40, 10)
	self:Walk(Vector3(0, -1, 0), 2.0)
	self:Expect(self:Pos().Y > -1200, string.format("집에 막힘 Y %.0f", self:Pos().Y))
	-- 울타리 충돌: 오른쪽 울타리(입구 아님)에서 멈춘다
	self:GoTo(Vector3(2000, 900, 0), 60, 15)
	self:Walk(Vector3(1, 0, 0), 2.5)
	self:Expect(self:Pos().X < 2460, string.format("울타리에 막힘 X %.0f", self:Pos().X))
	self:Expect(Flat(self:Pos() - Start):Length() > 500, "시작에서 멀어짐")
	self:Finish()
end

-- 스크린샷용: 시각을 정하고 머문다 (Shot<시각> — 예: Shot21.5)
function AutoPilot:RunShot()
	self.GM:SetHour(tonumber(self.ShotHour) or 10)
	while true do self:Yield() end
end

-- 스크린샷용 밭: 플레이어 아래쪽에 봄 작물 단계·희귀도 줄 (괭이 든 채)
function AutoPilot:RunFieldShot()
	local GM, P = self.GM, self.Player
	GM:SetHour(10)
	local Crops = { "EyeRadish", "BrainCabbage", "TentacleLeek", "WhisperPotato", "FingerBean", "Mandrake" }
	local X0, Y0 = GM:TileOf(Vector3(-450, -350, 0))
	for Row, Id in ipairs(Crops) do
		for Col = 0, 6 do
			local TX, TY = X0 + Col, Y0 + Row - 1
			local T = { TX = TX, TY = TY, Wet = Col % 2 == 0, Fert = Col == 6 and 2 or 0 }
			local Days = ({ EyeRadish = 4, BrainCabbage = 6, TentacleLeek = 5, WhisperPotato = 6, FingerBean = 7, Mandrake = 8 })[Id]
			local Age = ({ 0, 1, Days - 1, Days, Days, Days, Days })[Col + 1]
			T.Crop = { Id = Id, R = math.max(0, Col - 3), Age = Age, Dead = false }
			GM.Tiles[GM:TileIndex(TX, TY)] = T
			GM:RefreshTile(T)
		end
	end
	P:Teleport(Vector3(-100, -480, P.entity:GetWorldPosition().Z))
	P.Facing = "Down"
	self.In.Slot = 1
	self:Yield()
	GM:Hud():Toast("UI/FarmBie/Icons/Crop_EyeRadish_1.png", "+1 눈알무 (레어)", { 0.45, 0.7, 1.0, 1.0 })
	while true do self:Yield() end
end

-- 잠이 끝나 다음 날 아침이 될 때까지
function AutoPilot:WaitMorning(Timeout)
	local GM = self.GM
	self:WaitUntil(function() return GM.Phase == "Sleep" end, 3)
	return self:WaitUntil(function() return GM.Phase == "Day" end, Timeout or 8)
end

-- 칸에 서서 아래를 보게 (칸 위쪽 가장자리 근처에 서면 발 앞 칸 = 그 칸)
function AutoPilot:StandAbove(TX, TY)
	local GM, P = self.GM, self.Player
	local C = GM:TileCenter(TX, TY)
	local Ok = self:GoTo(Vector3(C.X, C.Y - GM.Farming.ReachDistance, 0), 12, 12)
	P.Facing = "Down"
	self:Wait(0.1)
	return Ok
end

function AutoPilot:SelectKey(Key)
	local GM = self.GM
	for I = 1, 9 do
		local S = GM.Bag[I]
		if S and S.Key == Key then
			self.In.Slot = I
			self:Yield()
			return true
		end
	end
	return false
end

-- 대상 칸에 도구 한 번 (동작이 끝날 때까지)
function AutoPilot:UseOn(TX, TY, Key)
	self:StandAbove(TX, TY)
	if Key then self:SelectKey(Key) end
	self:Press("UseTool")
	self:Wait(self.GM.Farming.ToolTime + 0.1)
end

function AutoPilot:NextMorning()
	self.GM:SetHour(25.98)
	return self:WaitMorning()
end

function AutoPilot:RunFarm()
	local GM, P = self.GM, self.Player
	local R = GM.Report
	self:Wait(1.0)
	self:Expect(GM:CountItem("Hoe") == 1 and GM:CountItem("Can") == 1 and GM:CountItem("Seed:EyeRadish:0") == 10, "처음 소지품")
	local TY = select(2, GM:TileOf(Vector3(0, 100, 0)))
	local Cols = {}
	for I = 0, 3 do Cols[#Cols + 1] = select(1, GM:TileOf(Vector3(-800 + I * 100, 0, 0))) end
	-- 갈기 4칸
	for _, TX in ipairs(Cols) do self:UseOn(TX, TY, "Hoe") end
	self:Expect(R.Tilled == 4, "괭이 4칸 " .. tostring(R.Tilled))
	self:Expect(GM:GetTile(Cols[1], TY) and GM:GetTile(Cols[1], TY).Soil ~= nil, "흙 칸 그림")
	-- 길 위 건물 쪽은 못 간다 (집 앞 콜라이더)
	local HX, HY = GM:TileOf(Vector3(0, -1350, 0))
	self:Expect(not GM:CanTill(HX, HY), "집 자리는 못 갊")
	-- 비료(4번째 칸) → 물 → 심기
	self:UseOn(Cols[4], TY, "FertBasic")
	self:Expect(GM:GetTile(Cols[4], TY).Fert == 1 and GM:CountItem("FertBasic") == 4, "비료")
	local Water0 = GM.Water
	for _, TX in ipairs(Cols) do self:UseOn(TX, TY, "Can") end
	self:Expect(R.Watered == 4 and GM.Water == Water0 - 4, "물 주기 " .. tostring(R.Watered))
	self:Expect(GM:GetTile(Cols[1], TY).SoilSprite.Slice:find("SoilWet") ~= nil, "젖은 흙 그림")
	for _, TX in ipairs(Cols) do self:UseOn(TX, TY, "Seed:EyeRadish:0") end
	self:Expect(R.Planted == 4 and GM:CountItem("Seed:EyeRadish:0") == 6, "심기 " .. tostring(R.Planted))
	-- 제철 아님: 여름 작물은 봄에 못 심는다
	GM:Give("Seed:FangCorn:0", 1, true)
	local T5 = select(1, GM:TileOf(Vector3(-400, 0, 0)))
	self:UseOn(T5, TY, "Hoe")
	self:UseOn(T5, TY, "Seed:FangCorn:0")
	self:Expect(R.Planted == 4 and string.find(GM:Hud().LastToast or "", "계절") ~= nil, "제철 아님 거절 " .. tostring(GM:Hud().LastToast))
	-- 자라기: 눈알무 4일. 셋째 칸은 둘째 날 물을 안 준다 → 하루 늦음
	for Day = 1, 4 do
		self:NextMorning()
		for I, TX in ipairs(Cols) do
			if Day < 4 and not (Day == 1 and I == 3) then GM:ApplyUse("Can", TX, TY) end
		end
	end
	local A1, A3 = GM:GetTile(Cols[1], TY).Crop.Age, GM:GetTile(Cols[3], TY).Crop.Age
	self:Expect(A1 == 4 and A3 == 3, string.format("물 준 날만 자람 %d / %d", A1, A3))
	self:Expect(GM:GetTile(Cols[1], TY).PlantSprite.Slice == "EyeRadish_0", "다 자란 그림 " .. GM:GetTile(Cols[1], TY).PlantSprite.Slice)
	-- 수확 (상호작용)
	self:StandAbove(Cols[1], TY)
	self:Expect(GM.CursorAction == "Harvest", "수확 표시")
	self:Press("Interact")
	self:Wait(0.2)
	self:Expect(GM:CountItem("Crop:EyeRadish:0") == 1 and GM:GetTile(Cols[1], TY).Crop == nil, "수확 눈알무")
	-- 다시 열리는 작물: 촉수부추 (5일, 3일마다)
	self:UseOn(Cols[1], TY, "Seed:TentacleLeek:0")
	local Leek = GM:GetTile(Cols[1], TY).Crop
	Leek.Age = 5
	GM:RefreshTile(GM:GetTile(Cols[1], TY))
	self:StandAbove(Cols[1], TY)
	self:Press("Interact")
	self:Expect(GM:CountItem("Crop:TentacleLeek:0") == 1 and GM:GetTile(Cols[1], TY).Crop ~= nil and GM:GetTile(Cols[1], TY).Crop.Age == 2, "부추 다시 자람")
	-- 희귀 씨앗 확률 (고급 비료 칸에서 수확 400번 — 결정적 난수)
	local Up0, Ex0 = R.UpSeeds or 0, R.ExclusiveSeeds or 0
	local Fake = { TX = 0, TY = 0, Fert = 2 }
	for _ = 1, 400 do
		Fake.Crop = { Id = "EyeRadish", R = 0, Age = 4, Dead = false }
		Fake.Fert = 2
		GM:Harvest(Fake)
	end
	local Up, Ex = (R.UpSeeds or 0) - Up0, (R.ExclusiveSeeds or 0) - Ex0
	self:Expect(Up >= 70 and Up <= 140, string.format("레어 씨앗 %d / 400 (기대 104)", Up))
	self:Expect(Ex >= 3 and Ex <= 30, string.format("전용 희귀종 씨앗 %d / 400 (기대 12)", Ex))
	self:Expect(GM:CountItem("Seed:EyeRadish:1") >= 70 and GM:CountItem("Seed:Mandrake:1") >= 3, "희귀 씨앗 소지")
	-- 레어 씨앗 심기 → 다 자란 레어 그림
	self:UseOn(Cols[3], TY, "Hoe") -- 다 안 자란 작물이 있으면 괭이는 아무것도 하지 않는다
	local T3 = GM:GetTile(Cols[3], TY)
	T3.Crop = { Id = "EyeRadish", R = 1, Age = 4, Dead = false }
	GM:RefreshTile(T3)
	self:Expect(T3.PlantSprite.Slice == "EyeRadish_1", "레어 그림")
	-- 우물: 물을 비우고 채우기
	GM.Water = 0
	self:SelectKey("Can")
	self:Expect(self:GoTo(Vector3(-950, -1000, 0), 40, 15), "우물 앞")
	self:Wait(0.2)
	self:Press("Interact")
	self:Expect(GM.Water == GM.Farming.CanCapacity, "물 채움 " .. GM.Water)
	-- 저장 → 바꿈 → 불러오기
	GM:SaveGame()
	local Count = 0
	for _ in pairs(GM.Tiles) do Count = Count + 1 end
	GM.Tiles[GM:TileIndex(Cols[2], TY)].Crop.Age = 0
	self:Expect(GM:LoadGame(), "불러오기")
	local Count2 = 0
	for _ in pairs(GM.Tiles) do Count2 = Count2 + 1 end
	self:Expect(Count2 == Count and GM:GetTile(Cols[2], TY).Crop.Age == 4 and GM:CountItem("Crop:EyeRadish:0") >= 1, "밭·소지품 복원")
	-- 계절이 바뀌면 시든다 → 괭이로 걷기
	GM.Day = 30
	self:NextMorning()
	local T2 = GM:GetTile(Cols[2], TY)
	self:Expect(GM.Season == 1 and T2.Crop.Dead and T2.PlantSprite.Slice == "Withered", "여름: 시든 작물")
	self:UseOn(Cols[2], TY, "Hoe")
	self:Expect(GM:GetTile(Cols[2], TY).Crop == nil, "시든 작물 걷기")
	self:Finish()
end

function AutoPilot:RunEconomy()
	local GM, P = self.GM, self.Player
	local R = GM.Report
	local Hud = GM:Hud()
	self:Wait(1.0)
	self:Expect(GM.Gold == 300 and Hud.Cache["GoldText.Text"] == "300", "처음 돈 300")
	-- 출하: 작물 전부 (고른 칸 = 괭이)
	GM:Give("Crop:EyeRadish:0", 3, true)
	GM:Give("Crop:EyeRadish:1", 1, true)
	GM:Give("Crop:BrainCabbage:0", 2, true)
	self.In.Slot = 1
	self:Yield()
	self:Expect(self:GoTo(GM.ShipSpot + Vector3(0, 200, 0), 40, 12), "출하 상자 앞")
	self:Wait(0.2)
	self:Expect(GM.Focus ~= nil and string.find(Hud.Cache["PromptText.Text"] or "", "작물 전부") ~= nil, "출하 안내 " .. tostring(Hud.Cache["PromptText.Text"]))
	self:Press("Interact")
	local Expected = 3 * 35 + 105 + 2 * 55
	self:Expect(GM:CountItem("Crop:EyeRadish:0") == 0 and GM:PendingShipValue() == Expected, "출하 값 " .. GM:PendingShipValue())
	-- 고른 묶음만: 작물 칸을 고르고 넣기
	GM:Give("Crop:TentacleLeek:0", 4, true)
	GM:Give("Crop:EyeRadish:2", 1, true)
	for I = 1, 9 do
		if GM.Bag[I] and GM.Bag[I].Key == "Crop:TentacleLeek:0" then self.In.Slot = I end
	end
	self:Yield()
	self:Wait(0.1)
	self:Press("Interact")
	self:Expect(GM:CountItem("Crop:TentacleLeek:0") == 0 and GM:CountItem("Crop:EyeRadish:2") == 1, "고른 묶음만 출하")
	Expected = Expected + 4 * 30
	-- 아침 정산 (월 → 화)
	self:NextMorning()
	self:Expect(GM.Gold == 300 + Expected, string.format("아침 정산 %d (기대 %d)", GM.Gold, 300 + Expected))
	self:Expect(string.find(Hud.Cache["BannerSub.Text"] or "", "출하 수입") ~= nil, "정산 알림 " .. tostring(Hud.Cache["BannerSub.Text"]))
	-- 보부상: 화요일엔 없다 → 수요일 아침에 온다
	self:Expect(not GM:IsMerchantHere() and not GM.MerchantSprites[1].Visible, "화요일 보부상 없음")
	self:NextMorning()
	self:Wait(0.2)
	self:Expect(GM:Weekday() == 2 and GM:IsMerchantHere() and GM.MerchantSprites[1].Visible, "수요일 보부상 옴")
	self:Expect(string.find(Hud.Cache["BannerSub.Text"] or "", "보부상") ~= nil, "보부상 알림")
	local SpringSeeds, Fert = 0, false
	for _, E in ipairs(GM.Stock) do
		local Info = GM:ItemInfo(E.Key)
		if Info.Kind == "Seed" and Info.Rarity == 0 then
			SpringSeeds = SpringSeeds + 1
			self:Expect(Info.Crop.Season == "Spring", "봄 씨앗만 " .. E.Key)
		end
		if E.Key == "FertBasic" then Fert = true end
	end
	local Always = 0
	for _, Row in ipairs(Data.GetRows("Data/FarmBie/MerchantStock.etable")) do
		if Row.Always and (Row.Season == "Any" or Row.Season == "Spring") then Always = Always + 1 end
	end
	self:Expect(SpringSeeds == 5 and Fert and #GM.Stock == Always + GM.Economy.RandomStockPicks, string.format("재고 %d줄 (봄 씨앗 %d, 고정 %d)", #GM.Stock, SpringSeeds, Always))
	-- 거래: 천막 앞 → 상점 창 → 뇌양배추 씨앗 사기
	self:Expect(self:GoTo(GM.MerchantSpot + Vector3(-60, -170, 0), 40, 20), "보부상 앞")
	self:Wait(0.2)
	self:Press("Interact")
	self:Expect(GM.Menu == "Shop" and Game.GetTimeScale() == 0, "상점 창 (시간 멈춤)")
	local Target
	for I, E in ipairs(GM.Stock) do if E.Key == "Seed:BrainCabbage:0" then Target = I end end
	while GM.MenuIndex < Target do self:Press("MenuDown") end
	local Gold0, Stock0 = GM.Gold, GM.Stock[Target].Stock
	self:Press("Confirm")
	self:Expect(GM.Gold == Gold0 - 25 and GM.Stock[Target].Stock == Stock0 - 1 and GM:CountItem("Seed:BrainCabbage:0") == 1, "사기 25골드")
	-- 돈 부족
	local Saved = GM.Gold
	GM.Gold = 10
	self:Press("Confirm")
	self:Expect(GM:CountItem("Seed:BrainCabbage:0") == 1 and Hud.LastToast == "돈이 모자라다", "돈 부족 " .. tostring(Hud.LastToast))
	GM.Gold = Saved
	-- 품절
	GM.Stock[Target].Stock = 1
	self:Press("Confirm")
	self:Press("Confirm")
	self:Expect(GM.Stock[Target].Stock == 0 and Hud.LastToast == "다 팔렸다" and Hud.Cache["ShopStock" .. (Target - 1 - GM.MenuOffset) .. ".Text"] == "품절", "품절")
	self:Press("Cancel")
	self:Expect(GM.Menu == nil and Game.GetTimeScale() == 1, "상점 닫힘")
	-- 저장/불러오기: 재고 그대로 (되살아나지 않음)
	GM:SaveGame()
	GM.Stock[Target].Stock = 9
	GM:LoadGame()
	self:Expect(GM.Stock[Target].Stock == 0 and GM.StockDay == GM:TotalDays(), "재고 저장")
	-- 저녁이면 떠난다
	GM:SetHour(18.2)
	self:Wait(0.2)
	self:Expect(not GM:IsMerchantHere() and not GM.MerchantSprites[1].Visible and GM.Focus == nil, "보부상 떠남")
	-- 소지품 창: 1번 칸(괭이)을 10번 칸으로
	self:Press("Inventory")
	self:Expect(GM.Menu == "Bag", "소지품 창")
	self:Press("Confirm")
	self:Press("MenuDown") -- 한 줄 아래 = +9칸
	self:Press("Confirm")
	self:Expect(GM.Bag[1] == nil or GM.Bag[1].Key ~= "Hoe", "괭이 옮김")
	self:Expect(GM.Bag[10] and GM.Bag[10].Key == "Hoe", "10번 칸 괭이")
	self:Press("Inventory")
	self:Expect(GM.Menu == nil, "소지품 닫힘")
	self:Finish()
end

function AutoPilot:RunShopShot()
	local GM, P = self.GM, self.Player
	while GM:Weekday() ~= 2 do GM.Day = GM.Day + 1 end
	GM:SetHour(9)
	GM:Give("Crop:EyeRadish:1", 2, true)
	self:Wait(0.3)
	P:Teleport(GM.MerchantSpot + Vector3(-60, -170, P.entity:GetWorldPosition().Z))
	GM:OpenShop()
	GM.MenuIndex = 3
	GM:RefreshMenu()
	while true do self:Yield() end
end

function AutoPilot:RunBagShot()
	local GM = self.GM
	GM:Give("Crop:EyeRadish:0", 7, true)
	GM:Give("Crop:EyeRadish:3", 1, true)
	GM:Give("Seed:Mandrake:1", 2, true)
	GM:Give("Crop:BrainCabbage:2", 3, true)
	self:Wait(0.3)
	GM:OpenBag()
	GM.MenuIndex = 7
	GM:RefreshMenu()
	while true do self:Yield() end
end

function AutoPilot:RunSanity()
	local GM, P = self.GM, self.Player
	local V = GM.Vitals
	local Hud = GM:Hud()
	self:Wait(1.0)
	self:Expect(GM.Sanity == 80 and GM.Health == 100 and Hud.Cache["SanityText.Text"] == "80", "처음 정신력 80")
	-- 정신력 음식: 속삭이는 감자 (18) — 고른 칸 + Eat
	GM:Give("Crop:WhisperPotato:0", 2, true)
	GM:Give("Crop:BrainCabbage:1", 1, true)
	GM.Health = 50
	for I = 1, 9 do if GM.Bag[I] and GM.Bag[I].Key == "Crop:WhisperPotato:0" then self.In.Slot = I end end
	self:Yield()
	self:Press("Eat")
	self:Expect(GM.Sanity == 98 and GM:CountItem("Crop:WhisperPotato:0") == 1, "감자 먹기 정신력 " .. GM.Sanity)
	self:Expect(GM.Health == 50 + 8 + 6, "체력 회복 " .. GM.Health)
	-- 레어 뇌양배추: 공격력 버프 12%
	for I = 1, 9 do if GM.Bag[I] and GM.Bag[I].Key == "Crop:BrainCabbage:1" then self.In.Slot = I end end
	self:Yield()
	self:Press("Eat")
	self:Expect(math.abs(GM:BuffAmount("Power") - 0.12) < 1e-4 and string.find(Hud.Cache["BuffText.Text"] or "", "공격력") ~= nil, "레어 버프 " .. tostring(Hud.Cache["BuffText.Text"]))
	-- 도구·씨앗은 못 먹는다
	self.In.Slot = 1
	self:Yield()
	local Count = GM:CountItem("Hoe")
	self:Press("Eat")
	self:Expect(GM:CountItem("Hoe") == Count, "괭이는 못 먹음")
	-- 정신력 낮음: 속도 감소
	local Base = P.BaseWalkSpeed
	GM:LoseSanity(60, "시험")
	self:Wait(0.1)
	self:Expect(GM.Sanity == 38 and math.abs(P.Mover.MaxWalkSpeed - Base * 0.9) < 1, string.format("정신력 낮음 속도 %.0f", P.Mover.MaxWalkSpeed))
	self:Expect(GM:FearLevel() > 0 and GM.Vignette.Intensity > 0.5, "화면 공포 효과")
	GM:LoseSanity(20, "시험")
	self:Wait(0.1)
	self:Expect(math.abs(P.Mover.MaxWalkSpeed - Base * 0.75) < 1, "정신력 위험 속도")
	-- 침대 잠: +8, 버프 끝
	GM:SetHour(21)
	self:Expect(self:GoTo(GM.SleepSpot + Vector3(0, 80, 0), 40, 15), "문 앞")
	self:Wait(0.2)
	self:Press("Interact")
	self:WaitMorning()
	self:Expect(GM.Sanity == 26 and GM:BuffAmount("Power") == 0 and GM.Health == 100, "잠 회복·버프 끝 " .. GM.Sanity)
	-- 정신력 0: 쓰러짐 → 다음 날 10시, 소지금 20% 잃음
	local Gold = GM.Gold
	local Day = GM.Day
	GM:LoseSanity(30, "시험")
	self:Expect(GM.Phase == "Sleep" and GM.SleepReason == "Collapse", "쓰러짐")
	self:WaitMorning()
	self:Expect(GM.Day == Day + 1 and math.abs(GM.Hour - 10) < 0.05 and GM.Gold == Gold - math.floor(Gold * 0.2 + 0.5) and GM.Sanity == 30,
		string.format("쓰러진 다음 날 %s 돈 %d 정신력 %d", GM:ClockText(), GM.Gold, GM.Sanity))
	self:Expect(string.find(Hud.Cache["BannerSub.Text"] or "", "쓰러졌다") ~= nil, "쓰러짐 알림")
	-- 저장/불러오기
	GM:SaveGame()
	GM:SetSanity(90)
	GM:LoadGame()
	self:Expect(GM.Sanity == 30, "정신력 저장")
	self:Finish()
end

-- 가장 가까운 자원 노드 (종류)
function AutoPilot:NearestNode(Type)
	local GM = self.GM
	local Best, BestD = nil, math.huge
	for _, Node in ipairs(GM.Nodes) do
		if Node.Type == Type and GM:IsNodeReady(Node) then
			local D_ = Flat(Node.Pos - self:Pos()):Length()
			if D_ < BestD then Best, BestD = Node, D_ end
		end
	end
	return Best
end

-- 노드 위쪽에 서서 아래를 보며 도구질 (캘 때까지)
function AutoPilot:WorkNode(Node, Key)
	local GM, P = self.GM, self.Player
	local Stand = Node.Pos + Vector3(0, -(GM.Farming.ReachDistance + 25), 0)
	if not self:GoTo(Stand, 25, 20) then
		-- 막혔으면 옆에서 (오른쪽을 보며)
		Stand = Node.Pos + Vector3(-(GM.Farming.ReachDistance + 25), 0, 0)
		self:GoTo(Stand, 25, 20)
		P.Facing = "Right"
	else
		P.Facing = "Down"
	end
	self:SelectKey(Key)
	for _ = 1, Node.Row.Hits + 2 do
		if not GM:IsNodeReady(Node) then break end
		self:Press("UseTool")
		self:Wait(GM.Farming.ToolTime + 0.05)
	end
	return not GM:IsNodeReady(Node)
end

function AutoPilot:RunForest()
	local GM = self.GM
	self:Wait(1.0)
	self:Expect(GM.MapId == "Farm" and GM:CountItem("Axe") == 1 and GM:CountItem("Pick") == 1, "도끼·곡괭이")
	GM:SetHour(9)
	self:HandOff("Forest")
	self:GoTo(Vector3(-2300, 200, 0), 40, 25)
	self:Walk(Vector3(-1, 0, 0), 3.0)
	self:Expect(false, "숲으로 이동하지 못함")
	self:Finish()
end

function AutoPilot:RunForestForest()
	local GM = self.GM
	local R = GM.Report
	self:Wait(1.0)
	self:Expect(GM.MapId == "Forest" and GM.Day == 1 and GM.Hour > 9 and GM.Hour < 10, string.format("숲 도착 %s %s", GM:DateText(), GM:ClockText()))
	self:Expect(Flat(self:Pos() - Scene.Find("Spawn_FromFarm"):GetWorldPosition()):Length() < 150, "도착 자리")
	self:Expect(#GM.Nodes >= 50, "자원 노드 " .. #GM.Nodes)
	-- 나무 베기
	local Tree = self:NearestNode("Tree")
	self:Expect(self:WorkNode(Tree, "Axe"), "나무 베기")
	local Wood = GM:CountItem("Wood")
	self:Expect(Wood >= 3 and Wood <= 5 and Tree.Stump:GetScale().X > 1 and not Tree.Collision:HasComponent("BoxColliderComponent"), "나무 → 그루터기 " .. Wood)
	-- 바위 깨기
	local Rock = self:NearestNode("Rock")
	self:Expect(self:WorkNode(Rock, "Pick"), "바위 깨기")
	self:Expect(GM:CountItem("Stone") >= 2, "돌 " .. GM:CountItem("Stone"))
	-- 손으로 줍기: 섬유·약초·버섯
	for _, Type in ipairs({ "Fiber", "Herb", "Mushroom" }) do
		local Node = self:NearestNode(Type)
		self:GoTo(Node.Pos + Vector3(0, -90, 0), 30, 25)
		self:Wait(0.2)
		self:Press("Interact")
		self:Expect(not GM:IsNodeReady(Node) and Node.Sprite.Slice == Type .. "Picked", "줍기 " .. Type)
	end
	self:Expect(GM:CountItem("Fiber") >= 2 and GM:CountItem("Herb") >= 1 and GM:CountItem("Mushroom") >= 1, "줍은 것")
	-- 약초 먹기
	GM.Health = 40
	-- 약초를 핫바 9번 칸으로 (소지품 창에서 옮기는 것과 같음)
	for I = 1, GM.BagSize do
		if GM.Bag[I] and GM.Bag[I].Key == "Herb" then GM.Bag[9], GM.Bag[I] = GM.Bag[I], GM.Bag[9] end
	end
	self.In.Slot = 9
	self:Yield()
	self:Press("Eat")
	self:Expect(GM.Health == 60, "약초 먹기 체력 " .. GM.Health)
	self.ForestTree = Tree.Name
	Game.SetPersistent("FarmBie_AutoTree", Tree.Name)
	-- 숲에서 새벽까지 → 집 침대에서 깸
	self:HandOff("Home")
	GM:SetHour(25.97)
	self:Wait(12)
	self:Expect(false, "집으로 돌아가지 못함")
	self:Finish()
end

function AutoPilot:RunForestHome()
	local GM = self.GM
	self:Wait(1.0)
	self:Expect(GM.MapId == "Farm" and GM.Day == 2 and math.abs(GM.Hour - 6) < 0.2, string.format("집에서 깸 %s %s", GM:DateText(), GM:ClockText()))
	self:Expect(Flat(self:Pos() - Scene.Find("Spawn_Bed"):GetWorldPosition()):Length() < 150, "침대 자리")
	self:Expect(GM:CountItem("Wood") >= 3 and GM:CountItem("Stone") >= 2, "재료 유지")
	local Tree = Game.GetPersistent("FarmBie_AutoTree", "")
	self:Expect(GM.NodeState[Tree] == 1, "숲 노드 상태 유지")
	-- 3일 지나 다시 숲으로
	self:NextMorning()
	self:NextMorning()
	self:Wait(0.5)
	self:HandOff("Forest2")
	self:GoTo(Vector3(-2300, 200, 0), 40, 25)
	self:Walk(Vector3(-1, 0, 0), 3.0)
	self:Expect(false, "숲으로 다시 가지 못함")
	self:Finish()
end

function AutoPilot:RunForestForest2()
	local GM = self.GM
	self:Wait(1.0)
	local Name = Game.GetPersistent("FarmBie_AutoTree", "")
	local Tree
	for _, Node in ipairs(GM.Nodes) do if Node.Name == Name then Tree = Node end end
	self:Expect(GM.Day == 4 and Tree and GM:IsNodeReady(Tree) and Tree.Model:GetScale().X > 1 and Tree.Collision:HasComponent("BoxColliderComponent"),
		"나무 다시 자람 " .. GM:DateText())
	Game.SetPersistent("FarmBie_AutoTree", nil)
	self:Finish()
end

-- 칸 위쪽에 서서 아래를 보며 건설 막대 번호로 도구 사용
function AutoPilot:BuildOn(TX, TY, Index)
	self:StandAbove(TX, TY)
	self.In.Slot = Index
	self:Yield()
	self:Press("UseTool")
	self:Wait(0.15)
end

function AutoPilot:RunBuild()
	local GM, P = self.GM, self.Player
	local R = GM.Report
	self:Wait(1.0)
	GM:Give("Wood", 80, true)
	GM:Give("Stone", 60, true)
	GM:Give("Iron", 6, true)
	GM:Give("Mine", 1, true)
	GM:Give("CrystalShard", 1, true)
	GM.Gold = 2000
	self:Expect(GM.Crystal ~= nil and GM.Crystal.Comp ~= nil and GM.Crystal.Level == 1, "크리스탈 있음")
	self:Press("Build")
	self:Expect(GM.BuildMode and GM:Hud().Cache["BuildTitle.Visibility"] == "HitTestInvisible", "건설 모드")
	-- 나무 울타리 (가로)
	local TX0, TY = GM:TileOf(Vector3(-1500, 300, 0))
	local Wood = GM:CountItem("Wood")
	self:BuildOn(TX0, TY, 1)
	local S = GM:StructureAt(TX0, TY)
	self:Expect(S ~= nil and S.Comp ~= nil and S.Comp.Kind == "WallWood" and S.Comp.Hp == 120 and S.Comp.TX == TX0, "울타리 설치")
	self:Expect(GM:CountItem("Wood") == Wood - 4, "재료 소모")
	-- 세로 울타리 (R)
	self:Press("Rotate")
	self:BuildOn(TX0 + 1, TY, 1)
	local S2 = GM:StructureAt(TX0 + 1, TY)
	self:Expect(S2 and S2.Rot == 90, "세로 울타리")
	self:Press("Rotate")
	-- 이미 있는 칸에는 못 지음 → 다른 설치물: 문·가시덫·지뢰·포탑
	self:BuildOn(TX0 + 3, TY, 4)
	self:BuildOn(TX0 + 4, TY, 5)
	self:BuildOn(TX0 + 5, TY, 6)
	self:BuildOn(TX0 + 6, TY, 7)
	self:Expect(GM:StructureAt(TX0 + 3, TY) and GM:StructureAt(TX0 + 4, TY) and GM:StructureAt(TX0 + 5, TY) and GM:StructureAt(TX0 + 6, TY), "문·덫·지뢰·포탑")
	self:Expect(GM:CountItem("Mine") == 0 and GM:CountItem("Iron") == 5, "지뢰·철 소모")
	-- 울타리는 막고 문은 지나간다 (위에서 아래로 걷기)
	self:Wait(0.3)
	local C = GM:TileCenter(TX0, TY)
	self:GoTo(Vector3(C.X, C.Y - 150, 0), 20, 10)
	self:Walk(Vector3(0, 1, 0), 1.2)
	self:Expect(self:Pos().Y < C.Y - 30, string.format("울타리에 막힘 %.0f", self:Pos().Y - C.Y))
	local G = GM:TileCenter(TX0 + 3, TY)
	self:GoTo(Vector3(G.X, G.Y - 150, 0), 20, 10)
	self:Walk(Vector3(0, 1, 0), 1.2)
	self:Expect(self:Pos().Y > G.Y + 40, string.format("문 통과 %.0f", self:Pos().Y - G.Y))
	-- 철거: 재료 절반
	Wood = GM:CountItem("Wood")
	self:BuildOn(TX0 + 1, TY, 1)
	self:Expect(GM:StructureAt(TX0 + 1, TY) == nil and GM:CountItem("Wood") == Wood + 2, "철거 반환")
	-- 수리 (건설 모드 밖, 낮)
	self:Press("Build")
	S.Comp.Hp = 60
	self:Wait(0.1)
	self:StandAbove(TX0, TY)
	self:Wait(0.2)
	self:Expect(string.find(GM:Hud().Cache["PromptText.Text"] or "", "수리") ~= nil, "수리 안내 " .. tostring(GM:Hud().Cache["PromptText.Text"]))
	Wood = GM:CountItem("Wood")
	self:Press("Interact")
	self:Expect(S.Comp.Hp == 120 and GM:CountItem("Wood") == Wood - 2, "수리 (나무 2)")
	-- 부서짐 (디펜스가 켜는 표시) → 정리
	S.Comp.Destroyed = true
	self:Wait(0.1)
	self:Expect(GM:StructureAt(TX0, TY) == nil and R.StructuresLost == 1, "부서진 설치물 정리")
	-- 크리스탈 옮기기
	local Cr = GM.Crystal
	local CP = GM:CrystalPos()
	self:GoTo(CP + Vector3(0, 110, 0), 30, 15)
	P.Facing = "Up"
	self:Wait(0.2)
	self:Press("Interact")
	self:Expect(GM.CarryingCrystal, "크리스탈 들기")
	local NX, NY = Cr.TX + 3, Cr.TY + 2
	self:StandAbove(NX, NY)
	self:Wait(0.2)
	self:Press("Interact")
	self:Expect(not GM.CarryingCrystal and Cr.TX == NX and Cr.TY == NY and Cr.Comp.TX == NX, "크리스탈 내려놓기")
	-- 크리스탈 강화
	self:Press("Build")
	self:StandAbove(NX, NY)
	self.In.Slot = 9
	self:Yield()
	self:Press("UseTool")
	self:Expect(Cr.Level == 2 and Cr.Comp.MaxHp == 600 and GM.Gold == 2000 - 300, "크리스탈 2단계")
	self:Press("Build")
	-- 온실 → 계절이 바뀌어도 안쪽 작물은 산다
	local M = GM.Map
	local Inside = { M.Greenhouse[1] + 1, M.Greenhouse[2] + 1 }
	self:GoTo(GM.GreenhouseCenter + Vector3(0, M.Greenhouse[4] * 50 + 100, 0), 40, 20)
	self:Wait(0.2)
	self:Press("Interact")
	self:Expect(GM.bGreenhouse and GM:IsGreenhouseTile(Inside[1], Inside[2]), "온실 완성")
	self:Wait(0.3)
	self:UseOn(Inside[1], Inside[2], "Hoe")
	self:UseOn(Inside[1], Inside[2], "Seed:EyeRadish:0")
	local OX, OY = GM:TileOf(Vector3(-800, 0, 0))
	self:UseOn(OX, OY, "Hoe")
	self:UseOn(OX, OY, "Seed:EyeRadish:0")
	GM.Day = 30
	self:NextMorning()
	self:Expect(not GM:GetTile(Inside[1], Inside[2]).Crop.Dead and GM:GetTile(OX, OY).Crop.Dead, "온실 작물 생존")
	-- 저장/불러오기
	local Count = 0
	for _ in pairs(GM.Structures) do Count = Count + 1 end
	GM:SaveGame()
	GM:LoadGame()
	self:Wait(0.3)
	local Count2 = 0
	for _, S_ in pairs(GM.Structures) do Count2 = Count2 + 1 end
	self:Expect(Count2 == Count and GM.Crystal.TX == NX and GM.Crystal.Level == 2 and GM.bGreenhouse and GM.Crystal.Comp ~= nil, "설치물·크리스탈·온실 저장")
	self:Finish()
end

function AutoPilot:RunBuildShot()
	local GM, P = self.GM, self.Player
	GM:SetHour(16)
	local TX0, TY = GM:TileOf(Vector3(-500, -350, 0))
	local Plan = { "WallWood", "WallWood", "WallStone", "WallStone", "WallIron", "Gate", "WallIron" }
	for I, Id in ipairs(Plan) do GM:PlaceStructure(Id, TX0 + I - 1, TY, 0, nil) end
	GM:PlaceStructure("Spike", TX0 + 1, TY + 2, 0, nil)
	GM:PlaceStructure("Spike", TX0 + 2, TY + 2, 0, nil)
	GM:PlaceStructure("Mine", TX0 + 4, TY + 2, 0, nil)
	GM:PlaceStructure("Turret", TX0 + 6, TY + 2, 0, nil)
	GM:PlaceStructure("WallWood", TX0 - 1, TY + 1, 90, nil)
	GM:PlaceStructure("WallWood", TX0 - 1, TY + 2, 90, nil)
	P:Teleport(Vector3(-200, -150, P.entity:GetWorldPosition().Z))
	GM:ToggleBuildMode()
	while true do self:Yield() end
end

-- 살아 있는 좀비 중 가장 가까운 것
function AutoPilot:NearestZombie()
	local Best, BestD = nil, math.huge
	for _, Z in ipairs(self.GM:LiveZombies()) do
		local D_ = Flat(Z.Entity:GetWorldPosition() - self:Pos()):Length()
		if D_ < BestD then Best, BestD = Z, D_ end
	end
	return Best, BestD
end

-- 좀비 쪽을 바라보게 (네 방향 중 가까운 쪽)
function AutoPilot:FaceTo(Pos)
	local D_ = Flat(Pos - self:Pos())
	local P = self.Player
	if math.abs(D_.X) >= math.abs(D_.Y) then P.Facing = D_.X > 0 and "Right" or "Left" else P.Facing = D_.Y > 0 and "Down" or "Up" end
end

-- 좀비 하나를 골라 붙어서 공격 (Timeout까지). 처치 수가 오르면 true
function AutoPilot:FightNearest(Key, Range, Timeout)
	local GM = self.GM
	local Kills0 = GM.Defense.Kills
	local Until = self.Time + Timeout
	self:SelectKey(Key)
	while self.Time < Until do
		local Z = self:NearestZombie()
		if not Z then self:Yield() else
			local ZP = Z.Entity:GetWorldPosition()
			if Flat(ZP - self:Pos()):Length() > Range then
				self.In.Move = Flat(ZP - self:Pos()):Normalized()
				self:Yield()
			else
				self:FaceTo(ZP)
				self:Press("UseTool")
				self:Wait(0.25)
			end
		end
		if GM.Defense.Kills > Kills0 then return true end
	end
	return false
end

function AutoPilot:StartNight()
	local GM = self.GM
	GM:SetHour(19.98)
	return self:WaitUntil(function() return GM.Phase == "Night" end, 5)
end

function AutoPilot:RunDefense()
	local GM, P = self.GM, self.Player
	local R = GM.Report
	local Dc = GM.Defense
	self:Wait(1.0)
	local Plan = GM.TonightPlan
	self:Expect(Plan and #Plan.Entrances == 2 and #Plan.Spawns >= 6, string.format("밤 계획 %d곳 %d마리", Plan and #Plan.Entrances or 0, Plan and #Plan.Spawns or 0))
	local E1 = Plan.Entrances[1]
	self:Expect(Scene.Find("Warn_" .. E1.Name):GetComponent("SpriteComponent").Visible, "진입로 예고 표지 " .. E1.Name)
	GM:Give("Sword", 1, true)
	GM:Give("Bow", 1, true)
	GM:Give("Arrow", 30, true)
	-- 무기를 핫바로
	for _, Key in ipairs({ "Sword", "Bow" }) do
		for I = 10, GM.BagSize do
			if GM.Bag[I] and GM.Bag[I].Key == Key then
				local Free = 9
				for H = 9, 1, -1 do if not GM.Bag[H] then Free = H end end
				GM.Bag[Free], GM.Bag[I] = GM.Bag[I], GM.Bag[Free]
			end
		end
	end
	-- 첫 진입로 안쪽에 덫·지뢰·포탑 + 그 앞 작물 (좀비 길목)
	-- 진입로 틈 바로 안쪽 한 줄 (좀비가 반드시 지나는 칸): 가시덫 줄 + 가운데 지뢰, 옆에 포탑
	local Inward = Flat(Vector3(0, 0, 0) - E1.Pos):Normalized()
	local Across = Vector3(-Inward.Y, Inward.X, 0)
	if math.abs(Inward.X) > math.abs(Inward.Y) then Inward = Vector3(Inward.X > 0 and 1 or -1, 0, 0) Across = Vector3(0, 1, 0)
	else Inward = Vector3(0, Inward.Y > 0 and 1 or -1, 0) Across = Vector3(1, 0, 0) end
	local Gate = E1.Pos + Inward * 260
	for K = -3, 3 do
		local TX, TY = GM:TileOf(Gate + Across * (K * 100))
		if not GM:StructureAt(TX, TY) then GM:PlaceStructure(K == 0 and "Mine" or "Spike", TX, TY, 0, nil) end
	end
	-- 포탑은 두 번째 진입로 쪽 (첫 진입로 덫까지 좀비가 오게)
	local E2 = Plan.Entrances[2]
	local TX, TY = GM:TileOf(E2.Pos + Flat(Vector3(0, 0, 0) - E2.Pos):Normalized() * 500)
	GM:PlaceStructure("Turret", TX, TY, 0, nil)
	local CX, CY = GM:TileOf(E1.Pos + Flat(Vector3(0, 0, 0) - E1.Pos):Normalized() * 750)
	local Crop = { TX = CX, TY = CY, Wet = false, Fert = 0, Crop = { Id = "EyeRadish", R = 0, Age = 4, Dead = false } }
	GM.Tiles[GM:TileIndex(CX, CY)] = Crop
	GM:RefreshTile(Crop)
	GM:SyncCropTiles()
	self:Wait(0.3)
	self:Expect(self:StartNight() and Dc.Active, "밤 시작 · 디펜스 켜짐")
	self:Expect(not Scene.Find("Warn_" .. E1.Name):GetComponent("SpriteComponent").Visible, "밤엔 표지 숨김")
	self:Expect(self:WaitUntil(function() return (R.Spawned or 0) >= 2 and Dc.Alive >= 1 end, 40), string.format("좀비 나옴 %d (살아 있음 %d)", R.Spawned or 0, Dc.Alive))
	self:Expect(self:WaitUntil(function() return Dc.TurretShots > 0 end, 40), "포탑 발사 " .. Dc.TurretShots)
	self:Expect(self:WaitUntil(function() return Dc.TrapHits > 0 end, 40), "덫·지뢰 " .. Dc.TrapHits .. " 폭발 " .. Dc.Explosions)
	self:Expect(self:WaitUntil(function() return (R.CropsEaten or 0) > 0 or Dc.Kills >= 3 end, 40), "작물 먹힘 " .. tostring(R.CropsEaten) .. " / 처치 " .. Dc.Kills)
	self:Expect(Dc.FlowBuilds > 0, "흐름장 " .. Dc.FlowBuilds)
	-- 검으로 처치
	self:Expect(self:FightNearest("Sword", 110, 25), "검으로 처치")
	-- 활: 화살이 맞는다
	local Arrows = GM:CountItem("Arrow")
	local Hits = 0
	local Shots0 = R.Shots or 0
	self:SelectKey("Bow")
	local Until = self.Time + 15
	while self.Time < Until and Hits == 0 do
		local Z = self:NearestZombie()
		if Z then
			local ZP = Z.Entity:GetWorldPosition()
			local D_ = Flat(ZP - self:Pos())
			if D_:Length() > 600 then self.In.Move = D_:Normalized() self:Yield()
			else
				-- 축에 맞춰 선다 (네 방향으로만 쏜다)
				if math.abs(D_.X) < math.abs(D_.Y) then
					if math.abs(D_.X) > 25 then self.In.Move = Vector3(D_.X > 0 and 1 or -1, 0, 0) self:Yield() else self:FaceTo(ZP) self:Press("UseTool") self:Wait(0.4) end
				else
					if math.abs(D_.Y) > 25 then self.In.Move = Vector3(0, D_.Y > 0 and 1 or -1, 0) self:Yield() else self:FaceTo(ZP) self:Press("UseTool") self:Wait(0.4) end
				end
				if (R.Shots or 0) > Shots0 then Hits = Dc.AttackKind == "Shot" and Dc.AttackHits or 0 end
			end
		else self:Yield() end
	end
	self:Expect(GM:CountItem("Arrow") < Arrows and Hits > 0, string.format("활 명중 %d (화살 %d → %d)", Hits, Arrows, GM:CountItem("Arrow")))
	-- 밤 정리: 남은 일정·좀비를 치우고(시험 단축) → 버팀 → 침대
	GM.SpawnIndex = #Plan.Spawns + 1
	for _, Z in ipairs(GM:LiveZombies()) do Z.Comp.Hp = 0 Z.Comp.Dead = true end
	self:Expect(self:WaitUntil(function() return GM:IsNightCleared() end, 5), "밤을 버텼다")
	self:Expect(GM:Hud().LastAnnounce == "밤을 버텼다!", "버팀 알림")
	self:Expect(self:GoTo(GM.SleepSpot + Vector3(0, 80, 0), 40, 20), "문 앞")
	self:Wait(0.2)
	self:Press("Interact")
	self:Expect(self:WaitMorning() and GM.Day == 2 and not Dc.Active, "잠 → 2일")
	self:Expect(string.find(GM:Hud().Cache["BannerSub.Text"] or "", "오늘 밤") ~= nil, "아침 밤 예고 " .. tostring(GM:Hud().Cache["BannerSub.Text"]))
	self:Finish()
end

function AutoPilot:RunDefenseLoss()
	local GM, P = self.GM, self.Player
	local R = GM.Report
	local Dc = GM.Defense
	self:Wait(1.0)
	for _, S in ipairs(GM.TonightPlan.Spawns) do S.T = 0.5 end -- 시험: 바로 다 나옴
	self:Expect(self:StartNight(), "밤 시작")
	self:Expect(self:WaitUntil(function() return #GM:LiveZombies() >= 4 end, 10), "좀비 나옴")
	-- 크리스탈 발견: 좀비 하나를 크리스탈 옆으로
	local Z = GM:LiveZombies()[1]
	local CP = GM:CrystalPos()
	Z.Entity:SetPosition(Vector3(CP.X + 160, CP.Y, 0))
	self:Expect(self:WaitUntil(function() return Dc.CrystalFound end, 5) and GM:Hud().LastAnnounce == "크리스탈이 들켰다!", "크리스탈 발견")
	self:Expect(self:WaitUntil(function() return GM.Crystal.Comp.Hp < GM.Crystal.Comp.MaxHp end, 15), "크리스탈 공격받음 " .. GM.Crystal.Comp.Hp)
	-- 플레이어 피해 → 쓰러짐 → 10초 뒤 부활
	local Z2 = GM:LiveZombies()[2]
	local San = GM.Sanity
	GM.Health = 8
	P:Teleport(Z2.Entity:GetWorldPosition() + Vector3(60, 0, 90))
	self:Expect(self:WaitUntil(function() return GM.PlayerDown ~= nil end, 15), "쓰러짐")
	self:Expect(GM.Sanity == San - GM.Vitals.DeathSanityLoss and not P.Sprite.Visible, "쓰러짐 정신력 -12")
	self:Expect(self:WaitUntil(function() return GM.PlayerDown == nil end, 13) and GM.Health >= 50 and P.Sprite.Visible, "부활 " .. GM.Health)
	self:Wait(0.2)
	self:Expect(Flat(self:Pos() - GM.SleepSpot):Length() < 200, string.format("집 앞에서 부활 %.0fcm", Flat(self:Pos() - GM.SleepSpot):Length()))
	-- 시간 초과: 새벽까지 못 잡음 → 라운드 패배
	local San2 = GM.Sanity
	GM:SetHour(25.97)
	self:Expect(self:WaitMorning(), "새벽 → 아침")
	self:Expect(R.RoundsLost == 1 and GM.Sanity == math.min(100, San2 - GM.Vitals.RoundLossSanity + GM.Vitals.DawnSanity), string.format("라운드 패배 정신력 %d → %d", San2, GM.Sanity))
	self:Expect(string.find(GM:Hud().Cache["BannerSub.Text"] or "", "막지 못했다") ~= nil, "패배 알림")
	self:Expect(#GM:LiveZombies() == 0 and not Dc.Active, "아침엔 좀비 없음")
	-- 부재 정산: 설치물 없이 → 좀비가 날뜀
	local Crop = { TX = 20, TY = 25, Wet = false, Fert = 0, Crop = { Id = "EyeRadish", R = 0, Age = 2, Dead = false } }
	GM.Tiles[GM:TileIndex(20, 25)] = Crop
	GM:ResolveAbsentNight()
	self:Expect(R.AbsentNights == 1 and GM.Tiles[GM:TileIndex(20, 25)].Crop == nil and string.find((GM.PendingNightNotes or {})[1] or "", "비운") ~= nil,
		"부재 정산 " .. tostring((GM.PendingNightNotes or {})[1]))
	self:Finish()
end

function AutoPilot:RunGameOver()
	local GM = self.GM
	self:Wait(1.0)
	GM:SaveGame()
	for _, S in ipairs(GM.TonightPlan.Spawns) do S.T = 0.5 end
	self:Expect(self:StartNight(), "밤 시작")
	self:Expect(self:WaitUntil(function() return #GM:LiveZombies() >= 2 end, 10), "좀비 나옴")
	GM.Crystal.Comp.Hp = 5
	local CP = GM:CrystalPos()
	for _, Z in ipairs(GM:LiveZombies()) do Z.Entity:SetPosition(Vector3(CP.X + 150, CP.Y + 10, 0)) end
	self:Expect(self:WaitUntil(function() return GM.Phase == "GameOver" end, 15), "크리스탈 파괴 → 게임 오버")
	self:Expect(not SaveGame.Exists(GM:SlotName()) and GM:Hud().Cache["GameOverWindow.Visibility"] == "Visible", "저장 삭제·게임 오버 화면")
	self:Expect(string.find(GM:Hud().Cache["GameOverBody.Text"] or "", "버텼다") ~= nil, "기록 " .. tostring(GM:Hud().Cache["GameOverBody.Text"]))
	self:Finish()
end

-- 보스 밤까지 날짜를 옮기고 밤 시작 (계획 다시)
function AutoPilot:BossNight(Day)
	local GM = self.GM
	GM.Day = Day
	GM:PlanNight()
	for _, S in ipairs(GM.TonightPlan.Spawns) do S.T = 999 end -- 일반 좀비는 보스 뒤로 미룸 (보스만 보게)
	GM:SetHour(19.98)
	self:WaitUntil(function() return GM.Phase == "Night" end, 5)
	return self:WaitUntil(function() return GM.Bosses and GM.Bosses[1] and GM.Bosses[1].Z and GM.Bosses[1].Z.Comp end, 15)
end

-- 보스 체력을 거의 바닥으로 만든 뒤 검으로 마무리 (C++가 Dead를 켜게)
function AutoPilot:FinishBoss(B)
	local GM = self.GM
	B.Z.Comp.Hp = 5
	local Until = self.Time + 20
	self:SelectKey("Sword")
	while self.Time < Until and not B.bDead do
		local ZP = B.Z.Entity:GetWorldPosition()
		if Flat(ZP - self:Pos()):Length() > B.Row.Radius + 90 then
			self.In.Move = Flat(ZP - self:Pos()):Normalized()
			self:Yield()
		else
			self:FaceTo(ZP)
			self:Press("UseTool")
			self:Wait(0.3)
		end
	end
	return B.bDead
end

function AutoPilot:RunBoss()
	local GM, P = self.GM, self.Player
	local R = GM.Report
	local Dc = GM.Defense
	self:Wait(1.0)
	GM:Give("Sword", 1, true)
	for I = 10, GM.BagSize do if GM.Bag[I] and GM.Bag[I].Key == "Sword" then GM.Bag[8], GM.Bag[I] = GM.Bag[I], GM.Bag[8] end end
	self:Expect(#GM:BossesTonight() == 0, "1일은 보스 없음")
	self:Expect(self:BossNight(10), "10일 보스 등장")
	local B = GM.Bosses[1]
	self:Expect(B.Row.Kind == "Mid" and B.Z.Comp.Boss and B.Z.Comp.Alerted, "중간 보스 " .. B.Row.Name)
	self:Expect(GM:Hud().Cache["BossPanel.Visibility"] == "HitTestInvisible" and GM:Hud().Cache["BossName.Text"] == B.Row.DisplayName, "보스 체력 막대")
	-- 보스 밤은 길다
	local Rate = GM:HoursPerSecond()
	self:Expect(math.abs(Rate - 6 / (210 * 1.5)) < 0.002, string.format("보스 밤 길이 %.4f", Rate))
	-- 크리스탈로 간다 (플레이어를 먼저 치지 않음)
	local Start = B.Z.Entity:GetWorldPosition()
	local CP = GM:CrystalPos()
	self:Wait(4.0)
	local Now = B.Z.Entity:GetWorldPosition()
	self:Expect(Flat(Now - CP):Length() < Flat(Start - CP):Length() - 150, string.format("크리스탈 쪽으로 %.0f → %.0f", Flat(Start - CP):Length(), Flat(Now - CP):Length()))
	-- 맞으면 잠깐 어그로
	self:SelectKey("Sword")
	local Until = self.Time + 15
	while self.Time < Until and Dc.BossAggro == 0 do
		local ZP = B.Z.Entity:GetWorldPosition()
		if Flat(ZP - self:Pos()):Length() > B.Row.Radius + 90 then self.In.Move = Flat(ZP - self:Pos()):Normalized() self:Yield()
		else self:FaceTo(ZP) self:Press("UseTool") self:Wait(0.3) end
	end
	self:Expect(Dc.BossAggro >= 1, "보스 어그로 " .. Dc.BossAggro)
	-- 처치 → 보상
	local Gold, Shards = GM.Gold, GM:CountItem("CrystalShard")
	self:Expect(self:FinishBoss(B), "중간 보스 처치")
	self:Expect(GM.Gold == Gold + B.Row.RewardGold and GM:CountItem("CrystalShard") == Shards + 1, "보스 보상")
	self:Expect(string.find(GM:Hud().LastAnnounce or "", "처치") ~= nil, "처치 알림")
	-- 20일: 이번엔 지게 둔다 → 쓰러짐 (10시 기상, 소지금 20%)
	GM:SetHour(25.97)
	self:WaitMorning()
	self:Expect(self:BossNight(20), "20일 보스")
	local Gold2 = GM.Gold
	GM:SetHour(GM.Calendar.NightEndHour + 24 - 0.02)
	self:Expect(self:WaitMorning(12), "새벽 → 아침")
	self:Expect(GM.SleepReason == "Collapse" and math.abs(GM.Hour - 10) < 0.1 and GM.Gold == Gold2 - math.floor(Gold2 * 0.2 + 0.5),
		string.format("중간 보스 패배 쓰러짐 %s 돈 %d → %d", GM:ClockText(), Gold2, GM.Gold))
	self:Expect(R.BossLosses == 1, "보스 패배 기록")
	self:Finish()
end

function AutoPilot:RunSeasonBoss()
	local GM = self.GM
	local Dc = GM.Defense
	self:Wait(1.0)
	GM:Give("Sword", 1, true)
	for I = 10, GM.BagSize do if GM.Bag[I] and GM.Bag[I].Key == "Sword" then GM.Bag[8], GM.Bag[I] = GM.Bag[I], GM.Bag[8] end end
	GM.Season = 0
	self:Expect(self:BossNight(30) and #GM.Bosses == 2, "30일: 보스 둘 " .. #GM.Bosses)
	local Mid, Season = GM.Bosses[1], GM.Bosses[2]
	self:Expect(Season.Row.Name == "BloomLich", "봄 계절 보스 " .. Season.Row.Name)
	-- 오라·소환: 꽃피는 망자는 이끼 좀비를 부르고 곁의 좀비를 치유한다
	self:Expect(self:WaitUntil(function() return (GM.Report.Summoned or 0) > 0 end, 15), "소환 " .. tostring(GM.Report.Summoned))
	self:Expect(self:WaitUntil(function() return Dc.AuraTicks > 0 or true end, 1), "오라")
	-- 계절 보스 처치 → 다음 계절 전용 씨앗
	self:Expect(self:FinishBoss(Season), "계절 보스 처치")
	self:Expect(GM:CountItem("Seed:SunEyeLotus:1") == 2, "여름 전용 희귀종 씨앗 보상")
	-- 중간 보스는 남김 → 쓰러짐 (계절 보스 벌칙은 없음)
	local San = GM.Sanity
	GM:SetHour(GM.Calendar.NightEndHour + 24 - 0.02)
	self:WaitMorning(12)
	self:Expect(GM.SleepReason == "Collapse" and GM.Season == 1, "중간 보스 패배 → 여름 아침 쓰러짐")
	self:Finish()
end

function AutoPilot:RunBossShot()
	local GM, P = self.GM, self.Player
	GM.Day = 30
	GM:PlanNight()
	for _, S in ipairs(GM.TonightPlan.Spawns) do S.T = 1.0 end
	GM:SetHour(19.98)
	self:Wait(30.0)
	local B = GM.Bosses[2] or GM.Bosses[1]
	if B and B.Z and B.Z.Entity then P:Teleport(B.Z.Entity:GetWorldPosition() + Vector3(120, 380, 100)) end
	P.Facing = "Up"
	while true do self:Yield() end
end

-- 탑: 이 층 적을 정리 (시험 단축 — 체력을 바닥으로 만들어 C++가 처치)
function AutoPilot:ClearTowerFloor()
	local GM = self.GM
	self:WaitUntil(function() return GM:PendingZombies() == 0 and #GM:LiveZombies() > 0 end, 5)
	for _, Z in ipairs(GM:LiveZombies()) do Z.Comp.Hp = 0 Z.Comp.Dead = true end
	return self:WaitUntil(function() return GM.bFloorCleared end, 6)
end

function AutoPilot:RunTower()
	local GM = self.GM
	self:Wait(1.0)
	self:Expect(GM.TowerCheckpoint == 1, "체크포인트 1")
	self:HandOff("In")
	local Door = Vector3(GM.TowerData.FarmDoor[1], GM.TowerData.FarmDoor[2], 0)
	self:GoTo(Door + Vector3(0, 60, 0), 40, 25)
	self:Wait(0.2)
	self:Press("Interact")
	self:Wait(5)
	self:Expect(false, "탑에 들어가지 못함")
	self:Finish()
end

-- 탑 안은 기둥·상자 때문에 직선으로 못 갈 수 있다 → 상호작용 자리 옆으로 옮김
function AutoPilot:TeleportNear(Pos)
	local P = self.Player
	P:Teleport(Vector3(Pos.X, Pos.Y, P.entity:GetWorldPosition().Z))
	P.Facing = "Up"
	self:Wait(0.3)
end

function AutoPilot:RunTowerIn()
	local GM, P = self.GM, self.Player
	local R = GM.Report
	self:Wait(1.0)
	self:Expect(GM.MapId == "Tower" and GM.TowerRun and GM.TowerRun.Floor == 1, "탑 1층")
	self:Expect(#GM.Zombies >= 4 and GM.Defense.StaticBlocked ~= "", string.format("적 %d · 배치 %s", #GM.Zombies, tostring(GM.TowerPresetName)))
	-- 적이 플레이어를 쫓는다
	local Z = GM:LiveZombies()[1]
	local D0 = Flat(Z.Entity:GetWorldPosition() - self:Pos()):Length()
	self:Wait(3.0)
	local D1 = Z.Entity:IsValid() and Flat(Z.Entity:GetWorldPosition() - self:Pos()):Length() or 0
	self:Expect(D1 < D0 - 100, string.format("적이 다가옴 %.0f → %.0f", D0, D1))
	-- 처치 → 보물상자 → 계단
	self:Expect(self:ClearTowerFloor(), "1층 정리")
	local Gold = GM.Gold
	self:TeleportNear(GM.ChestSpot + Vector3(0, 110, 0))
	self:Wait(0.2)
	self:Press("Interact")
	self:Expect(GM.bChestOpened and GM.Gold == Gold + 30 and #GM.TowerRun.Loot >= 1, "보물상자 " .. (GM:Hud().Cache["BannerSub.Text"] or ""))
	local Stairs = Vector3(GM.TowerData.Origin[1] + GM.TowerData.Width * 50, GM.TowerData.Origin[2] + 70, 0)
	self:TeleportNear(Stairs + Vector3(0, 90, 0))
	self:Wait(0.2)
	self:Press("Interact")
	self:Expect(GM.TowerRun.Floor == 2 and not GM.bFloorCleared, "2층")
	-- 4층까지 건너뛰고 5층 수호자
	GM.TowerRun.Floor = 4
	self:Expect(self:ClearTowerFloor(), "2층 정리")
	self:TeleportNear(Stairs + Vector3(0, 90, 0))
	self:Wait(0.2)
	self:Press("Interact")
	self:Expect(GM.TowerRun.Floor == 5 and GM.TowerCheckpoint == 5 and GM.Guardian ~= nil, "5층 수호자·체크포인트")
	self:Wait(1.0)
	self:Expect(GM:Hud().Cache["BossPanel.Visibility"] == "HitTestInvisible", "수호자 체력 막대")
	self:Expect(self:ClearTowerFloor(), "5층 정리")
	local Shards = GM:CountItem("CrystalShard")
	self:TeleportNear(GM.ChestSpot + Vector3(0, 110, 0))
	self:Wait(0.2)
	self:Press("Interact")
	self:Expect(GM:CountItem("CrystalShard") == Shards + 1 and GM:CountItem("Sword") == 1, "수호자 보물 (조각·검)")
	-- 6층에서 쓰러짐 → 전리품 절반 잃고 집으로
	self:TeleportNear(Stairs + Vector3(0, 90, 0))
	self:Wait(0.2)
	self:Press("Interact")
	Game.SetPersistent("FarmBie_AutoIron", GM:CountItem("Iron"))
	self:HandOff("Home")
	GM:Damage(1000, "시험")
	self:Wait(5)
	self:Expect(false, "쓰러져도 집으로 가지 못함")
	self:Finish()
end

function AutoPilot:RunTowerHome()
	local GM = self.GM
	self:Wait(1.0)
	local Iron = Game.GetPersistent("FarmBie_AutoIron", 0)
	self:Expect(GM.MapId == "Farm" and GM.TowerCheckpoint == 5 and GM.Health > 0, "집으로 귀환 · 체크포인트 5")
	self:Expect(GM:CountItem("Iron") < Iron and string.find(GM:Hud().Cache["BannerSub.Text"] or "", "잃은") ~= nil,
		string.format("전리품 절반 잃음 철 %d → %d", Iron, GM:CountItem("Iron")))
	Game.SetPersistent("FarmBie_AutoIron", nil)
	self:HandOff("Again")
	local Door = Vector3(GM.TowerData.FarmDoor[1], GM.TowerData.FarmDoor[2], 0)
	self:TeleportNear(Door + Vector3(0, 60, 0))
	self:Wait(0.2)
	self:Press("Interact")
	self:Wait(5)
	self:Expect(false, "다시 들어가지 못함")
	self:Finish()
end

function AutoPilot:RunTowerAgain()
	local GM = self.GM
	self:Wait(1.0)
	self:Expect(GM.MapId == "Tower" and GM.TowerRun.Floor == 5, "체크포인트 5층부터 " .. tostring(GM.TowerRun and GM.TowerRun.Floor))
	self:Finish()
end

function AutoPilot:RunTowerShot()
	local GM, P = self.GM, self.Player
	self:Wait(2.5)
	P.Facing = "Up"
	while true do self:Yield() end
end

function AutoPilot:RunTitleFarm()
	local GM = self.GM
	local Hud = GM:Hud()
	self:Wait(1.0)
	self:Expect(GM.Properties.Slot == "TitleTest1" and GM.Year == 1 and GM.Season == 0 and GM.Day == 1, "슬롯 TitleTest1 새 게임 " .. tostring(GM.Properties.Slot))
	self:Expect(GM.AmbientDay ~= nil and GM.AmbientNight ~= nil and GM.Settings ~= nil, "환경음·설정")
	self:Expect(GM.Menu == "Intro" and Game.GetTimeScale() == 0 and Hud.Cache["GameOverTitle.Text"] == "할아버지의 농장", "새 게임 배경 이야기")
	self:Press("Confirm")
	self:Expect(GM.Menu == nil and Hud.Cache["GameOverWindow.Visibility"] == "Collapsed", "이야기 닫기")
	self:Wait(3.5)
	self:Expect(GM.AmbientDay:GetComponent("AudioSourceComponent").Volume > 0.2 and GM.AmbientNight:GetComponent("AudioSourceComponent").Volume < 0.01,
		"낮 환경음 음량")
	GM:SaveGame()
	local Sounds = GM.Report.Sounds or 0
	self:Press("Pause")
	self:Expect(GM.Menu == "Pause" and Game.GetTimeScale() == 0 and Hud.Cache["OptWindow.Visibility"] == "HitTestInvisible"
		and Hud.Cache["OptName0.Text"] == "계속하기", "일시정지 창")
	self:Expect((GM.Report.Sounds or 0) > Sounds and GM.LastSound == "Open", "창 소리")
	self:Press("Confirm")
	self:Expect(GM.Menu == nil and Game.GetTimeScale() == 1 and Hud.Cache["OptWindow.Visibility"] == "Collapsed", "계속하기")
	self:Press("Pause")
	self:Press("MenuDown")
	self:Press("MenuDown")
	self:Press("Confirm")
	self:Expect(Hud.Cache["OptTitle.Text"] == "타이틀로 갈까?" and Hud.Cache["OptName1.Text"] == "예", "타이틀로 확인")
	self:HandOff("Back")
	self:Press("MenuDown")
	self:Press("Confirm")
	self:Wait(5)
	self:Expect(false, "타이틀로 가지 못함")
	self:Finish()
end

-- 1년 넘기기: 하루씩 AdvanceDay (밤 전투 없이 달력·일정만). 계절마다 요약 로그 = 밸런스 확인용
function AutoPilot:RunYear()
	local GM = self.GM
	local C = GM.Calendar
	local SeasonIds = { "Spring", "Summer", "Autumn", "Winter" }
	self:Wait(1.0)
	-- 그 날 밤 실제 좀비 수 (평화로운 밤 설정을 잠깐 끄고 계획만 세움)
	local function RealCount()
		GM.bPeacefulNights = false
		local N = #GM:PlanNight().Spawns
		GM.bPeacefulNights = true
		GM:PlanNight()
		return N
	end
	local Year1Spring1 = RealCount()
	local Seen = {}
	for Day = 1, 4 * C.SeasonDays do
		local S = GM.Season
		local Stat = Seen[S + 1] or { Days = 0, Merchant = 0, SeasonSeeds = 0, Bosses = {}, Counts = {} }
		Seen[S + 1] = Stat
		Stat.Days = Stat.Days + 1
		if not (GM.Day == (Day - 1) % C.SeasonDays + 1 and S == (Day - 1) // C.SeasonDays) then self:Expect(false, string.format("%d번째 날 날짜 %s", Day, GM:DateText())) end
		if GM:IsMerchantDay() then
			Stat.Merchant = Stat.Merchant + 1
			GM:Restock()
			for _, E in ipairs(GM.Stock) do
				local Info = GM:ItemInfo(E.Key)
				if Info.Kind == "Seed" and Info.Crop.Season == SeasonIds[S + 1] then Stat.SeasonSeeds = Stat.SeasonSeeds + 1 end
				if Info.Kind == "Seed" and Info.Crop.Season ~= SeasonIds[S + 1] then self:Expect(false, "제철 아닌 씨앗 재고 " .. E.Key) end
			end
		end
		for _, Row in ipairs(GM:BossesTonight()) do Stat.Bosses[#Stat.Bosses + 1] = GM.Day .. ":" .. Row.Name end
		if GM.Day == 1 or GM.Day % 10 == 0 then Stat.Counts[#Stat.Counts + 1] = GM.Day .. "일 " .. RealCount() end
		GM:AdvanceDay()
		self:Yield()
	end
	for I, Stat in ipairs(Seen) do
		local Season = SeasonIds[I]
		local Want = { "10:", "20:", "30:" }
		local bBoss = #Stat.Bosses == 4
		for J, W in ipairs(Want) do bBoss = bBoss and string.sub(Stat.Bosses[J] or "", 1, #W) == W end
		local SeasonBoss = Stat.Bosses[4] and D.ByName("Bosses.etable")[string.match(Stat.Bosses[4], ":(%a+)$")]
		bBoss = bBoss and SeasonBoss ~= nil and SeasonBoss.Kind == "Season" and SeasonBoss.Season == Season
		self:Expect(Stat.Days == C.SeasonDays and Stat.Merchant >= 8 and Stat.Merchant <= 9 and Stat.SeasonSeeds >= Stat.Merchant and bBoss,
			string.format("%s: %d일 · 보부상 %d번 · 제철 씨앗 재고 %d · 보스 %s · 밤 좀비 %s", GM:SeasonName(I - 1), Stat.Days, Stat.Merchant, Stat.SeasonSeeds,
				table.concat(Stat.Bosses, " "), table.concat(Stat.Counts, ", ")))
	end
	self:Expect(GM.Year == 2 and GM.Season == 0 and GM.Day == 1, string.format("2년차 봄 1일 (%d년차 %s)", GM.Year, GM:DateText()))
	local Year2Spring1 = RealCount()
	self:Expect(Year2Spring1 > Year1Spring1, string.format("2년차 밤이 더 어렵다 %d → %d마리", Year1Spring1, Year2Spring1))
	GM:SaveGame()
	GM.Year, GM.Day = 1, 9
	self:Expect(GM:LoadGame() and GM.Year == 2 and GM.Day == 1, "2년차 저장/불러오기")
	self:Finish()
end

function AutoPilot:RunIntroShot()
	self:Wait(0.5)
	self.GM:OpenIntro()
	while true do self:Yield() end
end

function AutoPilot:RunNightShot()
	local GM, P = self.GM, self.Player
	for _, S in ipairs(GM.TonightPlan.Spawns) do S.T = 0.5 end
	local E1 = GM.TonightPlan.Entrances[1]
	local In = E1.Pos + Flat(Vector3(0, 0, 0) - E1.Pos):Normalized() * 600
	local TX, TY = GM:TileOf(In)
	for K = -2, 2 do GM:PlaceStructure(K == 0 and "Gate" or "WallWood", TX + K, TY, 0, nil) end
	GM:PlaceStructure("Turret", TX + 3, TY + 1, 0, nil)
	GM:PlaceStructure("Spike", TX, TY + 1, 0, nil)
	GM:Give("Sword", 1, true)
	GM:SetHour(19.98)
	self:Wait(6.0)
	P:Teleport(Vector3(In.X, In.Y + 250, P.entity:GetWorldPosition().Z))
	P.Facing = "Up"
	while true do self:Yield() end
end

function AutoPilot:RunTime()
	local GM, P = self.GM, self.Player
	local Hud = GM:Hud()
	self:Wait(1.0)
	self:Expect(GM.Day == 1 and GM.Season == 0 and GM.Year == 1 and GM.Phase == "Day", "시작 날짜 " .. GM:DateText())
	self:Expect(Hud.Cache["ClockDate.Text"] == "봄 1일 (월)", "HUD 날짜 " .. tostring(Hud.Cache["ClockDate.Text"]))
	-- 낮 시계: 실제 8분 = 14시간
	local H0, T0 = GM.Hour, self.Time
	self:Wait(3.0)
	local Rate = (GM.Hour - H0) / (self.Time - T0)
	self:Expect(math.abs(Rate - 14 / 480) < 0.004, string.format("낮 시계 %.4f시/초", Rate))
	-- 밤 시작
	GM:SetHour(19.97)
	self:Expect(self:WaitUntil(function() return GM.Phase == "Night" end, 5), "밤 시작")
	self:Expect(Hud.LastAnnounce == "밤이 찾아온다", "밤 알림 " .. tostring(Hud.LastAnnounce))
	self:Wait(0.5)
	self:Expect(#GM.Lamps > 0 and GM.Lamps[1].Light.Intensity > 5, string.format("등불 켜짐 %d개", #GM.Lamps))
	self:Expect(Hud.Cache["ClockIcon.Texture"] == "UI/FarmBie/Moon.png", "달 아이콘")
	H0, T0 = GM.Hour, self.Time
	self:Wait(3.0)
	Rate = (GM.Hour - H0) / (self.Time - T0)
	self:Expect(math.abs(Rate - 6 / 210) < 0.004, string.format("밤 시계 %.4f시/초", Rate))
	-- 잠자기: 문 앞 → 안내 → 상호작용
	self:Expect(self:GoTo(GM.SleepSpot + Vector3(0, 80, 0), 40, 15), "문 앞 도착")
	self:Wait(0.2)
	self:Expect(GM.Focus ~= nil and Hud.Cache["Prompt.Visibility"] == "HitTestInvisible", "잠자기 안내")
	self:Press("Interact")
	self:Expect(self:WaitMorning(), "잠 → 아침")
	self:Expect(GM.Day == 2 and math.abs(GM.Hour - 6) < 0.05 and GM:Weekday() == 1, string.format("2일 아침 %s %s", GM:DateText(), GM:ClockText()))
	self:Expect(SaveGame.Exists(GM:SlotName()) and GM.Report.Saves == 1, "자동 저장")
	self:Expect(Flat(self:Pos() - GM.SleepSpot):Length() < 150, "문 앞에서 깸")
	-- 계절 끝 경고 (28일에 잠 → 29일 아침: 1일 남음)
	GM.Day = 28
	GM:SetHour(21.0)
	self:Press("Interact")
	self:WaitMorning()
	self:Expect(GM.Day == 29 and string.find(Hud.Cache["BannerSub.Text"] or "", "계절이") ~= nil, "계절 끝 경고 " .. tostring(Hud.Cache["BannerSub.Text"]))
	-- 마지막 날 밤: 새벽까지 버티면 쓰러지듯 잠 → 여름 1일
	GM.Day = 30
	GM:SetHour(25.97)
	self:Expect(self:WaitMorning(), "새벽 잠")
	self:Expect(GM.SleepReason == "Dawn" and GM.Day == 1 and GM.Season == 1, "여름 1일 " .. GM:DateText())
	self:Expect(string.find(Hud.Cache["BannerSub.Text"] or "", "여름") ~= nil, "계절 알림 " .. tostring(Hud.Cache["BannerSub.Text"]))
	-- 불러오기: 바꾼 값이 저장값으로 돌아온다
	GM.Day, GM.Season = 9, 2
	self:Expect(GM:LoadGame() and GM.Day == 1 and GM.Season == 1, "불러오기 " .. GM:DateText())
	-- 연도 넘김: 겨울 30일 → 2년차 봄 1일
	GM.Season, GM.Day = 3, 30
	GM:SetHour(25.97)
	self:WaitMorning()
	self:Expect(GM.Year == 2 and GM.Season == 0 and GM.Day == 1, string.format("연도 넘김 %d년차 %s", GM.Year, GM:DateText()))
	self:Finish()
end

return AutoPilot
