#include "arm7.h"
#include "arm_mem.h"
#include "hw/mem/_vmem.h"

#define arm_printf(...) DEBUG_LOG(AICA_ARM, __VA_ARGS__)

#define CPUReadMemoryQuick(addr) (*(u32*)&aica_ram[(addr) & ARAM_MASK])
#define CPUReadByte arm_ReadMem8
#define CPUReadMemory arm_ReadMem32
#define CPUReadHalfWord arm_ReadMem16
#define CPUReadHalfWordSigned(addr) ((s16)arm_ReadMem16(addr))

#define CPUWriteMemory arm_WriteMem32
#define CPUWriteHalfWord arm_WriteMem16
#define CPUWriteByte arm_WriteMem8

#define reg arm_Reg
#define armNextPC reg[R15_ARM_NEXT].I

#define CPUUpdateTicksAccesint(a) 1
#define CPUUpdateTicksAccessSeq32(a) 1
#define CPUUpdateTicksAccesshort(a) 1
#define CPUUpdateTicksAccess32(a) 1
#define CPUUpdateTicksAccess16(a) 1

alignas(8) reg_pair arm_Reg[RN_ARM_REG_COUNT];

static void CPUSwap(u32 *a, u32 *b)
{
	u32 c = *b;
	*b = *a;
	*a = c;
}

#define N_FLAG (reg[RN_PSR_FLAGS].FLG.N)
#define Z_FLAG (reg[RN_PSR_FLAGS].FLG.Z)
#define C_FLAG (reg[RN_PSR_FLAGS].FLG.C)
#define V_FLAG (reg[RN_PSR_FLAGS].FLG.V)

bool armIrqEnable;
bool armFiqEnable;
int armMode;

bool Arm7Enabled = false;

static u8 cpuBitsSet[256];

static void CPUSwitchMode(int mode, bool saveState);
extern "C" void CPUFiq();
static void CPUUpdateCPSR();
static void CPUUpdateFlags();
static void CPUSoftwareInterrupt(int comment);
static void CPUUndefinedException();

#if FEAT_AREC == DYNAREC_NONE

//
// ARM7 interpreter
//
void arm_Run(u32 CycleCount)
{
	if (!Arm7Enabled)
		return;

	u32 clockTicks = 0;
	while (clockTicks < CycleCount)
	{
		if (reg[INTR_PEND].I)
			CPUFiq();

		reg[15].I = armNextPC + 8;
		#include "arm-new.h"
	}
}
#endif

static void armt_init();

void arm_Init()
{
#if FEAT_AREC != DYNAREC_NONE
	armt_init();
#endif
	arm_Reset();

	for (int i = 0; i < 256; i++)
	{
		int count = 0;
		for (int j = 0; j < 8; j++)
			if (i & (1 << j))
				count++;

		cpuBitsSet[i] = count;
	}
}

static void CPUSwitchMode(int mode, bool saveState)
{
	CPUUpdateCPSR();

	switch(armMode)
	{
	case 0x10:
	case 0x1F:
		reg[R13_USR].I = reg[13].I;
		reg[R14_USR].I = reg[14].I;
		reg[RN_SPSR].I = reg[RN_CPSR].I;
		break;
	case 0x11:
		CPUSwap(&reg[R8_FIQ].I, &reg[8].I);
		CPUSwap(&reg[R9_FIQ].I, &reg[9].I);
		CPUSwap(&reg[R10_FIQ].I, &reg[10].I);
		CPUSwap(&reg[R11_FIQ].I, &reg[11].I);
		CPUSwap(&reg[R12_FIQ].I, &reg[12].I);
		reg[R13_FIQ].I = reg[13].I;
		reg[R14_FIQ].I = reg[14].I;
		reg[SPSR_FIQ].I = reg[RN_SPSR].I;
		break;
	case 0x12:
		reg[R13_IRQ].I  = reg[13].I;
		reg[R14_IRQ].I  = reg[14].I;
		reg[SPSR_IRQ].I =  reg[RN_SPSR].I;
		break;
	case 0x13:
		reg[R13_SVC].I  = reg[13].I;
		reg[R14_SVC].I  = reg[14].I;
		reg[SPSR_SVC].I =  reg[RN_SPSR].I;
		break;
	case 0x17:
		reg[R13_ABT].I  = reg[13].I;
		reg[R14_ABT].I  = reg[14].I;
		reg[SPSR_ABT].I =  reg[RN_SPSR].I;
		break;
	case 0x1b:
		reg[R13_UND].I  = reg[13].I;
		reg[R14_UND].I  = reg[14].I;
		reg[SPSR_UND].I =  reg[RN_SPSR].I;
		break;
	}

	u32 CPSR = reg[RN_CPSR].I;
	u32 SPSR = reg[RN_SPSR].I;

	switch(mode)
	{
	case 0x10:
	case 0x1F:
		reg[13].I = reg[R13_USR].I;
		reg[14].I = reg[R14_USR].I;
		reg[RN_CPSR].I = SPSR;
		break;
	case 0x11:
		CPUSwap(&reg[8].I, &reg[R8_FIQ].I);
		CPUSwap(&reg[9].I, &reg[R9_FIQ].I);
		CPUSwap(&reg[10].I, &reg[R10_FIQ].I);
		CPUSwap(&reg[11].I, &reg[R11_FIQ].I);
		CPUSwap(&reg[12].I, &reg[R12_FIQ].I);
		reg[13].I = reg[R13_FIQ].I;
		reg[14].I = reg[R14_FIQ].I;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_FIQ].I;
		break;
	case 0x12:
		reg[13].I = reg[R13_IRQ].I;
		reg[14].I = reg[R14_IRQ].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_IRQ].I;
		break;
	case 0x13:
		reg[13].I = reg[R13_SVC].I;
		reg[14].I = reg[R14_SVC].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_SVC].I;
		break;
	case 0x17:
		reg[13].I = reg[R13_ABT].I;
		reg[14].I = reg[R14_ABT].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_ABT].I;
		break;
	case 0x1b:
		reg[13].I = reg[R13_UND].I;
		reg[14].I = reg[R14_UND].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_UND].I;
		break;
	default:
		ERROR_LOG(AICA_ARM, "Unsupported ARM mode %02x", mode);
		die("Arm error..");
		break;
	}
	armMode = mode;
	CPUUpdateFlags();
	CPUUpdateCPSR();
}

