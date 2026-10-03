#include "Renderer/IesProfile.h"

#include "Core/Math/MathUtils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace
{
	std::string_view TrimView(std::string_view Text)
	{
		while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t' || Text.front() == '\r' || Text.front() == '\n'))
		{
			Text.remove_prefix(1);
		}
		while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t' || Text.back() == '\r' || Text.back() == '\n'))
		{
			Text.remove_suffix(1);
		}
		return Text;
	}

	bool StartsWithNoCase(std::string_view Text, std::string_view Prefix)
	{
		if (Text.size() < Prefix.size())
		{
			return false;
		}
		for (size_t Index = 0; Index < Prefix.size(); ++Index)
		{
			const char A = static_cast<char>(std::toupper(static_cast<unsigned char>(Text[Index])));
			const char B = static_cast<char>(std::toupper(static_cast<unsigned char>(Prefix[Index])));
			if (A != B)
			{
				return false;
			}
		}
		return true;
	}

	// 공백/쉼표로 나눈 숫자 읽기
	class FNumberReader
	{
	public:
		explicit FNumberReader(std::string_view InText) : Text(InText) {}

		bool Next(float& Out)
		{
			while (Position < Text.size() && (Text[Position] == ' ' || Text[Position] == '\t' || Text[Position] == '\r' || Text[Position] == '\n' ||
			                                  Text[Position] == ','))
			{
				++Position;
			}
			if (Position >= Text.size())
			{
				return false;
			}
			size_t End = Position;
			while (End < Text.size() && Text[End] != ' ' && Text[End] != '\t' && Text[End] != '\r' && Text[End] != '\n' && Text[End] != ',')
			{
				++End;
			}
			const char* Begin  = Text.data() + Position;
			const char* Finish = Text.data() + End;
			if (*Begin == '+')
			{
				++Begin; // from_chars는 '+'를 받지 않는다
			}
			const auto Result = std::from_chars(Begin, Finish, Out);
			Position          = End;
			return Result.ec == std::errc() && Result.ptr == Finish;
		}

	private:
		std::string_view Text;
		size_t           Position = 0;
	};

	// 오름차순 표에서 Value의 보간 위치 (구간 번호 + 비율). 범위 밖이면 false
	bool FindSpan(const std::vector<float>& Angles, float Value, size_t& OutIndex, float& OutT)
	{
		if (Angles.empty())
		{
			return false;
		}
		if (Angles.size() == 1)
		{
			OutIndex = 0;
			OutT     = 0.0f;
			return true; // 각 하나 = 그 값으로 고정
		}
		if (Value < Angles.front() - 1.0e-3f || Value > Angles.back() + 1.0e-3f)
		{
			return false;
		}
		const auto Upper = std::upper_bound(Angles.begin(), Angles.end(), Value);
		size_t     Index = Upper == Angles.begin() ? 0 : static_cast<size_t>(Upper - Angles.begin()) - 1;
		Index            = std::min(Index, Angles.size() - 2);
		const float Span = Angles[Index + 1] - Angles[Index];
		OutIndex         = Index;
		OutT             = Span > 1.0e-6f ? FMath::Clamp((Value - Angles[Index]) / Span, 0.0f, 1.0f) : 0.0f;
		return true;
	}
} // namespace

