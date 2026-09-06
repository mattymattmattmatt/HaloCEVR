#include "Menus.h"
#include "../Hooking/Hooks.h"

bool Helpers::IsMouseVisible()
{
	return !*reinterpret_cast<bool*>(Hooks::o.HideMouse);
}

bool Helpers::IsLoading()
{
    return *reinterpret_cast<int*>(Hooks::o.LoadingState) != 0;
}

bool Helpers::IsCampaignLoading()
{
	return *reinterpret_cast<bool*>(Hooks::o.CampaignLoading);
}

MouseInfo* Helpers::GetMouseInfo()
{
	if (Hooks::o.MouseInfoPush.Address == 0)
	{
		return nullptr;
	}

	// Skip the 0x68 opcode; the operand is the address of the block.
	const uint32_t address = *reinterpret_cast<uint32_t*>(Hooks::o.MouseInfoPush.Address + 1);
	return reinterpret_cast<MouseInfo*>(address);
}
