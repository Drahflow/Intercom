# Intercom

## Problem

* You want to have a voice link over WiFi (or actually any IP), e.g. between two Raspberries.
* You'd rather do this with a code-base you can read in an hour or less.

## Solution

```
<both hosts># apt-get install libpulse-dev
<both hosts># make
send-host# ./sender receiver-hostname 4000
recv-host# ./receiver 4000 
```

Bidirectional communication (no echo cancel) by adding
a link in the other direction, obviously.

If it does not work, and buf pos does not stabilize, try `receiver 4000 800`
(increase pulseaudio playback buffer to 800 samples).

If it does not work, and it says `Packet for +-0.12345s, buf pos ...`
and `Packet arrived too late` it means packets were received (123ms in this
case) after they were supposed to be played. Make sure system time is in-sync between
the two hosts. If it is, change receiver.c `targetLatency` to some value your
link can actually achieve.