bool FIesProfile::Parse(std::string_view Text, FIesProfile& Out, std::string* OutError)
{
	Out = FIesProfile{};
	auto Fail = [&](std::string Message) {
		if (OutError != nullptr)
		{
			*OutError = std::move(Message);
		}
		Out = FIesProfile{};
		return false;
	};
	if (Text.size() >= 3 && static_cast<unsigned char>(Text[0]) == 0xEF && static_cast<unsigned char>(Text[1]) == 0xBB &&
	    static_cast<unsigned char>(Text[2]) == 0xBF)
	{
		Text.remove_prefix(3); // UTF-8 BOM
	}

	// ---- 머리: TILT= 줄까지
	Out.FormatName = "LM-63-1986";
	size_t LineStart = 0;
	bool   bTiltFound = false;
	bool   bFirstLine = true;
	std::string_view TiltValue;
	while (LineStart < Text.size())
	{
		size_t LineEnd = Text.find('\n', LineStart);
		if (LineEnd == std::string_view::npos)
		{
			LineEnd = Text.size();
		}
		const std::string_view Line = TrimView(Text.substr(LineStart, LineEnd - LineStart));
		LineStart                   = LineEnd + 1;
		if (bFirstLine)
		{
			bFirstLine = false;
			if (StartsWithNoCase(Line, "IESNA"))
			{
				const size_t Colon = Line.find(':');
				Out.FormatName      = std::string(TrimView(Colon == std::string_view::npos ? Line.substr(5) : Line.substr(Colon + 1)));
				if (Out.FormatName == "91" || Out.FormatName.empty())
				{
					Out.FormatName = "LM-63-1991";
				}
				continue;
			}
		}
		if (StartsWithNoCase(Line, "TILT"))
		{
			const size_t Equal = Line.find('=');
			if (Equal == std::string_view::npos)
			{
				return Fail("TILT 줄에 '='이 없습니다");
			}
			TiltValue  = TrimView(Line.substr(Equal + 1));
			bTiltFound = true;
			break;
		}
		// [키워드] 줄과 그 밖의 머리 글은 무시
	}
	if (!bTiltFound)
	{
		return Fail("TILT= 줄을 찾지 못했습니다");
	}

	FNumberReader Reader(Text.substr(std::min(LineStart, Text.size())));
	auto Read = [&Reader](float& Value) { return Reader.Next(Value); };

	if (StartsWithNoCase(TiltValue, "INCLUDE"))
	{
		// 램프-기구 기하, 기울기 각 수, 각들, 배율들 → 건너뜀
		float Geometry   = 0.0f;
		float TiltCountF = 0.0f;
		if (!Read(Geometry) || !Read(TiltCountF) || TiltCountF < 0.0f || TiltCountF > 10000.0f)
		{
			return Fail("TILT=INCLUDE 기울기 표를 읽지 못했습니다");
		}
		const int32 TiltCount = static_cast<int32>(TiltCountF);
		for (int32 Index = 0; Index < TiltCount * 2; ++Index)
		{
			float Skip = 0.0f;
			if (!Read(Skip))
			{
				return Fail("TILT=INCLUDE 기울기 표가 짧습니다");
			}
		}
	}
	// TILT=NONE 또는 외부 파일 이름(무시 — 기울기 보정 없음)

	float Header[13] = {};
	for (float& Value : Header)
	{
		if (!Read(Value))
		{
			return Fail("측광 머리 숫자(13개)를 읽지 못했습니다");
		}
	}
	const float Multiplier    = Header[2];
	const float VerticalF     = Header[3];
	const float HorizontalF   = Header[4];
	const float BallastFactor = Header[10] > 0.0f ? Header[10] : 1.0f;
	if (VerticalF < 1.0f || HorizontalF < 1.0f || VerticalF > 10000.0f || HorizontalF > 10000.0f)
	{
		return Fail("각 수가 잘못되었습니다");
	}
	const size_t VerticalCount   = static_cast<size_t>(VerticalF);
	const size_t HorizontalCount = static_cast<size_t>(HorizontalF);
	Out.LumensPerLamp            = Header[1];
	Out.PhotometricType          = static_cast<int32>(Header[5]);

	Out.VerticalAngles.resize(VerticalCount);
	Out.HorizontalAngles.resize(HorizontalCount);
	for (float& Angle : Out.VerticalAngles)
	{
		if (!Read(Angle))
		{
			return Fail("수직 각 목록이 짧습니다");
		}
	}
	for (float& Angle : Out.HorizontalAngles)
	{
		if (!Read(Angle))
		{
			return Fail("수평 각 목록이 짧습니다");
		}
	}
	if (!std::is_sorted(Out.VerticalAngles.begin(), Out.VerticalAngles.end()) || !std::is_sorted(Out.HorizontalAngles.begin(), Out.HorizontalAngles.end()))
	{
		return Fail("각 목록이 오름차순이 아닙니다");
	}
	Out.Candela.resize(VerticalCount * HorizontalCount);
	const float Scale = (Multiplier > 0.0f ? Multiplier : 1.0f) * BallastFactor;
	for (float& Value : Out.Candela)
	{
		if (!Read(Value))
		{
			return Fail("칸델라 값이 모자랍니다");
		}
		Value          = FMath::Max(Value, 0.0f) * Scale;
		Out.MaxCandela = FMath::Max(Out.MaxCandela, Value);
	}
	if (Out.MaxCandela <= 0.0f)
	{
		return Fail("칸델라 값이 모두 0입니다");
	}
	if (OutError != nullptr && Out.PhotometricType != 1)
	{
		*OutError = "측광 종류 C(1)가 아님 — C로 읽음"; // 성공이지만 경고 문구
	}
	return true;
}

