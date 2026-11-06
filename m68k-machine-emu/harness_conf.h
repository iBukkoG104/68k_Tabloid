/* harness_conf.h - override di configurazione per Musashi (via -DMUSASHI_CNF).
 *
 * Tutte le opzioni di m68kconf.h sono protette da #ifndef, quindi basta
 * definirle qui PRIMA di includere il file originale.
 * Risultato: core 68010 puro (niente 020/030/040/FPU/PMMU), con gli hook
 * che servono all'harness.
 */
#ifndef HARNESS_CONF_H
#define HARNESS_CONF_H

/* Solo 68010 */
#define M68K_EMULATE_010            M68K_OPT_ON
#define M68K_EMULATE_EC020          M68K_OPT_OFF
#define M68K_EMULATE_020            M68K_OPT_OFF
#define M68K_EMULATE_030            M68K_OPT_OFF
#define M68K_EMULATE_040            M68K_OPT_OFF
#define M68K_EMULATE_PMMU           M68K_OPT_OFF

/* Come l'hardware vero: accesso word/long a indirizzo dispari = address error */
#define M68K_EMULATE_ADDRESS_ERROR  M68K_OPT_ON

/* Interrupt acknowledge: lo gestiamo noi (autovector + azzeramento linea) */
int harness_int_ack(int level);
#define M68K_EMULATE_INT_ACK        M68K_OPT_SPECIFY_HANDLER
#define M68K_INT_ACK_CALLBACK(A)    harness_int_ack(A)

/* Hook prima di ogni istruzione (trace) */
void harness_instr_hook(unsigned int pc);
#define M68K_INSTRUCTION_HOOK       M68K_OPT_SPECIFY_HANDLER
#define M68K_INSTRUCTION_CALLBACK(pc) harness_instr_hook(pc)

/* Function code FC2..FC0 (supervisor/user, program/data) */
void harness_set_fc(unsigned int fc);
#define M68K_EMULATE_FC             M68K_OPT_SPECIFY_HANDLER
#define M68K_SET_FC_CALLBACK(A)     harness_set_fc(A)

#include "m68kconf.h"

#endif
