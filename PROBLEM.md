# Problem memo -- moldwatch (Ricky Pena, Luke Sellers)

## The user
A renter in an older apartment with a windowless bathroom. Black
mold spots keep returning to the ceiling corner above the shower. the tenant
believes the exhaust fan barely works. Neither side has any data. The
device sits on a bathroom shelf with nothing attached to the walls or
ceiling, so it can be set up and removed without touching the rental.

## The problem
Mold can build up fast in a bathroom where humid showers take place, and
It can be virtually impossible to tell there's actual mold. maybe some spots
pop up randomly and you clean it and it comes back.

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

surface relative humidity is computed from the room's dew point and the ceiling's temperature.
with this we can determine whether stuff is natural or not.
(1) daily "wet hours" at the surface,
(2) shower events via a state machine
(3) post-shower drying time

## The mechanisms
- weeks of wetness history are
  the product itself and a bathroom outlet 
  losing power mid-write must
  not corrupt or erase them.
- a sensor that fails or gets knocked loose must not take the device down,
  so each sensor runs in
  its own process.

## The risk
We cannot observe actual mold growth within a semester, and that
would be far too risky, however, we can sample test data
that may be similar to prove this