static void CPUUpdateCPSR()
{
	reg_pair CPSR;

	CPSR.I = reg[RN_CPSR].I & 0x40;

	CPSR.PSR.NZCV = reg[RN_PSR_FLAGS].FLG.NZCV;

	if (!armFiqEnable)
		CPSR.I |= 0x40;
	if(!armIrqEnable)
		CPSR.I |= 0x80;

	CPSR.PSR.M = armMode;
	
	reg[RN_CPSR].I = CPSR.I;
}

static void CPUUpdateFlags()
{
	u32 CPSR = reg[RN_CPSR].I;

	reg[RN_PSR_FLAGS].FLG.NZCV = reg[RN_CPSR].PSR.NZCV;

	armIrqEnable = (CPSR & 0x80) ? false : true;
	armFiqEnable = (CPSR & 0x40) ? false : true;
	update_armintc();
}

static void CPUSoftwareInterrupt(int comment)
{
	u32 PC = reg[R15_ARM_NEXT].I+4;
	CPUSwitchMode(0x13, true);
	reg[14].I = PC;
	
	armIrqEnable = false;
	armNextPC = 0x08;
}

static void CPUUndefinedException()
{
	WARN_LOG(AICA_ARM, "arm7: CPUUndefinedException(). SOMETHING WENT WRONG");
	u32 PC = reg[R15_ARM_NEXT].I+4;
	CPUSwitchMode(0x1b, true);
	reg[14].I = PC;
	armIrqEnable = false;
	armNextPC = 0x04;
}

void FlushCache();

void arm_Reset()
{
#if FEAT_AREC != DYNAREC_NONE
	FlushCache();
#endif
	aica_interr = false;
	aica_reg_L = 0;
	e68k_out = false;
	e68k_reg_L = 0;
	e68k_reg_M = 0;

	Arm7Enabled = false;
	// clean registers
	memset(&arm_Reg[0], 0, sizeof(arm_Reg));

	armMode = 0x13;

	reg[13].I = 0x03007F00;
	reg[15].I = 0x0000000;
	reg[RN_CPSR].I = 0x00000000;
	reg[R13_IRQ].I = 0x03007FA0;
	reg[R13_SVC].I = 0x03007FE0;
	armIrqEnable = true;      
	armFiqEnable = false;
	update_armintc();

	C_FLAG = V_FLAG = N_FLAG = Z_FLAG = false;

	// disable FIQ
	reg[RN_CPSR].I |= 0x40;

	CPUUpdateCPSR();

	armNextPC = reg[15].I;
	reg[15].I += 4;
}

extern "C"
NOINLINE
void CPUFiq()
{
	u32 PC = reg[R15_ARM_NEXT].I+4;
	CPUSwitchMode(0x11, true);
	reg[14].I = PC;
	armIrqEnable = false;
	armFiqEnable = false;
	update_armintc();

	armNextPC = 0x1c;
}


/*
	--Seems like aica has 3 interrupt controllers actualy (damn lazy sega ..)
	The "normal" one (the one that exists on scsp) , one to emulate the 68k intc , and , 
	of course , the arm7 one

	The output of the sci* bits is input to the e68k , and the output of e68k is inputed into the FIQ
	pin on arm7
*/


void arm_SetEnabled(bool enabled)
{
	if(!Arm7Enabled && enabled)
			arm_Reset();
	
	Arm7Enabled=enabled;
}

void update_armintc()
{
	reg[INTR_PEND].I=e68k_out && armFiqEnable;
}

#if FEAT_AREC != DYNAREC_NONE
//
// ARM7 Recompiler
//

#if HOST_OS == OS_DARWIN
#include <sys/mman.h>
#endif

extern "C" void CompileCode();

//Emulate a single arm op, passed in opcode

u32 DYNACALL arm_single_op(u32 opcode)
{
	u32 clockTicks=0;

#define NO_OPCODE_READ

#include "arm-new.h"

	return clockTicks;
}

struct ArmDPOP
{
	u32 key;
	u32 mask;
	u32 flags;
};

static vector<ArmDPOP> ops;

enum OpFlags
{
	OP_SETS_PC         = 1,
	OP_READS_PC        = 32768,
	OP_IS_COND         = 65536,
	OP_MFB             = 0x80000000,

	OP_HAS_RD_12       = 2,
	OP_HAS_RD_16       = 4,
	OP_HAS_RS_0        = 8,
	OP_HAS_RS_8        = 16,
	OP_HAS_RS_16       = 32,
	OP_HAS_FLAGS_READ  = 4096,
	OP_HAS_FLAGS_WRITE = 8192,
	OP_HAS_RD_READ     = 16384, //For conditionals

	OP_WRITE_FLAGS     = 64,
	OP_WRITE_FLAGS_S   = 128,
	OP_READ_FLAGS      = 256,
	OP_READ_FLAGS_S    = 512,
	OP_WRITE_REG       = 1024,
	OP_READ_REG_1      = 2048,
};

#define DP_R_ROFC (OP_READ_FLAGS_S|OP_READ_REG_1) //Reads reg1, op2, flags if S
#define DP_R_ROF (OP_READ_FLAGS|OP_READ_REG_1)    //Reads reg1, op2, flags (ADC & co)
#define DP_R_OFC (OP_READ_FLAGS_S)                //Reads op2, flags if S

#define DP_W_RFC (OP_WRITE_FLAGS_S|OP_WRITE_REG)  //Writes reg, and flags if S
#define DP_W_F (OP_WRITE_FLAGS)                   //Writes only flags, always (S=1)

