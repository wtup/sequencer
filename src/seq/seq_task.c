/**************************************************************************
			GTA PROJECT   AT division
		Copyright, 1990-1994
		The Regents of the University of California and
		the University of Chicago.
		Los Alamos National Laboratory

	Copyright, 2010, Helmholtz-Zentrum Berlin f. Materialien
		und Energie GmbH, Germany (HZB)
		(see file Copyright.HZB included in this distribution)

	DESCRIPTION: Thread creation and control for sequencer state-sets.
***************************************************************************/
#define	DEBUG nothing /* "nothing", "printf", "errlogPrintf" etc. */

#include	<limits.h>
#include 	<string.h>
/*#include 	<unistd.h> */

#define		DECLARE_PV_SYS
#define epicsExportSharedSymbols
#include	"seq.h"

/* Used to disable debug output */
void nothing(const char *format,...) {}

/* Function declarations */
static long seq_waitConnect(SPROG *pSP, SSCB *pSS);
static void ss_entry(SSCB *pSS);
static void ss_thread_init(SPROG *, SSCB *);
static void ss_thread_uninit(SPROG *, SSCB *,int);
static void seq_clearDelay(SSCB *,STATE *);
static double seq_getTimeout(SSCB *);
epicsStatus seqAddProg(SPROG *pSP);
long seq_connect(SPROG *pSP);
long seq_disconnect(SPROG *pSP);

/*
 * sequencer() - Sequencer main thread entry point.
 */
long sequencer (SPROG *pSP)	/* ptr to original (global) state program table */
{
	SSCB		*pSS;
	int		nss;
	epicsThreadId	tid;
	size_t		threadLen;
	char		threadName[THREAD_NAME_SIZE+10];

	/* Retrieve info about this thread */
	pSP->threadId = epicsThreadGetIdSelf();
	epicsThreadGetName(pSP->threadId, threadName, sizeof(threadName));
	pSS = pSP->pSS;
	pSS->threadId = pSP->threadId;

	/* Add the program to the state program list */
	seqAddProg(pSP);

	/* Attach to PV context of pvSys creator (auxiliary thread) */
	pvSysAttach(pvSys);

	/* Initiate connect & monitor requests to database channels */
	seq_connect(pSP);

	/* Call sequencer entry function */
	pSP->entryFunc(pSS, pSP->pVar);

	/* Create each additional state-set task (additional state-set thread
	   names are derived from the first ss) */
	threadLen = strlen(threadName);
	for (nss = 1, pSS = pSP->pSS + 1; nss < pSP->numSS; nss++, pSS++)
	{
		/* Form thread name from program name + state-set number */
		sprintf(threadName+threadLen, "_%d", nss);

		/* Spawn the task */
		tid = epicsThreadCreate(
			threadName,			/* thread name */
			pSP->threadPriority,		/* priority */
			pSP->stackSize,			/* stack size */
			(EPICSTHREADFUNC)ss_entry,	/* entry point */
			pSS);				/* parameter */

		errlogPrintf("Spawning thread %p: \"%s\"\n", tid,
			    threadName);
	}

	/* First state-set jumps directly to entry point */
	ss_entry(pSP->pSS);

	return 0;
}
/*
 * ss_entry() - Thread entry point for all state-sets.
 * Provides the main loop for state-set processing.
 */
