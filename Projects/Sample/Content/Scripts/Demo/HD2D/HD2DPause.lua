-- HD-2D 데모 관리자 확장 ⑤ 일시정지 메뉴·저장 슬롯·확인 창 (HD2DGame.lua가 Script.Require로 받아 메서드로 붙인다 — 상태는 관리자 self에).
--   Menu: "Pause" | "Slots" | "Confirm" | "Leaving"(타이틀로 페이드) — 대장간 "Forge"는 HD2DCrafting.lua. 입력은 MetaMenuInput(HD2DMenu:MenuInput 맨 앞).
--   일시정지(ESC, 플레이 중에만): 왼쪽 메뉴 10줄(NAV) + 오른쪽 쪽. 메뉴에 커서가 있는 동안 오른쪽은 미리보기(상태/일지/지도/도감/기록/설정),
--     결정하면 일지·도감·설정은 쪽으로 커서가 들어간다(ESC로 메뉴로). 소지품·장비 = 기존 인벤토리 창(HD2DMenu) 탭 1/2 — 닫으면 일시정지로 돌아온다.
--     기록하기 = 게시판(SavePoint) Meta.SaveRadius 안에서만 → 슬롯 창. 타이틀로 = 확인 창 → 페이드 → Meta.TitleScene(이 씬에 타이틀이 있으면 이 씬)을 다시 연다.
--   슬롯 창: 3칸 카드(지역·메인 퀘스트·레벨·플레이 시간·골드), 저장은 찬 칸이면 덮어쓰기 확인, 이어하기(타이틀)는 빈 칸을 고를 수 없다.
--   시간: 열려 있는 동안 Game.SetTimeScale(0), 커서 깜빡임·페이드는 실제 시간(UpdateMetaUi).
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")
local M = Script.Require("Scripts/Demo/HD2D/HD2DMetaData.lua")

local Pause = {}
local MetaIcon = function(Name) return "UI/Demo/HD2D/Meta/" .. Name .. ".png" end
local ItemIcon = function(Name) return "UI/Demo/HD2D/Icons/" .. Name .. ".png" end

local NAV = { "Resume", "Items", "Equip", "Journal", "Map", "Bestiary", "Records", "Settings", "Save", "Title" }
local NavPage = { Journal = "Journal", Map = "Map", Bestiary = "Bestiary", Records = "Records", Settings = "Settings" }
local NavDesc = {
	Resume = "메뉴를 닫고 모험으로 돌아간다.", Items = "회복약과 모은 재료를 살펴보고 쓴다.", Equip = "무기·방어구·장신구를 비교하고 바꿔 낀다.",
	Save = "게시판 앞에서 지금까지의 모험을 기록한다.", Title = "타이틀 화면으로 돌아간다. 기록하지 않은 진행은 사라진다.",
}
local Pages = { Home = "PgHome", Journal = "PgJournal", Map = "PgMap", Bestiary = "PgBest", Records = "PgRec", Settings = "PgSet" }
local PageHead = { Home = { "Star", "현재 상태" }, Journal = { "Journal", "퀘스트 일지" }, Map = { "Map", "지도" }, Bestiary = { "Bestiary", "도감" },
                   Records = { "Records", "모험 기록" }, Settings = { "Settings", "설정" } }
local JournalRows, BestRows = 8, 9
local SettingRows = { "SfxVolume", "Shake", "Numbers", "TextSpeed", "Minimap", "Reset" }
local SettingInfo = {
	SfxVolume = { "Sound", "효과음 음량", "공격·아이템·메뉴 효과음의 크기. 0이면 효과음을 끈다. (주변 물소리·모닥불 소리는 그대로)" },
	Shake = { "Shake", "화면 흔들림", "강한 공격이나 폭발 때 화면이 흔들린다. 멀미가 나면 끄자." },
	Numbers = { "Numbers", "피해·회복 숫자", "적에게 준 피해, 받은 피해, 회복량을 숫자로 띄운다." },
	TextSpeed = { "TextSpeed", "대화 글자 속도", "대화 글자가 나오는 속도. '즉시'는 한 줄이 한 번에 보인다." },
	Minimap = { "Minimap", "미니맵", "화면 오른쪽 위에 주변 약도를 보인다. 지도 화면에서도 바꿀 수 있다." },
	Reset = { "Reset", "기본값으로", "모든 설정을 처음 값으로 되돌린다." },
}
local TextSpeedNames = { "느림", "보통", "빠름", "즉시" }

local function Flat(V) return Vector3(V.X, V.Y, 0) end

-- ================================================================ 열기 · 닫기
function Pause:CanOpenPause()
	return self.Menu == nil and self.Mode == "Play"
end

function Pause:OpenPause(Index)
	if self.Menu ~= nil then return false end
	self.Menu = "Pause"
	self.PauseIndex = Index or self.PauseIndex or 1
	self.PauseFocus = "Nav"
	self.JournalTab, self.JournalIndex = self.JournalTab or 1, 1
	self.BestTab, self.BestIndex, self.BestOffset = self.BestTab or 1, 1, 0
	self.SettingIndex = 1
	self:SetPaused(true)
	self:Hud():ShowPrompt(nil)
	self:Hud():SetHudVisible(false) -- 메뉴 뒤 HUD 패널이 비쳐 어수선하지 않게
	local H = self:MetaHud()
	H:Show("PauseScreen", true, "SelfHitTestInvisible")
	self.PauseFade, self.PageFade = 0.0, 0.0
	H:Set("PauseScreen", "Opacity", 0.0)
	self.Report.PauseOpened = (self.Report.PauseOpened or 0) + 1
	self:RefreshPause()
	Audio.PlayOneShot(self.Sounds.Open)
	Log.Info("[HD2D] 일시정지 메뉴 열림")
	return true
end

function Pause:ClosePause(bSilent)
	self.Menu = nil
	self.PauseFocus = nil
	self:SetPaused(false)
	self:MetaHud():Show("PauseScreen", false)
	self:Hud():SetHudVisible(true)
	if not bSilent then Audio.PlayOneShot(self.Sounds.Close) end
end

-- 인벤토리 등 HUD 창을 잠깐 열 때 일시정지 화면을 숨겼다가 닫히면 돌아온다 (HD2DMenu:CloseMenu → AfterMenuClosed)
function Pause:AfterMenuClosed(Was)
	if self.ReturnToPause and (Was == "Inventory") then
		self.ReturnToPause = false
		local Index = self.PauseIndex
		self.Menu = nil
		self:OpenPause(Index)
	end
end

function Pause:NavId() return NAV[self.PauseIndex] end

function Pause:IsNearSavePoint()
	local Player = self:GetPlayer()
	if not Player then return false end
	local PP = Player.entity:GetWorldPosition()
	local Radius = (M.Balance() or {}).SaveRadius or 320
	for _, Prop in ipairs(self.Props_) do
		if Prop.Id == "SavePoint" and Flat(Prop.Pos - PP):Length() <= Radius then return true end
	end
	return false
end

-- ================================================================ 그리기
function Pause:CurrentPage()
	if self.PauseFocus == "Page" then return self.PageName end
	return NavPage[self:NavId()] or "Home"
