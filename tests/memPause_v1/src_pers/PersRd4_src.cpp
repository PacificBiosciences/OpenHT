#include "Ht.h"
#include "PersRd4.h"

#if (RD4_ADDR1_W == 1)
#define PR_HTID 0
#else
#define PR_HTID (Rd4Addr1_t)PR_htId
#endif

void CPersRd4::PersRd4()
{
	if (PR_htValid) {
		switch (PR_htInst) {
		case RD4_INIT:
		{
			P_loopCnt = 0;
			P_err = 0;
			P_pauseLoopCnt = 0;

			P_arrayMemRd1Ptr = PR_HTID;

			HtContinue(RD4_READ1);
		}
		break;
		case RD4_READ1:
		{
			if (ReadMemBusy() || SendReturnBusy_rd4()) {
				HtRetry();
				break;
			}

			// Check if end of loop
			if (P_loopCnt == PAUSE_LOOP_CNT || P_err) {
				// Return to host interface
				SendReturn_rd4(P_err);
			} else {
				// Calculate memory read address
				MemAddr_t memRdAddr = P_arrayAddr + ((P_loopCnt & 0xf) << 3);

				// Issue read request to memory
				ReadMem_rd4Mem(memRdAddr, PR_HTID, 0);

				// Set address for reading memory response data

				HtContinue(RD4_READ2);
			}
		}
		break;
		case RD4_READ2:
		{
			if (ReadMemBusy()) {
				HtRetry();
				break;
			}

			// Calculate memory read address
			MemAddr_t memRdAddr = P_arrayAddr + (((P_loopCnt + 1) & 0xf) << 3);

			// Issue read request to memory
			ReadMem_rd4Mem(memRdAddr, PR_HTID, 1);

			HtContinue(RD4_READ3);
		}
		break;
		case RD4_READ3:
		{
			if (ReadMemBusy()) {
				HtRetry();
				break;
			}

			// Calculate memory read address
			MemAddr_t memRdAddr = P_arrayAddr + (((P_loopCnt + 2) & 0xf) << 3);

			// Issue read request to memory
			ReadMem_rd4Mem(memRdAddr, PR_HTID, 2);

			HtContinue(RD4_READ4);
		}
		break;
		case RD4_READ4:
		{
			if (ReadMemBusy()) {
				HtRetry();
				break;
			}

			// Calculate memory read address
			MemAddr_t memRdAddr = P_arrayAddr + (((P_loopCnt + 3) & 0xf) << 3);

			// Issue read request to memory
			ReadMem_rd4Mem(memRdAddr, PR_HTID, 3);

			HtContinue(RD4_LOOP);
		}
		break;
		case RD4_LOOP:
		{
			if (ReadMemBusy()) {
				HtRetry();
				break;
			}

			// wait a few instructions for last response to line up with call to ReadMemPause
			if (P_pauseLoopCnt == 2) {
				P_pauseLoopCnt = 0;
				P_arrayMemRd2Ptr = 0;

				if (PR_pauseDst)
					ReadMemPause(RD4_TEST1a);
				else
					ReadMemPause(RD4_TEST1b);
			} else {
				P_pauseLoopCnt += 1;
				HtContinue(RD4_LOOP);
			}
		}
		break;
		case RD4_TEST1a:
		case RD4_TEST1b:
		{
			if (PR_htInst != (PR_pauseDst ? RD4_TEST1a : RD4_TEST1b)) {
				HtAssert(0, 0);
			    P_err += 1;
			}
			P_pauseDst ^= 1;

			if (GR_rd4Mem.data != (P_loopCnt & 0xf)) {
				HtAssert(0, 0);
				P_err += 1;
			}

			P_arrayMemRd2Ptr = 1;

			HtContinue(RD4_TEST2);
		}
		break;
		case RD4_TEST2:
		{
			if (GR_rd4Mem.data != ((P_loopCnt + 1) & 0xf)) {
				HtAssert(0, 0);
				P_err += 1;
			}

			P_arrayMemRd2Ptr = 2;

			HtContinue(RD4_TEST3);
		}
		break;
		case RD4_TEST3:
		{
			if (GR_rd4Mem.data != ((P_loopCnt + 2) & 0xf)) {
				HtAssert(0, 0);
				P_err += 1;
			}

			P_arrayMemRd2Ptr = 3;

			HtContinue(RD4_TEST4);
		}
		break;
		case RD4_TEST4:
		{
			if (GR_rd4Mem.data != ((P_loopCnt + 3) & 0xf)) {
				HtAssert(0, 0);
				P_err += 1;
			}

			// Pauserement loop count
			P_loopCnt = P_loopCnt + 1;

			HtContinue(RD4_READ1);
		}
		break;
		default:
			assert(0);
		}
	}

	if (r_m1_rdRspRdy) {
#ifndef _HTV
#if (RD4_HTID_W == 0)
		if (!r_rdGrpRsmWait)
			printf("-");
		else if (r_rdGrpRspCnt == 1)
			printf("2");
		else
			printf("+");
#else
#if (RD4_HTID_W <= 2)
		if (!r_rdGrpRsmWait[INT(r_m1_rdRspGrpId)])
			printf("-");
		else if (r_rdGrpRspCnt[INT(r_m1_rdRspGrpId)] == 1)
			printf("4");
		else
			printf("+");
#else
		{
			m_rdGrpReqState1.read_addr(r_m1_rdRspInfo.m_grpId);
			m_rdGrpRspState0.read_addr(r_m1_rdRspInfo.m_grpId);
			CRdGrpRspState c_m1_rdGrpRspState = m_rdGrpRspState0.read_mem();
			CRdGrpReqState c_m1_rdGrpReqState = m_rdGrpReqState1.read_mem();

			if (c_m1_rdGrpRspState.m_pause == c_m1_rdGrpReqState.m_pause)
				printf("-");
			else if (c_m1_rdGrpReqState.m_cnt - c_m1_rdGrpRspState.m_cnt == 1)
				printf("4");
			else
				printf("+");
		}
#endif
#endif
#endif
	}
}