static void ss_entry(SSCB *pSS)
{
	SPROG		*pSP = pSS->sprog;
	epicsBoolean	ev_trig;
	STATE		*pST, *pStNext;
	double		delay;
	USER_VAR	*pVar;
	int		nWords;
	SS_ID		ssId;

	/* Initialize this state-set thread */
	ss_thread_init(pSP, pSS);

	/* If "+c" option, wait for all channels to connect (a failure
	 * return means that we have been asked to exit) */
	if ((pSP->options & OPT_CONN) != 0)
		if (seq_waitConnect(pSP, pSS) < 0) goto exit;

	/* Initialize state-set to enter the first state */
	pST = pSS->pStates;
	pSS->currentState = 0;
	pSS->nextState = -1;
	pSS->prevState = -1;

	/* Use the event mask for this state */
	pSS->pMask = (pST->pEventMask);
	nWords = (pSP->numEvents + NBITS - 1) / NBITS;

	/* Local ptr to user variables (for reentrant code only) */
	pVar = pSP->pVar;

	/* state-set id */
	ssId = pSS;

	/*
	 * ============= Main loop ==============
	 */
	while (TRUE)
	{
		/* If we've changed state, do any entry actions. Also do these
                 * even if it's the same state if option to do so is enabled. 
                 */
		if ((pSS->prevState != pSS->currentState ||
                     pST->options & OPT_DOENTRYFROMSELF)
		    && pST->entryFunc)
		{
			pST->entryFunc(ssId, pVar);
		}

		seq_clearDelay(pSS, pST); /* Clear delay list */
		pST->delayFunc(ssId, pVar); /* Set up new delay list */

		/* Setting this semaphore here guarantees that a when() is
		 * always executed at least once when a state is first
		 * entered.
		 */
		epicsEventSignal(pSS->syncSemId);

		/* Loop until an event is triggered, i.e. when() returns TRUE
		 */
		do {
			/* Wake up on PV event, event flag, or expired delay */
			delay = seq_getTimeout(pSS); /* min. delay from list */
			if (delay > 0.0)
			{
				epicsEventWaitWithTimeout(pSS->syncSemId, delay);
			}

			/* Check whether we have been asked to exit */
			if (epicsEventTryWait(pSS->death1SemId) == epicsEventWaitOK)
			{
				goto exit;
			}

			/* Call the event function to check for an event
			 * trigger. The statement inside the when() statement
			 * is executed. Note, we lock out PV events while doing 
			 * this. */
			epicsMutexMustLock(pSP->caSemId);

			ev_trig = pST->eventFunc(ssId, pVar,
				&pSS->transNum, &pSS->nextState);

		        /* Clear all event flags (old ef mode only) */
			if (ev_trig && !(pSP->options & OPT_NEWEF))
			{
				int i;
				for (i = 0; i < nWords; i++)
				{
					pSP->pEvents[i] = pSP->pEvents[i] & !pSS->pMask[i];
				}
			}

			epicsMutexUnlock(pSP->caSemId);

		} while (!ev_trig);

		/* An event triggered:
		 * execute the action statements and enter the new state.
		 */

		/* Execute the action for this event */
		pST->actionFunc(ssId, pVar, pSS->transNum, &pSS->nextState);

		/* Change event mask ptr for next state */
		pStNext = pSS->pStates + pSS->nextState;
		pSS->pMask = (pStNext->pEventMask);

		/* If changing state, do any exit actions. */
		if ((pSS->currentState != pSS->nextState ||
                     pST->options & OPT_DOEXITTOSELF)
		    && pST->exitFunc)
		{
			pST->exitFunc(ssId, pVar);
		}

		/* Flush any outstanding DB requests */
		pvSysFlush(pvSys);

		/* Change to next state */
		pSS->prevState = pSS->currentState;
		pSS->currentState = pSS->nextState;
		pST = pSS->pStates + pSS->currentState;
	}

	/* Thread exit has been requested */
exit:

	/* Uninitialize this state-set thread (phase 1) */
	ss_thread_uninit(pSP, pSS, 1);

	/* Pass control back (so all state-set threads can complete phase 1
	 * before embarking on phase 2) */
	epicsEventSignal(pSS->death2SemId);

	/* Wait for request to perform uninitialization (phase 2) */
	epicsEventMustWait(pSS->death3SemId);
	ss_thread_uninit(pSP, pSS, 2);

	/* Pass control back and die (i.e. exit) */
	epicsEventSignal(pSS->death4SemId);
}

/* Initialize a state-set thread */
static void ss_thread_init(SPROG *pSP, SSCB *pSS)
{
	/* Get this thread's id */
	pSS->threadId = epicsThreadGetIdSelf();

	/* Attach to PV context of pvSys creator (auxiliary thread); was
	   already done for the first state-set */
	if (pSP->threadId != pSS->threadId)
		pvSysAttach(pvSys);

	/* Register this thread with the EPICS watchdog (no callback func) */
	taskwdInsert(pSS->threadId, 0, (void *)0);
}

/* Uninitialize a state-set thread */
static void ss_thread_uninit(SPROG *pSP, SSCB *pSS, int phase)
{
	/* Phase 1: if this is the first state-set, call user exit routine
	   and disconnect all channels */
	if (phase == 1 && pSS->threadId == pSP->threadId)
	{
	    DEBUG("   Call exit function\n");
	    pSP->exitFunc(pSS, pSP->pVar);

	    DEBUG("   Disconnect all channels\n");
	    seq_disconnect(pSP);
	}

	/* Phase 2: unregister the thread with the EPICS watchdog */
	else if (phase == 2)
	{
	    DEBUG("   taskwdRemove(%p)\n", pSS->threadId);
	    taskwdRemove(pSS->threadId);
	}
}

/* Wait for all channels to connect */
static long seq_waitConnect(SPROG *pSP, SSCB *pSS)
{
	epicsStatus	status;
	double		delay;

	if (pSP->numChans == 0)
		return OK;
	delay = 10.0; /* 10, 20, 30, 40, 40,... sec */
	while (1) {
		status = epicsEventWaitWithTimeout(
                    pSS->allFirstConnectAndMonitorSemId, delay);
		if(status==OK) break;
		if (delay < 40.0) {
			delay += 10.0;
			errlogPrintf("numMonitoredChans %ld firstMonitorCount %ld",
				pSP->numMonitoredChans,pSP->firstMonitorCount);
			errlogPrintf(" assignCount %ld firstConnectCount %ld\n",
				pSP->assignCount,pSP->firstConnectCount);
		}
		/* Check whether we have been asked to exit */
		if (epicsEventTryWait(pSS->death1SemId) == epicsEventWaitOK)
			return ERROR;
	}
	return OK;
}

