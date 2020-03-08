/*
	Copyright 2020 flyinghead

	This file is part of flycast.

    flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with flycast.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "build.h"

#define TAIL_CALLING 1

#if	HOST_CPU == CPU_X64 && FEAT_AREC != DYNAREC_NONE

#include "deps/xbyak/xbyak.h"
#include "deps/xbyak/xbyak_util.h"
using namespace Xbyak::util;
#include "arm7_rec.h"

extern u32 arm_single_op(u32 opcode);
extern "C" void CompileCode();
extern "C" void CPUFiq();
extern "C" void arm_dispatch();

extern u8* icPtr;
extern u8* ICache;
extern const u32 ICacheSize;
extern reg_pair arm_Reg[RN_ARM_REG_COUNT];

#ifdef _WIN32
static const Xbyak::Reg32 call_regs[] = { ecx, edx, r8d, r9d };
#else
static const Xbyak::Reg32 call_regs[] = { edi, esi, edx, ecx  };
#endif
#ifdef TAIL_CALLING
extern "C" u32 (**entry_points)();
#endif
u32 (**entry_points)();

class Arm7Compiler;

#ifdef TAIL_CALLING
#ifdef _WIN32
static const std::array<Xbyak::Reg32, 7> alloc_regs {
		ebx, ebp, edi, esi, r12d, r13d, r15d
};
#else
static const std::array<Xbyak::Reg32, 5> alloc_regs {
		ebx, ebp, r12d, r13d, r15d
};
#endif
#else
#ifdef _WIN32
static const std::array<Xbyak::Reg32, 8> alloc_regs {
		ebx, ebp, edi, esi, r12d, r13d, r14d, r15d
};
#else
static const std::array<Xbyak::Reg32, 6> alloc_regs {
		ebx, ebp, r12d, r13d, r14d, r15d
};
#endif
#endif

class X64ArmRegAlloc : public ArmRegAlloc<sizeof(alloc_regs) / sizeof(alloc_regs[0]), X64ArmRegAlloc>
{
	using super = ArmRegAlloc<sizeof(alloc_regs) / sizeof(alloc_regs[0]), X64ArmRegAlloc>;
	Arm7Compiler& assembler;

	void LoadReg(int host_reg, Arm7Reg armreg);
	void StoreReg(int host_reg, Arm7Reg armreg);

	static const Xbyak::Reg32& getReg32(int i)
	{
		verify(i >= 0 && (u32)i < alloc_regs.size());
		return alloc_regs[i];
	}

public:
	X64ArmRegAlloc(Arm7Compiler& assembler, const std::vector<ArmOp>& block_ops)
		: super(block_ops), assembler(assembler) {}

	const Xbyak::Reg32& map(Arm7Reg r)
	{
		int i = super::map(r);
		return getReg32(i);
	}

	friend super;
};

class Arm7Compiler : public Xbyak::CodeGenerator
{
	bool logical_op_set_flags = false;
	bool set_carry_bit = false;
	bool set_flags = false;
	X64ArmRegAlloc *regalloc = nullptr;

	static const u32 N_FLAG = 1 << 31;
	static const u32 Z_FLAG = 1 << 30;
	static const u32 C_FLAG = 1 << 29;
	static const u32 V_FLAG = 1 << 28;


	Xbyak::Operand getOperand(const ArmOp::Operand& arg, Xbyak::Reg32 scratch_reg)
	{
		Xbyak::Reg32 r;
		if (!arg.isReg())
		{
			if (arg.isNone() || arg.shift_imm)
				return Xbyak::Operand();
			mov(scratch_reg, arg.getImmediate());
			r = scratch_reg;
		}
		else
			r = regalloc->map(arg.getReg().armreg);
		if (arg.isShifted())
		{
			if (r != scratch_reg)
			{
				mov(scratch_reg, r);
				r = scratch_reg;
			}
			if (arg.shift_imm)
			{
				// shift by immediate
				if (arg.shift_type != ArmOp::ROR && arg.shift_value != 0 && !logical_op_set_flags)
				{
					switch (arg.shift_type)
					{
					case ArmOp::LSL:
						shl(r, arg.shift_value);
						break;
					case ArmOp::LSR:
						shr(r, arg.shift_value);
						break;
					case ArmOp::ASR:
						sar(r, arg.shift_value);
						break;
					default:
						die("invalid");
						break;
					}
				}
				else if (arg.shift_value == 0)
				{
					// Shift by 32
					if (logical_op_set_flags)
						set_carry_bit = true;
					if (arg.shift_type == ArmOp::LSR)
					{
						if (set_carry_bit)
						{
							mov(r10d, r);			// r10d = rm[31]
							shr(r10d, 31);
						}
						mov(r, 0);					// r = 0
					}
					else if (arg.shift_type == ArmOp::ASR)
					{
						if (set_carry_bit)
						{
							mov(r10d, r);			// r10d = rm[31]
							shr(r10d, 31);
						}
						sar(r, 31);					// r = rm < 0 ? -1 : 0
					}
					else if (arg.shift_type == ArmOp::ROR)
					{
						// RRX
						mov(r10d, dword[rip + &arm_Reg[RN_PSR_FLAGS].I]);
						shl(r10d, 2);
						verify(r != eax);
						mov(eax, r);				// eax = rm
						and_(r10d, 0x80000000);		// r10[31] = C
						shr(eax, 1);				// eax = eax >> 1
						or_(eax, r10d);				// eax[31] = C
						if (set_carry_bit)
						{
							mov(r10d, r);
							and_(r10d, 1);			// r10 = rm[0] (new C)
						}
						mov(r, eax);				// r = eax
					}
					else
						die("Invalid shift");
				}
				else
				{
					// Carry must be preserved or Ror shift
					if (logical_op_set_flags)
						set_carry_bit = true;
					if (arg.shift_type == ArmOp::LSL)
					{
						if (set_carry_bit)
							mov(r10d, r);
						if (set_carry_bit)
							shr(r10d, 32 - arg.shift_value);
						shl(r, arg.shift_value);			// r <<= shift
						if (set_carry_bit)
							and_(r10d, 1);					// r10d = rm[lsb]
					}
					else
					{
						if (set_carry_bit)
						{
							mov(r10d, r);
							shr(r10d, arg.shift_value - 1);
							and_(r10d, 1);					// r10d = rm[msb]
						}

						if (arg.shift_type == ArmOp::LSR)
							shr(r, arg.shift_value);		// r >>= shift
						else if (arg.shift_type == ArmOp::ASR)
							sar(r, arg.shift_value);
						else if (arg.shift_type == ArmOp::ROR)
							ror(r, arg.shift_value);
						else
							die("Invalid shift");
					}
				}
			}
			else
			{
				// shift by register
				const Xbyak::Reg32 shift_reg = regalloc->map(arg.shift_reg.armreg);
				switch (arg.shift_type)
				{
				case ArmOp::LSL:
				case ArmOp::LSR:
					mov(ecx, shift_reg);
					mov(eax, 0);
					if (arg.shift_type == ArmOp::LSL)
						shl(r, cl);
					else
						shr(r, cl);
					cmp(shift_reg, 32);
					cmovnb(r, eax);		// LSL and LSR by 32 or more gives 0
					break;
				case ArmOp::ASR:
					mov(ecx, shift_reg);
					mov(eax, r);
					sar(eax, 31);
					sar(r, cl);
					cmp(shift_reg, 32);
					cmovnb(r, eax);		// ASR by 32 or more gives 0 or -1 depending on operand sign
					break;
				case ArmOp::ROR:
					mov(ecx, shift_reg);
					ror(r, cl);
					break;
				default:
					die("Invalid shift");
					break;
				}
			}
		}
		return r;
	}

	Xbyak::Label *startConditional(ArmOp::Condition cc)
	{
		if (cc == ArmOp::AL)
			return nullptr;
		Xbyak::Label *label = new Xbyak::Label();
		cc = (ArmOp::Condition)((u32)cc ^ 1);	// invert the condition
		mov(eax, dword[rip + &arm_Reg[RN_PSR_FLAGS].I]);
		switch (cc)
		{
		case ArmOp::EQ:	// Z==1
			and_(eax, Z_FLAG);
			jnz(*label);
			break;
		case ArmOp::NE:	// Z==0
			and_(eax, Z_FLAG);
			jz(*label);
			break;
		case ArmOp::CS:	// C==1
			and_(eax, C_FLAG);
			jnz(*label);
			break;
		case ArmOp::CC:	// C==0
			and_(eax, C_FLAG);
			jz(*label);
			break;
		case ArmOp::MI:	// N==1
			and_(eax, N_FLAG);
			jnz(*label);
			break;
		case ArmOp::PL:	// N==0
			and_(eax, N_FLAG);
			jz(*label);
			break;
		case ArmOp::VS:	// V==1
			and_(eax, V_FLAG);
			jnz(*label);
			break;
		case ArmOp::VC:	// V==0
			and_(eax, V_FLAG);
			jz(*label);
			break;
		case ArmOp::HI:	// (C==1) && (Z==0)
			and_(eax, C_FLAG | Z_FLAG);
			cmp(eax, C_FLAG);
			jz(*label);
			break;
		case ArmOp::LS:	// (C==0) || (Z==1)
			and_(eax, C_FLAG | Z_FLAG);
			cmp(eax, C_FLAG);
			jnz(*label);
			break;
		case ArmOp::GE:	// N==V
			mov(ecx, eax);
			shl(ecx, 3);
			xor_(eax, ecx);
			and_(eax, N_FLAG);
			jz(*label);
			break;
		case ArmOp::LT:	// N!=V
			mov(ecx, eax);
			shl(ecx, 3);
			xor_(eax, ecx);
			and_(eax, N_FLAG);
			jnz(*label);
			break;
		case ArmOp::GT:	// (Z==0) && (N==V)
			mov(ecx, eax);
			mov(edx, eax);
			shl(ecx, 3);
			shl(edx, 1);
			xor_(eax, ecx);
			or_(eax, edx);
			and_(eax, N_FLAG);
			jz(*label);
			break;
		case ArmOp::LE:	// (Z==1) || (N!=V)
			mov(ecx, eax);
			mov(edx, eax);
			shl(ecx, 3);
			shl(edx, 1);
			xor_(eax, ecx);
			or_(eax, edx);
			and_(eax, N_FLAG);
			jnz(*label);
			break;
		default:
			die("Invalid condition code");
			break;
		}

		return label;
	}

	void endConditional(Xbyak::Label *label)
	{
		if (label != NULL)
		{
			L(*label);
			delete label;
		}
	}

	bool emitDataProcOp(const ArmOp& op)
	{
		bool save_v_flag = true;

		Xbyak::Operand arg0 = getOperand(op.arg[0], r8d);
		Xbyak::Operand arg1 = getOperand(op.arg[1], r9d);
		Xbyak::Reg32 rd;
		if (op.rd.isReg())
			rd = regalloc->map(op.rd.getReg().armreg);
		if (logical_op_set_flags)
		{
			// When an Operand2 constant is used with the instructions MOVS, MVNS, ANDS, ORRS, ORNS, EORS, BICS, TEQ or TST,
			// the carry flag is updated to bit[31] of the constant,
			// if the constant is greater than 255 and can be produced by shifting an 8-bit value.
			if (op.arg[0].isImmediate() && op.arg[0].getImmediate() > 255)
			{
				set_carry_bit = true;
				mov(r10d, (op.arg[0].getImmediate() & 0x80000000) >> 31);
			}
			else if (op.arg[1].isImmediate() && op.arg[1].getImmediate() > 255)
			{
				set_carry_bit = true;
				mov(r10d, (op.arg[1].getImmediate() & 0x80000000) >> 31);
			}
		}

		switch (op.op_type)
		{
		case ArmOp::AND:
			if (arg1 == rd)
				and_(rd, arg0);
			else
			{
				if (rd != arg0)
				{
					mov(rd, arg0);
					verify(rd != arg1);
				}
				if (!arg1.isNone())
					and_(rd, arg1);
				else
					and_(rd, op.arg[1].getImmediate());
			}
			save_v_flag = false;
			break;
		case ArmOp::ORR:
			if (arg1 == rd)
				or_(rd, arg0);
			else
			{
				if (rd != arg0)
				{
					// FIXME need static evaluation or this must be duplicated
					if (arg0.isNone())
						mov(rd, op.arg[0].getImmediate());
					else
						mov(rd, arg0);
					verify(rd != arg1);
				}
				if (!arg1.isNone())
					or_(rd, arg1);
				else
					or_(rd, op.arg[1].getImmediate());
			}
			save_v_flag = false;
			break;
		case ArmOp::EOR:
			if (arg1 == rd)
				xor_(rd, arg0);
			else
			{
				if (rd != arg0)
				{
					verify(rd != arg1);
					mov(rd, arg0);
				}
				if (!arg1.isNone())
					xor_(rd, arg1);
				else
					xor_(rd, op.arg[1].getImmediate());
			}
			save_v_flag = false;
			break;
		case ArmOp::BIC:
			if (arg1.isNone())
			{
				mov(eax, op.arg[1].getImmediate());
				arg1 = eax;
			}
			andn(rd, static_cast<Xbyak::Reg32&>(arg1), arg0);
			save_v_flag = false;
			break;

		case ArmOp::TST:
			if (!arg1.isNone())
				test(arg0, static_cast<Xbyak::Reg32&>(arg1));
			else
				test(arg0, op.arg[1].getImmediate());
			save_v_flag = false;
			break;
		case ArmOp::TEQ:
			if (arg0 != r8d)
				mov(r8d, arg0);
			if (!arg1.isNone())
				xor_(r8d, arg1);
			else
				xor_(r8d, op.arg[1].getImmediate());
			save_v_flag = false;
			break;
		case ArmOp::CMP:
			if (!arg1.isNone())
				cmp(arg0, arg1);
			else
				cmp(arg0, op.arg[1].getImmediate());
			if (set_flags)
			{
				setnb(r10b);
				set_carry_bit = true;
			}
			break;
		case ArmOp::CMN:
			if (arg0 != r8d)
				mov(r8d, arg0);
			if (!arg1.isNone())
				add(r8d, arg1);
			else
				add(r8d, op.arg[1].getImmediate());
			if (set_flags)
			{
				setb(r10b);
				set_carry_bit = true;
			}
			break;

		case ArmOp::MOV:
			if (arg0.isNone())
				mov(rd, op.arg[0].getImmediate());
			else if (arg0 != rd)
				mov(rd, arg0);
			if (set_flags)
			{
				test(rd, rd);
				save_v_flag = false;
			}
			break;
		case ArmOp::MVN:
			if (arg0.isNone())
				mov(rd, ~op.arg[0].getImmediate());
			else
			{
				if (arg0 != rd)
					mov(rd, arg0);
				not_(rd);
			}
			if (set_flags)
			{
				test(rd, rd);
				save_v_flag = false;
			}
			break;

		case ArmOp::SUB:
			if (arg1 == rd)
			{
				sub(arg0, arg1);
				if (rd != arg0)
					mov(rd, arg0);
			}
			else
			{
				if (rd != arg0)
					mov(rd, arg0);
				if (arg1.isNone())
					sub(rd, op.arg[1].getImmediate());
				else
					sub(rd, arg1);
			}
			if (set_flags)
			{
				setnb(r10b);
				set_carry_bit = true;
			}
			break;
		case ArmOp::RSB:
			if (arg1 == rd)
				sub(rd, arg0);
			else
			{
				if (rd != arg0)
					mov(rd, arg0);
				neg(rd);
				if (arg1.isNone())
					add(rd, op.arg[1].getImmediate());
				else
					add(rd, arg1);
			}
			if (set_flags)
			{
				setb(r10b);
				set_carry_bit = true;
			}
			break;
		case ArmOp::ADD:
			if (arg1 == rd)
				add(rd, arg0);
			else
			{
				if (rd != arg0)
				{
					// FIXME need static evaluation or this must be duplicated
					if (arg0.isNone())
						mov(rd, op.arg[0].getImmediate());
					else
						mov(rd, arg0);
				}
				if (arg1.isNone())
					add(rd, op.arg[1].getImmediate());
				else
					add(rd, arg1);
			}
			if (set_flags)
			{
				setb(r10b);
				set_carry_bit = true;
			}
			break;
		case ArmOp::ADC:
			mov(r11d, dword[rip + &arm_Reg[RN_PSR_FLAGS].I]);
			and_(r11d, C_FLAG);
			neg(r11d);
			if (arg1 == rd)
				adc(rd, arg0);
			else
			{
				if (rd != arg0)
					mov(rd, arg0);
				if (arg1.isNone())
					adc(rd, op.arg[1].getImmediate());
				else
					adc(rd, arg1);
			}
			if (set_flags)
			{
				setb(r10b);
				set_carry_bit = true;
			}
			break;
		case ArmOp::SBC:
			// rd = rn - op2 - !C
			mov(r11d, dword[rip + &arm_Reg[RN_PSR_FLAGS].I]);
			and_(r11d, C_FLAG);
			neg(r11d);
			cmc();		// on arm, -1 if carry is clear
			if (arg1 == rd)
			{
				sbb(arg0, arg1);
				if (rd != arg0)
					mov(rd, arg0);
			}
			else
			{
				if (rd != arg0)
					mov(rd, arg0);
				if (arg1.isNone())
					sbb(rd, op.arg[1].getImmediate());
				else
					sbb(rd, arg1);
			}
			if (set_flags)
			{
				setnb(r10b);
				set_carry_bit = true;
			}
			break;
		case ArmOp::RSC:
			// rd = op2 - rn - !C
			mov(r11d, dword[rip + &arm_Reg[RN_PSR_FLAGS].I]);
			and_(r11d, C_FLAG);
			neg(r11d);
			cmc();		// on arm, -1 if carry is clear
			if (arg1 == rd)
				sbb(rd, arg0);
			else
			{
				if (arg1.isNone())
					mov(rd, op.arg[1].getImmediate());
				else if (rd != arg1)
					mov(rd, arg1);
				sbb(rd, arg0);
			}
			if (set_flags)
			{
				setnb(r10b);
				set_carry_bit = true;
			}
			break;
		default:
			die("invalid");
			break;
		}

		return save_v_flag;
	}

	void emitMemOp(const ArmOp& op)
	{
		Xbyak::Operand addr_reg = getOperand(op.arg[0], call_regs[0]);
		if (addr_reg != call_regs[0])
		{
			if (addr_reg.isNone())
				mov(call_regs[0], op.arg[0].getImmediate());
			else
				mov(call_regs[0], addr_reg);
			addr_reg = call_regs[0];
		}
		if (op.pre_index)
		{
			const ArmOp::Operand& offset = op.arg[1];
			Xbyak::Operand offset_reg = getOperand(offset, r9d);
			if (!offset_reg.isNone())
			{
				if (op.add_offset)
					add(addr_reg, offset_reg);
				else
					sub(addr_reg, offset_reg);
			}
			else if (offset.isImmediate() && offset.getImmediate() != 0)
			{
				if (op.add_offset)
					add(addr_reg, offset.getImmediate());
				else
					sub(addr_reg, offset.getImmediate());
			}
		}
		if (op.op_type == ArmOp::STR)
		{
			if (op.arg[2].isImmediate())
				mov(call_regs[1], op.arg[2].getImmediate());
			else
				mov(call_regs[1], regalloc->map(op.arg[2].getReg().armreg));
		}

		call(arm7rec_getMemOp(op.op_type == ArmOp::LDR, op.byte_xfer));

		if (op.op_type == ArmOp::LDR)
			mov(regalloc->map(op.rd.getReg().armreg), eax);
	}

	void saveFlags(bool save_v_flag)
	{
		if (!set_flags)
			return;

		pushf();
		pop(rax);

		if (save_v_flag)
		{
			mov(r11d, eax);
			shl(r11d, 28 - 11);		// V
		}
		shl(eax, 30 - 6);			// Z,N
		if (save_v_flag)
			and_(r11d, V_FLAG);		// V
		and_(eax, Z_FLAG | N_FLAG);	// Z,N
		if (save_v_flag)
			or_(eax, r11d);

		mov(r11d, dword[rip + &arm_Reg[RN_PSR_FLAGS].I]);
		if (set_carry_bit)
		{
			if (save_v_flag)
				and_(r11d, ~(Z_FLAG | N_FLAG | C_FLAG | V_FLAG));
			else
				and_(r11d, ~(Z_FLAG | N_FLAG | C_FLAG));
			shl(r10d, 29);
			or_(r11d, r10d);
		}
		else
		{
			if (save_v_flag)
				and_(r11d, ~(Z_FLAG | N_FLAG | V_FLAG));
			else
				and_(r11d, ~(Z_FLAG | N_FLAG));
		}
		or_(r11d, eax);
		mov(dword[rip + &arm_Reg[RN_PSR_FLAGS].I], r11d);
	}

	void emitBranch(const ArmOp& op)
	{
		Xbyak::Operand addr_reg = getOperand(op.arg[0], eax);
		if (addr_reg.isNone())
			mov(eax, op.arg[0].getImmediate());
		else
		{
			if (eax != addr_reg)
				mov(eax, addr_reg);
			and_(eax, 0xfffffffc);
		}
		mov(dword[rip + &arm_Reg[R15_ARM_NEXT].I], eax);
	}

	void emitMSR(const ArmOp& op)
	{
		if (op.arg[0].isImmediate())
			mov(call_regs[0], op.arg[0].getImmediate());
		else
			mov(call_regs[0], regalloc->map(op.arg[0].getReg().armreg));
		if (op.spsr)
			call(MSR_do<1>);
		else
			call(MSR_do<0>);
	}

	void emitMRS(const ArmOp& op)
	{
		call(CPUUpdateCPSR);

		if (op.spsr)
			mov(regalloc->map(op.rd.getReg().armreg), dword[rip + &arm_Reg[RN_SPSR]]);
		else
			mov(regalloc->map(op.rd.getReg().armreg), dword[rip + &arm_Reg[RN_CPSR]]);
	}

	void emitFallback(const ArmOp& op)
	{
		set_flags = false;
		mov(call_regs[0], op.arg[0].getImmediate());
		call(arm_single_op);
#ifdef TAIL_CALLING
		sub(r14d, eax);
#else
		sub(dword[rip + &arm_Reg[CYCL_CNT].I], eax);
#endif
	}

public:
	Arm7Compiler() : Xbyak::CodeGenerator(ICacheSize - (icPtr - ICache), icPtr) { }

	void compile(const std::vector<ArmOp> block_ops, u32 cycles)
	{
		regalloc = new X64ArmRegAlloc(*this, block_ops);

#ifndef TAIL_CALLING
#ifdef _WIN32
		sub(rsp, 40);	// 16-byte alignment + 32-byte shadow area
#else
		sub(rsp, 8);	// 16-byte alignment
#endif
#endif
		for (u32 i = 0; i < block_ops.size(); i++)
		{
			const ArmOp& op = block_ops[i];
			DEBUG_LOG(AICA_ARM, "-> %s", op.toString().c_str());

			set_flags = op.flags & ArmOp::OP_SETS_FLAGS;
			logical_op_set_flags = op.isLogicalOp() && set_flags;
			set_carry_bit = false;
			bool save_v_flag = true;

			Xbyak::Label *condLabel = nullptr;

			if (op.op_type != ArmOp::FALLBACK)
				condLabel = startConditional(op.condition);

			regalloc->load(i);

			if (op.op_type <= ArmOp::MVN)
				// data processing op
				save_v_flag = emitDataProcOp(op);
			else if (op.op_type <= ArmOp::STR)
				// memory load/store
				emitMemOp(op);
			else if (op.op_type <= ArmOp::BL)
				// branch
				emitBranch(op);
			else if (op.op_type == ArmOp::MRS)
				emitMRS(op);
			else if (op.op_type == ArmOp::MSR)
				emitMSR(op);
			else if (op.op_type == ArmOp::FALLBACK)
				emitFallback(op);
			else
				die("invalid");

			saveFlags(save_v_flag);

			regalloc->store(i);

			endConditional(condLabel);
		}
#ifdef TAIL_CALLING
		sub(r14d, cycles);
#else
		mov(eax, cycles);
#endif
#ifdef TAIL_CALLING
		jmp((void*)&arm_dispatch);
#else
#ifdef _WIN32
		add(rsp, 40);
#else
		add(rsp, 8);
#endif
		ret();
#endif
		ready();
		icPtr += getSize();

		delete regalloc;
		regalloc = nullptr;
	}
};

void X64ArmRegAlloc::LoadReg(int host_reg, Arm7Reg armreg)
{
	// printf("LoadReg X%d <- r%d\n", host_reg, armreg);
	assembler.mov(getReg32(host_reg), dword[rip + &arm_Reg[(u32)armreg].I]);
}

void X64ArmRegAlloc::StoreReg(int host_reg, Arm7Reg armreg)
{
	// printf("StoreReg X%d -> r%d\n", host_reg, armreg);
	assembler.mov(dword[rip + &arm_Reg[(u32)armreg].I], getReg32(host_reg));
}

void arm7backend_compile(const std::vector<ArmOp> block_ops, u32 cycles)
{
	Arm7Compiler assembler;
	assembler.compile(block_ops, cycles);
}

#ifndef TAIL_CALLING
extern "C"
u32 arm_compilecode()
{
	CompileCode();
	return 0;
}

extern "C"
void arm_mainloop(u32 cycl, void* regs, void* entrypoints)
{
	entry_points = (u32 (**)())entrypoints;
	arm_Reg[CYCL_CNT].I += cycl;

	__asm__ (
			"push %rbx				\n\t"
			"push %rbp				\n\t"
#ifdef _WIN32
			"push %rdi				\n\t"
			"push %rsi				\n\t"
#endif
			"push %r12				\n\t"
			"push %r13				\n\t"
			"push %r14				\n\t"
			"push %r15				\n\t"
	);

	while ((int)arm_Reg[CYCL_CNT].I > 0)
	{
		if (arm_Reg[INTR_PEND].I)
			CPUFiq();

		arm_Reg[CYCL_CNT].I -= entry_points[(arm_Reg[R15_ARM_NEXT].I & (ARAM_SIZE_MAX - 1)) / 4]();
	}

	__asm__ (
			"pop %r15				\n\t"
			"pop %r14				\n\t"
			"pop %r13				\n\t"
			"pop %r12				\n\t"
#ifdef _WIN32
			"pop %rsi				\n\t"
			"pop %rdi				\n\t"
#endif
			"pop %rbp				\n\t"
			"pop %rbx				\n\t"
	);
}

#else

#ifdef __MACH__
#define _U "_"
#else
#define _U
#endif
__asm__ (
		".globl " _U"arm_compilecode		\n"
	_U"arm_compilecode:						\n\t"
		"call " _U"CompileCode				\n\t"
		"jmp " _U"arm_dispatch				\n\t"

		".globl " _U"arm_mainloop			\n"
	_U"arm_mainloop:						\n\t"	//  arm_mainloop(cycles, regs, entry points)
#ifdef _WIN32
		"pushq %rdi							\n\t"
		"pushq %rsi							\n\t"
#endif
		"pushq %r12							\n\t"
		"pushq %r13							\n\t"
		"pushq %r14							\n\t"
		"pushq %r15							\n\t"
		"pushq %rbx							\n\t"
		"pushq %rbp							\n\t"
#ifdef _WIN32
		"subq $40, %rsp						\n\t"	// 32-byte shadow space + 16-byte stack alignment
#else
		"subq $8, %rsp						\n\t"	// 16-byte stack alignment
#endif

		"movl " _U"arm_Reg + 192(%rip), %r14d \n\t"	// CYCL_CNT
#ifdef _WIN32
		"add %ecx, %r14d					\n\t"	// add cycles for this timeslice
		"movq %r8, entry_points(%rip)		\n\t"
#else
		"add %edi, %r14d					\n\t"	// add cycles for this timeslice
		"movq %rdx, " _U"entry_points(%rip)	\n\t"
#endif

		".globl " _U"arm_dispatch			\n"
	_U"arm_dispatch:						\n\t"
		"movq " _U"entry_points(%rip), %rdx	\n\t"
		"movl " _U"arm_Reg + 184(%rip), %ecx \n\t"	// R15_ARM_NEXT
		"movl " _U"arm_Reg + 188(%rip), %eax \n\t"	// INTR_PEND
		"cmp $0, %r14d						\n\t"
		"jle 2f								\n\t"	// timeslice is over
		"test %eax, %eax					\n\t"
		"jne 1f								\n\t"	// if interrupt pending, handle it

		"and $0x7ffffc, %ecx				\n\t"
		"jmp *(%rdx, %rcx, 2)				\n"

	"1:										\n\t"	// arm_dofiq:
		"call " _U"CPUFiq					\n\t"
		"jmp " _U"arm_dispatch				\n"

	"2:										\n\t"	// arm_exit:
		"movl %r14d, " _U"arm_Reg + 192(%rip) \n\t"	// CYCL_CNT: save remaining cycles
#ifdef _WIN32
		"addq $40, %rsp						\n\t"
#else
		"addq $8, %rsp						\n\t"
#endif
		"popq %rbp							\n\t"
		"popq %rbx							\n\t"
		"popq %r15							\n\t"
		"popq %r14							\n\t"
		"popq %r13							\n\t"
		"popq %r12							\n\t"
#ifdef _WIN32
		"popq %rsi							\n\t"
		"popq %rdi							\n\t"
#endif
		"ret								\n"
);
#endif // TAIL_CALLING
#endif // X64 && DYNAREC_JIT
