/**
 * Alice Senki 2 - Input Prediction (re0.7 M4)
 *
 * The resurrected hold-last predictor is header-only (`HoldLastPredictor`
 * in include/rollback/prediction.h) so the pure engine2 core and the
 * socket-free test harness can instantiate it without linking mod TUs.
 * This TU exists only to compile the header standalone (include hygiene).
 */

#include "rollback/prediction.h"
