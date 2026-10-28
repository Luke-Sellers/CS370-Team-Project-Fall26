/* fake.h -- synthetic readings so capture and storage can be developed
 * without hardware. Every record it makes is flagged REC_SYNTHETIC. */
#ifndef MOLDWATCH_FAKE_H
#define MOLDWATCH_FAKE_H

#include "record.h"

/* Fill r->a and r->b from r->sensor and r->seq, and set REC_SYNTHETIC. */
void fake_fill(struct record *r);

#endif