/*
	COND | 00 0 OP1   S Rn Rd SA    ST 0  Rm -- Data opcode, PSR xfer (imm shifted reg)
	     | 00 0 OP1   S Rn Rd Rs   0 ST 1 Rm -- Data opcode, PSR xfer (reg shifted reg)
	     | 00 0 0 00A S Rd Rn Rs    1001  Rm -- Mult
		 | 00 0 1 0B0 0 Rn Rd 0000  1001  Rm -- SWP
		 | 00 1 OP1   S Rn Rd imm8r4         -- Data opcode, PSR xfer (imm8r4)

		 | 01 0 P UBW L Rn Rd Offset          -- LDR/STR (I=0)
		 | 01 1 P UBW L Rn Rd SHAM SHTP 0 Rs  -- LDR/STR (I=1)
		 | 10 0 P USW L Rn {RList}            -- LDM/STM
		 | 10 1 L {offset}                    -- B/BL
		 | 11 1 1 X*                          -- SWI

		 (undef cases)
		 | 01 1 XXXX X X*  X*  X* 1 XXXX - Undefined (LDR/STR w/ encodings that would be reg. based shift)
		 | 11 0 PUNW L Rn {undef} -- Copr. Data xfer (undef)
		 | 11 1 0 CPOP Crn Crd Cpn CP3 0 Crm -- Copr. Data Op (undef)
		 | 11 1 0 CPO3 L Crn Crd Cpn CP3 1 Crm -- Copr. Reg xf (undef)


		 Phase #1:
			-Non branches that don't touch memory (pretty much: Data processing, Not MSR, Mult)
			-Everything else is ifb

		 Phase #2:
			Move LDR/STR to templates

		 Phase #3:
			Move LDM/STM to templates
			

*/

static void AddDPOP(u32 subcd, u32 rflags, u32 wflags)
{
	ArmDPOP op;

	u32 key=subcd<<21;
	u32 mask=(15<<21) | (7<<25);

	op.flags=rflags|wflags;
	
	if (wflags==DP_W_F)
	{
		//also match S bit for opcodes that must write to flags (CMP & co)
		mask|=1<<20;
		key|=1<<20;
	}

	//ISR form (bit 25=0, bit 4 = 0)
	op.key=key;
	op.mask=mask | (1<<4);
	ops.push_back(op);

	//RSR form (bit 25=0, bit 4 = 1, bit 7=0)
	op.key =  key  | (1<<4);
	op.mask = mask | (1<<4) | (1<<7);
	ops.push_back(op);

	//imm8r4 form (bit 25=1) 
	op.key =  key  | (1<<25);
	op.mask = mask;
	ops.push_back(op);
}

static void InitHash()
{
	/*
		COND | 00 I OP1  S Rn Rd OPER2 -- Data opcode, PSR xfer
		Data processing opcodes
	*/
		 
	//AND   0000        Rn, OPER2, {Flags}    Rd, {Flags}
	//EOR   0001        Rn, OPER2, {Flags}    Rd, {Flags}
	//SUB   0010        Rn, OPER2, {Flags}    Rd, {Flags}
	//RSB   0011        Rn, OPER2, {Flags}    Rd, {Flags}
	//ADD   0100        Rn, OPER2, {Flags}    Rd, {Flags}
	//ORR   1100        Rn, OPER2, {Flags}    Rd, {Flags}
	//BIC   1110        Rn, OPER2, {Flags}    Rd, {Flags}
	AddDPOP(0,DP_R_ROFC, DP_W_RFC);
	AddDPOP(1,DP_R_ROFC, DP_W_RFC);
	AddDPOP(2,DP_R_ROFC, DP_W_RFC);
	AddDPOP(3,DP_R_ROFC, DP_W_RFC);
	AddDPOP(4,DP_R_ROFC, DP_W_RFC);
	AddDPOP(12,DP_R_ROFC, DP_W_RFC);
	AddDPOP(14,DP_R_ROFC, DP_W_RFC);
	
	//ADC   0101        Rn, OPER2, Flags      Rd, {Flags}
	//SBC   0110        Rn, OPER2, Flags      Rd, {Flags}
	//RSC   0111        Rn, OPER2, Flags      Rd, {Flags}
	AddDPOP(5,DP_R_ROF, DP_W_RFC);
	AddDPOP(6,DP_R_ROF, DP_W_RFC);
	AddDPOP(7,DP_R_ROF, DP_W_RFC);

	//TST   1000 S=1    Rn, OPER2, Flags      Flags
	//TEQ   1001 S=1    Rn, OPER2, Flags      Flags
	AddDPOP(8,DP_R_ROF, DP_W_F);
	AddDPOP(9,DP_R_ROF, DP_W_F);

	//CMP   1010 S=1    Rn, OPER2             Flags
	//CMN   1011 S=1    Rn, OPER2             Flags
	AddDPOP(10,DP_R_ROF, DP_W_F);
	AddDPOP(11,DP_R_ROF, DP_W_F);
	
	//MOV   1101        OPER2, {Flags}        Rd, {Flags}
	//MVN   1111        OPER2, {Flags}        Rd, {Flags}
	AddDPOP(13,DP_R_OFC, DP_W_RFC);
	AddDPOP(15,DP_R_OFC, DP_W_RFC);
}


void  armEmit32(u32 emit32);
void *armGetEmitPtr();


#define _DEVEL          (1)
#define EMIT_I          armEmit32((I))
#define EMIT_GET_PTR()  armGetEmitPtr()
u8* icPtr;
u8* ICache;

extern const u32 ICacheSize = 1024 * 1024;
#ifdef _WIN32
alignas(4096) static u8 ARM7_TCB[ICacheSize];
#elif HOST_OS == OS_LINUX

alignas(4096) static u8 ARM7_TCB[ICacheSize] __attribute__((section(".text")));

#elif HOST_OS==OS_DARWIN
alignas(4096) static u8 ARM7_TCB[ICacheSize] __attribute__((section("__TEXT, .text")));
#else
#error ARM7_TCB ALLOC
#endif

#include "arm_emitter/arm_emitter.h"
#undef I

using namespace ARM;

void* EntryPoints[ARAM_SIZE_MAX/4];

enum OpType
{
	VOT_Fallback,
	VOT_DataOp,
	VOT_B,
	VOT_BL,
	VOT_BR,     //Branch (to register)
	VOT_Read,   //Actually, this handles LDR and STR
	//VOT_LDM,  //This Isn't used anymore
	VOT_MRS,
	VOT_MSR,
};

void armv_call(void* target, bool expect_result);
void armv_setup();
void armv_intpr(u32 opcd);
void armv_end(void* codestart, u32 cycles);
void armv_imm_to_reg(u32 regn, u32 imm);
void armv_MOV32(eReg regn, u32 imm);

