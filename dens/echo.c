/* echo — minimal native C den for testing.
 * Receives an event via on_event() and logs it via bedrock. */

#include "strata/bedrock.h"

void on_event(const char *event, int len) {
    (void)len;
    bedrock_log(event);
}
