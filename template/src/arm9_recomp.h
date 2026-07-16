#pragma once

#include "nds_runtime.h"

#include <stdint.h>

extern const uint32_t nds_arm9_translated_instruction_count;
extern const uint32_t nds_arm7_translated_instruction_count;
NdsRunResult nds_run_arm9(NdsCpu *cpu, uint32_t budget);
NdsRunResult nds_run_arm7(NdsCpu *cpu, uint32_t budget);