extern "C" void arm_dispatch();
extern "C" void DYNACALL
#if defined(__GNUC__) && !defined(__llvm__)
	// Avoid inlining / duplicating / whatever
	__attribute__ ((optimize(0)))
#endif
		arm_mainloop(u32 cycl, void* regs, void* entrypoints);
extern "C" u32 DYNACALL arm_compilecode();

template <bool Load, bool Byte>
u32 DYNACALL DoMemOp(u32 addr,u32 data)
{
	u32 rv=0;

	if (Load)
	{
		if (Byte)
			rv=arm_ReadMem8(addr);
		else
			rv=arm_ReadMem32(addr);
	}
	else
	{
		if (Byte)
			arm_WriteMem8(addr,data);
		else
			arm_WriteMem32(addr,data);
	}

	return rv;
}

//findfirstset -- used in LDM/STM handling
#if defined(_WIN32) && !defined(__GNUC__)
#include <intrin.h>

static u32 findfirstset(u32 v)
{
	unsigned long rv;
	_BitScanForward(&rv,v);
	return rv+1;
}
#else
#define findfirstset __builtin_ffs
#endif

#if 0
//LDM isn't perf. citrical, and as a result, not implemented fully. 
//So this code is disabled
//mask is *2
template<u32 I>
void DYNACALL DoLDM(u32 addr, u32 mask)
{
	//addr=(addr); //force align ?

	u32 idx=-1;
	do
	{
		u32 tz=findfirstset(mask);
		mask>>=tz;
		idx+=tz;
		arm_Reg[idx].I=arm_ReadMem32(addr);
		addr+=4;
	} while(mask);
}
#endif

static void* GetMemOp(bool Load, bool Byte)
{
	if (Load)
	{
		if (Byte)
			return (void*)(u32(DYNACALL*)(u32,u32))&DoMemOp<true,true>;
		else
			return (void*)(u32(DYNACALL*)(u32,u32))&DoMemOp<true,false>;
	}
	else
	{
		if (Byte)
			return (void*)(u32(DYNACALL*)(u32,u32))&DoMemOp<false,true>;
		else
			return (void*)(u32(DYNACALL*)(u32,u32))&DoMemOp<false,false>;
	}
}

