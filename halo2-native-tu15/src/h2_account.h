/* Insignia account recovery for Halo 2 (TU 1.5), through the title's own XOnline.
 * Ported from CounterStrikeXB's cs_account.c (same author, MIT). */
#ifndef H2_ACCOUNT_H
#define H2_ACCOUNT_H

#include <stdint.h>

/* Results, also the process exit code of a recovery run (H2_ACCT=recover). */
enum {
    H2_ACCT_OK = 0,
    H2_ACCT_ERR_INPUT = 10,   /* no gamertag / number on the input pipe */
    H2_ACCT_ERR_LOGON = 11,   /* the console did not sign in */
    H2_ACCT_ERR_NETWORK = 12, /* no answer, or the request failed */
    H2_ACCT_ERR_REFUSED = 13, /* Insignia refused (wrong gamertag or number) */
    H2_ACCT_ERR_SAVE = 14,    /* the account could not be written */
    H2_ACCT_ERR_FULL = 15,    /* all eight account slots in use */
    H2_ACCT_ERR_OTHER = 19
};

int  h2_account_enabled(void);               /* H2_ACCT=recover (or reserve) */
/* recomp_lookup_manual hook for synthetic kernel addresses: the account step on
 * KeQuerySystemTime, and the account thread's routine. NULL otherwise. */
void (*h2_account_kernel_override(uint32_t xbox_va))(void);

/* The account thread's routine address: a synthetic kernel-range VA that
 * PsCreateSystemThreadEx is given and recomp_lookup_manual maps back here. */
#define H2_ACCOUNT_THREAD_VA 0xFEACC000u

#endif
