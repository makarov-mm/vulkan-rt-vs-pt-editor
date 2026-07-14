#pragma once

#include "platform.h"

void vkCheck(VkResult r, const char* what);

inline uint32_t alignUp(uint32_t v, uint32_t a)
{
	return (v + a - 1) & ~(a - 1);
}