//Decodes an opcode, returns type. 
//opcd might be changed (currently for LDM/STM -> LDR/STR transforms)
static OpType DecodeOpcode(u32& opcd,u32& flags)
{
	//by default, PC has to be updated
	flags=OP_READS_PC;

	u32 CC=(opcd >> 28);

	if (CC!=CC_AL)
		flags|=OP_IS_COND;

	//helpers ...
	#define CHK_BTS(M,S,V) ( (M & (opcd>>S)) == (V) ) //Check bits value in opcode
	#define IS_LOAD (opcd & (1<<20))                  //Is L bit set ? (LDM/STM LDR/STR)
	#define READ_PC_CHECK(S) if (CHK_BTS(15,S,15)) flags|=OP_READS_PC;

	//Opcode sets pc ?
	bool _set_pc=
		(CHK_BTS(3,26,0) && CHK_BTS(15,12,15))             || //Data processing w/ Rd=PC
		(CHK_BTS(3,26,1) && CHK_BTS(15,12,15) && IS_LOAD ) || //LDR/STR w/ Rd=PC 
		(CHK_BTS(7,25,4) && (opcd & 32768) &&  IS_LOAD)    || //LDM/STM w/ PC in list	
		CHK_BTS(7,25,5)                                    || //B or BL
		CHK_BTS(15,24,15);                                    //SWI
	
	//NV condition means VFP on newer cores, let interpreter handle it...
	if (CC==15)
		return VOT_Fallback;

	if (_set_pc)
		flags|=OP_SETS_PC;

	//B / BL ?
	if (CHK_BTS(7,25,5))
	{
		verify(_set_pc);
		if (!(flags&OP_IS_COND))
			flags&=~OP_READS_PC;  //not COND doesn't read from pc

		flags|=OP_SETS_PC;        //Branches Set pc ..

		//branch !
		return (opcd&(1<<24))?VOT_BL:VOT_B;
	}

	//Common case: MOVCC PC,REG
	if (CHK_BTS(0xFFFFFF,4,0x1A0F00))
	{
		verify(_set_pc);
		if (CC==CC_AL)
			flags&=~OP_READS_PC;

		return VOT_BR;
	}


	//No support for COND branching opcodes apart from the forms above ..
	if (CC!=CC_AL && _set_pc)
	{
		return VOT_Fallback;
	}

	u32 RList=opcd&0xFFFF;
	u32 Rn=(opcd>>16)&15;

#define LDM_REGCNT() (cpuBitsSet[RList & 255] + cpuBitsSet[(RList >> 8) & 255])


	//Data Processing opcodes -- find using mask/key
	//This will eventually be virtualised w/ register renaming
	for( u32 i=0;i<ops.size();i++)
	{
		if (!_set_pc && ops[i].key==(opcd&ops[i].mask))
		{
			//We fill in the cases that we have to read pc
			flags &= ~OP_READS_PC;

			//Conditionals always need flags read ...
			if ((opcd >> 28)!=0xE)
			{
				flags |= OP_HAS_FLAGS_READ;
				//if (flags & OP_WRITE_REG)
					flags |= OP_HAS_RD_READ;
			}

			//DPOP !

			if ((ops[i].flags & OP_READ_FLAGS) ||
			   ((ops[i].flags & OP_READ_FLAGS_S) && (opcd & (1<<20))))
			{
				flags |= OP_HAS_FLAGS_READ;
			}

			if ((ops[i].flags & OP_WRITE_FLAGS) ||
			   ((ops[i].flags & OP_WRITE_FLAGS_S) && (opcd & (1<<20))))
			{
				flags |= OP_HAS_FLAGS_WRITE;
			}

			if(ops[i].flags & OP_WRITE_REG)
			{
				//All dpops that write, write to RD_12
				flags |= OP_HAS_RD_12;
				verify(! (CHK_BTS(15,12,15) && CC!=CC_AL));
			}

			if(ops[i].flags & OP_READ_REG_1)
			{
				//Reg 1 is RS_16
				flags |= OP_HAS_RS_16;

				//reads from pc ?
				READ_PC_CHECK(16);
			}

			//op2 is imm or reg ?
			if ( !(opcd & (1<<25)) )
			{
				//its reg (register or imm shifted)
				flags |= OP_HAS_RS_0;
				//reads from pc ?
				READ_PC_CHECK(0);

				//is it register shifted reg ?
				if (opcd & (1<<4))
				{
					verify(! (opcd & (1<<7)) );	//must be zero
					flags |= OP_HAS_RS_8;
					//can't be pc ...
					verify(!CHK_BTS(15,8,15));
				}
				else
				{
					//is it RRX ?
					if ( ((opcd>>4)&7)==6)
					{
						//RRX needs flags to be read (even if the opcode doesn't)
						flags |= OP_HAS_FLAGS_READ;
					}
				}
			}

			return VOT_DataOp;
		}
	}

	//Lets try mem opcodes since its not data processing


	
	/*
		Lets Check LDR/STR !

		CCCC 01 0 P UBW L Rn Rd Offset	-- LDR/STR (I=0)
	*/
	if ((opcd>>25)==(0xE4/2) )
	{
		/*
			I=0

			Everything else handled
		*/
		arm_printf("ARM: MEM %08X L/S:%d, AWB:%d!",opcd,(opcd>>20)&1,(opcd>>21)&1);

		return VOT_Read;
	}
	else if ((opcd>>25)==(0xE6/2) && CHK_BTS(0x7,4,0) )
	{
		arm_printf("ARM: MEM REG to Reg %08X",opcd);
		
		/*
			I=1

			Logical Left shift, only
		*/
		return VOT_Read;
	}
	//LDM common case
	else if ((opcd>>25)==(0xE8/2) /*&& CHK_BTS(32768,0,0)*/ && CHK_BTS(1,22,0) && CHK_BTS(1,20,1) && LDM_REGCNT()==1)
	{
		//P=0
		//U=1
		//L=1
		//W=1
		//S=0
		
		u32 old_opcd=opcd;

		//One register xfered
		//Can be rewriten as normal mem opcode ..
		opcd=0xE4000000;

		//Imm offset
		opcd |= 0<<25;
		//Post incr
		opcd |= old_opcd & (1<<24);
		//Up/Dn
		opcd |= old_opcd & (1<<23);
		//Word/Byte
		opcd |= 0<<22;
		//Write back (must be 0 for PI)
		opcd |= old_opcd & (1<<21);
		//Load
		opcd |= old_opcd & (1<<20);

		//Rn
		opcd |= Rn<<16;

		//Rd
		u32 Rd=findfirstset(RList)-1;
		opcd |= Rd<<12;

		//Offset
		opcd |= 4;

		arm_printf("ARM: MEM TFX R %08X",opcd);

		return VOT_Read;
	}
	//STM common case
	else if ((opcd>>25)==(0xE8/2) && CHK_BTS(1,22,0) && CHK_BTS(1,20,0) && LDM_REGCNT()==1)
	{
		//P=1
		//U=0
		//L=1
		//W=1
		//S=0
		
		u32 old_opcd=opcd;

		//One register xfered
		//Can be rewriten as normal mem opcode ..
		opcd=0xE4000000;

		//Imm offset
		opcd |= 0<<25;
		//Pre/Post incr
		opcd |= old_opcd & (1<<24);
		//Up/Dn
		opcd |= old_opcd & (1<<23);
		//Word/Byte
		opcd |= 0<<22;
		//Write back
		opcd |= old_opcd & (1<<21);
		//Store/Load
		opcd |= old_opcd & (1<<20);

		//Rn
		opcd |= Rn<<16;

		//Rd
		u32 Rd=findfirstset(RList)-1;
		opcd |= Rd<<12;

		//Offset
		opcd |= 4;

		arm_printf("ARM: MEM TFX W %08X",opcd);

		return VOT_Read;
	}
	else if (CHK_BTS(0xE10F0FFF,0,0xE10F0000))
	{
		return VOT_MRS;
	}
	else if (CHK_BTS(0xEFBFFFF0,0,0xE129F000))
	{
		return VOT_MSR;
	}
	else if ((opcd>>25)==(0xE8/2) && CHK_BTS(32768,0,0))
	{
		arm_printf("ARM: MEM FB %08X",opcd);
		flags|=OP_MFB; //(flag Just for the fallback counters)
	}
	else
	{
		arm_printf("ARM: FB %08X",opcd);
	}

	//by default fallback to interpr
	return VOT_Fallback;
}

//helpers ...
#if HOST_CPU == CPU_ARM64 || HOST_CPU == CPU_X64

extern void LoadReg(eReg rd,u32 regn,ConditionCode cc=CC_AL);
extern void StoreReg(eReg rd,u32 regn,ConditionCode cc=CC_AL);
extern void armv_mov(ARM::eReg regd, ARM::eReg regn);
extern void armv_add(ARM::eReg regd, ARM::eReg regn, ARM::eReg regm);
extern void armv_sub(ARM::eReg regd, ARM::eReg regn, ARM::eReg regm);
extern void armv_add(ARM::eReg regd, ARM::eReg regn, s32 imm);
extern void armv_lsl(ARM::eReg regd, ARM::eReg regn, u32 imm);
extern void armv_bic(ARM::eReg regd, ARM::eReg regn, u32 imm);
extern void *armv_start_conditional(ARM::ConditionCode cc);
extern void armv_end_conditional(void *ref);
#if HOST_CPU == CPU_ARM64
// Use w25 for temp mem save because w9 is not callee-saved
#define r9 ((ARM::eReg)25)
#endif

#elif HOST_CPU == CPU_ARM

void LoadReg(eReg rd,u32 regn,ConditionCode cc=CC_AL)
{
	LDR(rd,r8,(u8*)&reg[regn].I-(u8*)&reg[0].I,Offset,cc);
}
void StoreReg(eReg rd,u32 regn,ConditionCode cc=CC_AL)
{
	STR(rd,r8,(u8*)&reg[regn].I-(u8*)&reg[0].I,Offset,cc);
}
void armv_mov(ARM::eReg regd, ARM::eReg regn)
{
	MOV(regd, regn);
}

void armv_add(ARM::eReg regd, ARM::eReg regn, ARM::eReg regm)
{
	ADD(regd, regn, regm);
}

