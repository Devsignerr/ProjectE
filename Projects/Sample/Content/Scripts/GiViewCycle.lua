-- Tests/GI 시점 전환: 숫자 키 1~8로 카메라를 그림자 유형별 시점에 둔다 (Tab = 다음 시점)
--   시점 표는 데모 씬 생성 값과 같다 (위치, 바라보는 점 — cm). StartView = 시작 시점 (0이면 씬에 저장된 트랜스폼 그대로)
local GiViewCycle = {
	Properties = {
		StartView = 1,
	},
}

local Views = {
	{ Name = "전체",            From = Vector3(-385, -60, 200),  At = Vector3(150, 40, 40) },
	{ Name = "알코브·선반",     From = Vector3(385, 140, 200),   At = Vector3(-300, -160, 60) },
	{ Name = "바닥 접촉",       From = Vector3(190, -110, 60),   At = Vector3(-20, -185, 10) },
	{ Name = "선반",            From = Vector3(-110, -150, 110), At = Vector3(-110, -295, 70) },
	{ Name = "계단·창살",       From = Vector3(0, -100, 220),    At = Vector3(280, -260, 40) },
	{ Name = "테이블·의자 아래", From = Vector3(-160, 210, 55),   At = Vector3(0, 60, 20) },
	{ Name = "칸막이 뒤",       From = Vector3(-150, 110, 170),  At = Vector3(-330, 250, 20) },
	{ Name = "장·벽 틈",        From = Vector3(100, 230, 150),    At = Vector3(330, 280, 30) },
}

function GiViewCycle:SetView(Index)
	local View = Views[Index]
	if View == nil then
		return
	end
	self.Current = Index
	self.entity:SetPosition(View.From)
	self.entity:SetRotation(Quat.LookRotation(View.At - View.From))
	Log.Info("Tests/GI 시점 " .. Index .. ": " .. View.Name)
end

function GiViewCycle:OnStart()
	self.Current = 1
	if self.Properties.StartView > 0 then
		self:SetView(self.Properties.StartView)
	end
end

function GiViewCycle:OnUpdate(dt)
	for Index = 1, #Views do
		if Input.IsKeyPressed(tostring(Index)) then
			self:SetView(Index)
		end
	end
	if Input.IsKeyPressed("Tab") then
		self:SetView(self.Current % #Views + 1)
	end
end

return GiViewCycle
