/*
	Highly inefficient and boring interpreter. Nothing special here
*/

#include "types.h"

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "hw/aica/aica_if.h"
#include "../sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "profiler/profiler.h"
#include "../dyna/blockmanager.h"
#include "../sh4_sched.h"

#define CPU_RATIO      (8)

static s32 l;

static void ExecuteOpcode(u16 op)
{
	if (sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
		RaiseFPUDisableException();
	OpPtr[op](op);
	l -= CPU_RATIO;
}

void Sh4_int_Run()
{
	sh4_int_bCpuRun=true;

	l = SH4_TIMESLICE;

	do
	{
#if !defined(NO_MMU)
		try {
#endif
			do
			{
				u32 addr = next_pc;
				next_pc += 2;
				u32 op = IReadMem16(addr);

				ExecuteOpcode(op);
			} while (l > 0);
			l += SH4_TIMESLICE;
			UpdateSystem_INTC();
#if !defined(NO_MMU)
		}
		catch (SH4ThrownException& ex) {
			Do_Exception(ex.epc, ex.expEvn, ex.callVect);
			l -= CPU_RATIO * 5;	// an exception requires the instruction pipeline to drain, so approx 5 cycles
		}
#endif
	} while (sh4_int_bCpuRun);

	sh4_int_bCpuRun = false;
}

void Sh4_int_Stop()
{
	if (sh4_int_bCpuRun)
		sh4_int_bCpuRun=false;
}

void Sh4_int_Start()
{
	if (!sh4_int_bCpuRun)
		sh4_int_bCpuRun=true;
}

void Sh4_int_Step()
{
	if (sh4_int_bCpuRun)
	{
		WARN_LOG(INTERPRETER, "Sh4 Is running , can't step");
	}
	else
	{
		u32 op=ReadMem16(next_pc);
		next_pc+=2;
		ExecuteOpcode(op);
	}
}

void Sh4_int_Skip()
{
	if (sh4_int_bCpuRun)
		WARN_LOG(INTERPRETER, "Sh4 Is running, can't Skip");
	else
		next_pc += 2;
}

void Sh4_int_Reset(bool hard)
{
	if (sh4_int_bCpuRun)
	{
		WARN_LOG(INTERPRETER, "Sh4 Is running, can't Reset");
	}
	else
	{
		if (hard)
			memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));
		next_pc = 0xA0000000;

		memset(r,0,sizeof(r));
		memset(r_bank,0,sizeof(r_bank));

		gbr=ssr=spc=sgr=dbr=vbr=0;
		mac.full=pr=fpul=0;

		sh4_sr_SetFull(0x700000F0);
		old_sr.status=sr.status;
		UpdateSR();

		fpscr.full = 0x0004001;
		old_fpscr=fpscr;
		UpdateFPSCR();

		//Any more registers have default value ?
		INFO_LOG(INTERPRETER, "Sh4 Reset");
	}
}

bool Sh4_int_IsCpuRunning()
{
	return sh4_int_bCpuRun;
}

//TODO : Check for valid delayslot instruction
void ExecuteDelayslot()
{
#if !defined(NO_MMU)
	try {
#endif
		u32 addr = next_pc;
		next_pc += 2;
		u32 op = IReadMem16(addr);

		if (op != 0)	// Looney Tunes: Space Race hack
			ExecuteOpcode(op);
#if !defined(NO_MMU)
	}
	catch (SH4ThrownException& ex) {
		AdjustDelaySlotException(ex);
		//printf("Delay slot exception\n");
		throw ex;
	}
#endif
}

void ExecuteDelayslot_RTE()
{
#if !defined(NO_MMU)
	try {
#endif
		ExecuteDelayslot();
#if !defined(NO_MMU)
	}
	catch (SH4ThrownException& ex) {
		ERROR_LOG(INTERPRETER, "Exception in RTE delay slot");
	}
#endif
}

//General update

int aica_schid = -1;
int rtc_schid = -1;

const int AICA_TICK = (u64)SH4_MAIN_CLOCK * 32 / 44100;

static int AicaUpdate(int tag, int c, int j)
{
	libARM_Update(32);
	libAICA_Update(32);

	return AICA_TICK;
}

static int DreamcastSecond(int tag, int c, int j)
{
	RealTimeClock++;

	prof_periodical();

#if FEAT_SHREC != DYNAREC_NONE
	bm_Periodical_1s();
#endif

	return SH4_MAIN_CLOCK;
}

// every SH4_TIMESLICE cycles
int UpdateSystem()
{
	//this is an optimisation (mostly for ARM)
	//makes scheduling easier !
	//update_fp* tmu=pUpdateTMU;
	
	Sh4cntx.sh4_sched_next-=SH4_TIMESLICE;
	if (Sh4cntx.sh4_sched_next<0)
		sh4_sched_tick(SH4_TIMESLICE);

	return Sh4cntx.interrupt_pend;
}

int UpdateSystem_INTC()
{
	if (UpdateSystem())
		return UpdateINTC();
	else
		return 0;
}

void sh4_int_resetcache() { }
//Get an interface to sh4 interpreter
void Get_Sh4Interpreter(sh4_if* rv)
{
	rv->Run=Sh4_int_Run;
	rv->Stop=Sh4_int_Stop;
	rv->Start=Sh4_int_Start;
	rv->Step=Sh4_int_Step;
	rv->Skip=Sh4_int_Skip;
	rv->Reset=Sh4_int_Reset;
	rv->Init=Sh4_int_Init;
	rv->Term=Sh4_int_Term;
	rv->IsCpuRunning=Sh4_int_IsCpuRunning;

	rv->ResetCache=sh4_int_resetcache;
}

void Sh4_int_Init()
{
	verify(sizeof(Sh4cntx)==448);

	if (aica_schid == -1)
	{
		aica_schid=sh4_sched_register(0,&AicaUpdate);
		sh4_sched_request(aica_schid,AICA_TICK);

		rtc_schid=sh4_sched_register(0,&DreamcastSecond);
		sh4_sched_request(rtc_schid,SH4_MAIN_CLOCK);
	}
	memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));
}

void Sh4_int_Term()
{
	Sh4_int_Stop();
	INFO_LOG(INTERPRETER, "Sh4 Term");
}
