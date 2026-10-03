-- 시간대 순환 (Phase 49 Demo_Sky): 시간대 컴포넌트 시각을 흘려 낮 → 해질녘 → 밤 → 새벽을 돈다 (Lua Sky 테이블)
--   T: 멈춤/재개, [ / ]: 1시간 뒤로/앞으로, 1~4: 아침/정오/해질녘/밤으로 바로
local SkyTimeCycle = {
	Properties = {
		HoursPerSecond = 0.25, -- 1초에 흐르는 시간 (0.25 = 96초에 하루)
		StartHour      = 15.0,
	},
}

function SkyTimeCycle:OnStart()
	self.Paused = false
	Sky.SetTimeOfDay(self.Properties.StartHour)
end

function SkyTimeCycle:OnUpdate(dt)
	local Hours = Sky.GetTimeOfDay()
	if Hours == nil then
		return -- 시간대 컴포넌트가 없음
	end
	if Input.IsKeyPressed("T") then
		self.Paused = not self.Paused
	end
	if Input.IsKeyPressed("LeftBracket") then
		Hours = Hours - 1.0
	elseif Input.IsKeyPressed("RightBracket") then
		Hours = Hours + 1.0
	elseif Input.IsKeyPressed("1") then
		Hours = 8.0
	elseif Input.IsKeyPressed("2") then
		Hours = 12.5
	elseif Input.IsKeyPressed("3") then
		Hours = 17.9
	elseif Input.IsKeyPressed("4") then
		Hours = 22.0
	end
	if not self.Paused then
		Hours = Hours + self.Properties.HoursPerSecond * dt
	end
	Sky.SetTimeOfDay(Hours)
end

return SkyTimeCycle
