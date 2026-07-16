#ifndef NEURALAXE_IDENTITY_H_
#define NEURALAXE_IDENTITY_H_

/*
 * NeuralAxe OS product identity — read-only, deterministic build metadata.
 *
 * NeuralAxe OS is based on the open-source ESP-Miner and AxeOS projects
 * (GPL-3.0). The upstream firmware version ("version") and AxeOS version
 * ("axeOSVersion") fields keep their original semantics; the values below
 * are additive product identity only and must never replace them.
 */

#define NEURALAXE_PRODUCT_NAME     "NeuralAxe OS"
#define NEURALAXE_PRODUCT_VERSION  "0.1.0-dev"
#define NEURALAXE_BUILD_CHANNEL    "development"
#define NEURALAXE_VENDOR           "NeuralShield"
#define NEURALAXE_UPSTREAM_PROJECT "ESP-Miner / AxeOS"
#define NEURALAXE_UPSTREAM_VERSION "v2.14.2"
#define NEURALAXE_TARGET_BOARD     "601"
#define NEURALAXE_TARGET_DEVICE    "Gamma"
#define NEURALAXE_TARGET_ASIC      "BM1370"

#endif /* NEURALAXE_IDENTITY_H_ */
