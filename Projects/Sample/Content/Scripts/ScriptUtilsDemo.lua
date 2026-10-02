-- Phase 41 스크립트 편의 데모: 타이머 / 코루틴 대기 / 겹침 검사·쓸어 보기 / 3D 디버그 선
--   코루틴: 탐지기가 경유점을 차례로 (1초 이동 → 0.5초 멈춤)
--   매 프레임: 탐지 구 안의 물체를 OverlapSphere로 찾고 (초록 = 없음, 빨강 = 있음, 찾은 물체로 화살표),
--             아래로 SphereCast해 닿은 곳을 표시
--   Timer.Every: 0.5초마다 지나온 자리에 2초 동안 남는 표시, Timer.After: 시작 1초 뒤 로그 한 번
local Demo = {
	Properties = {
		Radius   = 70.0,  -- 탐지 구 반지름 (cm)
		Height   = 80.0,  -- 탐지기 높이 (cm)
		Distance = 250.0, -- 경유점 거리 (cm)
	},
}

function Demo:OnStart()
	local D = self.Properties.Distance
	self.Points = { Vector3(D, 0, 0), Vector3(0, D, 0), Vector3(-D, -D, 0), Vector3(D * 0.6, -D * 0.8, 0) }
	self.Found  = 0
	self.Pulses = 0

	Timer.After(1.0, function() Log.Info("[ScriptUtilsDemo] 1초 타이머 — 지금까지 펄스", self.Pulses) end)
	Timer.Every(0.5, function()
		self.Pulses = self.Pulses + 1
		Debug.DrawSphere(self.entity:GetPosition(), 12, Vector3(1.0, 0.8, 0.1), 2.0) -- 2초 동안 남는 자국
	end)
	Coroutine.Start(function() self:Patrol() end)
end

-- 코루틴: 경유점 사이를 1초 동안 옮기고(매 프레임 Wait) 0.25초 쉰 뒤 다음 펄스(Timer.Every)를 기다린다. 영원히 반복
function Demo:Patrol()
	local Index = 1
	while true do
		local From    = self.entity:GetPosition()
		local To      = self.Points[Index] + Vector3(0, 0, self.Properties.Height)
		local Elapsed = 0
		while Elapsed < 1.0 do
			Wait() -- 다음 프레임
			Elapsed = Elapsed + Time.DeltaTime
			self.entity:SetPosition(Vector3.Lerp(From, To, math.min(Elapsed, 1.0)))
		end
		Wait(0.25)
		local Pulse = self.Pulses
		WaitUntil(function() return self.Pulses > Pulse end)
		Index = Index % #self.Points + 1
	end
end

function Demo:OnUpdate(dt)
	local Center = self.entity:GetPosition()
	local Radius = self.Properties.Radius

	-- 겹침 검사: 자기 자신(ignore)은 빼고 찾는다
	local Hits  = Physics.OverlapSphere(Center, Radius, self.entity)
	self.Found  = #Hits
	local Color = #Hits > 0 and Vector3(1.0, 0.15, 0.1) or Vector3(0.2, 1.0, 0.3)
	Debug.DrawSphere(Center, Radius, Color)
	for _, Entity in ipairs(Hits) do
		Debug.DrawArrow(Center, Entity:GetPosition(), Vector3(1.0, 0.4, 0.1))
		Debug.DrawBox(Entity:GetPosition(), Vector3(32, 32, 32), Vector3(1.0, 0.15, 0.1))
	end

	-- 쓸어 보기: 아래로 반지름 15 구를 내려 처음 닿는 곳
	local Hit = Physics.SphereCast(Center, 15, Vector3(0, 0, -1), 500, self.entity)
	if Hit then
		local Stop = Center + Vector3(0, 0, -Hit.distance)
		Debug.DrawArrow(Center, Hit.position, Vector3(0.3, 0.7, 1.0))
		Debug.DrawSphere(Stop, 15, Vector3(0.3, 0.7, 1.0))
	end

	-- 월드 축 (항상 위): X 빨강, Y 초록, Z 파랑
	Debug.DrawArrow(Vector3(0, 0, 0), Vector3(100, 0, 0), Vector3(1, 0, 0), 0, true)
	Debug.DrawArrow(Vector3(0, 0, 0), Vector3(0, 100, 0), Vector3(0, 1, 0), 0, true)
	Debug.DrawArrow(Vector3(0, 0, 0), Vector3(0, 0, 100), Vector3(0, 0.4, 1), 0, true)
end

return Demo