end

function Pause:RefreshPause()
	local H = self:MetaHud()
	local Page = self:CurrentPage()
	if self.ShownPage ~= Page then
		self.ShownPage = Page
		self.PageFade = 0.0
		for Name, Widget in pairs(Pages) do H:Show(Widget, Name == Page, "SelfHitTestInvisible") end
	end
	-- 왼쪽 메뉴
	local bNearSave = self:IsNearSavePoint()
	for I, Id in ipairs(NAV) do
		local bSel = I == self.PauseIndex
		H:SetRow("PNav" .. (I - 1), true, bSel, self.PauseFocus == "Nav")
		H:TextTone("PNavText" .. (I - 1), bSel, Id == "Save" and not bNearSave)
	end
	H:Set("PInfoText0", "Text", self:MetaHud().Properties.MapTitle ~= "" and self:MetaHud().Properties.MapTitle or self.Properties.Map)
	H:Set("PInfoText1", "Text", "플레이 시간  " .. self.FormatTime(self.PlayTime or 0))
	H:Set("PInfoText2", "Text", string.format("%d 골드", self.Gold))
	H:Set("PageIcon", "Texture", MetaIcon(PageHead[Page][1]))
	H:Set("PageTitle", "Text", PageHead[Page][2])
	local Sub, Hint = "", "W/S 고르기     E · J 결정     ESC 닫기"
	if Page == "Home" then
		self:DrawHome()
	elseif Page == "Journal" then
		Sub = self:DrawJournal()
		if self.PauseFocus == "Page" then Hint = "A/D 탭     W/S 고르기     E · J 추적하기     ESC 돌아가기" end
	elseif Page == "Map" then
		Sub = self:DrawMapScreen()
		Hint = "W/S 고르기     E · J 미니맵 " .. (self.Settings.Minimap and "끄기" or "켜기") .. "     ESC 닫기"
	elseif Page == "Bestiary" then
		Sub = self:DrawBestiary()
		if self.PauseFocus == "Page" then Hint = "A/D 탭     W/S 고르기     ESC 돌아가기" end
	elseif Page == "Records" then
		Sub = self:DrawRecords()
	elseif Page == "Settings" then
		Sub = ""
		self:DrawSettings()
		if self.PauseFocus == "Page" then Hint = "W/S 고르기     A/D 값 바꾸기     E · J 결정     ESC 돌아가기" end
	end
	H:Set("PageSub", "Text", Sub or "")
	H:Set("PageHint", "Text", Hint)
end

-- ---- 상태 (Home)
function Pause:DrawHome()
	local H = self:MetaHud()
	local P = self:GetPlayer()
	if not P then return end
	local B = D.Balance()
	H:Set("HomeName", "Text", P.Name or B.PlayerName)
	H:Set("HomeLevel", "Text", "Lv " .. P.Level)
	local T = B.ExpTable
	H:Set("HomeNext", "Text", T[P.Level + 1] and string.format("다음 레벨까지 %d EXP", T[P.Level + 1] - P.Exp) or "최고 레벨")
	H:Set("HomeHpBar", "Percent", math.floor(P.Health / P.MaxHealth * 200 + 0.5) / 200)
	H:Set("HomeHpText", "Text", string.format("%d / %d", math.ceil(P.Health), P.MaxHealth))
	H:Set("HomeMpBar", "Percent", P.MaxMana > 0 and math.floor(P.Mana / P.MaxMana * 200 + 0.5) / 200 or 0)
	H:Set("HomeMpText", "Text", string.format("%d / %d", math.floor(P.Mana), P.MaxMana))
	H:Set("HomeExpBar", "Percent", math.floor(P:ExpFraction() * 200 + 0.5) / 200)
	H:Set("HomeExpText", "Text", "")
	local W = self:GetWeapon()
	local Gear = self:GearStats()
	local Stats = {
		{ ItemIcon("Sword"), "공격력", string.format("%d", math.floor(W.Damage * P:DamageScale() + 0.5)) },
		{ ItemIcon("Mail"), "방어력", string.format("%d", math.floor(P.Defense or 0)) },
		{ MetaIcon("Heart"), "최대 HP", tostring(P.MaxHealth) },
		{ ItemIcon("Ring"), "치명타", string.format("%d%%", math.floor((B.CritChance + Gear.CritBonus) * 100 + 0.5)) },
		{ MetaIcon("Boot"), "이동 속도", string.format("%d%%", math.floor((1 + Gear.SpeedBonus) * 100 + 0.5)) },
		{ "UI/Demo/HD2D/OrbFull.png", "BP", string.format("%d / %d", P.BP or 0, B.BoostMax) },
	}
	for K, S in ipairs(Stats) do
		H:Set("HomeStatIcon" .. (K - 1), "Texture", S[1])
		H:Set("HomeStatName" .. (K - 1), "Text", S[2])
		H:Set("HomeStatVal" .. (K - 1), "Text", S[3])
	end
	local Slots = { { "무기", self.Equipped, ItemIcon("Sword") }, { "방어구", self.Armor, ItemIcon("Vest") }, { "장신구", self.Accessory, ItemIcon("Ring") } }
	for K, S in ipairs(Slots) do
		local Row = S[2] and D.Item(S[2])
		H:Set("HomeGearSlot" .. (K - 1), "Text", S[1])
		H:Set("HomeGearIcon" .. (K - 1), "Texture", Row and Row.Icon or S[3])
		H:SetColor("HomeGearIcon" .. (K - 1), 1, 1, 1, Row and 1.0 or 0.3)
		H:Set("HomeGearName" .. (K - 1), "Text", Row and Row.DisplayName or "― 없음 ―")
		H:TextTone("HomeGearName" .. (K - 1), false, Row == nil)
		local Level = (Row and Row.Kind == "Weapon") and self:UpgradeLevel(Row.Weapon) or 0
		H:Set("HomeGearPlus" .. (K - 1), "Text", Level > 0 and ("+" .. Level) or "")
	end
	local QT, QX = self:TrackedQuest(D.Quest(self.QuestStage).Title, self:MainObjective())
	H:Set("HomeQuestIcon", "Texture", MetaIcon(self.Tracked ~= "Main" and "Scroll" or "Crown"))
	H:Set("HomeQuestTitle", "Text", QT)
	H:Set("HomeQuestText", "Text", QX)
	local Id = self:NavId()
	local Desc = NavDesc[Id] or ""
	if Id == "Save" and not self:IsNearSavePoint() then Desc = "기록은 게시판 앞에서만 할 수 있다. 마을 광장의 게시판을 찾아가자." end
	H:Set("HomeMenuDesc", "Text", Desc)
	if self.NavError and self.NavError > 0 then H:SetColor("HomeMenuDesc", 1.0, 0.55, 0.5, 1) else H:SetColor("HomeMenuDesc", 0.6, 0.86, 1.0, 1) end
end

