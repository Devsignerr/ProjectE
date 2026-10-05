-- HD-2D 메타 시스템 자동 검증·스크린샷 시나리오 (HD2DAutoPilot.lua가 메서드로 붙인다 — self = 자동 조종 객체, 상태 없음).
--   Full 시나리오 사이사이에 끼는 확인: MetaResetSaves(시작 — 슬롯·예전 저장 지움), MetaTrackCheck(서브 퀘스트 추적 → HUD),
--     MetaChecksMain(일시정지 메뉴 각 화면·인벤토리 돌아오기·지도/미니맵·도감·기록·설정 저장/읽기/효과·기록 막힘·타이틀 확인 창·재료 → 무기 +1 → 공격력·물약 조합),
--     SaveAtBoardSlot(게시판 → 슬롯 창 → 빈 칸 저장 / 찬 칸 덮어쓰기 확인), MetaPrepareMigration(예전 단일 슬롯 저장 만들기),
--     MetaLoadChecks(타이틀 이어하기 → 슬롯 창 → 예전 저장이 슬롯 1로 옮겨졌는가 → 슬롯 고르기), MetaCaveChecks(동굴 지도·미니맵·기록 막힘).
--   스크린샷 시나리오(HD2DMetaGen.SHOT_SCENES): PauseHome / Journal / MapScreen / Bestiary / Records / Settings / SaveSlots / LoadSlots / Forge / Recipe /
--     Minimap / ConfirmTitle — 그 화면에서 머문다.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")
local M = Script.Require("Scripts/Demo/HD2D/HD2DMetaData.lua")

local Pilot = {}

local function Flat(V) return Vector3(V.X, V.Y, 0) end

-- ================================================================ 도우미
function Pilot:OpenPauseMenu()
	self:Press("Pause")
	self:Wait(0.3)
	return self.GM.Menu == "Pause"
end

function Pilot:NavTo(Id)
	for _ = 1, 12 do
		if self.GM:NavId() == Id then return true end
		self:Press("MenuDown")
		self:Wait(0.04)
	end
	return self.GM:NavId() == Id
end

function Pilot:MetaWidgetVisible(Name)
	return self.GM:MetaHud():W(Name).Visible
end

function Pilot:MetaResetSaves()
	for I = 1, self.GM.SlotCount do SaveGame.Delete(self.GM:SlotName(I)) end
	SaveGame.Delete(D.Balance().SaveSlot)
end

-- ================================================================ 퀘스트 추적 (서브 퀘스트 Id를 일지에서 추적 → HUD)
function Pilot:MetaTrackCheck(Id)
	local GM = self.GM
	self:OpenPauseMenu()
	self:NavTo("Journal")
	self:Press("Confirm")
	self:Wait(0.15)
	self:Press("MenuRight")
	self:Wait(0.1)
	for _ = 1, 4 do
		local E = GM:JournalEntries()[GM.JournalIndex]
		if E and E.Id == Id then break end
		self:Press("MenuDown")
		self:Wait(0.05)
	end
	self:Press("Confirm")
	self:Wait(0.15)
	local Title = GM:Hud():W("QuestTitle").Text
	self:Expect(GM.Tracked == Id and Title == "◆ " .. D.SubQuest(Id).Title, "퀘스트 일지에서 추적 변경 → HUD (" .. Title .. ")")
	self:Press("Cancel")
	self:Wait(0.1)
	self:Press("Cancel")
	self:Wait(0.3)
	self:Expect(GM.Menu == nil and Game.GetTimeScale() == 1, "일시정지 닫힘 → 시간 재개")
end