float FIesProfile::FoldHorizontalAngle(float PhiDegrees) const
{
	float Phi = std::fmod(PhiDegrees, 360.0f);
	if (Phi < 0.0f)
	{
		Phi += 360.0f;
	}
	if (HorizontalAngles.empty())
	{
		return 0.0f;
	}
	const float First = HorizontalAngles.front();
	const float Last  = HorizontalAngles.back();
	if (HorizontalAngles.size() == 1)
	{
		return First; // 회전 대칭
	}
	if (First >= 89.0f && Last <= 271.0f)
	{
		// 90..270 (앞뒤 대칭: 90-270 평면 기준 거울 — φ → 180 - φ)
		return Phi >= 90.0f && Phi <= 270.0f ? Phi : std::fmod(540.0f - Phi, 360.0f);
	}
	if (Last <= 90.5f)
	{
		// 4분면 대칭
		if (Phi > 180.0f)
		{
			Phi = 360.0f - Phi;
		}
		if (Phi > 90.0f)
		{
			Phi = 180.0f - Phi;
		}
		return Phi;
	}
	if (Last <= 180.5f)
	{
		return Phi > 180.0f ? 360.0f - Phi : Phi; // 좌우 대칭
	}
	return Phi; // 전체
}

float FIesProfile::Sample(float ThetaDegrees, float PhiDegrees) const
{
	if (VerticalAngles.empty() || HorizontalAngles.empty())
	{
		return 0.0f;
	}
	size_t VIndex = 0;
	float  VT     = 0.0f;
	if (!FindSpan(VerticalAngles, ThetaDegrees, VIndex, VT))
	{
		return 0.0f;
	}
	size_t HIndex = 0;
	float  HT     = 0.0f;
	if (!FindSpan(HorizontalAngles, FoldHorizontalAngle(PhiDegrees), HIndex, HT))
	{
		// 표 밖 수평 각 (불완전한 파일): 가까운 끝 열
		HIndex = PhiDegrees < HorizontalAngles.front() ? 0 : HorizontalAngles.size() - 1;
		HT     = 0.0f;
	}
	const size_t VCount = VerticalAngles.size();
	auto At = [&](size_t H, size_t V) {
		H = std::min(H, HorizontalAngles.size() - 1);
		V = std::min(V, VCount - 1);
		return Candela[H * VCount + V];
	};
	const float A = At(HIndex, VIndex) + (At(HIndex, VIndex + 1) - At(HIndex, VIndex)) * VT;
	const float B = At(HIndex + 1, VIndex) + (At(HIndex + 1, VIndex + 1) - At(HIndex + 1, VIndex)) * VT;
	return A + (B - A) * HT;
}

std::vector<float> FIesProfile::BakeTexture(uint32 Width, uint32 Height) const
{
	std::vector<float> Pixels(static_cast<size_t>(Width) * Height, 0.0f);
	if (MaxCandela <= 0.0f || Width < 2 || Height < 2)
	{
		return Pixels;
	}
	const float InvMax = 1.0f / MaxCandela;
	for (uint32 Y = 0; Y < Height; ++Y)
	{
		const float Phi = 360.0f * static_cast<float>(Y) / static_cast<float>(Height - 1);
		for (uint32 X = 0; X < Width; ++X)
		{
			const float Theta              = 180.0f * static_cast<float>(X) / static_cast<float>(Width - 1);
			Pixels[static_cast<size_t>(Y) * Width + X] = FMath::Clamp(Sample(Theta, Phi) * InvMax, 0.0f, 1.0f);
		}
	}
	return Pixels;
}
