#pragma once

struct MouseInfo
{
	int x;
	int y;
	int z;
	char buttonState[8];
	char buttonState2[8];
};

namespace Helpers
{
	bool IsMouseVisible();
	bool IsLoading();
	bool IsCampaignLoading();
	// Halo's mouse state block. Only valid once sig scanning has run.
	MouseInfo* GetMouseInfo();
}