-- ---- 퀘스트 일지
-- 메인 퀘스트 장 (Title이 같은 단계 묶음): { Title, First, Last, Index }
function Pause:MainChapters()
	local List = {}
	local Final = D.FinalQuestStage()
	for Stage = 0, Final do
		local Q = D.Quest(Stage)
		local Last = List[#List]
		if Last and Last.Title == Q.Title then
			Last.Last = Stage
		else
			List[#List + 1] = { Title = Q.Title, First = Stage, Last = Stage, Index = #List }
		end
	end
	return List
end

function Pause:JournalEntries()
	local Out = {}
	if self.JournalTab == 1 then
		for _, C in ipairs(self:MainChapters()) do
			if C.First <= self.QuestStage then
				local bDone = self.QuestStage > C.Last or (C.Last == D.FinalQuestStage() and self.QuestStage >= C.Last)
				Out[#Out + 1] = { Kind = "Main", Chapter = C, bDone = bDone }
			end
		end
	else
		for _, Id in ipairs(D.SubQuestOrder) do
			if self.Sub[Id] then Out[#Out + 1] = { Kind = "Sub", Id = Id, bDone = self.Sub[Id].State == "Done" } end
		end
	end
	return Out
end

function Pause:RewardText(Gold, Item, Exp)
	local Parts = {}
	if Item and Item ~= "" and D.Item(Item) then Parts[#Parts + 1] = D.Item(Item).DisplayName end
	if Gold and Gold > 0 then Parts[#Parts + 1] = Gold .. " 골드" end
	if Exp and Exp > 0 then Parts[#Parts + 1] = "경험치 " .. Exp end
	return #Parts > 0 and table.concat(Parts, " · ") or "특별한 보상은 없다"
end

function Pause:DrawJournal()
	local H = self:MetaHud()
	local Entries = self:JournalEntries()
	self.JournalIndex = math.max(1, math.min(self.JournalIndex, math.max(1, #Entries)))
	H:SetTabs("JTab", 2, self.JournalTab)
	local bFocus = self.PauseFocus == "Page"
	local Active, Done = 0, 0
	for _, E in ipairs(Entries) do if E.bDone then Done = Done + 1 else Active = Active + 1 end end
	H:Set("JCount", "Text", string.format("진행 중 %d · 완료 %d", Active, Done))
	for I = 1, JournalRows do
		local E = Entries[I]
		local Name = "JRow" .. (I - 1)
		H:SetRow(Name, E ~= nil, bFocus and I == self.JournalIndex, bFocus)
		if E then
			local Title, bTracked, Tag
			if E.Kind == "Main" then
				Title = E.Chapter.Title
				bTracked = not E.bDone and self.Tracked == "Main"
			else
				Title = D.SubQuest(E.Id).Title
				bTracked = self.Tracked == E.Id
			end
			Tag = bTracked and "추적 중" or ((E.Kind == "Sub" and not E.bDone and self:SubReady(E.Id)) and "보고 가능" or "")
			H:Set("JRowIcon" .. (I - 1), "Texture", MetaIcon(E.bDone and "Check" or (E.Kind == "Main" and "Crown" or "Scroll")))
			H:Set("JRowName" .. (I - 1), "Text", Title)
			H:Set("JRowTag" .. (I - 1), "Text", Tag)
			H:TextTone("JRowName" .. (I - 1), bFocus and I == self.JournalIndex, E.bDone)
		end
	end
	H:Show("JEmpty", #Entries == 0)
	H:Show("JDetail", #Entries > 0)
	local E = Entries[self.JournalIndex]
	if not E then return "" end
	local Track = ""
	if E.Kind == "Main" then
		local C = E.Chapter
		local Row = M.Journal("Main_" .. C.Index)
		local Summary = (Row and Row.Title == C.Title) and Row.Summary or ((D.Quest(C.First + 1) and D.Quest(C.First + 1).Lines[1]) or "")
		local Next = D.Quest(C.Last + 1)
		H:Set("JPortrait", "Texture", D.Npc("Elder").Portrait)
		H:Set("JTitle", "Text", C.Title)
		H:Set("JGiver", "Text", "메인 퀘스트 · " .. D.Npc("Elder").DisplayName)
		H:Set("JState", "Text", E.bDone and "완료" or string.format("진행 중 · %d장", C.Index + 1))
		H:Set("JDesc", "Text", Summary)
		H:Set("JGoal", "Text", E.bDone and "완료했다" or self:MainObjective())
		H:Set("JReward", "Text", Next and self:RewardText(Next.RewardGold, Next.RewardItem) or "특별한 보상은 없다")
		if not E.bDone then Track = self.Tracked == "Main" and "◆ HUD에 추적 중" or "E · J 를 눌러 추적한다" end
	else
		local Q = D.SubQuest(E.Id)
		local Giver = D.Npc(Q.Giver)
		H:Set("JPortrait", "Texture", Giver.Portrait)
		H:Set("JTitle", "Text", Q.Title)
		H:Set("JGiver", "Text", "서브 퀘스트 · " .. Giver.DisplayName)
		H:Set("JState", "Text", self:SubProgressText(E.Id))
		H:Set("JDesc", "Text", Q.Summary)
		H:Set("JGoal", "Text", E.bDone and "완료했다" or self:SubObjective(E.Id))
		H:Set("JReward", "Text", self:RewardText(Q.RewardGold, Q.RewardItem, Q.RewardExp))
		if not E.bDone then Track = self.Tracked == E.Id and "◆ HUD에 추적 중" or "E · J 를 눌러 추적한다" end
	end
	H:Set("JTrack", "Text", Track)
	H:SetColor("JGoal", E.bDone and 0.72 or 0.96, E.bDone and 0.7 or 0.93, E.bDone and 0.78 or 0.86, 1)
	return ""
end

function Pause:JournalConfirm()
	local E = self:JournalEntries()[self.JournalIndex]
	if not E or E.bDone then
		Audio.PlayOneShot(self.Sounds.Error)
		return
	end
	local Id = E.Kind == "Main" and "Main" or E.Id
	if self.Tracked == Id then
		Audio.PlayOneShot(self.Sounds.Move)
		return
	end
	self:SetTracked(Id)
	self.Report.TrackChanges = (self.Report.TrackChanges or 0) + 1
	Audio.PlayOneShot(self.Sounds.Confirm)
end

-- ---- 지도
-- 목표 자리 목록 (추적 중인 퀘스트 기준 — 이 맵에 있으면 그 자리, 다른 맵이면 출구)
function Pause:GoalPositions()
	local Out = {}
	local function Exit()
		for _, L in ipairs(self:MetaHud().Landmarks or {}) do
			if L.Kind == "Exit" then Out[#Out + 1] = L.Pos return end
		end
	end
	local function NpcPos(Id)
		for _, Npc in ipairs(self.Npcs) do
			if Npc.Id == Id then return Npc.Pos end
		end
	end
	if self.Tracked ~= "Main" and self:SubState(self.Tracked) == "Active" then
		local Q = D.SubQuest(self.Tracked)
		if self:SubReady(self.Tracked) then
			local P = NpcPos(Q.Giver)
			if P then Out[#Out + 1] = P else Exit() end
		elseif Q.Kind == "Find" then
			for _, Prop in ipairs(self.Props_) do
				if self:IsPropActive(Prop) and Prop.Id ~= "SavePoint" then Out[#Out + 1] = Prop.Pos end
			end
		elseif Q.Kind == "Hunt" then
			Out[#Out + 1] = Vector3(Q.AreaX, Q.AreaY, 0)
		end
		return Out
	end
	local Q = D.Quest(self.QuestStage)
	if not Q then return Out end
	if self:CanAdvanceByTalk() then
		local P = NpcPos("Elder")
		if P then Out[#Out + 1] = P else Exit() end
	elseif Q.BossGoal ~= "" then
		if Q.BossGoal == self.Properties.Map and self.BossPos and not self:IsBossDefeated() then
			Out[#Out + 1] = self.BossPos
		elseif Q.BossGoal ~= self.Properties.Map then
			Exit()
		end
	elseif Q.KillGoal > 0 and #self.Slots > 0 then
		local Sum, N = Vector3(0, 0, 0), 0
		for _, Slot in ipairs(self.Slots) do Sum, N = Sum + Flat(Slot.Pos), N + 1 end
		Out[#Out + 1] = Sum * (1.0 / N)
	end
	return Out
end

-- 지도·미니맵 표시물 { Kind, Pos } (bMini = 미니맵 — 같은 목록)
function Pause:MapMarkers(bMini)
	local L = {}
	for _, Npc in ipairs(self.Npcs) do
		local S = Npc.Marker and Npc.Marker:GetComponent("SpriteComponent")
		L[#L + 1] = { Kind = (S and S.Visible) and "NpcQuest" or "Npc", Pos = Npc.Pos }
	end
	for _, Chest in ipairs(self.Chests) do
		if not Chest.bOpened and Chest.Entity then L[#L + 1] = { Kind = "Chest", Pos = Chest.Pos } end
	end
	for _, Prop in ipairs(self.Props_) do
		if Prop.Id == "SavePoint" then L[#L + 1] = { Kind = "Save", Pos = Prop.Pos } end
	end
	for _, Lm in ipairs(self:MetaHud().Landmarks or {}) do
		if Lm.Kind == "Exit" then L[#L + 1] = { Kind = "Exit", Pos = Lm.Pos } end
	end
	if self.BossPos and not self:IsBossDefeated() then L[#L + 1] = { Kind = "Boss", Pos = self.BossPos } end
	for _, P in ipairs(self:GoalPositions()) do L[#L + 1] = { Kind = "Goal", Pos = P } end
	return L
end

function Pause:DrawMapScreen()
	local H = self:MetaHud()
	local Player = self:GetPlayer()
	H:DrawMapPage(self:MapMarkers(false), Player and Player.entity:GetWorldPosition() or nil)
	local QT, QX = self:TrackedQuest(D.Quest(self.QuestStage).Title, self:MainObjective())
	H:Set("MapGoalText", "Text", QT .. "  ―  " .. QX)
	H:Set("PageTitle", "Text", H.Properties.MapTitle ~= "" and H.Properties.MapTitle or "지도")
	self.Report.MapViews = (self.Report.MapViews or 0) + 1
	return H:HasMap() and ("미니맵 " .. (self.Settings.Minimap and "표시 중" or "숨김")) or ""
end

-- ---- 도감
function Pause:BestEntries()
	local Out = {}
	if self.BestTab == 1 then
		for _, Row in ipairs(M.Bestiary()) do
			local bKnown = (self.Bestiary.Seen[Row.Name] or 0) + (self.Bestiary.Kills[Row.Name] or 0) > 0
			Out[#Out + 1] = { Kind = "Enemy", Id = Row.Name, Row = Row, bKnown = bKnown }
		end
	else
		for _, Row in ipairs(M.ItemRows()) do
			Out[#Out + 1] = { Kind = "Item", Id = Row.Name, Row = Row, bKnown = self.ItemsSeen[Row.Name] == true }
		end
	end
	return Out
end

function Pause:BestCompletion()
	local E, EN, I, IN = 0, 0, 0, 0
	for _, Row in ipairs(M.Bestiary()) do
		EN = EN + 1
		if (self.Bestiary.Seen[Row.Name] or 0) + (self.Bestiary.Kills[Row.Name] or 0) > 0 then E = E + 1 end
	end
	for _, Row in ipairs(M.ItemRows()) do
		IN = IN + 1
		if self.ItemsSeen[Row.Name] then I = I + 1 end
	end
	return E, EN, I, IN
end

local WeaponKindNames = { Slash = "베기", Thrust = "찌르기", Arrow = "활", Bolt = "마법" }
local ItemKindNames = { Heal = "회복 아이템", Mana = "마나 회복 아이템", Elixir = "귀한 회복 아이템", Armor = "방어구", Accessory = "장신구", Material = "재료" }

function Pause:DrawBestiary()
	local H = self:MetaHud()
	local Entries = self:BestEntries()
	self.BestIndex = math.max(1, math.min(self.BestIndex, #Entries))
	-- 9줄 창: 선택이 창 밖이면 따라 움직인다
	if self.BestIndex <= self.BestOffset then self.BestOffset = self.BestIndex - 1 end
	if self.BestIndex > self.BestOffset + BestRows then self.BestOffset = self.BestIndex - BestRows end
	self.BestOffset = math.max(0, math.min(self.BestOffset, math.max(0, #Entries - BestRows)))
	H:SetTabs("BTab", 2, self.BestTab)
	local bFocus = self.PauseFocus == "Page"
	for I = 1, BestRows do
		local Index = self.BestOffset + I
		local E = Entries[Index]
		local Name = "BRow" .. (I - 1)
		local bSel = bFocus and Index == self.BestIndex
		H:SetRow(Name, E ~= nil, bSel, bFocus)
		if E then
			local Icon, Title, Count = MetaIcon("Lock"), "？？？", ""
			if E.bKnown then
				if E.Kind == "Enemy" then
					Icon = (string.gsub(E.Row.Picture, "%.png$", "_Icon.png")) -- 목록용 32px 1:1 아이콘 (HD2DMetaArt.WriteListIcon)
					Title = D.Enemy(E.Id).DisplayName
					Count = "처치 " .. (self.Bestiary.Kills[E.Id] or 0)
				else
					Icon = E.Row.Icon
					Title = E.Row.DisplayName
					Count = (E.Row.Kind == "Weapon" or E.Row.Kind == "Armor" or E.Row.Kind == "Accessory") and "" or ("×" .. self:Count(E.Id))
				end
			end
			H:Set("BRowIcon" .. (I - 1), "Texture", Icon)
			H:Set("BRowName" .. (I - 1), "Text", Title)
			H:Set("BRowCount" .. (I - 1), "Text", Count)
			H:TextTone("BRowName" .. (I - 1), bSel, not E.bKnown)
		end
	end
	local More = {}
	if self.BestOffset > 0 then More[#More + 1] = "▲ 위에 더 있다" end
	if self.BestOffset + BestRows < #Entries then More[#More + 1] = "▼ 아래에 더 있다" end
	H:Set("BMore", "Text", table.concat(More, "   "))
	local E = Entries[self.BestIndex]
	if E then self:DrawBestDetail(E) end
	local EK, EN, IK, IN = self:BestCompletion()
	return string.format("마물 %d / %d   ·   물건 %d / %d", EK, EN, IK, IN)
end

function Pause:DrawBestDetail(E)
	local H = self:MetaHud()
	local Stats = {}
	if E.Kind == "Enemy" then
		local Row, Enemy = E.Row, D.Enemy(E.Id)
		H:Set("BPic", "Texture", Row.Picture)
		H:SetSize("BPic", Row.PictureW, Row.PictureH)
		if not E.bKnown then
			H:SetColor("BPic", 0.0, 0.0, 0.02, 0.85)
			H:Set("BName", "Text", "？？？")
			H:Set("BKind", "Text", "아직 만나지 못한 마물")
			H:Set("BDesc", "Text", "어딘가에서 만나면 기록된다.")
			H:Set("BDrop", "Text", "")
			H:Set("BWeak", "Text", "")
		else
			H:SetColor("BPic", 1, 1, 1, 1)
			H:Set("BName", "Text", Enemy.DisplayName)
			H:Set("BKind", "Text", (Enemy.Behavior == "Boss" and "강적 · " or "마물 · ") .. Row.Habitat)
			H:Set("BDesc", "Text", Row.Description)
			local Drops = {}
			if Enemy.DropItem ~= "" and D.Item(Enemy.DropItem) then Drops[#Drops + 1] = D.Item(Enemy.DropItem).DisplayName end
			local Mat = M.Drop(E.Id)
			if Mat and D.Item(Mat.Material) then Drops[#Drops + 1] = D.Item(Mat.Material).DisplayName end
			H:Set("BDrop", "Text", "떨어뜨리는 것: " .. (#Drops > 0 and table.concat(Drops, ", ") or "없음"))
			H:Set("BWeak", "Text", "약점: " .. (self:BestiaryWeakness(E.Id) or "？？？ (아직 밝혀지지 않았다)"))
			Stats = {
				{ MetaIcon("Pin"), string.format("만난 수  %d", self.Bestiary.Seen[E.Id] or 0) },
				{ MetaIcon("Skull"), string.format("쓰러뜨린 수  %d", self.Bestiary.Kills[E.Id] or 0) },
				{ ItemIcon("Coin"), string.format("경험치 %d · 골드 %d~%d", Enemy.Exp, Enemy.GoldMin, Enemy.GoldMax) },
			}
		end
	else
		local Row = E.Row
		H:Set("BPic", "Texture", Row.Icon)
		H:SetSize("BPic", 128, 128)
		if not E.bKnown then
			H:SetColor("BPic", 0.0, 0.0, 0.02, 0.85)
			H:Set("BName", "Text", "？？？")
			H:Set("BKind", "Text", "아직 손에 넣지 못한 물건")
			H:Set("BDesc", "Text", "손에 넣으면 기록된다.")
			H:Set("BDrop", "Text", "")
		else
			H:SetColor("BPic", 1, 1, 1, 1)
			H:Set("BName", "Text", self:ItemDisplayName(E.Id))
			local Kind = ItemKindNames[Row.Kind] or ""
			local Desc = Row.Description
			if Row.Kind == "Weapon" then
				local W = D.Weapon(Row.Weapon)
				Kind = "무기 · " .. WeaponKindNames[W.Kind]
				Desc = W.Description
			end
			H:Set("BKind", "Text", Kind)
			H:Set("BDesc", "Text", Desc)
			local From = {}
			for _, B in ipairs(M.Bestiary()) do
				local Mat = M.Drop(B.Name)
				local En = D.Enemy(B.Name)
				if (Mat and Mat.Material == E.Id) or (En and En.DropItem == E.Id) then From[#From + 1] = En.DisplayName end
			end
			H:Set("BDrop", "Text", #From > 0 and ("얻는 곳: " .. table.concat(From, ", ")) or "")
			Stats = { { MetaIcon("Items"), string.format("가진 수  %d", self:Count(E.Id)) } }
			if Row.Price > 0 then Stats[#Stats + 1] = { ItemIcon("Coin"), string.format("상점 가격  %d G", Row.Price) } end
			if Row.Kind == "Weapon" then Stats[#Stats + 1] = { MetaIcon("Hammer"), string.format("강화  +%d / +%d", self:UpgradeLevel(Row.Weapon), M.MaxUpgrade) } end
		end
		H:Set("BWeak", "Text", "")
	end
	for K = 1, 3 do
		local S = Stats[K]
		H:Show("BStat" .. (K - 1), S ~= nil)
		if S then
			H:Set("BStatIcon" .. (K - 1), "Texture", S[1])
			H:Set("BStatText" .. (K - 1), "Text", S[2])
		end
	end
end

-- ---- 모험 기록
function Pause:DrawRecords()
	local H = self:MetaHud()
	local P = self:GetPlayer()
	local S = self.Stats
	local Chapters = 0
	for _, C in ipairs(self:MainChapters()) do
		if self.QuestStage > C.Last or (C.Last == D.FinalQuestStage() and self.QuestStage >= C.Last) then Chapters = Chapters + 1 end
	end
	local SubTotal = #D.SubQuestOrder
	local Left = {
		{ MetaIcon("Time"), "플레이 시간", self.FormatTime(self.PlayTime or 0) },
		{ MetaIcon("Star"), "레벨", P and ("Lv " .. P.Level) or "-" },
		{ ItemIcon("Sword"), "쓰러뜨린 마물", S.Kills .. " 마리" },
		{ MetaIcon("Skull"), "쓰러뜨린 강적", S.Bosses .. " 마리" },
		{ MetaIcon("Chest"), "연 보물상자", S.Chests .. " 개" },
		{ MetaIcon("Crown"), "메인 퀘스트", string.format("%d / %d 장", Chapters, #self:MainChapters()) },
		{ MetaIcon("Scroll"), "서브 퀘스트", string.format("%d / %d", S.SubDone, SubTotal) },
	}
	local Right = {
		{ ItemIcon("Coin"), "모은 골드", S.GoldEarned .. " G" },
		{ MetaIcon("Boot"), "걸은 거리", string.format("%.2f km", S.Distance / 100000.0) },
		{ MetaIcon("Heart"), "쓰러진 횟수", S.Deaths .. " 번" },
		{ MetaIcon("Hammer"), "무기 강화", S.Upgrades .. " 번" },
		{ ItemIcon("Potion"), "물약 조합", S.Crafts .. " 번" },
		{ ItemIcon("Jelly"), "모은 재료", S.Materials .. " 개" },
		{ MetaIcon("Save"), "기록한 횟수", S.Saves .. " 번" },
	}
	for Side, List in pairs({ RecL = Left, RecR = Right }) do
		for K, Row in ipairs(List) do
			H:Set(Side .. "Icon" .. (K - 1), "Texture", Row[1])
			H:Set(Side .. "Name" .. (K - 1), "Text", Row[2])
			H:Set(Side .. "Val" .. (K - 1), "Text", Row[3])
		end
	end
	local Kinds = {}
	for _, Row in ipairs(M.Bestiary()) do
		local N = self.Bestiary.Kills[Row.Name] or 0
		if N > 0 then Kinds[#Kinds + 1] = D.Enemy(Row.Name).DisplayName .. " " .. N end
	end
	H:Set("RecKinds", "Text", #Kinds > 0 and table.concat(Kinds, "   ·   ") or "아직 쓰러뜨린 마물이 없다.")
	return ""
end

-- ---- 설정
function Pause:DrawSettings()
	local H = self:MetaHud()
	local S = self.Settings
	local bFocus = self.PauseFocus == "Page"
	for I, Key in ipairs(SettingRows) do
		local Info = SettingInfo[Key]
		local Name = "SRow" .. (I - 1)
		local bSel = bFocus and I == self.SettingIndex
		H:SetRow(Name, true, bSel, bFocus)
		H:Set("SRowIcon" .. (I - 1), "Texture", MetaIcon(Info[1]))
		H:Set("SRowName" .. (I - 1), "Text", Info[2])
		H:TextTone("SRowName" .. (I - 1), bSel, false)
		local Value, bBar = "", false
		if Key == "SfxVolume" then
			Value, bBar = string.format("%d%%", S.SfxVolume * 10), true
			H:Set("SRowBar" .. (I - 1), "Percent", S.SfxVolume / 10.0)
		elseif Key == "Shake" then
			Value = S.Shake and "켜기" or "끄기"
		elseif Key == "Numbers" then
			Value = S.Numbers and "표시" or "숨김"
		elseif Key == "Minimap" then
			Value = S.Minimap and "표시" or "숨김"
		elseif Key == "TextSpeed" then
			Value = TextSpeedNames[S.TextSpeed]
		else
			Value = "E · J"
		end
		H:Show("SRowBar" .. (I - 1), bBar)
		H:Set("SRowVal" .. (I - 1), "Text", Value)
		local bArrows = Key ~= "Reset"
		H:Set("SRowL" .. (I - 1), "Visibility", bArrows and "HitTestInvisible" or "Hidden")
		H:Set("SRowR" .. (I - 1), "Visibility", bArrows and "HitTestInvisible" or "Hidden")
		if bBar then H:SetColor("SRowVal" .. (I - 1), 1, 1, 1, 1) else H:SetColor("SRowVal" .. (I - 1), 1.0, 0.85, 0.42, 1) end
	end
	H:Set("SDesc", "Text", SettingInfo[SettingRows[self.SettingIndex]][3])
end

-- 설정 바꾸기 (Dir = -1/+1, 0 = 결정)
function Pause:ChangeSetting(Dir)
	local Key = SettingRows[self.SettingIndex]
	local S = self.Settings
	local Before = self:SettingsSignature()
	if Key == "SfxVolume" then
		if Dir == 0 then
			Audio.PlayOneShot(self.Sounds.Move)
			return
		end
		S.SfxVolume = math.max(0, math.min(10, S.SfxVolume + Dir))
	elseif Key == "TextSpeed" then
		S.TextSpeed = (S.TextSpeed - 1 + (Dir == 0 and 1 or Dir)) % #TextSpeedNames + 1
	elseif Key == "Reset" then
		if Dir ~= 0 then return end
		self.Settings = self.DefaultSettings()
	else
		S[Key] = not S[Key]
	end
	self:ApplySettings()
	if self:SettingsSignature() ~= Before then
		self:SaveSettings()
		self.Report.SettingChanges = (self.Report.SettingChanges or 0) + 1
	end
	Audio.PlayOneShot(Key == "Reset" and self.Sounds.Confirm or self.Sounds.Move)
end

-- ================================================================ 일시정지 입력
function Pause:PauseInput(In)
	local H = self:MetaHud()
	if self.PauseFocus == "Nav" then
		if In.Cancel or In.Inventory then
			self:ClosePause()
			return
		end
		if In.MenuUp or In.MenuDown then
			self.PauseIndex = (self.PauseIndex - 1 + (In.MenuDown and 1 or -1)) % #NAV + 1
			self.NavError = 0
			Audio.PlayOneShot(self.Sounds.Move)
			self:RefreshPause()
		elseif In.Confirm then
			self:NavConfirm()
		end
		return
	end
	-- 쪽 안
	if In.Cancel or In.Inventory then
		self.PauseFocus = "Nav"
		Audio.PlayOneShot(self.Sounds.Close)
		self:RefreshPause()
		return
	end
	local Page = self.PageName
	local Dir = In.MenuDown and 1 or (In.MenuUp and -1 or 0)
	if Page == "Journal" then
		if In.MenuLeft or In.MenuRight then
			self.JournalTab = self.JournalTab == 1 and 2 or 1
			self.JournalIndex = 1
			Audio.PlayOneShot(self.Sounds.Move)
		elseif Dir ~= 0 then
			local N = #self:JournalEntries()
			if N > 0 then self.JournalIndex = (self.JournalIndex - 1 + Dir) % N + 1 end
			Audio.PlayOneShot(self.Sounds.Move)
		elseif In.Confirm then
			self:JournalConfirm()
		else
			return
		end
	elseif Page == "Bestiary" then
		if In.MenuLeft or In.MenuRight then
			self.BestTab = self.BestTab == 1 and 2 or 1
			self.BestIndex, self.BestOffset = 1, 0
			Audio.PlayOneShot(self.Sounds.Move)
		elseif Dir ~= 0 then
			local N = #self:BestEntries()
			self.BestIndex = (self.BestIndex - 1 + Dir) % N + 1
			Audio.PlayOneShot(self.Sounds.Move)
		else
			return
		end
	elseif Page == "Settings" then
		if Dir ~= 0 then
			self.SettingIndex = (self.SettingIndex - 1 + Dir) % #SettingRows + 1
			Audio.PlayOneShot(self.Sounds.Move)
		elseif In.MenuLeft or In.MenuRight then
			self:ChangeSetting(In.MenuRight and 1 or -1)
		elseif In.Confirm then
			self:ChangeSetting(0)
		else
			return
		end
	end
	self:RefreshPause()
end

function Pause:NavConfirm()
	local Id = self:NavId()
	if Id == "Resume" then
		self:ClosePause()
	elseif Id == "Items" or Id == "Equip" then
		-- 기존 인벤토리 창 (닫으면 일시정지로 돌아온다)
		self.Menu = nil
		self:MetaHud():Show("PauseScreen", false)
		self.MenuTab = Id == "Items" and 1 or 2
		self.ReturnToPause = true
		self:Hud():SetHudVisible(true)
		self:OpenInventory()
	elseif Id == "Map" then
		if not self:MetaHud():HasMap() then
			Audio.PlayOneShot(self.Sounds.Error)
			return
		end
		self.Settings.Minimap = not self.Settings.Minimap
		self:ApplySettings()
		self:SaveSettings()
		Audio.PlayOneShot(self.Sounds.Confirm)
		self:RefreshPause()
	elseif Id == "Records" then
		Audio.PlayOneShot(self.Sounds.Move)
	elseif NavPage[Id] then
		self.PauseFocus, self.PageName = "Page", NavPage[Id]
		Audio.PlayOneShot(self.Sounds.Confirm)
		self:RefreshPause()
	elseif Id == "Save" then
		if not self:IsNearSavePoint() then
			self.NavError = 1.2
			Audio.PlayOneShot(self.Sounds.Error)
			self:RefreshPause()
			return
		end
		Audio.PlayOneShot(self.Sounds.Confirm)
		self:OpenSlots("Save", "Pause")
	elseif Id == "Title" then
		Audio.PlayOneShot(self.Sounds.Confirm)
		self:OpenConfirm("타이틀로 돌아갈까요?", "기록하지 않은 진행은 사라집니다.", function() self:LeaveToTitle() end)
	end
end

function Pause:LeaveToTitle()
	self.Menu = "Leaving"
	self.LeaveTimer = 0.55
	self:MetaHud():Show("PauseScreen", false)
	self:Hud():FadeTo(1.0, 0.5)
	Log.Info("[HD2D] 타이틀로")
end

-- ================================================================ 저장 슬롯 창
-- Mode: "Save" | "Load", Return: 닫을 때 돌아갈 메뉴 ("Pause" | "Title" | nil = 플레이)
function Pause:OpenSlots(Mode, Return)
	self.Menu = "Slots"
	self.SlotMode, self.SlotReturn = Mode, Return
	self.SlotCache = {}
	for I = 1, self.SlotCount do self.SlotCache[I] = self:SlotData(I) or false end
	self.SlotIndex = self.CurrentSlot or 1
	if Mode == "Load" and not self.SlotCache[self.SlotIndex] then
		for I = 1, self.SlotCount do
			if self.SlotCache[I] then self.SlotIndex = I break end
		end
	end
	self:SetPaused(true)
	local H = self:MetaHud()
	if Return == "Pause" then H:Show("PauseScreen", false) end
	if Return ~= "Title" then self:Hud():SetHudVisible(false) else self:Hud():Show("TitleScreen", false) end -- 타이틀 글자가 창 뒤로 비치지 않게
	self:Hud():ShowPrompt(nil)
	H:Show("SlotScreen", true, "SelfHitTestInvisible")
	self.SlotFade = 0.0
	H:Set("SlotScreen", "Opacity", 0.0)
	H:Set("SlotHeadIcon", "Texture", MetaIcon(Mode == "Save" and "Save" or "Resume"))
	H:Set("SlotTitle", "Text", Mode == "Save" and "모험 기록하기" or "이어하기")
	H:Set("SlotSub", "Text", Mode == "Save" and "기록할 곳을 고른다" or "불러올 기록을 고른다")
	self:RefreshSlots()
	self.Report.SlotScreens = (self.Report.SlotScreens or 0) + 1
end

local function MapName(Data)
	local Name = (Data.Meta or {}).MapName
	if Name and Name ~= "" then return Name end
	return Data.Map == "Cave" and "폭포 옆 동굴 유적" or "하르트 마을과 황혼의 들판"
end

function Pause:RefreshSlots()
	local H = self:MetaHud()
	for I = 1, self.SlotCount do
		local Data = self.SlotCache[I]
		local K = I - 1
		local bSel = I == self.SlotIndex
		H:Show("SlotSel" .. K, bSel)
		H:Set("SlotCursor" .. K, "Visibility", bSel and "HitTestInvisible" or "Hidden")
		H:Set("SlotBg" .. K, "Opacity", bSel and 1.0 or 0.7)
		local bData = Data ~= false
		for _, W in ipairs({ "SlotMapRow", "SlotQuestRow", "SlotLv", "SlotTimeRow", "SlotGoldRow" }) do H:Show(W .. K, bData) end
		H:Show("SlotEmpty" .. K, not bData)
		H:Set("SlotNum" .. K, "Text", "기록 " .. I .. ((I == self.CurrentSlot and self.SlotMode == "Save") and "  (지금 기록)" or ""))
		if bData then
			H:Set("SlotMap" .. K, "Text", MapName(Data))
			local Q = D.Quest(math.floor(Data.QuestStage or 0))
			H:Set("SlotQuest" .. K, "Text", Q and Q.Title or "")
			H:Set("SlotLv" .. K, "Text", "Lv " .. math.floor(Data.Level or 1))
			H:Set("SlotTime" .. K, "Text", self.FormatTime(Data.PlayTime or 0))
			H:Set("SlotGold" .. K, "Text", string.format("%d G", math.floor(Data.Gold or 0)))
		end
		local Dim = (self.SlotMode == "Load" and not bData) and 0.5 or 1.0
		H:SetColor("SlotNum" .. K, 1.0 * Dim, 0.85 * Dim, 0.42 * Dim, 1)
	end
end

function Pause:SlotsInput(In)
	if In.Cancel or In.Inventory then
		self:CloseSlots()
		Audio.PlayOneShot(self.Sounds.Close)
		return
	end
	if In.MenuUp or In.MenuDown then
		self.SlotIndex = (self.SlotIndex - 1 + (In.MenuDown and 1 or -1)) % self.SlotCount + 1
		Audio.PlayOneShot(self.Sounds.Move)
		self:RefreshSlots()
	elseif In.Confirm then
		self:SlotConfirm()
	end
end

function Pause:SlotConfirm()
	local I = self.SlotIndex
	local Data = self.SlotCache[I]
	if self.SlotMode == "Load" then
		if not Data then
			Audio.PlayOneShot(self.Sounds.Error)
			return
		end
		Audio.PlayOneShot(self.Sounds.Confirm)
		self:MetaHud():Show("SlotScreen", false)
		self:ContinueFromSlot(I)
		return
	end
	if Data then
		Audio.PlayOneShot(self.Sounds.Confirm)
		self:OpenConfirm(string.format("기록 %d에 덮어쓸까요?", I),
			string.format("이전 기록 (Lv %d · %s · %s)은 사라집니다.", math.floor(Data.Level or 1), self.FormatTime(Data.PlayTime or 0), MapName(Data)),
			function() self:FinishSave(I) end, "Slots")
	else
		self:FinishSave(I)
	end
end

function Pause:FinishSave(I)
	local bOk = self:SaveToSlot(I)
	Audio.PlayOneShot(bOk and self.Sounds.Confirm or self.Sounds.Error)
	self:CloseSlots()
	if bOk then
		self:Hud():Toast(MetaIcon("Save"), string.format("기록 %d에 기록했다", I))
		local Player = self:GetPlayer()
		if Player then self:SpawnHealFx(Player.entity:GetWorldPosition(), { 1, 0.9, 0.6, 1 }) end
	end
end

function Pause:CloseSlots()
	local H = self:MetaHud()
	H:Show("SlotScreen", false)
	local Return = self.SlotReturn
	self.SlotReturn = nil
	if Return == "Pause" then
		self.Menu = nil
		self:OpenPause(self.PauseIndex)
	elseif Return == "Title" then
		self.Menu = "Title"
		self:Hud():Show("TitleScreen", true, "SelfHitTestInvisible")
	else
		self.Menu = nil
		self:SetPaused(false)
		self:Hud():SetHudVisible(true)
	end
end

-- 타이틀 "이어하기" → 슬롯 I (HD2DMenu:TitleConfirm에서 넘어온다)
function Pause:ContinueFromSlot(I)
	local H = self:Hud()
	self.Menu = nil
	self.SlotReturn = nil
	H:ShowTitle(false)
	H:SetHudVisible(true)
	self:SetPaused(false)
	self.Mode = "Play"
	self.CurrentSlot = I
	if self:ContinueGame(self:SlotName(I)) then
		H:FadeFrom(1.0, 0.8)
		H:Announce("이어하기", string.format("기록 %d에서 다시 시작한다", I), 2.0)
		Log.Info(string.format("[HD2D] 이어하기: 슬롯 %d", I))
	end
end

-- ================================================================ 확인 창 (예/아니요 — 기본 아니요)
function Pause:OpenConfirm(Title, Text, OnYes, Return)
	self.ConfirmPrev = Return or self.Menu
	self.Menu = "Confirm"
	self.ConfirmIndex = 2
	self.ConfirmYes = OnYes
	local H = self:MetaHud()
	H:Show("ConfirmScreen", true, "SelfHitTestInvisible")
	H:Set("ConfirmTitle", "Text", Title)
	H:Set("ConfirmText", "Text", Text)
	self:RefreshConfirm()
end

function Pause:RefreshConfirm()
	local H = self:MetaHud()
	for K = 0, 1 do
		local bSel = (K + 1) == self.ConfirmIndex
		H:Set("ConfirmCursor" .. K, "Visibility", bSel and "HitTestInvisible" or "Hidden")
		H:TextTone("ConfirmText" .. K, bSel, not bSel)
	end
end

function Pause:ConfirmInput(In)
	if In.Cancel or In.Inventory then
		self:CloseConfirm(false)
		return
	end
	if In.MenuLeft or In.MenuRight or In.MenuUp or In.MenuDown then
		self.ConfirmIndex = self.ConfirmIndex == 1 and 2 or 1
		Audio.PlayOneShot(self.Sounds.Move)
		self:RefreshConfirm()
	elseif In.Confirm then
		self:CloseConfirm(self.ConfirmIndex == 1)
	end
end

function Pause:CloseConfirm(bYes)
	self:MetaHud():Show("ConfirmScreen", false)
	self.Menu = self.ConfirmPrev
	local OnYes = self.ConfirmYes
	self.ConfirmYes = nil
	if bYes and OnYes then
		OnYes()
	else
		Audio.PlayOneShot(self.Sounds.Close)
	end
end

-- ================================================================ 입력 분배 (HD2DMenu:MenuInput 맨 앞 — 처리했으면 true)
function Pause:MetaMenuInput(UDt, In)
	local Menu = self.Menu
	if Menu == "Pause" then
		self:PauseInput(In)
	elseif Menu == "Slots" then
		self:SlotsInput(In)
	elseif Menu == "Confirm" then
		self:ConfirmInput(In)
	elseif Menu == "Forge" then
		self:ForgeInput(In)
	elseif Menu == "Leaving" then
		self.LeaveTimer = self.LeaveTimer - UDt
		if self.LeaveTimer <= 0 then
			self.LeaveTimer = 1.0e9
			local Target = self.Properties.Title and Game.GetCurrentScene() or ((M.Balance() or {}).TitleScene or "Scenes/Demo/HD2D.escene")
			Game.SetTimeScale(1.0)
			Game.OpenScene(Target)
		end
	else
		return false
	end
	return true
end

-- 실제 시간 연출 (MetaHud OnLateUpdate가 부른다): 창 페이드·선택 띠 깜빡임
function Pause:UpdateMetaUi(UDt)
	local H = self:MetaHud()
	self.MetaClock = (self.MetaClock or 0) + UDt
	if self.NavError and self.NavError > 0 then
		self.NavError = self.NavError - UDt
		if self.NavError <= 0 and self.Menu == "Pause" then self:RefreshPause() end
	end
	if self.Menu == "Pause" then
		if self.PauseFade < 1 then
			self.PauseFade = math.min(1.0, self.PauseFade + UDt / 0.14)
			H:Set("PauseScreen", "Opacity", math.floor(self.PauseFade * 20 + 0.5) / 20)
		end
		if self.PageFade < 1 then
			self.PageFade = math.min(1.0, self.PageFade + UDt / 0.12)
			local Widget = Pages[self.ShownPage]
			if Widget then H:Set(Widget, "Opacity", math.floor((0.3 + 0.7 * self.PageFade) * 20 + 0.5) / 20) end
		end
		local Pulse = math.floor((0.78 + 0.22 * math.sin(self.MetaClock * 5.0)) * 20 + 0.5) / 20
		H:Set("PNav" .. (self.PauseIndex - 1) .. "Sel", "Opacity", self.PauseFocus == "Nav" and Pulse or 0.55)
	elseif self.Menu == "Slots" then
		if self.SlotFade < 1 then
			self.SlotFade = math.min(1.0, self.SlotFade + UDt / 0.14)
			H:Set("SlotScreen", "Opacity", math.floor(self.SlotFade * 20 + 0.5) / 20)
		end
		H:Set("SlotSel" .. (self.SlotIndex - 1), "Opacity", math.floor((0.78 + 0.22 * math.sin(self.MetaClock * 5.0)) * 20 + 0.5) / 20)
	end
end

-- 마우스
function Pause:OnMetaClicked(What, Index)
	if What == "Nav" and self.Menu == "Pause" then
		self.PauseIndex, self.PauseFocus = Index, "Nav"
		self:NavConfirm()
	elseif What == "Slot" and self.Menu == "Slots" then
		self.SlotIndex = Index
		self:SlotConfirm()
	elseif What == "Confirm" and self.Menu == "Confirm" then
		self:CloseConfirm(Index == 1)
	elseif What == "Journal" and self.Menu == "Pause" and self.ShownPage == "Journal" then
		self.PauseFocus, self.PageName, self.JournalIndex = "Page", "Journal", Index
		self:JournalConfirm()
		self:RefreshPause()
	elseif What == "Bestiary" and self.Menu == "Pause" and self.ShownPage == "Bestiary" then
		self.PauseFocus, self.PageName, self.BestIndex = "Page", "Bestiary", self.BestOffset + Index
		self:RefreshPause()
	elseif What == "Settings" and self.Menu == "Pause" and self.ShownPage == "Settings" then
		self.PauseFocus, self.PageName, self.SettingIndex = "Page", "Settings", Index
		self:ChangeSetting(0)
		self:RefreshPause()
	elseif What == "Forge" and self.Menu == "Forge" then
		self.ForgeIndex = Index
		self:ForgeConfirm()
	end
end

function Pause:OnMetaHovered(What, Index)
	if What == "Nav" and self.Menu == "Pause" and self.PauseFocus == "Nav" and self.PauseIndex ~= Index then
		self.PauseIndex = Index
		self:RefreshPause()
	elseif What == "Slot" and self.Menu == "Slots" and self.SlotIndex ~= Index then
		self.SlotIndex = Index
		self:RefreshSlots()
	elseif What == "Confirm" and self.Menu == "Confirm" and self.ConfirmIndex ~= Index then
		self.ConfirmIndex = Index
		self:RefreshConfirm()
	elseif What == "Forge" and self.Menu == "Forge" and self.ForgeIndex ~= Index and Index <= #self:ForgeRows() then
		self.ForgeIndex = Index
		self:RefreshForge()
	end
end

return Pause