void armv_sub(ARM::eReg regd, ARM::eReg regn, ARM::eReg regm)
{
	SUB(regd, regn, regm);
}

void armv_add(ARM::eReg regd, ARM::eReg regn, s32 imm)
{
	if (imm >= 0)
		ADD(regd, regn, imm);
	else
		SUB(regd, regn, -imm);
}

void armv_lsl(ARM::eReg regd, ARM::eReg regn, u32 imm)
{
	LSL(regd, regn, imm);
}

void armv_bic(ARM::eReg regd, ARM::eReg regn, u32 imm)
{
	BIC(regd, regn, imm);
}

void *armv_start_conditional(ARM::ConditionCode cc)
{
	return NULL;
}
void armv_end_conditional(void *ref)
{
}
#endif

//very quick-and-dirty register rename based virtualisation
static u32 renamed_regs[16];
static u32 rename_reg_base;

static void RenameRegReset()
{
	rename_reg_base=r1;
	memset(renamed_regs, 0, sizeof(renamed_regs));
}

//returns new reg #. didrn is true if a rename mapping was added
static u32 RenameReg(u32 reg, bool& didrn)
{
	if (renamed_regs[reg] == 0)
	{
		renamed_regs[reg]=rename_reg_base;
		rename_reg_base++;
		didrn=true;
	}
	else
	{
		didrn=false;
	}

	return renamed_regs[reg];
}

//For reg reads (they need to be loaded)
//load can be used to skip loading (for RD if not cond)
static void LoadAndRename(u32& opcd, u32 bitpos, bool load,u32 pc)
{
	bool didrn;
	u32 reg=(opcd>>bitpos)&15;

	u32 nreg=RenameReg(reg,didrn);

	opcd = (opcd& ~(15<<bitpos)) | (nreg<<bitpos);

	if (load && didrn)
	{
		if (reg==15)
			armv_MOV32((eReg)nreg,pc);
		else
			LoadReg((eReg)nreg,reg);
	}
}

//For results store (they need to be stored)
static void StoreAndRename(u32 opcd, u32 bitpos)
{
	bool didrn;
	u32 reg=(opcd>>bitpos)&15;

	u32 nreg=RenameReg(reg,didrn);

	verify(!didrn);

	if (reg==15)
		reg=R15_ARM_NEXT;

	StoreReg((eReg)nreg,reg);
}

#if HOST_CPU == CPU_ARM64 || HOST_CPU == CPU_X64
extern void LoadFlags();
extern void StoreFlags();
#elif HOST_CPU == CPU_ARM
//For COND
static void LoadFlags()
{
	//Load flags
	LoadReg(r0,RN_PSR_FLAGS);
	//move them to flags register
	MSR(0,8,r0);
}

static void StoreFlags()
{
	//get results from flags register
	MRS(r1,0);
	//Store flags
	StoreReg(r1,RN_PSR_FLAGS);
}
#endif

//Virtualise Data Processing opcode
static void VirtualizeOpcode(u32 opcd,u32 flag,u32 pc)
{
	//Keep original opcode for info
	u32 orig=opcd;

	//Load arm flags, RS0/8/16, RD12/16 (as indicated by the decoder flags)

	if (flag & OP_HAS_FLAGS_READ)
		LoadFlags();

	void *cond_op_label = armv_start_conditional((ARM::ConditionCode)(opcd >> 28));

	// Dynamic LSL/LSR/ASR/ROR adds +4 to pc due to delay
	bool shiftByReg = !(opcd & (1 << 25)) && (opcd & (1 << 4));
	if (flag & OP_HAS_RS_0)
		LoadAndRename(opcd, 0, true, pc + (shiftByReg ? 12 : 8));
	if (flag & OP_HAS_RS_8)
		LoadAndRename(opcd, 8, true, pc + 8);
	if (flag & OP_HAS_RS_16)
		LoadAndRename(opcd, 16, true, pc + (shiftByReg ? 12 : 8));

	if (flag & OP_HAS_RD_12)
		LoadAndRename(opcd,12,flag&OP_HAS_RD_READ,pc+4);

	if (flag & OP_HAS_RD_16)
	{
		verify(! (flag & OP_HAS_RS_16));
		LoadAndRename(opcd,16,flag&OP_HAS_RD_READ,pc+4);
	}

	//Opcode has been modified to use the new regs
	//Emit it ...
	arm_printf("Arm Virtual: %08X -> %08X",orig,opcd);
	armEmit32(opcd);

	//Store arm flags, rd12/rd16 (as indicated by the decoder flags)
	if (flag & OP_HAS_RD_12)
		StoreAndRename(orig,12);

	if (flag & OP_HAS_RD_16)
		StoreAndRename(orig,16);

	//Sanity check ..
	if (renamed_regs[15] != 0)
	{
		verify(flag&OP_READS_PC || (flag&OP_SETS_PC && !(flag&OP_IS_COND)));
	}

	if (flag & OP_HAS_FLAGS_WRITE)
		StoreFlags();

	armv_end_conditional(cond_op_label);
}

void *armGetEmitPtr()
{
	if (icPtr < (ICache+ICacheSize-1024))	//ifdebug
		return static_cast<void *>(icPtr);

	ERROR_LOG(AICA_ARM, "JIT buffer full: %zd bytes free", ICacheSize - (icPtr - ICache));
	die("AICA ARM code buffer full");
	return NULL;
}

#if	(HOST_CPU == CPU_ARM)

extern "C" void arm_exit();

/*
 *
 *	ARMv7 Compiler
 *
 */

void  armEmit32(u32 emit32)
{
	if (icPtr >= (ICache+ICacheSize-1024))
		die("ICache is full, invalidate old entries ...");	//ifdebug

	*(u32*)icPtr = emit32;  
	icPtr+=4;
}

#if HOST_OS==OS_DARWIN
#include <libkern/OSCacheControl.h>
extern "C" void armFlushICache(void *code, void *pEnd) {
    sys_dcache_flush(code, (u8*)pEnd - (u8*)code + 1);
    sys_icache_invalidate(code, (u8*)pEnd - (u8*)code + 1);
}
#else
extern "C" void armFlushICache(void *bgn, void *end) {
	__clear_cache(bgn, end);
}
#endif


