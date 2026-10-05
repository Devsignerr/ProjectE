-- HD-2D 데모 관리자 확장 ④ 세계 (HD2DGame.lua가 Script.Require로 받아 메서드로 붙인다 — 상태는 관리자 self에). 4차 세계 확장:
--   지역 이름: 맵에 들어서면 위 가운데 "─ 갈매기 항구 ─" 띠 + 부제 (Regions.etable — 행 이름 = 관리자 Map 속성).
--   낮밤: 세 맵 공통 게임 시각 GameHour (DayNight.edata — 실제 DayMinutes분 = 하루, 게임 시간이라 메뉴로 멈춘다, 저장·세션에 유지).
--     하늘 = Sky.SetTimeOfDay(시각 — 0.02시 단위로 끊어 그림자 캐시가 사이사이 재사용되게), 열쇠 표(DayNightKeys.etable)를 보간해
--     카메라 색 보정·비네트, 하늘빛(SkyLight), 높이 안개 색, 방향광 세기를 바꾼다. 등불(Regions.NightLights 이름 패턴 — #은 0부터 번호)은
--     켬 정도(LampOn~LampFull, LampDim~LampOff)를 곱하고, 창 유리(NightWindows)는 반쯤 켜지면 불 켠 머티리얼로. 동굴(DayNight = false)은
--     입구 빛(DayLights)만 낮 밝기를 따른다.
--   밤: NightStart~NightEnd. 관리자 NightEnemies("종류,x,y,z;...") = 밤에만 나오는 적(해 뜨면 사라짐), NightOnly 주민(유령 선원)은 밤에만 보이고
--     말을 걸 수 있다. 대화·상점 주인은 밤에 NightLines (상점 주인은 밤에 문을 닫는다).
--   여관(Role Inn): 하룻밤 InnPrice 골드 → 화면이 어두워졌다 SleepHour(다음 날 아침)로 + 체력·마나·BP 회복.
--   항구 등대: 서브 퀘스트 "Lighthouse"를 마치면 밤마다 등실 유리·심지가 켜지고 빛줄기(스포트)가 돈다 (Lighthouse_*).
--   항구 소품 Lens: 서브 퀘스트 "Lighthouse"를 받은 뒤 렌즈가 없을 때만 보이고 주울 수 있다.
--   갈매기: 관리자 Gulls("x,y,z,반지름;...") 둘레를 도는 장식 갈매기 (효과 조각 — 시간에 따라 위치만).
--   상점 진열·제목·주인 말은 주민 행(ShopStock/ShopTitle/ShopLines)이 있으면 그것, 없으면 Balance (HD2DMenu가 ShopStockList/ShopLine으로 읽는다).
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local World = {}
local IconDir = "UI/Demo/HD2D/Icons/"
local HarborMat = "Materials/Demo/HD2D/Harbor/"
local WindowLit, WindowDark = "Materials/Demo/HD2D/EnvWindowLit.emat", "Materials/Demo/HD2D/EnvWindowDark.emat"

local function Flat(V) return Vector3(V.X, V.Y, 0) end
local function Lerp(A, B, T) return A + (B - A) * T end
local function Smooth(E0, E1, X)
	local T = math.max(0, math.min(1, (X - E0) / (E1 - E0)))
	return T * T * (3 - 2 * T)
end

-- 이름 패턴(#은 0부터 번호 — 없는 번호에서 멈춤)에 맞는 엔티티들
local function FindPattern(Pattern, Out)
	if string.find(Pattern, "#", 1, true) then
		for I = 0, 255 do
			local E = Scene.Find((string.gsub(Pattern, "#", tostring(I))))
			if not E then break end
			Out[#Out + 1] = E
		end
	else
		local E = Scene.Find(Pattern)
		if E then Out[#Out + 1] = E end
	end
	return Out
end

-- ================================================================ 시작
function World:InitWorld(bSession)
	local DN = D.DayNight()
	self.Region = D.Region(self.Properties.Map)
	if self.GameHour == nil then self.GameHour = DN and DN.StartHour or 12.0 end
	self.WorldTime = 0
	-- 장면 쪽 기본값 (열쇠 배율의 기준)
	local Sky = Scene.Find("Sky")
	self.SkyLight = Sky and Sky:GetComponent("SkyLightComponent") or nil
	self.Fog = Sky and Sky:GetComponent("HeightFogComponent") or nil
	self.SkyBase = self.SkyLight and self.SkyLight.Intensity or 1.0
	self.Atmosphere = Sky and Sky:GetComponent("SkyAtmosphereComponent") or nil
	local Sun = Scene.Find("Sun")
	self.SunLight = Sun and Sun:GetComponent("DirectionalLightComponent") or nil
	self.SunBase = self.SunLight and self.SunLight.Intensity or 1.0
	local Cam = Scene.Find("Camera")
	self.Grading = Cam and Cam:GetComponent("ColorGradingComponent") or nil
	self.Vignette = Cam and Cam:GetComponent("VignetteComponent") or nil
	-- 등불·창·낮 빛
	self.NightLights, self.NightWindows, self.DayLights = {}, {}, {}
	local R = self.Region
	if R then
		for _, Pattern in ipairs(R.NightLights or {}) do
			for _, E in ipairs(FindPattern(Pattern, {})) do
				local L = E:GetComponent("PointLightComponent") or E:GetComponent("SpotLightComponent")
				if L then
					local S = E:GetScript()
					self.NightLights[#self.NightLights + 1] = { Light = L, Script = S, Base = (S and S.Properties.BaseIntensity) or L.Intensity, Radius = L.Radius }
				end
			end
		end
		if R.NightWindows ~= "" then
			for _, E in ipairs(FindPattern(R.NightWindows, {})) do
				local M = E:GetComponent("StaticMeshComponent")
				if M then self.NightWindows[#self.NightWindows + 1] = { Mesh = M, Lit = M.MaterialAsset } end
			end
		end
		for _, Pattern in ipairs(R.DayLights or {}) do
			for _, E in ipairs(FindPattern(Pattern, {})) do
				local L = E:GetComponent("PointLightComponent") or E:GetComponent("SpotLightComponent")
				if L then self.DayLights[#self.DayLights + 1] = { Light = L, Base = L.Intensity } end
			end
		end
	end
	-- 항구 등대
	local Lamp = Scene.Find("Lighthouse_Lamp")
	if Lamp then
		self.Lighthouse = { Lamp = Lamp:GetComponent("PointLightComponent"), Glass = {}, Angle = 0 }
		local Beam = Scene.Find("Lighthouse_Beam")
		self.Lighthouse.BeamEntity = Beam
		self.Lighthouse.Beam = Beam and Beam:GetComponent("SpotLightComponent") or nil
		self.Lighthouse.Cone = Scene.Find("Lighthouse_BeamCone")
		FindPattern("Lighthouse_Glass_#", self.Lighthouse.Glass)
		local Core = Scene.Find("Lighthouse_Core")
		if Core then self.Lighthouse.Glass[#self.Lighthouse.Glass + 1] = Core end
	end
	-- 밤 적 자리
	self.NightSlots = {}
	for Item in string.gmatch(self.Properties.NightEnemies or "", "[^;]+") do
		local Kind, X, Y, Z = string.match(Item, "^(%a+),([-%d%.]+),([-%d%.]+),([-%d%.]+)$")
		if Kind then self.NightSlots[#self.NightSlots + 1] = { Kind = Kind, Pos = Vector3(tonumber(X), tonumber(Y), tonumber(Z)) } end
	end
	self.NightSpawned = {}
	self.bNight = nil
	-- 장식 갈매기
	self.Gulls = {}
	local GIndex = 0
	for Item in string.gmatch(self.Properties.Gulls or "", "[^;]+") do
		local X, Y, Z, Rad = string.match(Item, "([-%d%.]+),([-%d%.]+),([-%d%.]+),([-%d%.]+)")
		if X then
			GIndex = GIndex + 1
			local G = { Center = Vector3(tonumber(X), tonumber(Y), tonumber(Z)), Radius = tonumber(Rad), Phase = GIndex * 1.7, Speed = 0.22 + GIndex * 0.03 }
			G.Fx = self:SpawnSprite({ Sprite = "Sprites/HD2D/HarborProps.esprite", Flipbook = "Sprites/HD2D/Harbor_GullFly.eflipbook", Position = G.Center,
			                          Blend = 3, Lit = true, Life = 1.0e9, Scale = 1.3 })
			self.Gulls[#self.Gulls + 1] = G
		end
	end
	self.Report.NightSpawns, self.Report.NightTalks, self.Report.Sleeps, self.Report.LighthouseOn = 0, 0, 0, 0
	self.Report.Regions = 0
	self:ApplyDayNight(true)
	-- 지역 이름 (첫 갱신에서 — HUD 준비 뒤)
	self.bShowRegion = self.Region ~= nil and self.Mode ~= "Title"
	Log.Info(string.format("[HD2D] 세계: 지역 %s, 시각 %.2f, 등불 %d, 창 %d, 밤 적 자리 %d, 갈매기 %d", R and R.DisplayName or "-", self.GameHour,
		#self.NightLights, #self.NightWindows, #self.NightSlots, #self.Gulls))
end

-- ================================================================ 시각
function World:HourText()
	local H = math.floor(self.GameHour) % 24
	local M = math.floor((self.GameHour - math.floor(self.GameHour)) * 60)
	return string.format("%02d:%02d", H, M)
end

function World:IsNight()
	local DN = D.DayNight()
	local H = self.GameHour % 24
	return H >= DN.NightStart or H < DN.NightEnd
end

-- 등불 켬 정도 0~1 (저녁 LampOn→LampFull 켜짐, 새벽 LampDim→LampOff 꺼짐)
function World:LampLevel()
	local DN = D.DayNight()
	local H = self.GameHour % 24
	if H >= 12 then return Smooth(DN.LampOn, DN.LampFull, H) end
	return 1.0 - Smooth(DN.LampDim, DN.LampOff, H)
end

function World:SetGameHour(Hour)
	self.GameHour = Hour % 24
	self:ApplyDayNight(true)
end

-- 열쇠 표 보간 (24시 = 0시로 감음)
function World:SampleKeys(Hour)
	local Keys = D.DayNightKeys()
	if not Keys or #Keys == 0 then return nil end
	local H = Hour % 24
	local Prev, Next = Keys[#Keys], Keys[1]
	local PH, NH = Prev.Hour - 24, Next.Hour
	for I = 1, #Keys do
		if Keys[I].Hour > H then
			Next, NH = Keys[I], Keys[I].Hour
			Prev = Keys[I - 1] or Keys[#Keys]
			PH = Keys[I - 1] and Keys[I - 1].Hour or (Keys[#Keys].Hour - 24)
			break
		end
		Prev, PH = Keys[I], Keys[I].Hour
		Next, NH = Keys[I + 1] or Keys[1], Keys[I + 1] and Keys[I + 1].Hour or (Keys[1].Hour + 24)
	end
	local T = (NH - PH) > 1e-4 and (H - PH) / (NH - PH) or 0
	local Out = {}
	for K, V in pairs(Prev) do
		local N = Next[K]
		if type(V) == "number" and type(N) == "number" then
			Out[K] = Lerp(V, N, T)
		elseif type(V) == "table" and type(N) == "table" then
			Out[K] = { Lerp(V[1], N[1], T), Lerp(V[2], N[2], T), Lerp(V[3], N[3], T) }
		end
	end
	return Out
end

function World:ApplyDayNight(bForce)
	local R = self.Region
	if not R then return end
	local Lamp = self:LampLevel()
	if R.DayNight then
		-- 하늘 (0.02시 단위 — 그림자 캐시가 끊긴 사이에는 재사용된다)
		local Q = math.floor(self.GameHour / 0.02 + 0.5) * 0.02
		local bStep = bForce or Q ~= self.SkyHour
		if bStep then
			self.SkyHour = Q
			Sky.SetTimeOfDay(Q)
		end
		-- 화면 열쇠도 같은 단위로만 (색 보정 LUT 굽기·환경광 갱신을 매 프레임 일으키지 않게)
		local K = bStep and self:SampleKeys(Q) or nil
		if K then
			if self.SkyLight then self.SkyLight.Intensity = self.SkyBase * K.SkyScale end
			if self.SunLight then self.SunLight.Intensity = self.SunBase * K.SunScale end
			local G = self.Grading
			if G then
				G.Temperature, G.Tint, G.Saturation, G.Contrast = K.Temperature, K.Tint, K.Saturation, K.Contrast
				G.Lift = Vector3(K.Lift[1], K.Lift[2], K.Lift[3])
				G.Gamma = Vector3(K.Gamma[1], K.Gamma[2], K.Gamma[3])
				G.Gain = Vector3(K.Gain[1] * K.Exposure, K.Gain[2] * K.Exposure, K.Gain[3] * K.Exposure)
			end
			if self.Vignette then self.Vignette.Intensity = K.Vignette end
			if self.Atmosphere then self.Atmosphere.MoonIntensity = K.Moon end
			if self.Fog then self.Fog.VolumetricLocalLightScale = K.FogLocal end
			self.LampScale = K.LampScale
			if self.Fog then
				self.Fog.Color = Vector3(K.Fog[1], K.Fog[2], K.Fog[3])
				self.Fog.DirectionalInscatteringColor = Vector3(K.Inscatter[1], K.Inscatter[2], K.Inscatter[3])
			end
		end
		-- 등불 (점광원 세기 = 기본 × 켬 정도 × 밤 배율, 깜빡이는 등은 깜빡임 스크립트의 기본 세기)
		local Level = Lamp * (self.LampScale or 1.0)
		if bForce or math.abs(Level - (self.AppliedLamp or -1)) > 0.004 then
			self.AppliedLamp = Level
			for _, L in ipairs(self.NightLights) do
				if L.Script then L.Script.Properties.BaseIntensity = L.Base * Level end
				L.Light.Intensity = L.Base * Level
				L.Light.Radius = L.Radius * (0.8 + 0.4 * Level) -- 밤에는 빛 웅덩이를 넓게 (켬 2.0이면 1.6배)
			end
			local bLit = Lamp > 0.5
			if bForce or bLit ~= self.bWindowsLit then
				self.bWindowsLit = bLit
				for _, W in ipairs(self.NightWindows) do W.Mesh.MaterialAsset = bLit and W.Lit or WindowDark end
			end
		end
	else
		-- 동굴: 입구 빛만 낮 밝기 (밤엔 달빛 20%)
		local Day = 1.0 - Lamp * 0.8
		for _, L in ipairs(self.DayLights) do L.Light.Intensity = L.Base * Day end
	end
	self:ApplyLighthouse(Lamp)
end

function World:ApplyLighthouse(Lamp)
	local LH = self.Lighthouse
	if not LH then return end
	local bOn = self:SubState("Lighthouse") == "Done" and Lamp > 0.3
	if bOn ~= LH.bOn then
		LH.bOn = bOn
		for _, E in ipairs(LH.Glass) do
			local M = E:GetComponent("StaticMeshComponent")
			if M then M.MaterialAsset = HarborMat .. (bOn and "LampOn.emat" or "LampOff.emat") end
		end
		if bOn then
			self.Report.LighthouseOn = self.Report.LighthouseOn + 1
			Log.Info("[HD2D] 등대 불 켜짐")
		end
	end
	if LH.Lamp then LH.Lamp.Intensity = bOn and 26.0 * Lamp or 0.0 end
	if LH.Beam then LH.Beam.Intensity = bOn and 1600.0 * Lamp or 0.0 end
	if LH.Cone then
		local S = bOn and math.max(0.001, Lamp) or 0.001 -- 끄면 아주 작게 (0 배율은 역행렬이 없다)
		if S ~= LH.ConeScale then
			LH.ConeScale = S
			LH.Cone:SetScale(Vector3(S, S, S))
		end
	end
end

-- ================================================================ 갱신 (관리자 OnUpdate — 게임 시간 Dt)
function World:UpdateWorld(Dt)
	local UDt = Time.GetUnscaledDelta()
	self.WorldTime = self.WorldTime + UDt
	if self.bShowRegion then
		self.bShowRegion = false
		self:ShowRegion()
	end
	if not self.bCameraBounds then
		local Player = self:GetPlayer()
		if Player and Player.bStarted then
			self.bCameraBounds = true
			if (self.Properties.CameraMaxY or 0) ~= 0 then
				Player.Properties.MaxY = self.Properties.CameraMaxY
				Player:SnapCamera()
			end
		end
	end
	self:UpdateRegionBanner(UDt)
	local DN = D.DayNight()
	if DN and Dt > 0 and self.Mode ~= "Title" and self.Mode ~= "Travel" then
		self.GameHour = (self.GameHour + Dt * 24.0 / (DN.DayMinutes * 60.0)) % 24
	end
	self:ApplyDayNight(false)
	-- 등대 빛줄기 회전 (초당 50도)
	local LH = self.Lighthouse
	if LH and LH.bOn and LH.BeamEntity and Dt > 0 then
		LH.Angle = (LH.Angle + Dt * 50.0) % 360
		LH.BeamEntity:SetRotation(Quat.FromEuler(-4.0, LH.Angle, 0.0))
	end
	-- 밤 적·밤 주민
	local bNight = self:IsNight()
	if bNight ~= self.bNight then
		local bFirst = self.bNight == nil
		self.bNight = bNight
		self:OnNightChanged(bNight, bFirst)
	end
	-- 갈매기
	for _, G in ipairs(self.Gulls) do
		if G.Fx.Entity then
			local A = self.WorldTime * G.Speed + G.Phase
			local P = G.Center + Vector3(math.cos(A) * G.Radius, math.sin(A) * G.Radius * 0.45, math.sin(A * 2.0) * 30.0)
			G.Fx.Entity:SetPosition(P)
			local bLeft = -math.sin(A) < 0
			if bLeft ~= G.bLeft then
				G.bLeft = bLeft
				G.Fx.Entity:SetSpriteFlip(bLeft, false)
			end
		end
	end
	-- 시계
	local H = self:Hud()
	if H and self.Region then
		H:Set("ClockText", "Text", self:HourText())
		H:Set("ClockIcon", "Texture", IconDir .. ((self:LampLevel() > 0.5) and "Moon.png" or "Sun.png"))
	end
end

function World:OnNightChanged(bNight, bFirst)
	-- 밤 적: 밤이 되면 자리마다 만들고, 해 뜨면 남은 것을 펑 효과와 함께 지운다
	if bNight then
		for _, Slot in ipairs(self.NightSlots) do
			Scene.SpawnPrefab("Prefabs/Demo/HD2D/" .. Slot.Kind .. ".eprefab", Slot.Pos, function(E) self.NightSpawned[#self.NightSpawned + 1] = E end)
			self.Report.NightSpawns = self.Report.NightSpawns + 1
		end
		if #self.NightSlots > 0 then Log.Info(string.format("[HD2D] 밤: 망령 %d 나타남 (%s)", #self.NightSlots, self:HourText())) end
	else
		for _, E in ipairs(self.NightSpawned) do
			if E:IsValid() then
				self:SpawnFx("Poof", E:GetWorldPosition() + Vector3(0, 10, 0), { Scale = 1.2, Color = { 0.8, 0.9, 1, 1 } })
				E:Destroy()
			end
		end
		self.NightSpawned = {}
	end
	-- 밤 주민 (유령 선원): 밤에만 보이고 말을 걸 수 있다
	for _, Npc in ipairs(self.Npcs) do
		if Npc.Row.NightOnly then self:SetNpcHidden(Npc, not bNight) end
	end
	if self.Region and not bFirst then Log.Info(string.format("[HD2D] %s (%s)", bNight and "밤이 되었다" or "아침이 되었다", self:HourText())) end
end

function World:SetNpcHidden(Npc, bHidden)
	Npc.bHidden = bHidden
	if not Npc.Entity then return end
	local Body = Npc.Entity:FindChild("Body")
	local Shadow = Npc.Entity:FindChild("Shadow")
	if Body then Body:GetComponent("SpriteComponent").Visible = not bHidden end
	if Shadow then Shadow:GetComponent("SpriteComponent").Visible = not bHidden end
	if bHidden and Npc.Marker then Npc.Marker:GetComponent("SpriteComponent").Visible = false end
	-- 숨으면 막지도 않는다 (콜라이더째 땅속으로)
	Npc.Entity:SetPosition(bHidden and (Npc.Pos - Vector3(0, 0, 2000)) or Npc.Pos)
end

-- 관리자가 마을 사람을 만든 직후 (밤 주민은 낮이면 숨김)
function World:OnNpcSpawned(Npc)
	if Npc.Row.NightOnly then
		self:SetNpcHidden(Npc, not self:IsNight())
	end
	if Npc.Row.NightOnly then
		local S = Npc.Body:GetComponent("SpriteComponent")
		S.Lit = false  -- 스스로 빛나는 유령
	end
end

-- ================================================================ 지역 이름 띠
function World:ShowRegion()
	local R = self.Region
	local H = self:Hud()
	if not R or not H then return end
	H:Set("RegionName", "Text", "─  " .. R.DisplayName .. "  ─")
	H:Set("RegionSub", "Text", R.Subtitle)
	H:Show("RegionBanner", true, "HitTestInvisible")
	self.RegionTime = 0
	self.Report.Regions = self.Report.Regions + 1
	Log.Info("[HD2D] 지역: " .. R.DisplayName)
end

function World:UpdateRegionBanner(UDt)
	if not self.RegionTime then return end
	self.RegionTime = self.RegionTime + UDt
	local T = self.RegionTime
	local A = math.min(1.0, T / 0.6, math.max(0.0, (3.6 - T) / 0.8))
	local H = self:Hud()
	if H then H:Set("RegionBanner", "Opacity", math.floor(A * 20 + 0.5) / 20) end
	if T > 3.6 then
		self.RegionTime = nil
		if H then H:Show("RegionBanner", false) end
	end
end

-- ================================================================ 대화 (관리자 TalkTo 앞에서 — 처리했으면 true)
function World:TalkWorld(Npc)
	local Row = Npc.Row
	self.ShopRow = nil
	local bNight = self:IsNight()
	if Row.Role == "Inn" then
		self:TalkInn(Npc)
		return true
	end
	if bNight and Row.NightLines and #Row.NightLines > 0 and (Row.Role == "Talk" or Row.Role == "Shop") then
		self.Report.NightTalks = self.Report.NightTalks + 1
		self:StartDialog(Row.DisplayName, Row.NightLines, nil, Row.Portrait)
		return true
	end
	if Row.Role == "Shop" and Row.ShopStock and #Row.ShopStock > 0 then
		self:StartDialog(Row.DisplayName, Row.Lines, function()
			self.ShopRow = Row
			self:OpenShop()
		end, Row.Portrait)
		return true
	end
	return false
end

-- 상점 진열·주인 말 (HD2DMenu가 부른다 — 주민 행 값, 없으면 Balance)
function World:ShopStockList()
	local R = self.ShopRow
	if R and R.ShopStock and #R.ShopStock > 0 then return R.ShopStock end
	return D.Balance().ShopStock
end

function World:ShopLine(Index)
	local R = self.ShopRow
	if R and R.ShopLines and R.ShopLines[Index] then return R.ShopLines[Index] end
	return D.Balance().ShopLines[Index]
end

-- 상점 창을 연 직후: 제목·주인 초상화 (주민 행이 없으면 기본 "미라의 잡화점")
function World:ApplyShopLook()
	local H = self:Hud()
	local R = self.ShopRow
	H:Set("ShopTitle", "Text", (R and R.ShopTitle ~= "") and R.ShopTitle or "미라의 잡화점")
	H:Set("ShopSayPortrait", "Texture", R and R.Portrait or "UI/Demo/HD2D/Portraits/Merchant.png")
end

function World:ShopKeeperName()
	local R = self.ShopRow
	return R and R.DisplayName or "미라", R and R.Portrait or "UI/Demo/HD2D/Portraits/Merchant.png"
end

-- ================================================================ 여관 (잠자기)
function World:TalkInn(Npc)
	local Row = Npc.Row
	local DN = D.DayNight()
	if self.Gold < DN.InnPrice then
		self:StartDialog(Row.DisplayName, { "어머, 골드가 조금 모자라네요. 하룻밤에 " .. DN.InnPrice .. "골드예요." }, nil, Row.Portrait)
		return
	end
	self:StartDialog(Row.DisplayName, Row.Lines, function() self:Sleep(Npc) end, Row.Portrait)
end

function World:Sleep(Npc)
	local DN = D.DayNight()
	self.Gold = self.Gold - DN.InnPrice
	self.Report.InnPaid = (self.Report.InnPaid or 0) + DN.InnPrice
	self.Menu = "Sleep"
	self:SetPaused(true)
	self:Hud():ShowPrompt(nil)
	self:Hud():FadeTo(1.0, 0.7)
	self.Report.Sleeps = self.Report.Sleeps + 1
	Log.Info(string.format("[HD2D] 여관: 잠듦 (%s → %.1f시, 골드 -%d)", self:HourText(), DN.SleepHour, DN.InnPrice))
	Timer.After(1.1, function()
		self:SetGameHour(DN.SleepHour)
		self.bNight = nil  -- 다음 갱신에서 밤 적·밤 주민을 다시 맞춘다
		local Player = self:GetPlayer()
		if Player then
			Player:Heal(99999, 99999)
			Player.BP = math.max(Player.BP, D.Balance().BoostStart + 1)
		end
		self:Hud():FadeFrom(1.0, 0.9)
		Timer.After(0.5, function()
			self.Menu = nil
			self:SetPaused(false)
			self:Hud():Announce("아침이 밝았다", "푹 자고 일어나 기운이 넘친다", 2.4)
			if Player then self:SpawnHealFx(Player.entity:GetWorldPosition(), { 1, 0.95, 0.7, 1 }) end
			self:Fanfare()
		end, { Unscaled = true })
	end, { Unscaled = true })
end

-- ================================================================ 항구 소품 (등대 렌즈) — 관리자 소품 함수가 먼저 묻는다 (nil = 이 모듈 몫 아님)
function World:WorldPropLook(Id)
	if Id == "Lens" then return "PropSmall.eprefab", "Sprites/HD2D/Harbor_Lens.eflipbook" end
	return nil
end

function World:WorldPropActive(Prop)
	if Prop.Id == "Lens" then return self:SubState("Lighthouse") == "Active" and self:Count("LighthouseLens") == 0 end
	return nil
end

function World:WorldPropPrompt(Prop)
	if Prop.Id == "Lens" then return "E  등대 렌즈를 줍기" end
	return nil
end

function World:InteractWorldProp(Prop)
	if Prop.Id ~= "Lens" then return false end
	self:AddItem("LighthouseLens", 1, true)
	self:SpawnFx("Sparkle", Prop.Pos + Vector3(0, 30, 10), { Blend = 2, Scale = 1.4, Color = { 0.7, 0.9, 1, 1 } })
	Audio.PlayOneShot(self.Sounds.Pickup)
	self:StartDialog("", { "아르펜|Hero|이게 등대 렌즈구나. 해적들이 상자에 처박아 두었네.", "||커다란 유리 렌즈를 조심스럽게 싸서 짐에 넣었다." },
		function() self:OnSubQuestChanged() end)
	self:OnSubQuestChanged()
	return true
end

-- ================================================================ 저장 · 세션
function World:BuildWorldSave()
	return { Hour = self.GameHour }
end

function World:ApplyWorldSave(Data)
	if Data and Data.Hour then self.GameHour = Data.Hour % 24 end
end

return World
