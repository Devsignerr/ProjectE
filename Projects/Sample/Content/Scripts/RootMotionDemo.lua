-- 루트 모션 데모 (Demo_Retarget, Phase 52): 몽타주 클립의 루트 이동이 엔티티를 움직인다
--   모델의 AnimationComponent RootMotionMode = 몽타주만 → 기본 클립(제자리)은 그대로, 몽타주만 추출
--   캐릭터 이동 컴포넌트가 있으면(이 엔티티가 캐릭터 루트) 루트 모션은 캐릭터 이동(충돌 유지)으로 들어가고,
--   없으면 모델 엔티티 트랜스폼을 직접 옮긴다.
--   Montages 목록을 차례로 재생 ("<모델 경로>:<클립>"이면 그 모델에서 리타기팅). 집에서 멀어지면:
--     ReturnHome(캐릭터 이동) = 이동 입력으로 걸어서 돌아온다 / ResetAfterLast = 마지막 몽타주가 끝나면 집으로 순간이동
local RootMotionDemo = {
	Properties = {
		Montages       = "Dodge_Left,Dodge_Right",
		Interval       = 0.6,   -- 몽타주 사이 쉬는 시간 (초)
		BlendIn        = 0.1,
		BlendOut       = 0.15,
		ReturnHome     = false,
		ReturnDistance = 40.0,  -- cm
		ResetAfterLast = false,
		Played         = 0,     -- 재생한 몽타주 수 (확인용)
	},
}

function RootMotionDemo:OnStart()
	self.List = {}
	for Name in string.gmatch(self.Properties.Montages, "[^,]+") do
		table.insert(self.List, (Name:gsub("^%s+", ""):gsub("%s+$", "")))
	end
	self.Index   = 0
	self.Wait    = self.Properties.Interval
	self.Home    = self.entity:GetPosition()
	self.Playing = false
end

function RootMotionDemo:OnUpdate(dt)
	local P = self.Properties
	if self.Playing then
		return
	end
	local Offset = self.Home - self.entity:GetPosition()
	Offset.Z = 0
	if P.ReturnHome and Offset:Length() > P.ReturnDistance then
		self.entity:AddMovementInput(Offset) -- 몽타주 사이: 걸어서 집으로
		return
	end
	self.Wait = self.Wait - dt
	if self.Wait > 0 or #self.List == 0 then
		return
	end
	self.Index = self.Index % #self.List + 1
	if self.entity:PlayMontage(self.List[self.Index], { BlendIn = P.BlendIn, BlendOut = P.BlendOut }) then
		self.Playing = true
		P.Played     = P.Played + 1
	else
		self.Wait = P.Interval
	end
end

function RootMotionDemo:OnMontageEnded(clip, interrupted, slot)
	self.Playing = false
	self.Wait    = self.Properties.Interval
	Log.Info("루트 모션 몽타주 끝:", clip, interrupted and "(중단)" or "(완료)", "위치", tostring(self.entity:GetPosition()))
	if self.Properties.ResetAfterLast and self.Index == #self.List then
		self.entity:SetPosition(self.Home)
	end
end

return RootMotionDemo