void armv_imm_to_reg(u32 regn, u32 imm)
{
	MOV32(r0,imm);
	StoreReg(r0,regn);
}

void armv_call(void* loc, bool expect_result)
{
	CALL((u32)loc);
}

void armv_setup()
{
	//Setup emitter

	//r9: temp for mem ops (PI WB)
	//r8: base
	//Stored on arm_mainloop so no need for push/pop
}

void armv_intpr(u32 opcd)
{
	//Call interpreter
	MOV32(r0,opcd);
	CALL((u32)arm_single_op);
	SUB(r5, r5, r0, false);
}

void armv_end(void* codestart, u32 cycl)
{
	//Normal block end
	//cycle counter rv

	//pop registers & return
	if (is_i8r4(cycl))
		SUB(r5,r5,cycl,true);
	else
	{
		u32 togo = cycl;
		while(ARMImmid8r4_enc(togo) == -1)
		{
			SUB(r5,r5,256);
			togo -= 256;
		}
		SUB(r5,r5,togo,true);
	}
	JUMP((u32)&arm_exit,CC_MI);	//statically predicted as not taken
	JUMP((u32)&arm_dispatch);

	armFlushICache(codestart,(void*)EMIT_GET_PTR());
}

//Hook cus varm misses this, so x86 needs special code
void armv_MOV32(eReg regn, u32 imm)
{
	MOV32(regn,imm);
}

#endif	// HOST_CPU == CPU_ARM

//Run a timeslice for ARMREC
//CycleCount is pretty much fixed to (512*32) for now (might change to a diff constant, but will be constant)
void arm_Run(u32 CycleCount)
{
	if (Arm7Enabled)
		arm_mainloop(CycleCount, arm_Reg, EntryPoints);
}


#undef r

/*
	TODO:
	R15 read/writing is kind of .. weird
	Gotta investigate why ..
*/

//Mem operand 2 calculation, if Reg or large imm
static void MemOperand2(eReg dst,bool I, bool U,u32 offs, u32 opcd)
{
	if (I==true)
	{
		u32 Rm=(opcd>>0)&15;
		verify(CHK_BTS(7,4,0));// only SHL mode
		LoadReg(r1,Rm);
		u32 SA=31&(opcd>>7);
		//can't do shifted add for now -- EMITTER LIMIT --
		if (SA)
			armv_lsl(r1, r1, SA);
	}
	else
	{
		armv_MOV32(r1,offs);
	}

	if (U)
		armv_add(dst, r0, r1);
	else
		armv_sub(dst, r0, r1);
}

template<u32 Pd>
void DYNACALL MSR_do(u32 v)
{
	if (Pd)
	{
		if(armMode > 0x10 && armMode < 0x1f) /* !=0x10 ?*/
		{
			reg[RN_SPSR].I = (reg[RN_SPSR].I & 0x00FFFF00) | (v & 0xFF0000FF);
		}
	}
	else
	{
		CPUUpdateCPSR();
	
		u32 newValue = reg[RN_CPSR].I;
		if(armMode > 0x10)
		{
			newValue = (newValue & 0xFFFFFF00) | (v & 0x000000FF);
		}

		newValue = (newValue & 0x00FFFFFF) | (v & 0xFF000000);
		newValue |= 0x10;
		if(armMode > 0x10)
		{
			CPUSwitchMode(newValue & 0x1f, false);
		}
		reg[RN_CPSR].I = newValue;
		CPUUpdateFlags();
	}
}

