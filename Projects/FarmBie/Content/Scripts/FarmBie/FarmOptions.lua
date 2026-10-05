-- FarmBie 선택지 창(OptWindow)과 설정 — 게임(일시정지)·타이틀이 같이 쓴다. 위젯 이름은 Tools/FarmBieUI.py OptionWindow와 약속이다.
--   쪽(Page) = { Title, Sub, Items = { { Label, Value = fn() → 글|nil, Act = fn(), Adjust = fn(-1|1) } }, Back = fn() }
--   상태는 주인 표에 (Owner.OptPage / OptIndex / 입력 반복) — 이 모듈은 상태를 갖지 않는다 (Script.Require 값은 공유).
--   설정: 효과음·환경음 음량은 저장 슬롯 "FarmBie_Settings", 수직 동기화·화면 모드는 엔진 사용자 설정(Game.SetVSync/SetWindowMode가 저장)
local O = {}

O.Rows = 7
O.SettingsSlot = "FarmBie_Settings"

function O.LoadSettings()
	local S = { Sfx = 0.8, Ambient = 0.6 }
	local T = SaveGame.Load(O.SettingsSlot)
	if type(T) == "table" then
		S.Sfx = math.max(0, math.min(1, tonumber(T.Sfx) or S.Sfx))
		S.Ambient = math.max(0, math.min(1, tonumber(T.Ambient) or S.Ambient))
	end
	return S
end

function O.SaveSettings(S)
	SaveGame.Save(O.SettingsSlot, { Sfx = S.Sfx, Ambient = S.Ambient })
end

-- 이동 축 → 메뉴 입력 (누른 순간 + 누르고 있으면 0.35초 뒤 0.11초마다 반복). FarmPlayer:MenuNavigation과 같은 규칙
function O.ReadInput(Owner)
	local MX, MY = Input.GetAction("Move")
	local In = { Interact = Input.WasActionPressed("Interact"), UseTool = Input.WasActionPressed("UseTool"),
	             Pause = Input.WasActionPressed("Pause"), Dodge = Input.WasActionPressed("Dodge") }
	local DirY = MY > 0.5 and 1 or (MY < -0.5 and -1 or 0)
	local DirX = MX > 0.5 and 1 or (MX < -0.5 and -1 or 0)
	local Held = DirY * 3 + DirX
	if Held ~= 0 and Held ~= Owner.MenuHeld then
		Owner.MenuRepeat = 0.35
		In.MenuUp, In.MenuDown, In.MenuLeft, In.MenuRight = DirY == 1, DirY == -1, DirX == -1, DirX == 1
	elseif Held ~= 0 then
		Owner.MenuRepeat = (Owner.MenuRepeat or 0) - Time.GetUnscaledDelta()
		if Owner.MenuRepeat <= 0 then
			Owner.MenuRepeat = 0.11
			In.MenuUp, In.MenuDown, In.MenuLeft, In.MenuRight = DirY == 1, DirY == -1, DirX == -1, DirX == 1
		end
	end
	Owner.MenuHeld = Held
	In.Confirm = In.Interact or In.UseTool
	In.Cancel = In.Pause or In.Dodge
	return In
end

function O.Open(Owner, Hud, Page, Index)
	Owner.OptPage = Page
	Owner.OptIndex = Index or 1
	Hud:Show("OptWindow", true)
	O.Draw(Owner, Hud)
end

function O.Close(Owner, Hud)
	Owner.OptPage = nil
	Hud:Show("OptWindow", false)
end

function O.Draw(Owner, Hud)
	local P = Owner.OptPage
	if not P then return end
	Hud:Set("OptTitle", "Text", P.Title or "")
	Hud:Set("OptSub", "Text", P.Sub or "")
	for Row = 0, O.Rows - 1 do
		local Item = P.Items[Row + 1]
		Hud:Show("OptRow" .. Row, Item ~= nil)
		if Item then
			Hud:Show("OptSel" .. Row, Row + 1 == Owner.OptIndex)
			Hud:Show("OptIcon" .. Row, false)
			Hud:Set("OptName" .. Row, "Text", Item.Label)
			local Value = Item.Value and Item.Value() or ""
			if Item.Adjust and Row + 1 == Owner.OptIndex then Value = "◀  " .. Value .. "  ▶" end
			Hud:Set("OptPrice" .. Row, "Text", Value)
			Hud:Set("OptStock" .. Row, "Text", "")
		end
	end