/*
 * seq_clearDelay() - clear the time delay list.
 */
static void seq_clearDelay(SSCB *pSS, STATE *pST)
{
	int	ndelay;

        /* On state change set time we entered this state; or if transition from
         * same state if option to do so is on for this state. 
         */
	if ((pSS->currentState != pSS->prevState) ||
             !(pST->options & OPT_NORESETTIMERS))
	{
		pvTimeGetCurrentDouble(&pSS->timeEntered);
	}

	for (ndelay = 0; ndelay < MAX_NDELAY; ndelay++)
	{
		pSS->delay[ndelay] = 0;
	 	pSS->delayExpired[ndelay] = FALSE;
	}

	pSS->numDelays = 0;
}

/*
 * seq_getTimeout() - return time-out for pending on events.
 * Returns number of seconds to next expected timeout of a delay() call.
 * Returns (double) INT_MAX if no delays pending 
 */
static double seq_getTimeout(SSCB *pSS)
{
	int	ndelay, delayMinInit;
	double	cur, delay, delayMin, delayN;

	if (pSS->numDelays == 0)
		return (double) INT_MAX;

	/*
	 * calculate the delay since this state was entered
	 */
	pvTimeGetCurrentDouble(&cur);
	delay = cur - pSS->timeEntered;

	delayMinInit = 0;
	delayMin = (double) INT_MAX;

	/* Find the minimum  delay among all non-expired timeouts */
	for (ndelay = 0; ndelay < pSS->numDelays; ndelay++)
	{
		if (pSS->delayExpired[ndelay])
			continue; /* skip if this delay entry already expired */

		delayN = pSS->delay[ndelay];
		if (delay >= delayN)
		{	/* just expired */
			pSS->delayExpired[ndelay] = TRUE; /* mark as expired */
			return 0.0;
		}

		if (delayN<=delayMin) {
			delayMinInit=1;
			delayMin = delayN;  /* this is the min. delay so far */
		}
	}

	/*
	 * If there is no unexpired delay in the list
	 * then wait forever until there is a PV state
	 * change
	 */
	if (!delayMinInit) {
		return (double) INT_MAX;
	}

	/*
	 * unexpired delay _is_ in the list
	 */
	if (delayMin>delay) {
		delay = delayMin - delay;
		return (double) delay;
	}
	else {
		return 0.0;
	}
}

/*
 * Delete all state-set threads and do general clean-up.
 */