-- ================================================================ 일시정지 메뉴 각 화면 · 설정 · 지도 · 도감 · 기록 · 대장간
function Pilot:MetaChecksMain()
	local GM, P = self.GM, self.Player
	local H = GM:MetaHud()
	self:Expect(GM.Tracked == "Main", "추적 서브 퀘스트 완료 → 메인 추적으로")
	self:Expect(H.bMiniShown == true and GM:Hud():W("QuestPanel").Position.Y > 100, "미니맵 표시 + 퀘스트 칸 아래로")
	self:Expect((GM.Report.MaterialDrops or 0) > 0, "재료 드랍 (" .. (GM.Report.MaterialDrops or 0) .. "번)")
	self:Expect((GM.Bestiary.Kills.Slime or 0) >= 2 and (GM.Bestiary.Seen.Slime or 0) >= GM.Bestiary.Kills.Slime,
		string.format("도감 기록 (슬라임 만남 %d, 처치 %d)", GM.Bestiary.Seen.Slime or 0, GM.Bestiary.Kills.Slime or 0))

	self:OpenPauseMenu()
	self:Expect(GM.Menu == "Pause" and Game.GetTimeScale() == 0 and self:MetaWidgetVisible("PauseScreen"), "일시정지 메뉴 (ESC) + 시간 정지")
	self:Expect(not H.bMiniShown, "메뉴 동안 미니맵 숨김")
	-- 소지품·장비 = 인벤토리 → 닫으면 일시정지로
	for _, Case in ipairs({ { "Items", 1 }, { "Equip", 2 } }) do
		self:NavTo(Case[1])
		self:Press("Confirm")
		self:Wait(0.25)
		self:Expect(GM.Menu == "Inventory" and GM.MenuTab == Case[2] and not self:MetaWidgetVisible("PauseScreen"), "일시정지 → " .. Case[1] .. " (인벤토리 탭 " .. Case[2] .. ")")
		self:Press("Cancel")
		self:Wait(0.25)
		self:Expect(GM.Menu == "Pause" and self:MetaWidgetVisible("PauseScreen"), "인벤토리 닫기 → 일시정지로 돌아옴")
	end
	-- 쪽 미리보기
	for _, Case in ipairs({ { "Journal", "Journal", "PgJournal" }, { "Map", "Map", "PgMap" }, { "Bestiary", "Bestiary", "PgBest" },
	                        { "Records", "Records", "PgRec" }, { "Settings", "Settings", "PgSet" }, { "Resume", "Home", "PgHome" } }) do
		self:NavTo(Case[1])
		self:Wait(0.1)
		self:Expect(GM.ShownPage == Case[2] and self:MetaWidgetVisible(Case[3]), "일시정지 쪽: " .. Case[2])
	end
	-- 퀘스트 일지
	self:NavTo("Journal")
	self:Press("Confirm")
	self:Wait(0.1)
	if GM.JournalTab ~= 1 then self:Press("MenuRight") end -- 탭은 지난번 것을 기억한다
	local Main = #GM:JournalEntries()
	self:Press("MenuRight")
	self:Wait(0.1)
	local Subs = #GM:JournalEntries()
	self:Expect(GM.PauseFocus == "Page" and Main >= 1 and Subs == 3, string.format("퀘스트 일지 (메인 %d장, 서브 %d)", Main, Subs))
	self:Press("Cancel")
	self:Wait(0.1)
	-- 지도 (+ 미니맵 켜고 끄기)
	self:NavTo("Map")
	self:Wait(0.1)
	local Marks, Goals = GM:MapMarkers(false), 0
	for _, Mk in ipairs(Marks) do if Mk.Kind == "Goal" then Goals = Goals + 1 end end
	self:Expect(H:HasMap() and self:MetaWidgetVisible("MapImg") and #Marks >= 8 and Goals >= 1,
		string.format("지도 화면 (표시물 %d, 목표 %d)", #Marks, Goals))
	self:Press("Confirm")
	self:Wait(0.1)
	local bOff = GM.Settings.Minimap == false
	self:Press("Confirm")
	self:Wait(0.1)
	self:Expect(bOff and GM.Settings.Minimap == true, "지도 화면에서 미니맵 끄기/켜기")
	-- 도감
	self:NavTo("Bestiary")
	self:Press("Confirm")
	self:Wait(0.1)
	local Known = 0
	for _, E in ipairs(GM:BestEntries()) do if E.bKnown then Known = Known + 1 end end
	self:Press("MenuRight")
	self:Wait(0.1)
	local Items = 0
	for _, E in ipairs(GM:BestEntries()) do if E.bKnown then Items = Items + 1 end end
	for _ = 1, 12 do self:Press("MenuDown") end
	self:Expect(Known >= 5 and Items >= 10 and GM.BestOffset > 0, string.format("도감 (마물 %d종, 물건 %d종, 스크롤 %d)", Known, Items, GM.BestOffset))
	self:Press("Cancel")
	self:Wait(0.1)
	-- 기록
	self:NavTo("Records")
	self:Wait(0.1)
	self:Expect(H:W("RecLVal2").Text == GM.Stats.Kills .. " 마리" and GM.Stats.Kills >= 10, "모험 기록 (처치 " .. GM.Stats.Kills .. ")")
	-- 설정: 바꾸기 → 저장 파일 → 다시 읽기 → 효과 → 원래대로
	local Orig = {}
	for K, V in pairs(GM.Settings) do Orig[K] = V end
	GM.Settings = GM.DefaultSettings() -- 사용자 설정과 상관없이 같은 값에서 (끝나면 Orig로 되돌림)
	GM:ApplySettings()
	local Base = GM.DefaultSettings()
	self:NavTo("Settings")
	self:Press("Confirm")
	self:Wait(0.1)
	self:Press("MenuLeft")
	self:Press("MenuLeft")
	self:Press("MenuDown")
	self:Press("MenuRight")   -- 흔들림 끔
	self:Press("MenuDown")
	self:Press("Confirm")     -- 숫자 숨김
	self:Press("MenuDown")
	self:Press("MenuRight")   -- 글자 빠름
	self:Wait(0.1)
	local Want = string.format("Vol=%d;Shake=false;Num=false;Text=%d;Mini=true", Base.SfxVolume - 2, Base.TextSpeed % 4 + 1)
	local Saved = SaveGame.Load("HD2D_Settings") or {}
	GM:LoadSettings()
	self:Expect(GM:SettingsSignature() == Want and Saved.SfxVolume == Base.SfxVolume - 2, "설정 저장/읽기 (" .. GM:SettingsSignature() .. ")")
	self:Expect(math.abs((Audio.HD2DVolume or 0) - (Base.SfxVolume - 2) / 10) < 1e-3, "효과음 음량 배율")
	GM:AddShake(12, 0.4)
	local Before = #GM:Hud().Numbers
	GM:DamageNumber(P.entity:GetWorldPosition(), "99", { 1, 1, 1, 1 }, 1.0)
	self:Expect(GM:GetShakeOffset():Length() == 0 and #GM:Hud().Numbers == Before, "설정 효과 (흔들림 없음, 숫자 없음)")
	GM.ShakeTime = 0
	GM.Settings = Orig
	GM:ApplySettings()
	GM:SaveSettings()
	self:Press("Cancel")
	self:Wait(0.1)
	-- 기록하기: 게시판에서 멀면 막힘 / 타이틀로: 확인 창 → 아니요
	self:NavTo("Save")
	self:Press("Confirm")
	self:Wait(0.1)
	self:Expect(GM.Menu == "Pause" and (GM.NavError or 0) > 0, "게시판에서 멀면 기록하기 막힘")
	self:NavTo("Title")
	self:Press("Confirm")
	self:Wait(0.15)
	self:Expect(GM.Menu == "Confirm" and self:MetaWidgetVisible("ConfirmScreen") and GM.ConfirmIndex == 2, "타이틀로 → 확인 창 (기본 아니요)")
	self:Press("Cancel")
	self:Wait(0.1)
	self:Expect(GM.Menu == "Pause" and not self:MetaWidgetVisible("ConfirmScreen"), "확인 창 취소 → 일시정지")
	self:Press("Cancel")
	self:Wait(0.3)
	self:Expect(GM.Menu == nil and Game.GetTimeScale() == 1 and H.bMiniShown, "일시정지 닫기 → 플레이 + 미니맵")

	-- 대장간: 재료 모으기 → 대장장이 → 검 +1 → 공격력, 회복약 조합
	-- 검 +1 = 젤리 1 + 30G, 회복약 = 포자 1 + 5G (MetaGen 표) — 모자라면 그 마물을 더 잡는다
	if GM:Count("Jelly") < 1 then self:Fight("Slime", "Spear", 0, 90, false, nil, function() return GM:Count("Jelly") >= 1 end) end
	if GM:Count("Spore") < 1 then self:Fight("Mushroom", "Spear", 0, 90, false, nil, function() return GM:Count("Spore") >= 1 end) end
	self:Expect(GM:Count("Jelly") >= 1 and GM:Count("Spore") >= 1, string.format("재료 모음 (젤리 %d, 포자 %d)", GM:Count("Jelly"), GM:Count("Spore")))
	self:TalkToNpc("Smith")
	self:TalkThrough()
	self:WaitUntil(function() return GM.Menu == "Forge" end, 2)
	self:Expect(GM.Menu == "Forge" and self:MetaWidgetVisible("ForgeWindow") and Game.GetTimeScale() == 0, "대장장이 → 대장간 창")
	for _ = 1, 6 do
		local R = GM:ForgeRows()[GM.ForgeIndex]
		if R and R.Weapon == "Sword" then break end
		self:Press("MenuDown")
	end
	local D0, Gold0 = GM:GetWeaponById("Sword").Damage, GM.Gold
	self:Press("Confirm")
	self:Wait(0.2)
	local D1 = GM:GetWeaponById("Sword").Damage
	self:Expect(GM:UpgradeLevel("Sword") == 1 and D1 > D0 and GM.Gold == Gold0 - M.Upgrade("Sword", 1).Gold,
		string.format("무기 강화 검 +1 (공격력 %d → %d)", D0, D1))
	self:Press("MenuRight")
	self:Wait(0.1)
	local Potions = GM:Count("Potion")
	self:Press("Confirm")
	self:Wait(0.2)
	self:Expect(GM.ForgeTab == 2 and GM:Count("Potion") == Potions + 1 and GM.Stats.Crafts == 1, "물약 조합 (회복약 +1)")
	self:Press("Cancel")
	self:Wait(0.3)
	self:Expect(GM.Menu == nil, "대장간 닫힘")
	self:EquipBySwitch("Sword")
	self:Expect(P.Weapon.Damage == D1 and string.find(GM:Hud():W("WeaponName").Text, "+1", 1, true) ~= nil,
		string.format("강화한 검 장비 → 공격 피해 %d, HUD %s", P.Weapon.Damage, GM:Hud():W("WeaponName").Text))
	self:EquipBySwitch("Spear")
end

-- ================================================================ 게시판 → 슬롯 저장
function Pilot:SaveAtBoardSlot(Slot)
	local GM = self.GM
	local Board = self:FindProp("SavePoint")
	local Saves = GM.Report.Saves
	local bExisted = SaveGame.Exists(GM:SlotName(Slot))
	self:InteractAt(Board.Pos, "게시판")
	self:TalkThrough()
	self:WaitUntil(function() return GM.Menu == "Slots" end, 2)
	self:Expect(GM.Menu == "Slots" and self:MetaWidgetVisible("SlotScreen") and GM.SlotMode == "Save", "게시판 → 저장 슬롯 창")
	while GM.SlotIndex ~= Slot do
		self:Press("MenuDown")
		self:Wait(0.05)
	end
	self:Press("Confirm")
	self:Wait(0.2)
	if bExisted then
		self:Expect(GM.Menu == "Confirm" and GM.ConfirmIndex == 2, "찬 슬롯 → 덮어쓰기 확인 (기본 아니요)")
		self:Press("MenuLeft")
		self:Press("Confirm")
		self:Wait(0.2)
	end
	return self:Expect(GM.Report.Saves == Saves + 1 and SaveGame.Exists(GM:SlotName(Slot)) and GM.CurrentSlot == Slot and GM.Menu == nil,
		string.format("게시판 저장 → 슬롯 %d%s", Slot, bExisted and " (덮어쓰기)" or ""))
end

-- 예전 단일 슬롯 저장 (슬롯 1은 비움) — 다음 씬 시작에서 슬롯 1로 옮겨져야 한다
function Pilot:MetaPrepareMigration(FromSlot)
	local Data = self.GM:SlotData(FromSlot)
	SaveGame.Delete(self.GM:SlotName(1))
	if Data then
		Data.Meta = Data.Meta or {}
		Data.Meta.Slot = nil
		Data.Gold = (Data.Gold or 0) + 1 -- 슬롯 2와 구별
		SaveGame.Save(D.Balance().SaveSlot, Data)
		Game.SetPersistent("HD2D_AutoLegacyGold", Data.Gold)
	end
end

-- 타이틀 이어하기 → 슬롯 창 (예전 저장 이전 확인) → Slot 고르기
function Pilot:MetaLoadChecks(Slot)
	local GM = self.GM
	self:Expect((GM.Report.Migrated or 0) == 1 and not SaveGame.Exists(D.Balance().SaveSlot) and SaveGame.Exists(GM:SlotName(1)),
		"예전 단일 슬롯 저장 → 슬롯 1로 옮김")
	local One = GM:SlotData(1)
	self:Expect(One ~= nil and One.Gold == Game.GetPersistent("HD2D_AutoLegacyGold", -1), "옮긴 슬롯 1 내용 = 예전 저장")
	self:TitleChoose(2)
	self:WaitUntil(function() return GM.Menu == "Slots" end, 2)
	self:Expect(GM.Menu == "Slots" and GM.SlotMode == "Load" and self:MetaWidgetVisible("SlotScreen") and not GM:Hud():W("TitleScreen").Visible,
		"이어하기 → 슬롯 선택 창 (타이틀 글자는 잠시 숨김)")
	self:Expect(GM.SlotCache[1] and GM.SlotCache[2] and not GM.SlotCache[3], "슬롯 요약 (1·2 기록, 3 비어 있음)")
	-- 빈 칸은 고를 수 없다
	while GM.SlotIndex ~= 3 do
		self:Press("MenuDown")
		self:Wait(0.05)
	end
	self:Press("Confirm")
	self:Wait(0.1)
	self:Expect(GM.Menu == "Slots", "빈 슬롯 이어하기 막힘")
	while GM.SlotIndex ~= Slot do
		self:Press("MenuDown")
		self:Wait(0.05)
	end
	self:Press("Confirm")
	self:Wait(0.5)
	self:Expect(GM.CurrentSlot == Slot, "슬롯 " .. Slot .. " 이어하기")
end

-- ================================================================ 동굴: 지도·미니맵·기록 막힘
function Pilot:MetaCaveChecks()
	local GM = self.GM
	local H = GM:MetaHud()
	self:Wait(0.3)
	self:Expect(H:HasMap() and H.Properties.Map == "Cave" and H.bMiniShown, "동굴 미니맵 표시")
	self:OpenPauseMenu()
	self:NavTo("Map")
	self:Wait(0.1)
	local Exits = 0
	for _, Mk in ipairs(GM:MapMarkers(false)) do if Mk.Kind == "Exit" then Exits = Exits + 1 end end
	self:Expect(GM.ShownPage == "Map" and string.find(H:W("MapImg").Texture, "Cave", 1, true) ~= nil and Exits >= 1, "동굴 지도 화면 (출구 표시)")
	self:Expect(not GM:IsNearSavePoint(), "동굴에는 기록 장소 없음")
	self:Press("Cancel")
	self:Wait(0.3)
end

-- ================================================================ 스크린샷 시나리오
function Pilot:MetaDressing()
	local GM = self.GM
	self:Wait(0.5)
	self:DemoLoadout(3)
	GM:AddItem("BatWing", 2, false)
	GM:AddItem("Spore", 3, false)
	GM:AddItem("GoblinFang", 1, false)
	GM:AddItem("CrystalShard", 1, false)
	GM.Upgrades.Sword = 1
	GM.Sub.Scarecrow = { State = "Active", Count = 1 }
	for Kind, N in pairs({ Slime = 14, Bat = 5, Goblin = 4, Archer = 3, Mushroom = 6 }) do
		GM.Bestiary.Seen[Kind] = N + 2
		GM.Bestiary.Kills[Kind] = N
	end
	local S = GM.Stats
	S.Kills, S.Chests, S.SubDone, S.GoldEarned, S.Distance, S.Deaths, S.Upgrades, S.Crafts, S.Saves, S.Materials = 32, 2, 1, 486, 184000, 1, 1, 2, 3, 11
	GM.PlayTime = 1834
	for Id in pairs(GM.Items) do GM.ItemsSeen[Id] = true end
	GM:RefreshQuest()
	self.Player:OnWeaponChanged()
	self:Wait(1.2)
end

function Pilot:RunPauseHome()
	self:MetaDressing()
	self:OpenPauseMenu()
	self:Note("일시정지 메뉴(상태)에서 대기")
	self:Idle()
end

function Pilot:RunJournal()
	self:MetaDressing()
	self.GM:SetTracked("Scarecrow")
	self:OpenPauseMenu()
	self:NavTo("Journal")
	self:Press("Confirm")
	self:Press("MenuRight")
	self:Wait(0.2)
	self:Note("퀘스트 일지(서브)에서 대기")
	self:Idle()
end

function Pilot:RunMapScreen()
	self:MetaDressing()
	self:OpenPauseMenu()
	self:NavTo("Map")
	self:Note("지도 화면에서 대기")
	self:Idle()
end

function Pilot:RunBestiary()
	self:MetaDressing()
	self:OpenPauseMenu()
	self:NavTo("Bestiary")
	self:Press("Confirm")
	self:Press("MenuDown")
	self:Press("MenuDown")
	self:Wait(0.2)
	self:Note("도감에서 대기")
	self:Idle()
end

function Pilot:RunRecords()
	self:MetaDressing()
	self:OpenPauseMenu()
	self:NavTo("Records")
	self:Note("모험 기록에서 대기")
	self:Idle()
end

function Pilot:RunSettings()
	self:MetaDressing()
	self:OpenPauseMenu()
	self:NavTo("Settings")
	self:Press("Confirm")
	self:Wait(0.2)
	self:Note("설정에서 대기")
	self:Idle()
end

function Pilot:RunSaveSlots()
	self:MetaDressing()
	local GM = self.GM
	GM:SaveToSlot(1)
	GM.Gold = GM.Gold + 250
	GM.PlayTime = GM.PlayTime + 2600
	GM:SaveToSlot(3)
	SaveGame.Delete(GM:SlotName(2))
	GM.CurrentSlot = 1
	GM:OpenSlots("Save")
	self:Wait(0.3)
	self:Note("저장 슬롯 창에서 대기")
	self:Idle()
end

function Pilot:RunLoadSlots()
	self:MetaDressing()
	local GM = self.GM
	GM:SaveToSlot(2)
	GM:OpenTitle()
	self:Wait(1.5)
	self:TitleChoose(2)
	self:Wait(0.3)
	self:Note("이어하기 슬롯 창에서 대기")
	self:Idle()
end

function Pilot:RunForge()
	self:MetaDressing()
	self.GM.Sub.Smith = { State = "Done", Count = 0 }
	self.GM:AddItem("Jelly", 2, false)
	self:TalkToNpc("Smith")
	self:TalkThrough()
	self:Wait(0.3)
	self:Press("MenuDown")
	self:Wait(0.2)
	self:Note("대장간(강화)에서 대기")
	self:Idle()
end

function Pilot:RunRecipe()
	self:MetaDressing()
	self.GM.Sub.Smith = { State = "Done", Count = 0 }
	self:TalkToNpc("Smith")
	self:TalkThrough()
	self:Wait(0.3)
	self:Press("MenuRight")
	self:Press("MenuDown")
	self:Wait(0.2)
	self:Note("대장간(조합)에서 대기")
	self:Idle()
end

function Pilot:RunMinimap()
	self:MetaDressing()
	self.GM:SetTracked("Scarecrow")
	self:Note("필드(미니맵)에서 대기")
	self:Idle()
end

function Pilot:RunConfirmTitle()
	self:MetaDressing()
	self:OpenPauseMenu()
	self:NavTo("Title")
	self:Press("Confirm")
	self:Wait(0.2)
	self:Note("타이틀로 확인 창에서 대기")
	self:Idle()
end

return Pilot
