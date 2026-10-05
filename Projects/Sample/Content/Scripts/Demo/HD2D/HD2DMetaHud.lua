-- HD-2D 데모 메타 UI (씬의 MetaUI 엔티티 — UI/Demo/HD2D/Meta.eui, Tools/DemoMap/HD2DMetaGen.py가 만든다. HUD 위 ZOrder 1).
--   관리자(HD2DGame.lua + HD2DPause/HD2DCrafting 모듈)가 값을 넘기고, 여기는 위젯 쓰기(바뀔 때만)·지도 좌표 변환·미니맵 갱신·단추 이벤트 전달만 한다.
--   지도: 속성 MapImage/MapBounds("최소X,최소Y,최대X,최대Y" — 그림이 덮는 월드 영역)/MapTitle/Landmarks("이름,X,Y,Place|Exit;...").
--     그림 위 = -Y(화면 안쪽), 오른쪽 = +X. 지도 쪽은 영역 MapArea 안에 비율 맞춰 넣고, 미니맵은 MiniScale(UI 단위/cm)로 플레이어 중심 —
--     스크롤 상자 MiniClip이 바깥을 잘라 준다(스크롤은 쓰지 않음). 표시물은 관리자 MapMarkers()가 준 목록 { Kind, Pos }.
--   미니맵은 플레이 중(메뉴 없음) + 설정 켬 + 지도 있음일 때만 보이고, 그동안 HUD 퀘스트 칸을 미니맵 아래로 내린다.
local Hud = { Properties = { Map = "", MapImage = "", MapBounds = "", MapTitle = "", Landmarks = "" } }

local MapArea = { X = 28, Y = 82, W = 832, H = 400 }  -- HD2DMetaGen.MAP_AREA
local MapBlockBelow = 96                               -- 지도 아래 목표 줄 + 범례 (지도 끝에서 범례 끝까지)
local MapMarkers, MapLabels, MiniMarkers = 40, 12, 20
local MiniClip = { X = 12, Y = 12, W = 228, H = 144 }  -- 미니맵 창 안 잘림 영역 (MINI = 252x168, 여백 12)
local MiniScale = 0.09
local QuestPanelY = { Normal = 14, Below = 190 }