end

-- 입력 처리. Sound(이름)은 효과음 콜백 (없어도 됨)
function O.Input(Owner, Hud, In, Sound)
	local P = Owner.OptPage
	if not P then return end
	local Count = #P.Items
	local Item = P.Items[Owner.OptIndex]
	local function Play(Name) if Sound then Sound(Name) end end
	if In.MenuUp and Owner.OptIndex > 1 then Owner.OptIndex = Owner.OptIndex - 1 Play("Click") end
	if In.MenuDown and Owner.OptIndex < Count then Owner.OptIndex = Owner.OptIndex + 1 Play("Click") end
	Item = P.Items[Owner.OptIndex]
	if Item and Item.Adjust and (In.MenuLeft or In.MenuRight) then
		Item.Adjust(In.MenuLeft and -1 or 1)
		Play("Click")
	end
	if In.Confirm and Item then
		if Item.Act then
			Play("Confirm")
			Item.Act()
		elseif Item.Adjust then
			Item.Adjust(1)
			Play("Click")
		end
	elseif In.Cancel and P.Back then
		Play("Close")
		P.Back()
	end
	if Owner.OptPage then O.Draw(Owner, Hud) end
end

-- 예/아니오 확인 쪽 (아니오가 먼저 — 실수로 넘기지 않게)
function O.ConfirmPage(Title, Sub, OnYes, OnNo)
	return { Title = Title, Sub = Sub, Back = OnNo, Items = {
		{ Label = "아니오", Act = OnNo },
		{ Label = "예", Act = OnYes },
	} }
end

-- 설정 쪽. OnChanged(설정) = 음량을 바꿀 때마다, OnBack = 뒤로 (닫을 때 저장)
function O.SettingsPage(S, OnChanged, OnBack)
	local function Percent(V) return string.format("%d%%", math.floor(V * 100 + 0.5)) end
	local function Step(Field, Dir)
		S[Field] = math.max(0, math.min(1, math.floor(S[Field] * 10 + 0.5) / 10 + Dir * 0.1))
		if OnChanged then OnChanged(S) end
	end
	local function Back()
		O.SaveSettings(S)
		OnBack()
	end
	return { Title = "설정", Sub = "◀ ▶ 로 바꾸기", Back = Back, Items = {
		{ Label = "효과음 음량", Value = function() return Percent(S.Sfx) end, Adjust = function(Dir) Step("Sfx", Dir) end },
		{ Label = "환경음 음량", Value = function() return Percent(S.Ambient) end, Adjust = function(Dir) Step("Ambient", Dir) end },
		{ Label = "수직 동기화", Value = function() return Game.IsVSync() and "켬" or "끔" end,
		  Act = function() Game.SetVSync(not Game.IsVSync()) end },
		{ Label = "화면 모드", Value = function() return Game.GetWindowMode() == "Windowed" and "창 모드" or "전체 화면" end,
		  Act = function() Game.SetWindowMode(Game.GetWindowMode() == "Windowed" and "BorderlessFullscreen" or "Windowed") end },
		{ Label = "뒤로", Act = Back },
	} }
end

-- 저장 슬롯 요약 ("1년차 봄 12일 · 1520골드") | nil
function O.SlotSummary(SlotName)
	local T = SaveGame.Load(SlotName)
	if type(T) ~= "table" or not T.Day then return nil end
	local Seasons = { "봄", "여름", "가을", "겨울" }
	return string.format("%d년차 %s %d일 · %d골드", math.floor(T.Year or 1), Seasons[math.floor(T.Season or 0) + 1] or "?", math.floor(T.Day), math.floor(T.Gold or 0))
end

return O
