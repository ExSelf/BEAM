# BEAM
BEAM is short for Bidirectional Engine for Asynchronous Messaging.

## SPACE/SUN packet bridge

BEAM relays fixed-size 250-byte binary packets between SPACE over USB serial
and SUN over ESP-NOW. SPACE, BEAM, and SUN authenticate each packet with the
same key. The final 8 bytes are the first 8 bytes of HMAC-SHA-256 over packet
bytes 0-241. BEAM verifies packets from either side and relays authenticated
packets without changing them.

Set `SUN_PACKET_HMAC_KEY` to the same value configured on SUN; SUN requires at
least 32 characters. Configure BEAM's ESP-NOW channel to the same channel as
SUN. Do not commit `src/secrets.h`.
