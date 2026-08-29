# gun_stripezone_swans

This directory is an experiment-specific copy of `Source/gun_swans`.

## Experiment definition

- `Stripe_Unit_LBA`: 512 LBA
- `SWANS_Zone_Size_LBA`: 512 LBA
- `Zone_Stripe_Multiplier`: 1
- Zone size: 512 LBA × 512 bytes = 256 KiB
- SSD count: 4
- SWANS thresholds: precautionary 5, critical 15
- OP ratio and cache policy are unchanged from `gun_swans`.

In this implementation, SWANS `ZoneDirectory` compares zone size against
`Stripe_Unit_LBA`, so “Zone = Stripe” means one stripe unit (512 LBA). A full
four-SSD RAID stripe spans 2,048 LBA (1 MiB), but that is not the value used by
the zone-to-stripe validation in the code.

## Trace reproducibility

The experiment uses the same `prxy0_500k_realtime.trace` as the 16 MiB run.
Its SHA-256 is:

`3B818444E5892647BDB0EDFAB221663C0E0ABB264A89C0442D54D0AC259B51BD`
