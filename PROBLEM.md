# Problem memo -- moldwatch (Ricky Pena, Luke Sellers)

## The user
A renter in an older apartment with a windowless bathroom. Black
mold spots keep returning to the ceiling corner above the shower. The
landlord blames tenant behavior ("you don't run the fan"); the tenant
believes the exhaust fan barely works. Neither side has any data. The
device sits on a bathroom shelf with nothing attached to the walls or
ceiling, so it can be set up and removed without touching the rental.

## The problem
The spots return within 2-3 weeks of every cleaning. Each cleaning
costs an hour and a bottle of bleach spray, the tenant worries about
breathing it, and a mold finding at move-out can cost part or all of a
security deposit. The dispute with the landlord cannot be settled
because nobody knows how long the ceiling actually stays wet after a
shower, or whether the fan changes that.

## Why a device
Mold does not grow during the shower; it grows in the hours afterward,
while the cold ceiling stays damp, day after day. Nobody's phone sits
in the bathroom from 11 p.m. to 3 a.m. recording how slowly the corner
dries, and a phone cannot measure a surface temperature across the
room. The evidence only exists if something is present and awake for
weeks, through every shower, unattended.

## The sensors
- **BME280** (I2C): room air temperature and relative humidity, from
  which we compute the air's dew point.
- **MLX90614** (I2C): non-contact infrared thermometer on the shelf,
  aimed at the ceiling corner, measuring its surface temperature from
  a distance.

They cooperate by fusion: surface relative humidity is computed from
the room's dew point *and* the ceiling's temperature. A cold ceiling
can sit near 100% surface humidity while the room reads a harmless
65%, so neither sensor alone can say whether mold conditions exist.
From this fused signal we derive (1) daily "wet hours" at the surface,
(2) shower events via a state machine, and (3) post-shower drying time,
whose trend over weeks reveals whether ventilation is working or
degrading.

## The mechanisms
- **D, crash-consistent append-only log:** weeks of wetness history are
  the product itself, and a bathroom outlet losing power mid-write must
  not corrupt or erase them.
- **E, multi-process with supervisor:** a sensor that fails or gets
  knocked loose must not take the device down, so each sensor runs in
  its own process, and a supervisor detects the loss, keeps the other
  sensor running, logs the degradation, and recovers when it returns.

## The risk
We cannot observe actual mold growth within a semester, so we cannot
validate "detects mold." We narrow the claim to "measures mold-risk
conditions and ventilation effectiveness," and build our own ground
truth: deliberate showers with the fan on versus off, plus a one-week
paper log of real showers to check the shower detector's true- and
false-positive rates. A second risk is infrared accuracy: the
MLX90614 reads a spot, not a point, and steam between the sensor and
the ceiling may skew readings, so we will check it against a contact
thermometer held on the ceiling. Condensation on the electronics is
handled by placing the device away from the shower spray.
