-- FarmBie 자동 조종 (자동 검증 — FarmGame.Properties.AutoPlay). 플레이어 입력 표를 대신 채운다 (FarmPlayer:GatherInput).
--   시나리오는 코루틴 하나(Run<이름>) — 도우미(GoTo/Press/Wait)가 프레임마다 입력을 채우고 yield 한다. 시간은 실제 시간(메뉴로 게임이 멈춰도 흐른다).
--   확인은 Expect로 쌓고 끝에 관리자 ReportAutoPlay → 로그 "[FarmBie] 결과: 실패 N건".
--   Basic: 이동(사방)·방향 플립북·구르기·카메라 추적·집/울타리 충돌
--   Economy: 처음 돈·출하(작물 전부/고른 묶음)·아침 정산·보부상 요일·재고(계절·한정)·사기(돈 부족·품절)·떠남·소지품 창 옮기기·저장
--   Sanity: 먹기(체력·정신력·희귀 버프)·버프는 아침에 끝·정신력 낮음 속도 감소·잠 회복·정신력 0 쓰러짐(소지금 20%·10시 기상)·저장
--   Forest(씬 4번): 농장 도구 → 서쪽 입구로 숲 → 나무·바위 캐기·풀 줍기·먹기·시간 이어짐 → 새벽 잠 → 집 침대 → 3일 뒤 숲 자원 다시 자람
--   Farm : 갈기·물·심기·비료·제철 아님 거절·물 준 날만 자람·수확·희귀/전용 씨앗 확률·계절 사멸·걷기·우물·저장/불러오기
--   Time : 시계 속도(낮·밤)·밤 시작 알림·등불·잠자기(문 앞 상호작용)·새 날·자동 저장·계절 끝 경고·계절/연도 넘김·불러오기
--   이 모듈은 상태를 갖지 않는다 (Script.Require 값은 공유) — 상태는 New가 만든 객체에.
local AutoPilot = {}
AutoPilot.__index = AutoPilot

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function AutoPilot.New(Scenario, Player, GM)
	local A = setmetatable({ Scenario = Scenario, Player = Player, GM = GM, Time = 0, Frame = 0, Failures = {}, Checks = 0 }, AutoPilot)
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
	self:Expect(SpringSeeds == 5 and Fert and #GM.Stock == 5 + 1 + 3, string.format("재고 %d줄 (봄 씨앗 %d)", #GM.Stock, SpringSeeds))
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
