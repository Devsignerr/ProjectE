-- 대시 (능력 표 Data/Abilities/DemoAbilities.etable "Dash"): 스태미나 30, 쿨다운 1.5초.
-- 이동 속도는 ActivationEffects "Dashing"(MoveSpeed ×3, 0.25초)이 올린다 — 커밋에서 걸리므로 소유 클라이언트가 예측한다.
-- 조종하는 쪽(소유 클라이언트/Standalone)이 그동안 바라보는 방향으로 이동 입력을 넣는다 (서버는 클라이언트 무브를 그대로 시뮬레이션)
local Dash = { Properties = { Duration = 0.25 } }

function Dash:OnActivate(ctx)
	local Owner   = ctx.Owner
	local Forward = Owner:GetForward()
	Forward       = Vector3(Forward.X, Forward.Y, 0):Normalized()
	local Start   = Time.TotalTime
	while Time.TotalTime - Start < self.Properties.Duration do
		if ctx:IsLocallyControlled() then
			Owner:AddMovementInput(Forward)
		end
		ctx:Wait(0)
	end
end

return Dash