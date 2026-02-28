#include "strata/store.h"
#include "strata/change.h"

/* Forward declaration from store_sqlite.c */
extern void strata_store_set_change_pub(strata_store *store, strata_change_pub *pub);

void strata_store_attach_change_pub(strata_store *store, strata_change_pub *pub) {
    strata_store_set_change_pub(store, pub);
}