//Compile & run block of code, starting armNextPC
extern "C" void CompileCode()
{
	//Get the code ptr
	void* rv=EMIT_GET_PTR();

	//update the block table
	// Note that we mask with the max aica size (8 MB), which is
	// also the size of the EntryPoints table. This way the dynarec
	// main loop doesn't have to worry about the actual aica
	// ram size. The aica ram always wraps to 8 MB anyway.
	EntryPoints[(armNextPC & (ARAM_SIZE_MAX - 1)) / 4] = rv;

	//setup local pc counter
	u32 pc=armNextPC;

	//emitter/block setup
	armv_setup();

	u32 Cycles=0;

	//the ops counter is used to terminate the block (max op count for a single block is 32 currently)
	//We don't want too long blocks for timing accuracy
	for (u32 ops = 0; ops < 32; ops++)
	{
		// Each opcode takes at least 6 cycles
		Cycles += 6;

		//Read opcode ...
		u32 opcd=CPUReadMemoryQuick(pc);

		u32 op_flags;

		//Decode & handle opcode

		OpType opt=DecodeOpcode(opcd,op_flags);

		switch(opt)
		{
		case VOT_DataOp:
			{
				//data processing opcode that can be virtualised
				RenameRegReset();

				VirtualizeOpcode(opcd,op_flags,pc);
			}
			break;
		
		case VOT_BR:
			{
				//Branch to reg
				ConditionCode cc=(ConditionCode)(opcd>>28);

				verify(op_flags&OP_SETS_PC);

				if (cc!=CC_AL)
				{
					LoadFlags();
					armv_imm_to_reg(R15_ARM_NEXT,pc+4);
				}

				void *ref = armv_start_conditional(cc);
				LoadReg(r0,opcd&0xF);
				armv_bic(r0, r0, 3);
				StoreReg(r0,R15_ARM_NEXT,cc);
				armv_end_conditional(ref);
				Cycles += 3;
			}
			break;

		case VOT_B:
		case VOT_BL:
			{
				//Branch to imm

				//<<2, sign extend !
				s32 offs=((s32)opcd<<8)>>6;

				if (op_flags & OP_IS_COND)
				{
					armv_imm_to_reg(R15_ARM_NEXT,pc+4);
					LoadFlags();
					ConditionCode cc=(ConditionCode)(opcd>>28);
					void *ref = armv_start_conditional(cc);
					if (opt==VOT_BL)
					{
						armv_MOV32(r0,pc+4);
						StoreReg(r0,14,cc);
					}

					armv_MOV32(r0,pc+8+offs);
					StoreReg(r0,R15_ARM_NEXT,cc);
					armv_end_conditional(ref);
				}
				else
				{
					if (opt==VOT_BL)
						armv_imm_to_reg(14,pc+4);

					armv_imm_to_reg(R15_ARM_NEXT,pc+8+offs);
				}
				Cycles += 3;
			}
			break;

		case VOT_Read:
			{
				//LDR/STR

				u32 offs=opcd&4095;
				bool U=opcd&(1<<23);
				bool Pre=opcd&(1<<24);
				
				bool W=opcd&(1<<21);
				bool I=opcd&(1<<25);
				bool L = opcd & (1 << 20);
				
				u32 Rn=(opcd>>16)&15;
				u32 Rd=(opcd>>12)&15;

				bool DoWB = (W || !Pre) && Rn != Rd;	//Write back if pre- or post-indexed and Rn!=Rd
				bool DoAdd=DoWB || Pre;

				//Register not updated anyway
				if (!I && offs == 0)
				{
					DoWB = false;
					DoAdd = false;
				}

				//verify(Rd!=15);
				verify(!((Rn==15) && DoWB));

				//AGU
				if (Rn!=15)
				{
					LoadReg(r0,Rn);

					if (DoAdd)
					{
						eReg dst=Pre?r0:r9;

						if (!I && is_i8r4(offs))
						{
							if (U)
								armv_add(dst, r0, offs);
							else
								armv_add(dst, r0, -offs);
						}
						else
						{
							MemOperand2(dst,I,U,offs,opcd);
						}

						if (DoWB && dst==r0)
							armv_mov(r9, r0);
					}
				}
				else
				{
					u32 addr=pc+8;

					if (Pre && offs && !I)
					{
						addr+=U?offs:-offs;
					}
					
					armv_MOV32(r0,addr);
					
					if (Pre && I)
					{
						MemOperand2(r1,I,U,offs,opcd);
						armv_add(r0, r0, r1);
					}
				}

				if (!L)
				{
					if (Rd==15)
					{
						armv_MOV32(r1,pc+12);
					}
					else
					{
						LoadReg(r1,Rd);
					}
				}
				//Call handler
				armv_call(GetMemOp(L, CHK_BTS(1,22,1)), L);

				if (L)
				{
					if (Rd==15)
					{
						verify(op_flags & OP_SETS_PC);
						StoreReg(r0,R15_ARM_NEXT);
					}
					else
					{
						StoreReg(r0,Rd);
					}
				}
				
				//Write back from AGU, if any
				if (DoWB)
				{
					StoreReg(r9,Rn);
				}
				if (L)
					Cycles += 4;
				else
					Cycles += 3;
			}
			break;

		case VOT_MRS:
			{
				u32 Rd=(opcd>>12)&15;

				armv_call((void*)&CPUUpdateCPSR, false);

				if (opcd & (1<<22))
					LoadReg(r0, RN_SPSR);
				else
					LoadReg(r0, RN_CPSR);

				StoreReg(r0,Rd);
			}
			break;

		case VOT_MSR:
			{
				u32 Rm=(opcd>>0)&15;

				LoadReg(r0,Rm);
				if (opcd & (1<<22))
					armv_call((void*)(void (DYNACALL*)(u32))&MSR_do<1>, false);
				else
					armv_call((void*)(void (DYNACALL*)(u32))&MSR_do<0>, false);

				if (op_flags & OP_SETS_PC)
					armv_imm_to_reg(R15_ARM_NEXT,pc+4);
				Cycles++;
			}
			break;
		/*
		//LDM is disabled for now
		//Common cases of LDM/STM are converted to STR/LDR (tsz==1)
		//Other cases are very uncommon and not worth implementing
		case VOT_LDM:
			{
				//P=0, U=1, S=0, L=1, W=1
				
				u32 Rn=(opcd>>16)&15;
				u32 RList=opcd&0xFFFF;
				u32 tsz=(cpuBitsSet[RList & 255] + cpuBitsSet[(RList >> 8) & 255]);

				verify(CHK_BTS(1,24,0)); //P=0
				verify(CHK_BTS(1,23,1)); //U=1
				verify(CHK_BTS(1,22,0)); //S=0
				verify(CHK_BTS(1,21,1)); //W=1
				verify(CHK_BTS(1,20,1)); //L=0

				
				//if (tsz!=1)
				//	goto FALLBACK;

				bool _W=true; //w=1
				

				if (RList & (1<<Rn))
					_W=false;

				bool _AGU=_W; // (w=1 && p=0) || p=1 (P=0)

				LoadReg(r0,Rn);
				if (_AGU)
				{
					ADD(r9,r0,tsz*4);
				}
				armv_MOV32(r1,RList);
				armv_call((void*)(u32(DYNACALL*)(u32,u32))&DoLDM<0>);

				if (_W)
				{
					StoreReg(r9,Rn);
				}
			}
			break;
			*/
			
		case VOT_Fallback:
			{
				//interpreter fallback

				// Let the interpreter count cycles
				Cycles -= 6;

				//arm_single_op needs PC+4 on r15
				//TODO: only write it if needed -> Probably not worth the code, very few fallbacks now...
				armv_imm_to_reg(15,pc+8);

				//For cond branch, MSR
				if (op_flags & OP_SETS_PC)
					armv_imm_to_reg(R15_ARM_NEXT,pc+4);

				armv_intpr(opcd);
			}
			break;

		default:
			die("can't happen");
		}

		//Branch ?
		if (op_flags & OP_SETS_PC)
		{
			arm_printf("ARM: %06X: Block End %d",pc,ops);
			break;
		}

		//block size limit ?
		if (ops == 31)
		{
			arm_printf("ARM: %06X: Block split", pc);

			armv_imm_to_reg(R15_ARM_NEXT, pc + 4);
		}
		
		//Goto next opcode
		pc += 4;
	}

	armv_end((void*)rv,Cycles);
}

void FlushCache()
{
	icPtr=ICache;
	for (u32 i = 0; i < ARRAY_SIZE(EntryPoints); i++)
		EntryPoints[i] = (void*)&arm_compilecode;
}

static void armt_init()
{
	InitHash();

	if (!vmem_platform_prepare_jit_block(ARM7_TCB, ICacheSize, (void**)&ICache))
		die("vmem_platform_prepare_jit_block failed");

	icPtr = ICache;
}


#endif	// FEAT_AREC != DYNAREC_NONE