function Hud:Init()
	if self.bInit then return end
	self.bInit = true
	self.Cache = {}
	local MinX, MinY, MaxX, MaxY = string.match(self.Properties.MapBounds or "", "([-%d%.]+),([-%d%.]+),([-%d%.]+),([-%d%.]+)")
	if MinX and self.Properties.MapImage ~= "" then
		self.Bounds = { MinX = tonumber(MinX), MinY = tonumber(MinY), MaxX = tonumber(MaxX), MaxY = tonumber(MaxY) }
	end
	self.Landmarks = {}
	for Name, X, Y, Kind in string.gmatch(self.Properties.Landmarks or "", "([^,;]+),([-%d%.]+),([-%d%.]+),(%a+)") do
		self.Landmarks[#self.Landmarks + 1] = { Name = Name, Pos = Vector3(tonumber(X), tonumber(Y), 0), Kind = Kind }
	end
	self.MiniList, self.MiniTimer = {}, 0
end

function Hud:OnStart()
	self:Init()
	self.GM = Scene.Find("HD2DGame"):GetScript()
	if self.Bounds then
		local B = self.Bounds
		self:Set("MiniImg", "Texture", self.Properties.MapImage)
		self:W("MiniImg").Size = Vector2(math.floor((B.MaxX - B.MinX) * MiniScale + 0.5), math.floor((B.MaxY - B.MinY) * MiniScale + 0.5))
		self:Set("MiniName", "Text", self.Properties.MapTitle)
	end
end

function Hud:OnDestroy()
	-- 데모가 덧붙인 ESC 바인딩을 되돌린다 (다른 데모·다음 실행에 남지 않게)
	if self.GM and self.GM.bEscapeBound then
		Input.ResetBindings("Inventory")
		self.GM.bEscapeBound = false
	end
end

function Hud:HasMap() return self.Bounds ~= nil end

function Hud:W(Name) return self.entity:GetWidget(Name) end

function Hud:Set(Name, Field, Value)
	self:Init()
	local Key = Name .. "." .. Field
	if self.Cache[Key] == Value then return end
	self.Cache[Key] = Value
	self:W(Name)[Field] = Value
end

function Hud:Show(Name, bShow, Mode)
	self:Set(Name, "Visibility", bShow and (Mode or "HitTestInvisible") or "Collapsed")
end

function Hud:SetColor(Name, R, G, B, A)
	self:Init()
	local Key = Name .. ".Color"
	local Value = string.format("%.3f,%.3f,%.3f,%.3f", R, G, B, A or 1)
	if self.Cache[Key] == Value then return end
	self.Cache[Key] = Value
	self:W(Name).Color = Vector4(R, G, B, A or 1)
end

function Hud:SetPos(Name, X, Y)
	self:Init()
	X, Y = math.floor(X + 0.5), math.floor(Y + 0.5)
	local Key = Name .. ".Pos"
	local Value = X * 100000 + Y
	if self.Cache[Key] == Value then return end
	self.Cache[Key] = Value
	self:W(Name).Position = Vector2(X, Y)
end

function Hud:SetSize(Name, W, H)
	self:Init()
	W, H = math.floor(W + 0.5), math.floor(H + 0.5)
	local Key = Name .. ".Size"
	local Value = W * 100000 + H
	if self.Cache[Key] == Value then return end
	self.Cache[Key] = Value
	self:W(Name).Size = Vector2(W, H)
end

-- 선택 줄 (HD2DMetaGen.RowButton): bSel = 선택 띠 + 커서, Dim = 글자 흐리게(0~1)
function Hud:SetRow(Name, bVisible, bSel, bCursor)
	self:Set(Name, "Visible", bVisible)
	if not bVisible then return end
	self:Show(Name .. "Sel", bSel)
	self:Set(Name .. "Cursor", "Visibility", (bSel and bCursor ~= false) and "HitTestInvisible" or "Hidden")
end

-- 글자 색: 선택(밝은 금빛 흰색) / 보통 / 흐림
function Hud:TextTone(Name, bSel, bDim)
	local C = bDim and 0.5 or 1.0
	self:SetColor(Name, (bSel and 1.0 or 0.92) * C, (bSel and 0.95 or 0.9) * C, (bSel and 0.82 or 0.86) * C, 1)
end

-- 탭 (Prefix0..N-1): Active = 1부터
function Hud:SetTabs(Prefix, Count, Active)
	for K = 0, Count - 1 do
		local bOn = (K + 1) == Active
		self:SetColor(Prefix .. "Text" .. K, bOn and 1.0 or 0.6, bOn and 0.85 or 0.58, bOn and 0.42 or 0.66, 1)
		self:SetColor(Prefix .. K, bOn and 0.32 or 0.1, bOn and 0.22 or 0.09, bOn and 0.12 or 0.18, bOn and 0.95 or 0.7)
	end
end

-- ================================================================ 지도
function Hud:MapRect()
	local B = self.Bounds
	local Scale = math.min(MapArea.W / (B.MaxX - B.MinX), MapArea.H / (B.MaxY - B.MinY))
	local W, H = (B.MaxX - B.MinX) * Scale, (B.MaxY - B.MinY) * Scale
	-- 지도 + 목표 줄 + 범례 묶음을 쪽 안(82~620)에서 세로 가운데로
	local Y0 = MapArea.Y + math.max(0, (538 - (H + MapBlockBelow)) * 0.5)
	return MapArea.X + (MapArea.W - W) * 0.5, Y0, W, H, Scale
end

-- 지도 쪽: 그림·지명·표시물·플레이어 (Markers = { { Kind, Pos } }, PlayerPos)
function Hud:DrawMapPage(Markers, PlayerPos)
	self:Init()
	local bMap = self.Bounds ~= nil
	self:Show("MapImg", bMap)
	self:Show("MapFrameLine", bMap)
	self:Show("MapNoMap", not bMap)
	self:Show("MapPlayer", bMap)
	if not bMap then
		for I = 0, MapMarkers - 1 do self:Show("MapMk" .. I, false) end
		for I = 0, MapLabels - 1 do self:Show("MapLbl" .. I, false) end
		return
	end
	local B = self.Bounds
	local X0, Y0, W, H, Scale = self:MapRect()
	self:Set("MapImg", "Texture", self.Properties.MapImage)
	self:SetPos("MapImg", X0, Y0)
	self:SetSize("MapImg", W, H)
	self:SetPos("MapFrameLine", X0, Y0)
	self:SetSize("MapFrameLine", W, H)
	self:SetPos("MapBack", X0 - 6, Y0 - 6)
	self:SetSize("MapBack", W + 12, H + 12)
	self:SetPos("MapGoalRow", 32, Y0 + H + 26)
	self:SetPos("MapLegend", 32, Y0 + H + 70)
	local function ToMap(P)
		return X0 + (math.max(B.MinX, math.min(B.MaxX, P.X)) - B.MinX) * Scale, Y0 + (math.max(B.MinY, math.min(B.MaxY, P.Y)) - B.MinY) * Scale
	end
	for I = 0, MapLabels - 1 do
		local L = self.Landmarks[I + 1]
		self:Show("MapLbl" .. I, L ~= nil)
		if L then
			local X, Y = ToMap(L.Pos)
			self:Set("MapLbl" .. I, "Text", L.Name)
			self:SetPos("MapLbl" .. I, X, Y + (L.Kind == "Exit" and 20 or 0))
			if L.Kind == "Exit" then self:SetColor("MapLbl" .. I, 0.7, 0.9, 1.0, 1) else self:SetColor("MapLbl" .. I, 1.0, 0.96, 0.84, 1) end
		end
	end
	for I = 0, MapMarkers - 1 do
		local M = Markers[I + 1]
		self:Show("MapMk" .. I, M ~= nil)
		if M then
			local X, Y = ToMap(M.Pos)
			self:Set("MapMk" .. I, "Texture", "UI/Demo/HD2D/Meta/Mk" .. M.Kind .. ".png")
			local Size = (M.Kind == "Goal" or M.Kind == "Boss") and 26 or 22
			self:SetSize("MapMk" .. I, Size, Size)
			self:SetPos("MapMk" .. I, X, Y)
		end
	end
	if PlayerPos then
		local X, Y = ToMap(PlayerPos)
		self:SetPos("MapPlayer", X, Y)
	end
end

-- ================================================================ 미니맵 (매 프레임 — 플레이어 중심)
function Hud:RefreshMinimapVisibility()
	self:Init()
	local GM = self.GM
	local bShow = self.Bounds ~= nil and GM ~= nil and GM.Settings ~= nil and GM.Settings.Minimap and GM.Menu == nil and GM.Mode == "Play"
	if self.bMiniShown ~= bShow then
		self.bMiniShown = bShow
		self:Show("Minimap", bShow)
		-- HUD 퀘스트 칸을 미니맵 아래로 (HUD 위젯 — 캔버스 자식이라 위치만 바꾼다)
		local H = GM and GM:Hud()
		local Panel = H and H.entity:GetWidget("QuestPanel")
		if Panel then Panel.Position = Vector2(-16, bShow and QuestPanelY.Below or QuestPanelY.Normal) end
	end
	return bShow
end

function Hud:UpdateMinimap(UDt)
	if not self:RefreshMinimapVisibility() then return end
	local GM = self.GM
	local Player = GM:GetPlayer()
	if not Player then return end
	local P = Player.entity:GetWorldPosition()
	local B = self.Bounds
	local CX, CY = MiniClip.W * 0.5, MiniClip.H * 0.5
	self:SetPos("MiniImg", CX - (P.X - B.MinX) * MiniScale, CY - (P.Y - B.MinY) * MiniScale)
	self.MiniTimer = self.MiniTimer - UDt
	if self.MiniTimer <= 0 then
		self.MiniTimer = 0.3
		self.MiniList = GM:MapMarkers(true)
	end
	local Shown = 0
	for _, M in ipairs(self.MiniList) do
		local X, Y = CX + (M.Pos.X - P.X) * MiniScale, CY + (M.Pos.Y - P.Y) * MiniScale
		-- 창 밖 표시물은 가장자리에 붙인다 (목표·출구만 — 나머지는 숨김)
		local bEdge = M.Kind == "Goal" or M.Kind == "Exit" or M.Kind == "Boss"
		local bInside = X >= 6 and X <= MiniClip.W - 6 and Y >= 6 and Y <= MiniClip.H - 6
		if (bInside or bEdge) and Shown < MiniMarkers then
			X, Y = math.max(8, math.min(MiniClip.W - 8, X)), math.max(8, math.min(MiniClip.H - 8, Y))
			local Name = "MiniMk" .. Shown
			self:Show(Name, true)
			self:Set(Name, "Texture", "UI/Demo/HD2D/Meta/Mk" .. M.Kind .. ".png")
			self:SetPos(Name, X, Y)
			Shown = Shown + 1
		end
	end
	for I = Shown, MiniMarkers - 1 do self:Show("MiniMk" .. I, false) end
end

function Hud:OnLateUpdate(Dt)
	self:Init()
	if not self.GM then return end
	self:UpdateMinimap(Time.GetUnscaledDelta())
	if self.GM.UpdateMetaUi then self.GM:UpdateMetaUi(Time.GetUnscaledDelta()) end
end

-- ================================================================ 단추 (마우스) → 관리자
for I = 0, 9 do
	Hud["OnUIClicked_PNav" .. I] = function(self) self.GM:OnMetaClicked("Nav", I + 1) end
	Hud["OnUIHoverBegin_PNav" .. I] = function(self) self.GM:OnMetaHovered("Nav", I + 1) end
end
for I = 0, 8 do
	Hud["OnUIClicked_JRow" .. I] = function(self) self.GM:OnMetaClicked("Journal", I + 1) end
	Hud["OnUIClicked_BRow" .. I] = function(self) self.GM:OnMetaClicked("Bestiary", I + 1) end
end
for I = 0, 5 do
	Hud["OnUIClicked_SRow" .. I] = function(self) self.GM:OnMetaClicked("Settings", I + 1) end
end
for I = 0, 2 do
	Hud["OnUIClicked_SlotCard" .. I] = function(self) self.GM:OnMetaClicked("Slot", I + 1) end
	Hud["OnUIHoverBegin_SlotCard" .. I] = function(self) self.GM:OnMetaHovered("Slot", I + 1) end
end
for I = 0, 1 do
	Hud["OnUIClicked_ConfirmBtn" .. I] = function(self) self.GM:OnMetaClicked("Confirm", I + 1) end
	Hud["OnUIHoverBegin_ConfirmBtn" .. I] = function(self) self.GM:OnMetaHovered("Confirm", I + 1) end
end
for I = 0, 9 do
	Hud["OnUIClicked_ForgeRow" .. I] = function(self) self.GM:OnMetaClicked("Forge", I + 1) end
	Hud["OnUIHoverBegin_ForgeRow" .. I] = function(self) self.GM:OnMetaHovered("Forge", I + 1) end
end

return Hud
