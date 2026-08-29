#include "bandwidth.h"

#include <stdlib.h>
#include <tgmath.h>

#define SAMPLES_MAX 16

// bandwidth estimator
struct bwe {
	int rtt_min;
	int n_samples;
	int index;
	double estimate;
	struct bwe_sample samples[0];
};

struct bwe* bwe_create(int rtt_min)
{
	struct bwe* self = calloc(1, sizeof(*self) + sizeof(*self->samples) *
			SAMPLES_MAX);
	if (!self)
		return NULL;

	// This was silently dropped before, leaving rtt_min at the zero calloc
	// had written. Honouring it matters now that it is subtracted from
	// every sample: anything larger than a real round trip drives the
	// delay negative and the estimate to nonsense, so callers pass zero
	// and let the first measurement set it.
	self->rtt_min = rtt_min;

	return self;
}

void bwe_destroy(struct bwe* self)
{
	free(self);
}

static inline const struct bwe_sample* get_sample(const struct bwe* self, int index)
{
	int head = (self->index + index - self->n_samples + SAMPLES_MAX)
		% SAMPLES_MAX;
	return &self->samples[head];
}

// Under non-congested circumstances, there will be some space between packages
//
// Each sample gives one delivery rate: bytes carried, divided by how much
// longer the round trip took than the fastest round trip ever seen. The
// estimate is the largest of those, not their aggregate.
//
// Taking the aggregate is what a first version did, and it fails badly
// against a real viewer. The round-trip time here is not transmission time:
// it also contains however long the client took to decode the frame and get
// round to acknowledging it, and a viewer answers on its own paint cadence.
// A 994 byte frame acknowledged after 208 ms was measured against TigerVNC;
// read as transmission that is 4.8 kB/s, and averaged in with the rest it
// dragged the estimate for a link that sustains ~590 kB/s down to 83 kB/s.
// The window then fell below the size of a single frame and the server
// started throttling itself against a limit it had invented.
//
// A small frame acknowledged slowly is evidence of a slow client, not of a
// slow link, and the useful property of those samples is that they can only
// ever understate the rate. Taking the maximum therefore ignores them for
// free: whichever sample got the most bytes through per unit of delay is the
// closest to a true measurement of the link, and the rest cannot pull it
// down. This is the max-filtered delivery rate that BBR uses, and the ring
// buffer bounds how long one good sample stays believed.
static double estimate_non_congested_bandwidth(const struct bwe* self)
{
	double best = 0;

	for (int i = 0; i < self->n_samples; ++i) {
		const struct bwe_sample* s = get_sample(self, i);

		int rtt = s->arrival_time - s->departure_time;
		int bw_delay = rtt - self->rtt_min;

		// A round trip no slower than the fastest ever seen means the
		// bytes went out faster than the clock can resolve. That is
		// evidence of a fast link, not a slow one, so it must not be
		// discarded: dropping these samples leaves the estimate at zero
		// on a loopback connection, where almost every round trip ties
		// the minimum, and zero is then read as "no capacity" and pins
		// the server in permanent slow start. Charge one microsecond
		// instead, which is the floor the timestamps can express.
		if (bw_delay < 1)
			bw_delay = 1;

		double rate = (double)s->bytes / (bw_delay * 1e-6);
		if (rate > best)
			best = rate;
	}

	return best;
}

// Under congested circumstances, there will be no space between packages
static double estimate_congested_bandwidth(const struct bwe* self)
{
	if (self->n_samples == 0)
		return 0;

	const struct bwe_sample* s0 = get_sample(self, 0);
	const struct bwe_sample* s1 = get_sample(self, self->n_samples - 1);

	int bytes_total = 0;

	for (int i = 0; i < self->n_samples; ++i) {
		const struct bwe_sample* s = get_sample(self, i);
		bytes_total += s->bytes;
	}

	int rtt = s1->arrival_time - s0->departure_time;
	int bw_delay = rtt - self->rtt_min;

	if (bw_delay < 1)
		bw_delay = 1;

	return (double)bytes_total / (bw_delay * 1e-6);
}

static void update_estimate(struct bwe* self)
{
	double non_congested = estimate_non_congested_bandwidth(self);
	double congested = estimate_congested_bandwidth(self);
	self->estimate = fmax(non_congested, congested);
}

void bwe_feed(struct bwe* self, const struct bwe_sample* sample)
{
	self->samples[self->index] = *sample;
	self->index = (self->index + 1) % SAMPLES_MAX;

	if (self->n_samples < SAMPLES_MAX)
		self->n_samples++;

	update_estimate(self);
}

void bwe_update_rtt_min(struct bwe* self, int rtt_min)
{
	self->rtt_min = rtt_min;
}

int bwe_get_estimate(const struct bwe* self)
{
	return round(self->estimate);
}
