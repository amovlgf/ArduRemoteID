# Source and local changes

The files `src/opendroneidcn.c` and `src/opendroneidcn.h` are adapted from
[OpenDroneID core C](https://github.com/opendroneid/opendroneid-core-c), fixed at
commit `6484f26545d4f012682524e2d843fab0fbdc0b34`, directory `libopendroneidcn`.
The upstream copyright and Apache-2.0 notices are retained; `LICENSE` is copied
from that commit. This vendored library does not replace or update the existing
ASTM/ASD-STAN `modules/opendroneid-core-c` dependency.

SHA-256 of the unmodified downloaded files:

| Upstream file | SHA-256 |
| --- | --- |
| `libopendroneidcn/opendroneidcn.c` | `502dc4d78c1d02e202a6173d856db788b0904c9178501609061d9b8583ff9f21` |
| `libopendroneidcn/opendroneidcn.h` | `2c990bcaabf0d3558f4d2267d418c3f4fab5efb723e175ce9224b36cad22980b` |
| `LICENSE` | `c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4` |

Local changes (2026):

- Permit an unfilled real-name registration mark: field 002 is still present,
  containing eight NULL bytes (GB 46750-2025 Table 3).
- Append `unknown_fields` to `DroneRIDData_t`. Bit `field_number - 1` records an
  unavailable field; named `CN46750_UNKNOWN_*` macros cover fields with wire
  sentinels. Callers must zero-initialize the structure. The old function names
  and ordinary known-value behavior remain, but the structure's ABI has changed:
  rebuild every caller with this header.
- Preserve unavailable coordinates, altitude, angle, speed and time when
  decoding. A position bit describes the entire longitude/latitude pair. If
  either component is unavailable, the whole pair is treated as unavailable;
  encoding emits the sentinel for both components. A valid zero coordinate or
  zero vertical speed is distinct from unavailable data.
- Validate finite known values, geographic and encodable ranges, reserved enum
  and accuracy values, ASCII input and 48-bit time before writing output. Values
  ignored by an explicit unknown bit need not be finite. Unknown timestamp zero
  is still accepted; decoding records its unknown bit. Reject a known coordinate
  that quantizes to the reserved `FFFFFFFF` code.
- Reject truncated, unterminated, missing-mandatory-field and inconsistent-length
  packets. For the three known flag bytes the declared content is consumed
  exactly. Longer flag chains retain upstream's partial extension result, without
  claiming to validate unknown CAAC extensions. Outputs are only valid after a
  successful decode; callers must not consume partial output after an error.
- A scanner candidate must decode successfully. `CN46750_FindPacket` remains a
  byte-stream search helper, not an IEEE 802.11 IE parser and not evidence of an
  official Wi-Fi encapsulation convention.
- Add Arduino metadata and independent host golden-byte fixtures and boundary
  tests. No SDK or radio is required for the codec tests.

## Deliberate limits

GB 46750-2025 Tables 1-3 in the user-provided publication are the comparison
source. Do not inherit regulatory assertions from the upstream README or source
publication-date comments. This library only handles payload bytes; radio
encapsulation, sending intervals, power, flight-controller behavior, storage,
network identification, identity provisioning and certification are outside it.

The version byte emitted is the upstream V1.0 choice `0x20`. Table 1 specifies
three fixed bits plus five version bits but also says the latter range is 0-63;
that discrepancy still needs authoritative clarification for future versions.

The upstream trailing-NULL convention for short historical serial numbers is
retained and is not asserted to resolve Table 3's big-endian/high-order-padding
wording. The main golden fixtures use a full 20-byte identifier to avoid that
unresolved interpretation. A production identity must separately meet
GB/T 41300; these synthetic strings are not registered aircraft identities.

Coordinate and altitude/vertical-speed quantization retain the upstream rounding
convention where the publication does not explicitly specify rounding. Angle
and ground speed use the required downward quantization. Known descending speed
is limited to -63 m/s because the next magnitude encodes `FF`, reserved for
unavailable data; unavailable optional vertical speed uses its explicit bit.
