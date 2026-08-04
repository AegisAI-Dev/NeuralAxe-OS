#ifndef MAIN_NX_MUTATION_ADAPTER_H_
#define MAIN_NX_MUTATION_ADAPTER_H_

/*
 * NeuralAxe Gate W6.1 — configuration-fingerprint provider installation.
 *
 * The mutation-observability component deliberately does not depend on the
 * application's configuration store. This translation unit is the one place
 * that bridges them: it registers a READ-ONLY reader which reports the
 * CONFIGURED tuning values (never the live ramp, live regulator output or
 * live fan duty).
 *
 * Installing the provider changes no behaviour: it stores one function
 * pointer. Without CONFIG_NX_MUTATION_OBSERVABILITY this compiles to an empty
 * function AND its only caller is compiled out, so the default image links
 * nothing at all from the observability component. Should a future caller
 * invoke it unguarded, the empty definition keeps that safe: no reader is
 * registered, and the fingerprint is then reported unavailable rather than as
 * a set of agreeable zeroes.
 *
 * Safe to call more than once; the last registration wins and each is
 * equivalent.
 */
void nx_mutation_adapter_install(void);

#endif /* MAIN_NX_MUTATION_ADAPTER_H_ */
