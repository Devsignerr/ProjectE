-- 쇼케이스 탐지기 (ScriptUtilsDemo.lua의 시작 위치 기준판, 월드 축 표시 없음): 타이머 / 코루틴 / 겹침 검사 / 3D 디버그 선
--   코루틴: 시작 위치 둘레 경유점을 차례로 (1초 이동 → 0.25초 쉼 → 다음 펄스까지 대기)
--   매 프레임: Physics.OverlapSphere로 탐지 구 안 물체 (초록 = 없음, 빨강 = 있음, 화살표/상자), 아래로 SphereCast한 곳 표시
--   Timer.Every: 0.5초마다 지나온 자리에 2초 남는 표시
local ShowcaseScanner = {
	Properties = {
		Radius   = 70.0,  -- 탐지 구 반지름 (cm)
		Distance = 230.0, -- 경유점 거리 (cm)
	},
}

function ShowcaseScanner:OnStart()
	local C = self.entity:GetPosition()
	local D = self.Properties.Distance
	self.Points = { C + Vector3(D, 0, 0), C + Vector3(0, D, 0), C + Vector3(-D, -D * 0.6, 0), C + Vector3(D * 0.5, -D, 0) }
	self.Pulses = 0
	Timer.Every(0.5, function()
		self.Pulses = self.Pulses + 1
		Debug.DrawSphere(self.entity:GetPosition(), 12, Vector3(1.0, 0.8, 0.1), 2.0)
	end)
	Coroutine.Start(function() self:Patrol() end)
end

function ShowcaseScanner:Patrol()
	local Index = 1
	while true do
		local From    = self.entity:GetPosition()
		local To      = self.Points[Index]
		local Elapsed = 0
		while Elapsed < 1.0 do
			Wait()
			Elapsed = Elapsed + Time.DeltaTime
			local T = math.min(Elapsed, 1.0)
			self.entity:SetPosition(Vector3.Lerp(From, To, T * T * (3 - 2 * T)))
		end
		Wait(0.25)
		local Pulse = self.Pulses
		WaitUntil(function() return self.Pulses > Pulse end)
		Index = Index % #self.Points + 1
	end
end

function ShowcaseScanner:OnUpdate(dt)
	local Center = self.entity:GetPosition()
	local Radius = self.Properties.Radius
	local Hits   = Physics.OverlapSphere(Center, Radius, self.entity)
	local Color  = #Hits > 0 and Vector3(1.0, 0.15, 0.1) or Vector3(0.2, 1.0, 0.3)
	Debug.DrawSphere(Center, Radius, Color)
	for _, Entity in ipairs(Hits) do
		Debug.DrawArrow(Center, Entity:GetPosition(), Vector3(1.0, 0.4, 0.1))
		Debug.DrawBox(Entity:GetPosition(), Vector3(32, 32, 32), Vector3(1.0, 0.15, 0.1))
	end
	local Hit = Physics.SphereCast(Center, 15, Vector3(0, 0, -1), 500, self.entity)
	if Hit then
		Debug.DrawArrow(Center, Hit.position, Vector3(0.3, 0.7, 1.0))
		Debug.DrawSphere(Center + Vector3(0, 0, -Hit.distance), 15, Vector3(0.3, 0.7, 1.0))
	end
end

return ShowcaseScanner