long epicsShareAPI seqStop(epicsThreadId tid)
{
	SPROG		*pSP;
	SSCB		*pSS;
	int		nss;

	/* Check that this is indeed a state program thread */
	pSP = seqFindProg(tid);
	if (pSP == NULL)
		return -1;

	DEBUG("Stop %s: pSP=%p, tid=%p\n", pSP->pProgName, pSP,tid);

	/* Ask all state-set threads to exit (phase 1) */
	DEBUG("   Asking state-set threads to exit (phase 1):\n");
	for (nss = 0, pSS = pSP->pSS; nss < pSP->numSS; nss++, pSS++)
	{
		/* Just possibly hasn't started yet, so check... */
		if (pSS->threadId == 0)
			continue;

		/* Ask the thread to exit */
		DEBUG("      tid=%p\n", pSS->threadId);
		epicsEventSignal(pSS->death1SemId);
	}

	/* Wake up all state-sets */
	DEBUG("   Waking up all state-sets\n");
	seqWakeup (pSP, 0);

	/* Wait for them all to complete phase 1 of their deaths */
	DEBUG("   Waiting for state-set threads phase 1 death:\n");
	for (nss = 0, pSS = pSP->pSS; nss < pSP->numSS; nss++, pSS++)
	{
		if (pSS->threadId == 0)
			continue;

		if (epicsEventWaitWithTimeout(pSS->death2SemId,10.0) != epicsEventWaitOK)
		{
			errlogPrintf("Timeout waiting for thread %p "
				     "(\"%s\") death phase 1 (ignored)\n",
				     pSS->threadId, pSS->pSSName);
		}
		else
		{
			DEBUG("      tid=%p\n", pSS->threadId);
		}
	}

	/* Ask all state-set threads to exit (phase 2) */
	DEBUG("   Asking state-set threads to exit (phase 2):\n");
	for (nss = 0, pSS = pSP->pSS; nss < pSP->numSS; nss++, pSS++)
	{
		if (pSS->threadId == 0)
			continue;

		DEBUG("      tid=%p\n", pSS->threadId);
		epicsEventSignal(pSS->death3SemId);
	}

	/* Wait for them all to complete phase 2 of their deaths */
	DEBUG("   Waiting for state-set threads phase 2 death:\n");
	for (nss = 0, pSS = pSP->pSS; nss < pSP->numSS; nss++, pSS++)
	{
		if (pSS->threadId == 0)
			continue;

		if (epicsEventWaitWithTimeout(pSS->death4SemId,10.0) != epicsEventWaitOK)
		{
			errlogPrintf("Timeout waiting for thread %p "
				     "(\"%s\") death phase 2 (ignored)\n",
				     pSS->threadId, pSS->pSSName);
		}
		else
		{
			DEBUG("      tid=%p\n", pSS->threadId);
		}
	}

	/* Close the log file */
	if (pSP->logFd != NULL)
	{
		DEBUG("Log file closed, fd=%d, file=%s\n", fileno(pSP->logFd),
		      pSP->pLogFile);
		fclose(pSP->logFd);
		pSP->logFd = NULL;
		pSP->pLogFile = "";
	}

	/* Remove the state program from the state program list */
	seqDelProg(pSP);

	/* Delete state-set semaphores */
	for (nss = 0, pSS = pSP->pSS; nss < pSP->numSS; nss++, pSS++)
	{
		if (pSS->allFirstConnectAndMonitorSemId != NULL)
			epicsEventDestroy(pSS->allFirstConnectAndMonitorSemId);
		if (pSS->syncSemId != NULL)
			epicsEventDestroy(pSS->syncSemId);
		if (pSS->getSemId != NULL)
			epicsEventDestroy(pSS->getSemId);
		if (pSS->putSemId != NULL)
			epicsEventDestroy(pSS->putSemId);
		if (pSS->death1SemId != NULL)
			epicsEventDestroy(pSS->death1SemId);
		if (pSS->death2SemId != NULL)
			epicsEventDestroy(pSS->death2SemId);
		if (pSS->death3SemId != NULL)
			epicsEventDestroy(pSS->death3SemId);
		if (pSS->death4SemId != NULL)
			epicsEventDestroy(pSS->death4SemId);
	}

	/* Delete program-wide semaphores */
	epicsMutexDestroy(pSP->caSemId);
	epicsMutexDestroy(pSP->logSemId);

	/* Free all allocated memory */
	seqFree(pSP);

	DEBUG("   Done\n");
	return 0;
}

/* seqFree()--free all allocated memory */
void seqFree(SPROG *pSP)
{
	SSCB		*pSS;
	CHAN		*pDB;
	MACRO		*pMac;
	int		n;

	/* Free macro table entries */
	for (pMac = pSP->pMacros, n = 0; n < MAX_MACROS; pMac++, n++)
	{
		if (pMac->pName != NULL)
			free(pMac->pName);
		if (pMac->pValue != NULL)
			free(pMac->pValue);
	}

	/* Free MACRO table */
	free(pSP->pMacros);

	/* Free channel names */
	for (pDB = pSP->pChan, n = 0; n < pSP->numChans; pDB++, n++)
	{
		if (pDB->dbName != NULL)
			free(pDB->dbName);
	}

	/* Free channel structures */
	free(pSP->pChan);

	/* Free STATE blocks */
	pSS = pSP->pSS;
	free(pSS->pStates);

	/* Free event words */
	free(pSP->pEvents);

	/* Free SSCBs */
	free(pSP->pSS);

	/* Free SPROG */
	free(pSP);
}

/* 
 * Sequencer auxiliary thread -- loops on pvSysPend().
 */
void *seqAuxThread(void *tArgs)
{
	AUXARGS		*pArgs = (AUXARGS *)tArgs;
	char		*pPvSysName = pArgs->pPvSysName;
	long		debug = pArgs->debug;
	int		status;

	/* Register this thread with the EPICS watchdog */
	taskwdInsert(epicsThreadGetIdSelf(), 0, 0);

	/* All state program threads will use a common PV context (subtract
	   1 from debug level for PV debugging) */
	status = pvSysCreate(pPvSysName, debug>0?debug-1:0, &pvSys);
        if (status != pvStatOK)
        {
                errlogPrintf("seqAuxThread: pvSysCreate() %s failure: %s\n",
                            pPvSysName, pvSysGetMess(pvSys));
		seqAuxThreadId = (epicsThreadId) -1;
                return NULL;
        }
	seqAuxThreadId = epicsThreadGetIdSelf(); /* AFTER pvSysCreate() */

	/* This loop allows for check for connect/disconnect on PVs */
	for (;;)
	{
		pvSysPend(pvSys, 10.0, TRUE); /* returns every 10 sec. */
	}

	/* Return no result (never exit in any case) */
	return NULL;
}

