#include "Scene/Gameplay.h"

#include "Scene/Scene.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace Gameplay
{
	float ApplyDamage(FHealthComponent& Health, float Amount, bool& bOutKilled)
	{
		bOutKilled = false;
		if (!std::isfinite(Amount) || Amount <= 0.0f || Health.bInvulnerable || IsDead(Health))
		{
			return 0.0f;
		}
		const float Applied = std::min(Amount, Health.Health);
		Health.Health -= Applied;
		if (Health.Health <= 0.0f)
		{
			Health.Health = 0.0f;
			bOutKilled    = true;
		}
		return Applied;
	}

	float Heal(FHealthComponent& Health, float Amount)
	{
		if (!std::isfinite(Amount) || Amount <= 0.0f || IsDead(Health))
		{
			return 0.0f;
		}
		const float Applied = std::clamp(Health.MaxHealth - Health.Health, 0.0f, Amount);
		Health.Health += Applied;
		return Applied;
	}

	float ApplyDamage(FScene& Scene, FEntity Target, float Amount, FEntity Instigator)
	{
		FRegistry&        Registry = Scene.GetRegistry();
		FHealthComponent* Health   = Registry.IsValid(Target) ? Registry.TryGet<FHealthComponent>(Target) : nullptr;
		if (Health == nullptr)
		{
			return 0.0f;
		}
		bool        bKilled = false;
		const float Applied = ApplyDamage(*Health, Amount, bKilled);
		if (Applied > 0.0f)
		{
			Health->Runtime.PendingEvents.push_back({ Applied, Registry.IsValid(Instigator) ? Instigator : FEntity(), bKilled });
		}
		return Applied;
	}

	float Heal(FScene& Scene, FEntity Target, float Amount)
	{
		FRegistry&        Registry = Scene.GetRegistry();
		FHealthComponent* Health   = Registry.IsValid(Target) ? Registry.TryGet<FHealthComponent>(Target) : nullptr;
		return Health != nullptr ? Heal(*Health, Amount) : 0.0f;
	}

	bool IsDead(const FScene& Scene, FEntity Target)
	{
		const FRegistry&        Registry = Scene.GetRegistry();
		const FHealthComponent* Health   = Registry.IsValid(Target) ? Registry.TryGet<FHealthComponent>(Target) : nullptr;
		return Health != nullptr && IsDead(*Health);
	}

	const char* ToString(EMatchState State)
	{
		switch (State)
		{
		case EMatchState::InProgress: return "InProgress";
		case EMatchState::Ended:      return "Ended";
		default:                      return "WaitingToStart";
		}
	}

	FEntity FindGameMode(FScene& Scene)
	{
		FEntity Result;
		Scene.GetRegistry().View<FGameModeComponent>().Each([&](FEntity Entity, FGameModeComponent&) {
			if (!Result.IsValid() || Entity.Index < Result.Index)
			{
				Result = Entity; // 여러 개면 엔티티 인덱스가 가장 작은 것 (순회 순서와 무관하게 결정적)
			}
		});
		return Result;
	}

	std::map<int32, int32> ParseScores(std::string_view Text)
	{
		std::map<int32, int32> Result;
		size_t                 Begin = 0;
		while (Begin < Text.size())
		{
			const size_t           End   = std::min(Text.find(';', Begin), Text.size());
			const std::string_view Entry = Text.substr(Begin, End - Begin);
			const size_t           Equal = Entry.find('=');
			int32                  Player = 0;
			int32                  Score  = 0;
			if (Equal != std::string_view::npos &&
			    std::from_chars(Entry.data(), Entry.data() + Equal, Player).ec == std::errc() &&
			    std::from_chars(Entry.data() + Equal + 1, Entry.data() + Entry.size(), Score).ec == std::errc() && Player >= 0)
			{
				Result[Player] = Score;
			}
			Begin = End + 1;
		}
		return Result;
	}

	std::string FormatScores(const std::map<int32, int32>& Scores)
	{
		std::string Result;
		for (const auto& [Player, Score] : Scores)
		{
			if (!Result.empty())
			{
				Result += ';';
			}
			Result += std::to_string(Player) + '=' + std::to_string(Score);
		}
		return Result;
	}

	int32 GetScore(const FGameModeComponent& GameMode, int32 PlayerId)
	{
		const std::map<int32, int32> Scores = ParseScores(GameMode.Scores);
		const auto                   Found  = Scores.find(PlayerId);
		return Found != Scores.end() ? Found->second : 0;
	}

	void AddScore(FGameModeComponent& GameMode, int32 PlayerId, int32 Amount)
	{
		if (PlayerId < 0 || Amount == 0)
		{
			return;
		}
		std::map<int32, int32> Scores = ParseScores(GameMode.Scores);
		Scores[PlayerId] += Amount;
		GameMode.Scores = FormatScores(Scores);
	}

	int32 FindLeader(const std::map<int32, int32>& Scores)
	{
		int32 Leader = -1;
		int32 Best   = 0;
		bool  bTie   = false;
		for (const auto& [Player, Score] : Scores)
		{
			if (Leader < 0 || Score > Best)
			{
				Leader = Player;
				Best   = Score;
				bTie   = false;
			}
			else if (Score == Best)
			{
				bTie = true;
			}
		}
		return bTie ? -1 : Leader;
	}

	void SetMatchState(FGameModeComponent& GameMode, EMatchState State)
	{
		GameMode.MatchState        = State;
		GameMode.Runtime.StateTime = 0.0f;
		if (State == EMatchState::InProgress)
		{
			GameMode.Scores.clear();
			GameMode.WinnerPlayerId   = -1;
			GameMode.RemainingSeconds = GameMode.TimeLimit > 0.0f ? static_cast<int32>(std::ceil(GameMode.TimeLimit)) : 0;
		}
	}

	bool TickMatch(FGameModeComponent& GameMode, float DeltaSeconds)
	{
		GameMode.Runtime.StateTime += std::max(0.0f, DeltaSeconds);
		switch (GameMode.MatchState)
		{
		case EMatchState::WaitingToStart:
			if (GameMode.Runtime.StateTime >= GameMode.StartDelay)
			{
				SetMatchState(GameMode, EMatchState::InProgress);
				return true;
			}
			return false;

		case EMatchState::InProgress:
		{
			const std::map<int32, int32> Scores = ParseScores(GameMode.Scores);
			if (GameMode.ScoreToWin > 0)
			{
				const int32 Leader = FindLeader(Scores);
				if (Leader >= 0 && Scores.at(Leader) >= GameMode.ScoreToWin)
				{
					EndMatch(GameMode, Leader);
					return true;
				}
			}
			if (GameMode.TimeLimit > 0.0f)
			{
				const float Remaining     = GameMode.TimeLimit - GameMode.Runtime.StateTime;
				GameMode.RemainingSeconds = std::max(0, static_cast<int32>(std::ceil(Remaining)));
				if (Remaining <= 0.0f)
				{
					EndMatch(GameMode, FindLeader(Scores));
					return true;
				}
			}
			return false;
		}

		default:
			return false;
		}
	}

	void EndMatch(FGameModeComponent& GameMode, int32 WinnerPlayerId)
	{
		SetMatchState(GameMode, EMatchState::Ended);
		GameMode.WinnerPlayerId = WinnerPlayerId;
	}

	void RestartMatch(FGameModeComponent& GameMode)
	{
		SetMatchState(GameMode, EMatchState::WaitingToStart);
		GameMode.Scores.clear();
		GameMode.WinnerPlayerId   = -1;
		GameMode.RemainingSeconds = 0;
	}
} // namespace Gameplay
